// SPDX-License-Identifier: GPL-2.0-only
/* Yoga router-0 RAM-only MCU and one-shot link checkpoint. Not a complete host driver. */
#include <crypto/sha2.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interconnect.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/pinctrl/consumer.h>
#include <linux/phy/phy-thunderbolt.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/socinfo.h>
#include <linux/soc/qcom/x1-usb4-diag.h>
#include <linux/unaligned.h>
#include <dt-bindings/phy/phy-qcom-qmp.h>
#include "qcom-usb4-x1-startup.h"
#include "qcom-usb4-x1-drom.h"
#include "qcom-usb4-drom.h"
#include "qcom-usb4-fw.h"
#include "qcom-usb4-mcu.h"
#include "qcom-usb4-typec.h"

static bool run;
module_param(run, bool, 0400);
MODULE_PARM_DESC(run, "Explicitly start one isolated router-0 MCU experiment");
static bool reference_only;
module_param(reference_only, bool, 0400);
MODULE_PARM_DESC(reference_only, "Stop after a reference-clock router read; no firmware upload/run");
static bool attempted;
static const char * const clock_names[] = { "ahb", "sys", "master", "axi", "tmu", "sb_if" };
static const char * const pin_names[] = {
	"rx-prepare", "txb-prepare", "txa-prepare", "rx-active", "txb-active", "txa-active"
};
static const u8 firmware_sha256[SHA256_DIGEST_SIZE] = {
	0xcd,0x4f,0x59,0x29,0xb5,0x1f,0x2d,0xbb,0x0b,0x58,0x36,0x93,0xff,0x8d,0x02,0x45,
	0x21,0xc8,0x7f,0x0d,0x2e,0x45,0xc7,0xad,0xc1,0x42,0xfe,0xd9,0x76,0x65,0x0b,0x99
};

struct probe {
	struct device *dev;
	void __iomem *base;
	struct phy *phy;
	struct regmap *tcsr;
	struct clk_bulk_data clocks[ARRAY_SIZE(clock_names)];
	struct icc_path *apps, *ddr;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins[ARRAY_SIZE(pin_names)];
	const struct firmware *fw;
	struct qcom_usb4_mcu mcu;
	struct x1_config cfg;
	struct x1_state state;
	u32 shared;
	int error;
	const char *stage;
	bool reference_read_complete;
};

static int identity(struct device *dev)
{
	struct of_phandle_args args;
	struct device_node *expected, *tcsr;
	struct resource res;
	u32 rx;
	int ret;
	bool valid;
	if (!run || !x1_diag_board_allowed() ||
	    !of_property_read_bool(dev->of_node, "birk,empty-phyc"))
		return -EPERM;
	ret = of_parse_phandle_with_args(dev->of_node, "phys", "#phy-cells", 0, &args);
	if (ret)
		return ret;
	expected = of_find_node_by_path("/soc@0/phy@fd5000");
	valid = expected && args.np == expected && args.args_count == 1 &&
		args.args[0] == QMP_USB43DP_USB4_PHY && of_device_is_available(expected) &&
		of_device_is_compatible(expected, "qcom,x1e80100-qmp-usb3-dp-phy") &&
		!of_property_read_u32(expected, "qcom,usb4-rx-eq", &rx) && rx == 0x6f;
	of_node_put(expected);
	of_node_put(args.np);
	if (!valid)
		return -EINVAL;
	tcsr = of_parse_phandle(dev->of_node, "qcom,tcsr", 0);
	valid = tcsr && of_device_is_compatible(tcsr, "qcom,x1e80100-tcsr") &&
		!of_address_to_resource(tcsr, 0, &res) && res.start == 0x1fc0000 &&
		resource_size(&res) == 0x30000;
	of_node_put(tcsr);
	return valid ? 0 : -EINVAL;
}

static int read_router(void *ctx, u32 offset, u32 *value)
{
	struct probe *p = ctx;
	if (offset & 3 || offset > 0x100000 - 4)
		return -EINVAL;
	dev_info(p->dev, "ACCESS ROUTER read BEGIN off=%05x\n", offset);
	*value = readl(p->base + offset);
	dev_info(p->dev, "ACCESS ROUTER read END off=%05x got=%08x\n", offset, *value);
	if (offset == 0x1200c)
		dev_info(p->dev, "DIAG ROUTER poll off=1200c got=%08x\n", *value);
	return 0;
}
static int check_router(struct probe *p, u32 offset, u32 mask, u32 expected)
{
	u32 actual;
	/* Firmware/DROM blocks have outer markers; do not print 10,200 words. */
	bool trace = offset < 0x13000 || offset >= 0x22000;

	if (trace)
		dev_info(p->dev, "ACCESS ROUTER verify BEGIN off=%05x\n", offset);
	actual = readl(p->base + offset);
	if (trace)
		dev_info(p->dev, "ACCESS ROUTER verify END off=%05x got=%08x\n", offset, actual);

	if ((actual & mask) == expected)
		return 0;
	dev_err(p->dev, "DIAG ROUTER mismatch off=%05x mask=%08x want=%08x got=%08x\n",
		offset, mask, expected, actual);
	return -EIO;
}
static int update_router(void *ctx, u32 offset, u32 mask, u32 value)
{
	struct probe *p = ctx;
	u32 old_value;
	int ret = read_router(ctx, offset, &old_value);
	if (ret || value & ~mask)
		return ret ?: -EINVAL;
	dev_info(p->dev, "ACCESS ROUTER write BEGIN off=%05x value=%08x\n",
		 offset, (old_value & ~mask) | value);
	writel((old_value & ~mask) | value, p->base + offset);
	dev_info(p->dev, "ACCESS ROUTER write END off=%05x\n", offset);
	/* Self-changing 0x1200c is verified by the bounded sequencer poll. */
	if (offset != 0x1200c && check_router(p, offset, mask, value))
		return -EIO;
	return 0;
}
static void delay_us(void *ctx, unsigned int us)
{
	if (us < 20)
		udelay(us);
	else
		usleep_range(us, us + us / 10 + 10);
}
static int owner(void *ctx, const struct x1_config *cfg)
{
	struct probe *p = ctx;
	if (cfg != &p->cfg || cfg->firmware != p->fw->data)
		return -EINVAL;
	if (check_router(p, 0x78640, U32_MAX, 3) || check_router(p, 0x22000, U32_MAX, 0) ||
	    check_router(p, 0x18, U32_MAX, 0) || check_router(p, 0x22010, U32_MAX, 0) ||
	    check_router(p, 0x2201c, U32_MAX, 0))
		return -EBUSY;
	return 0;
}

static int upload_checked(struct probe *p)
{
	size_t pos = 0;
	int ret;
	dev_info(p->dev, "ACCESS firmware upload BEGIN\n");
	ret = qcom_usb4_mcu_upload(&p->mcu, p->fw->data, p->fw->size);
	dev_info(p->dev, "ACCESS firmware upload END ret=%d\n", ret);
	if (ret) {
		dev_err(p->dev, "DIAG firmware upload helper ret=%d (before verify)\n", ret);
		return ret;
	}
	/* Recheck every uploaded word while RUN remains clear. */
	while (pos < p->fw->size) {
		u32 offset = get_unaligned_le32(p->fw->data + pos);
		u32 words = get_unaligned_le32(p->fw->data + pos + 4);
		dev_info(p->dev, "ACCESS firmware record verify BEGIN offset=%05x words=%u\n", offset, words);
		pos += 8;
		while (words--) {
			if (check_router(p, 0x13000 + offset, U32_MAX, get_unaligned_le32(p->fw->data + pos)))
				return -EIO;
			offset += 4;
			pos += 4;
		}
		dev_info(p->dev, "ACCESS firmware record verify END\n");
	}
	if (check_router(p, 0x22000, U32_MAX, 0))
		return -EBUSY;
	dev_info(p->dev, "DIAG firmware upload verified: all words; RUN remains clear\n");
	return 0;
}
static int store_drom(struct probe *p)
{
	u8 drom[QCOM_USB4_DROM_SIZE];
	unsigned int i;
	int ret = qcom_usb4_x1_drom_build(drom, sizeof(drom), p->cfg.serial, 0);
	if (ret)
		return ret;
	dev_info(p->dev, "ACCESS DROM bounds/control check BEGIN\n");
	if (p->mcu.drom_offset != 0xdf20 || sizeof(drom) > p->mcu.ram_size - 0xdf20 ||
	    readl(p->base + 0x22000))
		return -EINVAL;
	dev_info(p->dev, "ACCESS DROM UID writes BEGIN\n");
	writel(get_unaligned_le32(drom + 5), p->base + 0x801c);
	writel(get_unaligned_le32(drom + 1), p->base + 0x8020);
	dev_info(p->dev, "ACCESS DROM UID writes END\n");
	if (check_router(p, 0x801c, U32_MAX, get_unaligned_le32(drom + 5)) ||
	    check_router(p, 0x8020, U32_MAX, get_unaligned_le32(drom + 1)))
		return -EIO;
	dev_info(p->dev, "ACCESS DROM descriptor writes BEGIN\n");
	for (i = 0; i < sizeof(drom); i += 4)
		writel(get_unaligned_le32(drom + i), p->base + 0x20f20 + i);
	dev_info(p->dev, "ACCESS DROM descriptor writes END; verify BEGIN\n");
	for (i = 0; i < sizeof(drom); i += 4)
		if (check_router(p, 0x20f20 + i, U32_MAX, get_unaligned_le32(drom + i)))
			return -EIO;
	dev_info(p->dev, "DIAG DROM verified: UID and all descriptor words\n");
	return 0;
}

static void mcu_snapshot(struct probe *p, const char *label, int ret)
{
	/* Only the four status/control registers used by the existing MCU helper. */
	u32 run, ready, shared, command;

	dev_info(p->dev, "ACCESS MCU snapshot control BEGIN\n");
	run = readl(p->base + p->mcu.control_offset);
	dev_info(p->dev, "ACCESS MCU snapshot control END; ready BEGIN\n");
	ready = readl(p->base + 0x18);
	dev_info(p->dev, "ACCESS MCU snapshot ready END; shared BEGIN\n");
	shared = readl(p->base + p->mcu.shared_offset_reg);
	dev_info(p->dev, "ACCESS MCU snapshot shared END; command BEGIN\n");
	command = readl(p->base + p->mcu.control_offset + 0xc);
	dev_info(p->dev, "ACCESS MCU snapshot command END\n");

	dev_info(p->dev, "DIAG MCU %s ret=%d run=%08x ready=%08x shared=%08x command=%08x\n",
		label, ret, run, ready, shared, command);
}

static int select_rx_source(struct probe *p)
{
	u32 value;
	int ret;

	if (!reference_only)
		return x1_diag_gcc_apply(p->dev, X1_GCC_RX_PHY);
	/* Comparative read only. Never continue the startup on reference clocks. */
	dev_info(p->dev, "REFERENCE COMPARISON: keep RX source=REFERENCE; read 1200c then STOP\n");
	ret = read_router(p, 0x1200c, &value);
	if (ret)
		return ret;
	p->reference_read_complete = true;
	dev_info(p->dev, "REFERENCE READ COMPLETE got=%08x; intentional stop before firmware\n", value);
	return -ECANCELED;
}

static int enable_clocks(struct probe *p)
{
	int i, ret;

	/* Rates are framework reports, not a measurement of physical oscillation. */
	for (i = 0; i < ARRAY_SIZE(clock_names); ++i)
		dev_info(p->dev, "CLOCK before name=%s rate=%lu\n", clock_names[i], clk_get_rate(p->clocks[i].clk));
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(clock_names), p->clocks);
	dev_info(p->dev, "CLOCK enable six required clocks including sb_if ret=%d\n", ret);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(clock_names); ++i)
		dev_info(p->dev, "CLOCK after name=%s rate=%lu\n", clock_names[i], clk_get_rate(p->clocks[i].clk));
	return 0;
}

static int step(void *ctx, enum x1_step op, const struct x1_config *cfg)
{
	struct probe *p = ctx;
	u32 command, route;
	int ret;
	p->stage = x1_step_name(op);
	dev_info(p->dev, "CHECKPOINT %s\n", p->stage);
#define G(s, g) case X1_##s: return x1_diag_gcc_apply(p->dev, X1_GCC_##g)
	switch (op) {
	case X1_PHY_PREPARE: return x1_diag_phy_apply(p->phy, X1_DIAG_PHY_PREPARE);
	case X1_RX_PREPARE: return pinctrl_select_state(p->pinctrl, p->pins[0]);
	case X1_TXB_PREPARE: return pinctrl_select_state(p->pinctrl, p->pins[1]);
	case X1_TXA_PREPARE: return pinctrl_select_state(p->pinctrl, p->pins[2]);
	case X1_RX_ACTIVE: return pinctrl_select_state(p->pinctrl, p->pins[3]);
	case X1_TXB_ACTIVE: return pinctrl_select_state(p->pinctrl, p->pins[4]);
	case X1_TXA_ACTIVE: return pinctrl_select_state(p->pinctrl, p->pins[5]);
	G(RX_CLOCK_REFERENCE, RX_REFERENCE);
	case X1_RX_CLOCK_PHY: return select_rx_source(p);
	G(MISC_RESET_ASSERT, MISC_ASSERT); G(MISC_RESET_CLEAR, MISC_CLEAR);
	G(EXTRA_RESET_ASSERT, EXTRA_ASSERT); G(EXTRA_RESET_CLEAR, EXTRA_CLEAR);
	G(PIPE_RESET_ASSERT, PIPE_ASSERT); G(PIPE_RESET_CLEAR, PIPE_CLEAR);
	G(SYS_READY, SYS_READY); G(SYS_MEMORY_FORCE, MEMORY_FORCE); G(PIPE_HWCG_OFF, HWCG_OFF);
	case X1_DP_RESET_ASSERT: return x1_diag_phy_apply(p->phy, X1_DIAG_PHY_DP_ASSERT);
	case X1_DP_RESET_CLEAR: return x1_diag_phy_apply(p->phy, X1_DIAG_PHY_DP_CLEAR);
	case X1_TCSR_ROUTE:
		ret = regmap_update_bits(p->tcsr, 0x1b000, BIT(0), BIT(0));
		if (ret)
			return ret;
		ret = regmap_read(p->tcsr, 0x1b000, &route);
		return ret ?: (route & BIT(0) ? 0 : -EIO);
	case X1_UPLOAD: return upload_checked(p);
	case X1_DROM: return store_drom(p);
	case X1_PHY_COMPOSITE:
		ret = phy_set_mode_ext(p->phy, PHY_MODE_TBT, PHY_SUBMODE_USB4);
		dev_info(p->dev, "DIAG PHY set_mode ret=%d\n", ret);
		if (ret)
			return ret;
		ret = phy_init(p->phy);
		dev_info(p->dev, "DIAG PHY init ret=%d\n", ret);
		return ret;
	case X1_MCU_START:
		ret = qcom_usb4_mcu_start(&p->mcu, &p->shared);
		mcu_snapshot(p, ret ? "start-after-cleanup" : "start", ret);
		return ret;
	case X1_PRESET:
		ret = qcom_usb4_typec_preset_command(cfg->mcu_preset, &command);
		if (!ret)
			ret = qcom_usb4_mcu_command(&p->mcu, command, 10000);
		mcu_snapshot(p, "preset", ret);
		return ret;
	default: return -EINVAL;
	}
#undef G
}
static const struct x1_ops ops = { owner, step, read_router, update_router, delay_us };

static ssize_t result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct probe *p = dev_get_drvdata(dev);
	if (reference_only && p->reference_read_complete && p->error == -ECANCELED)
		return sysfs_emit(buf, "REFERENCE READ COMPLETE; intentionally stopped with RX on reference; no firmware or link test\n");
	return sysfs_emit(buf,
		"CONTROL %s stage=%s sequence=%s completed=%s errno=%d shared=%08x; NOT a USB4 link or storage test\n",
		p->error ? "FAILED" : "READS COMPLETE", p->stage,
		x1_step_name(p->state.active), x1_step_name(p->state.completed), p->error, p->shared);
}
static DEVICE_ATTR_RO(result);

static int link_connect(void *ctx, u32 word)
{
	struct probe *p = ctx;
	int ret;
	/* Same one-shot owner as startup; only v13-observed passive Gen3 words.
	 * The caller serializes commands and validates a live, ACKed PAN.
	 */
	if (p->error || !x1_diag_nhi_complete() || !x1_diag_board_allowed() ||
	    !of_property_read_bool(of_root, "birk,usb4-link-test") ||
	    (word != 0x2501 && word != 0x2701)) return -EPERM;
	dev_emerg(p->dev, "V33 MCU COMMAND BEGIN word=%08x\n", word);
	ret = qcom_usb4_mcu_command(&p->mcu, word, 10000);
	dev_emerg(p->dev, "V33 MCU COMMAND END errno=%d\n", ret);
	dev_emerg(p->dev, "V33 MCU SNAPSHOT BEGIN\n");
	mcu_snapshot(p, "connect", ret);
	dev_emerg(p->dev, "V33 MCU SNAPSHOT END\n");
	return ret;
}
static ssize_t control_stop_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	if (!sysfs_streq(buf, "1")) return -EINVAL;
	x1_diag_link_stop();
	return count;
}
static DEVICE_ATTR_WO(control_stop);

static int probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct probe *p;
	struct socinfo *info;
	size_t size;
	u8 digest[SHA256_DIGEST_SIZE];
	int ret, i;
	ret = identity(dev);
	if (ret)
		return dev_err_probe(dev, ret, "REFUSED: exact RAM-only Yoga image and board configuration required\n");
	if (attempted)
		return -EALREADY;
	attempted = true;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != 0x15600000 || resource_size(res) != 0x100000)
		return -EINVAL;
	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	platform_set_drvdata(pdev, p);
	p->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);
	p->phy = devm_phy_get(dev, "usb4");
	if (IS_ERR(p->phy))
		return dev_err_probe(dev, PTR_ERR(p->phy), "PHY unavailable; no retry\n");
	p->tcsr = syscon_regmap_lookup_by_phandle(dev->of_node, "qcom,tcsr");
	if (IS_ERR(p->tcsr))
		return PTR_ERR(p->tcsr);
	p->apps = devm_of_icc_get(dev, "apps-usb4");
	if (IS_ERR(p->apps))
		return PTR_ERR(p->apps);
	if (!p->apps || !dev->pm_domain)
		return -ENODEV;
	p->ddr = devm_of_icc_get(dev, "usb4-ddr");
	if (IS_ERR(p->ddr))
		return PTR_ERR(p->ddr);
	if (!p->ddr)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(clock_names); ++i)
		p->clocks[i].id = clock_names[i];
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(clock_names), p->clocks);
	if (ret)
		return ret;
	p->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(p->pinctrl))
		return PTR_ERR(p->pinctrl);
	for (i = 0; i < ARRAY_SIZE(pin_names); ++i) {
		p->pins[i] = pinctrl_lookup_state(p->pinctrl, pin_names[i]);
		if (IS_ERR(p->pins[i]))
			return PTR_ERR(p->pins[i]);
	}
	info = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID, &size);
	if (IS_ERR(info))
		return PTR_ERR(info);
	if (offsetofend(struct socinfo, serial_num) > size || !le32_to_cpu(info->serial_num))
		return -ENODEV;
	ret = request_firmware_direct(&p->fw, "qcom/x1-diag-mcu.bin", dev);
	if (ret)
		return ret;
	sha256(p->fw->data, p->fw->size, digest);
	if (p->fw->size != 40816 || memcmp(digest, firmware_sha256, sizeof(digest))) {
		release_firmware(p->fw);
		return -EKEYREJECTED;
	}
	ret = qcom_usb4_fw_validate(p->fw->data, p->fw->size, 0xf000);
	if (ret) {
		release_firmware(p->fw);
		return ret;
	}
	p->cfg = (struct x1_config){p->fw->data, p->fw->size, le32_to_cpu(info->serial_num), 0, 0x6f, 0x0c63, 100};
	ret = qcom_usb4_mcu_init(&p->mcu, p->base, 0x100000, 0xf000, QCOM_USB4_MCU_LEGACY);
	if (ret) {
		release_firmware(p->fw);
		return ret;
	}
	ret = x1_diag_gcc_claim(dev);
	if (ret) {
		release_firmware(p->fw);
		return ret;
	}
	/* Prevent module removal after a partial hardware operation. */
	__module_get(THIS_MODULE);
	p->stage = "interconnect";
	ret = icc_set_bw(p->apps, 0, MBps_to_icc(40));
	if (ret)
		goto finish;
	pm_runtime_enable(dev);
	p->stage = "router-power";
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto finish;
	p->stage = "router-clocks";
	ret = enable_clocks(p);
	if (ret)
		goto finish;
	p->stage = "sequence";
	ret = x1_startup_run(&p->state, &p->cfg, &ops, p);
	if (!ret) {
		p->stage = "ddr-interconnect";
		ret = icc_set_bw(p->ddr, 0, MBps_to_icc(40));
		if (!ret) {
			p->stage = "NHI_CONTROL";
			ret = x1_diag_nhi_run(dev, p->base);
			if (!ret) ret = x1_diag_link_register(link_connect, p);
			if (ret) x1_diag_link_stop();
		}
	}
finish:
	p->error = ret < 0 ? ret : ret ? -EPROTO : 0;
	dev_info(dev, "MCU checkpoint %s at %s / %s, error %d. Cold shutdown required.\n",
		p->error ? "FAILED" : "COMPLETE", p->stage, x1_step_name(p->state.active), p->error);
	ret = device_create_file(dev, &dev_attr_result);
	if (ret)
		dev_err(dev, "result file failed: %d; photograph kernel output\n", ret);
	ret = device_create_file(dev, &dev_attr_control_stop);
	if (ret) {
		x1_diag_link_stop();
		dev_err(dev, "control_stop file failed: %d; cold shutdown required\n", ret);
	}
	/* Retain all mappings/references. Link requires a separate guarded request. */
	return 0;
}
static const struct of_device_id match[] = { { .compatible = "birk,yoga-x1-usb4-mcu-test" }, {} };
MODULE_DEVICE_TABLE(of, match);
static struct platform_driver yoga_x1_mcu_driver = {
	.probe = probe,
	.prevent_deferred_probe = true,
	.driver = { .name = "yoga-x1-mcu-probe", .of_match_table = match,
		    .suppress_bind_attrs = true, .probe_type = PROBE_FORCE_SYNCHRONOUS },
};
module_platform_driver(yoga_x1_mcu_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Manual isolated Yoga X1 MCU and one-shot link-read checkpoint; no storage");
