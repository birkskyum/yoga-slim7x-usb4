// SPDX-License-Identifier: GPL-2.0-only
/* Private Yoga PCI0 bring-up. Derived from this board's captured PEP D0 list.
 * Not a general Qualcomm PCIe driver. PCI0-only initialization/reset;
 * no PHY reset, EL2 or SMMU takeover. See PCIE-INIT-EVIDENCE.md.
 * Register only after the single-owner USB4 CM has identified the LaCie.
 * No storage driver is registered here. Failure state is retained until off.
 * v24 uses emergency console severity for private live checkpoints because
 * the RAM-only wrapper silences ordinary logs. These are NOT panic reports.
 * No extra register reads, polling delays or reset operations are introduced.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interconnect.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/pci-ecam.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <dt-bindings/clock/qcom,x1e80100-gcc.h>
#include <linux/soc/qcom/x1-usb4-diag.h>
#include <asm/virt.h>
#include "x1-pcie.h"
#include "x1-pcie-init.h"
#include "x1-pcie-msi-audit.h"

struct x1_bridge {
	struct platform_device *pdev;
	struct pci_host_bridge *bridge;
	struct clk *clocks[8];
	struct icc_path *mem, *cfg;
	bool attempted, powered, downstream, ready;
	int error;
	unsigned int enabled;
	struct pci_dev *endpoint;
	bool endpoint_validated;
	struct pci_config_window *root_cfg;
	bool root_validated, root_fixup_done;
	u32 root_class;
	void __iomem *parf, *router;
	struct reset_control *tunnel_reset;
	struct x1_pci0_io io;
	bool trained, train_attempted;
};
static DEFINE_MUTEX(bridge_lock);
static struct x1_bridge *bridge_owner;
static bool registered;
static bool registration_window;
static void __iomem *registration_router;
static const char *const clock_names[] = {
	"usb4-pcie", "cnoc", "pipe", "cfg", "aux", "slv-q2a", "slv", "master",
};

static u32 x1_bridge_read(void *ctx, unsigned int region, u32 off)
{
	struct x1_bridge *b = ctx;
	void __iomem *base = region == X1_ROOT ? b->root_cfg->win :
		region == X1_PARF ? b->parf : b->router;
	u32 v;
	dev_emerg(&b->pdev->dev, "PCI0 INIT READ BEGIN region=%u off=%05x\n", region, off);
	v = readl(base + off);
	dev_emerg(&b->pdev->dev, "PCI0 INIT READ END region=%u off=%05x value=%08x\n", region, off, v);
	return v;
}
static void x1_bridge_write(void *ctx, unsigned int region, u32 off, u32 value)
{
	struct x1_bridge *b = ctx;
	void __iomem *base = region == X1_ROOT ? b->root_cfg->win :
		region == X1_PARF ? b->parf : b->router;
	dev_emerg(&b->pdev->dev, "PCI0 INIT WRITE BEGIN region=%u off=%05x value=%08x\n", region, off, value);
	writel(value, base + off);
	mb();
	dev_emerg(&b->pdev->dev, "PCI0 INIT WRITE END region=%u off=%05x\n", region, off);
}
static int x1_bridge_reset(void *ctx, bool asserted)
{
	struct x1_bridge *b = ctx;
	int ret;
	dev_emerg(&b->pdev->dev, "PCI0 TUNNEL RESET BEGIN asserted=%u\n", asserted);
	ret = asserted ? reset_control_assert(b->tunnel_reset) :
		reset_control_deassert(b->tunnel_reset);
	dev_emerg(&b->pdev->dev, "PCI0 TUNNEL RESET END asserted=%u errno=%d\n", asserted, ret);
	return ret;
}
static void x1_bridge_delay(void *ctx, unsigned int ms)
{
	msleep(ms);
}
static int x1_bridge_init_resources(struct x1_bridge *b)
{
	struct device *dev = &b->pdev->dev;
	struct of_phandle_args reset, clock;
	struct device_node *gcc;
	struct resource *res = platform_get_resource(b->pdev, IORESOURCE_MEM, 1);
	bool valid;
	int ret;
	if (!registration_router || !res || res->start != 0x1c28000 ||
	    resource_size(res) != 0x8000 ||
	    !of_property_read_bool(of_root, "birk,usb4-pci0-init-v23")) return -EPERM;
	/* Yoga PRT0 D0 requests this branch separately from PCI0 PIPE. They
	 * share a parent, not an enable bit. Use the existing GCC provider;
	 * do not infer gate state from clk_get_rate() or program raw GCC MMIO.
	 * Check the new consumer before mappings, reset or power acquisition. */
	if (of_property_match_string(dev->of_node, "clock-names", "usb4-pcie") != 0 ||
	    of_count_phandle_with_args(dev->of_node, "clocks", "#clock-cells") != ARRAY_SIZE(clock_names))
		return -EPERM;
	ret = of_parse_phandle_with_args(dev->of_node, "clocks", "#clock-cells", 0, &clock);
	if (ret) return ret;
	gcc = of_find_node_by_path("/soc@0/clock-controller@100000");
	valid = gcc && clock.np == gcc && clock.args_count == 1 &&
		clock.args[0] == GCC_USB4_0_PHY_PCIE_PIPE_CLK;
	of_node_put(gcc);
	of_node_put(clock.np);
	if (!valid) return -EPERM;
	ret = of_parse_phandle_with_args(dev->of_node, "resets", "#reset-cells", 0, &reset);
	if (ret) return ret;
	gcc = of_find_node_by_path("/soc@0/clock-controller@100000");
	valid = gcc && reset.np == gcc && reset.args_count == 1 &&
		reset.args[0] == GCC_PCIE_0_TUNNEL_BCR &&
		of_count_phandle_with_args(dev->of_node, "resets", "#reset-cells") == 1;
	of_node_put(gcc);
	of_node_put(reset.np);
	if (!valid) return -EPERM;
	b->parf = devm_platform_ioremap_resource(b->pdev, 1);
	if (IS_ERR(b->parf)) return PTR_ERR(b->parf);
	b->tunnel_reset = devm_reset_control_get_exclusive(dev, "tunnel");
	if (IS_ERR(b->tunnel_reset)) return PTR_ERR(b->tunnel_reset);
	b->router = registration_router; /* Borrow the sole NHI/frontend mapping. */
	b->io = (struct x1_pci0_io) { .ctx = b, .read = x1_bridge_read,
		.write = x1_bridge_write, .reset = x1_bridge_reset, .delay = x1_bridge_delay };
	return 0;
}

/* v21 read 17cb:0111 / ff000001 at this board's ACPI PCI0 ECAM.
 * A class value alone does NOT establish that it is a bridge. Read only the
 * standard header/capability list and independently require Type 1 + PCIe RP.
 * No DBI, PARF, reset, class-register or other configuration writes here.
 */
static int x1_root_validate(struct x1_bridge *b, struct pci_config_window *cfg)
{
	struct device *dev = &b->pdev->dev;
	u32 h[16], cap, flags, ptr, pcie = 0, i;
	u64 seen = 0;

	for (i = 0; i < ARRAY_SIZE(h); i++) {
		dev_emerg(dev, "PCI0 HEADER READ BEGIN off=%02x\n", 4 * i);
		h[i] = readl(cfg->win + 4 * i);
		dev_emerg(dev, "PCI0 HEADER READ END off=%02x value=%08x\n", 4 * i, h[i]);
	}
	for (i = 0; i < ARRAY_SIZE(h); i += 4)
		dev_emerg(dev, "PCI0 ROOT HEADER off=%02x words=%08x %08x %08x %08x\n",
			 4 * i, h[i], h[i + 1], h[i + 2], h[i + 3]);
	if (h[0] != 0x011117cb ||
	    (h[2] >> 8 != PCI_CLASS_BRIDGE_PCI_NORMAL && h[2] != 0xff000001) ||
	    ((h[3] >> 16) & 0xff) != PCI_HEADER_TYPE_BRIDGE ||
	    !(h[1] & ((u32)PCI_STATUS_CAP_LIST << 16)))
		return -ENODEV;

	ptr = h[PCI_CAPABILITY_LIST / 4] & 0xff;
	for (i = 0; ptr && i < 48; i++) {
		/* Strict aligned pointers into the conventional capability space. */
		if (ptr < 0x40 || ptr > 0xfc || (ptr & 3) ||
		    (seen & BIT_ULL(ptr / 4 - 16)))
			return -EPROTO;
		seen |= BIT_ULL(ptr / 4 - 16);
		dev_emerg(dev, "PCI0 CAP READ BEGIN off=%02x\n", ptr);
		cap = readl(cfg->win + ptr);
		dev_emerg(dev, "PCI0 CAP READ END off=%02x word=%08x\n", ptr, cap);
		if (!(cap & 0xff) || (cap & 0xff) == 0xff)
			return -EPROTO;
		if ((cap & 0xff) == PCI_CAP_ID_EXP) {
			flags = cap >> 16;
			if (pcie || (flags & PCI_EXP_FLAGS_TYPE) !=
			    (PCI_EXP_TYPE_ROOT_PORT << 4) ||
			    !(flags & PCI_EXP_FLAGS_VERS) ||
			    (flags & PCI_EXP_FLAGS_VERS) > 2 || ptr > 0xc4)
				return -ENODEV;
			pcie = ptr;
		}
		ptr = (cap >> 8) & 0xff;
	}
	if (ptr || !pcie)
		return -ENODEV;
	b->root_cfg = cfg;
	b->root_class = h[2];
	b->root_validated = true;
	dev_emerg(dev, "PCI0 ROOT IDENTITY PASS: 17cb:0111 Type1 PCIe Root Port cap=%02x raw_class=%08x; no class-register write\n",
		 pcie, h[2]);
	return 0;
}

/* Like qcom_fixup_class() in the upstream DWC driver, correct Linux's class
 * only. Unlike that broad quirk, this private experiment requires the exact
 * owned PCI0 mapping plus independently validated header and RP capability.
 * Hardware class remains unchanged. Other QCOM roots/endpoints are untouched.
 */
static void x1_root_fixup_class(struct pci_dev *pdev)
{
	struct x1_bridge *b = READ_ONCE(bridge_owner);

	if (!b || !b->root_validated || !b->powered || b->error ||
	    !pci_is_root_bus(pdev->bus) || pdev->bus->sysdata != b->root_cfg ||
	    pci_domain_nr(pdev->bus) || pdev->bus->number || pdev->devfn ||
	    pdev->vendor != 0x17cb || pdev->device != 0x0111 ||
	    pdev->hdr_type != PCI_HEADER_TYPE_BRIDGE || pdev->multifunction ||
	    !pci_is_pcie(pdev) || pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT ||
	    pdev->class != (b->root_class >> 8) ||
	    pdev->revision != (b->root_class & 0xff))
		return;
	if (b->root_class == 0xff000001) {
		pdev->class = PCI_CLASS_BRIDGE_PCI_NORMAL;
		dev_emerg(&b->pdev->dev, "PCI0 ROOT CLASS FIXUP: software ff0000 -> 060400; hardware unchanged\n");
	}
	b->root_fixup_done = pdev->class == PCI_CLASS_BRIDGE_PCI_NORMAL;
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0111, x1_root_fixup_class);

bool x1_native_nvme_allowed(struct pci_dev *pdev)
{
	/* May run inside pci_rescan_bus(), with bridge_lock already held.
	 * The owner and endpoint references are retained until cold shutdown.
	 * Publish permission only after the entire bus inventory passes.
	 */
	struct x1_bridge *b = READ_ONCE(bridge_owner);
	return b && smp_load_acquire(&b->endpoint_validated) && b->endpoint == pdev;
}
EXPORT_SYMBOL_GPL(x1_native_nvme_allowed);

static ssize_t endpoint_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct x1_bridge *b = dev_get_drvdata(dev);
	ssize_t n;
	mutex_lock(&bridge_lock);
	if (b && b->ready && !b->error && b->endpoint)
		n = sysfs_emit(buf, "%s\n", pci_name(b->endpoint));
	else
		n = sysfs_emit(buf, "none\n");
	mutex_unlock(&bridge_lock);
	return n;
}
static DEVICE_ATTR_RO(endpoint);

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct x1_bridge *b = dev_get_drvdata(dev);
	return sysfs_emit(buf, "PCI0 attempted=%u powered=%u clocks=%u ready=%u downstream=%u errno=%d\n",
		b->attempted, b->powered, b->enabled, b->ready, b->downstream, b->error);
}
static DEVICE_ATTR_RO(status);
static struct attribute *x1_attrs[] = { &dev_attr_endpoint.attr, &dev_attr_status.attr, NULL };
ATTRIBUTE_GROUPS(x1);

static void __iomem *x1_ecam_map(struct pci_bus *bus, unsigned int devfn, int where)
{
	struct x1_bridge *b = bridge_owner;
	struct pci_config_window *cfg = bus->sysdata;
	if (!b || !b->powered || where < 0 || where >= 4096)
		return NULL;
	if (bus->number == cfg->busr.start) {
		if (devfn) return NULL; /* one root port, no aliases */
	} else if (!READ_ONCE(b->downstream)) {
		return NULL; /* no config requests beyond root until tunnel exists */
	}
	return pci_ecam_map_bus(bus, devfn, where);
}
static const struct pci_ecam_ops x1_ecam_ops = {
	.pci_ops = {
		.map_bus = x1_ecam_map,
		.read = pci_generic_config_read,
		.write = pci_generic_config_write,
	},
};

static int x1_bridge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_config_window *cfg;
	struct resource *res, buses = { .start = 0, .end = 255, .flags = IORESOURCE_BUS };
	struct x1_bridge *b;
	struct device_node *smmu;
	u32 domain;
	int i, ret;
	bool reserved;

	if (!READ_ONCE(registration_window) || is_hyp_mode_available() ||
	    !x1_diag_board_allowed() || !x1_diag_nhi_complete() ||
	    !of_property_read_bool(of_root, "birk,usb4-pci0-test") ||
	    !of_property_read_bool(of_root, "birk,usb4-pci0-init-v23") ||
	    !x1_diag_disabled("/soc@0/pcie@1bf8000") ||
	    !x1_diag_disabled("/soc@0/pcie@1c08000"))
		return -EPERM;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != 0x400000000ULL || resource_size(res) != 0x10000000 ||
	    of_property_read_u32(dev->of_node, "linux,pci-domain", &domain) || domain)
		return -EINVAL;
	/* Match ordinary EL1/Gunyah boot. Never take ownership of this SMMU. */
	smmu = of_find_node_by_path("/soc@0/iommu@15400000");
	reserved = smmu && of_property_match_string(smmu, "status", "reserved") == 0;
	of_node_put(smmu);
	if (!reserved) return -EPERM;
	if (bridge_owner) return -EBUSY;
	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b) return -ENOMEM;
	b->pdev = pdev;
	platform_set_drvdata(pdev, b);
	dev_emerg(dev, "PCI0 RESOURCES BEGIN\n");
	ret = x1_bridge_init_resources(b);
	dev_emerg(dev, "PCI0 RESOURCES END errno=%d\n", ret);
	if (ret) return ret;
	for (i = 0; i < ARRAY_SIZE(clock_names); i++) {
		b->clocks[i] = devm_clk_get(dev, clock_names[i]);
		if (IS_ERR(b->clocks[i])) return PTR_ERR(b->clocks[i]);
	}
	b->mem = devm_of_icc_get(dev, "pcie-mem");
	if (IS_ERR(b->mem)) return PTR_ERR(b->mem);
	b->cfg = devm_of_icc_get(dev, "cpu-pcie");
	if (IS_ERR(b->cfg)) return PTR_ERR(b->cfg);
	if (!b->mem || !b->cfg) return -ENODEV;
	bridge_owner = b;
	b->attempted = true;
	/* From this point bind even on failure: retain votes/references for cold off. */
	dev_emerg(dev, "PCI0 D0 BEGIN: Yoga GDSC, eight clocks, two votes; v26 USB4 PCIe branch candidate\n");
	pm_runtime_enable(dev);
	dev_emerg(dev, "PCI0 POWER RESUME BEGIN\n");
	ret = pm_runtime_resume_and_get(dev);
	dev_emerg(dev, "PCI0 POWER RESUME END errno=%d\n", ret);
	if (ret < 0) goto failed;
	pm_runtime_forbid(dev);
	for (i = 0; i < ARRAY_SIZE(clock_names); i++) {
		dev_emerg(dev, "PCI0 CLOCK ENABLE BEGIN name=%s\n", clock_names[i]);
		ret = clk_prepare_enable(b->clocks[i]);
		dev_emerg(dev, "PCI0 CLOCK ENABLE END name=%s errno=%d; rate-read follows\n", clock_names[i], ret);
		dev_emerg(dev, "PCI0 clock %s ret=%d rate=%lu\n", clock_names[i], ret, clk_get_rate(b->clocks[i]));
		if (ret) goto failed;
		b->enabled++;
	}
	/* PEP BUSARB values are bytes/sec; Linux ICC uses kbytes/sec. */
	dev_emerg(dev, "PCI0 ICC MEM BEGIN\n");
	ret = icc_set_bw(b->mem, 2000000, 2000000);
	dev_emerg(dev, "PCI0 ICC MEM END errno=%d\n", ret);
	if (ret) goto failed;
	dev_emerg(dev, "PCI0 ICC CFG BEGIN\n");
	ret = icc_set_bw(b->cfg, 75000, 0);
	dev_emerg(dev, "PCI0 ICC CFG END errno=%d\n", ret);
	if (ret) goto failed;
	b->powered = true;
	b->bridge = devm_pci_alloc_host_bridge(dev, 0);
	if (!b->bridge) { ret = -ENOMEM; goto failed; }
	cfg = pci_ecam_create(dev, res, &buses, &x1_ecam_ops);
	if (IS_ERR(cfg)) { ret = PTR_ERR(cfg); goto failed; }
	b->root_cfg = cfg;
	dev_emerg(dev, "PCI0 MODE BEGIN: quiescent identity, tunnel reset, RC selection; mailbox deferred to training\n");
	ret = x1_pci0_mode(&b->io);
	dev_emerg(dev, "PCI0 MODE END errno=%d\n", ret);
	if (ret) goto failed;
	/* Keep mapping on every later exit. No DMA exists at this point. */
	dev_emerg(dev, "PCI0 ECAM ROOT READ BEGIN\n");
	ret = x1_root_validate(b, cfg);
	dev_emerg(dev, "PCI0 ECAM ROOT READ END validation=%d\n", ret);
	if (ret) goto failed;
	dev_emerg(dev, "PCI0 SETUP BEGIN\n");
	ret = x1_pci0_setup(&b->io);
	dev_emerg(dev, "PCI0 SETUP END errno=%d\n", ret);
	if (ret) goto failed;
	/* Refresh the independent identity after the protected DBI setup. */
	ret = x1_root_validate(b, cfg);
	if (ret) goto failed;
	b->bridge->sysdata = cfg;
	b->bridge->ops = (struct pci_ops *)&x1_ecam_ops.pci_ops;
	b->bridge->msi_domain = true;
	dev_emerg(dev, "PCI0 HOST PROBE BEGIN; root-only PCI-core config accesses\n");
	ret = pci_host_probe(b->bridge);
	dev_emerg(dev, "PCI0 HOST PROBE END errno=%d\n", ret);
	if (ret) goto failed;
	if (!b->root_fixup_done) { ret = -ENODEV; goto failed; }
	b->ready = true;
	dev_emerg(dev, "PCI0 ROOT READY: segment=0, downstream config still blocked; no storage DMA\n");
	return 0;
failed:
	b->error = ret;
	dev_emerg(dev, "PCI0 STOP error=%d; resources retained, full shutdown required\n", ret);
	return 0;
}

static const struct of_device_id x1_bridge_match[] = {
	{ .compatible = "birk,yoga-x1-pci0-test" }, {},
};
static struct platform_driver x1_bridge_driver = {
	.probe = x1_bridge_probe,
	.prevent_deferred_probe = true,
	.driver = {
		.name = "yoga-x1-pci0-test",
		.of_match_table = x1_bridge_match,
		.suppress_bind_attrs = true,
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
		.dev_groups = x1_groups,
	},
};

int x1_pcie_start(void __iomem *router)
{
	int ret;
	/* Before driver registration (and genpd attachment), not just in probe. */
	if (!router || is_hyp_mode_available() || !x1_diag_board_allowed() || !x1_diag_nhi_complete() ||
	    !of_property_read_bool(of_root, "birk,usb4-pci0-test") ||
	    !of_property_read_bool(of_root, "birk,usb4-pci0-init-v23") ||
	    !x1_diag_disabled("/soc@0/pcie@1bf8000") ||
	    !x1_diag_disabled("/soc@0/pcie@1c08000")) return -EPERM;
	mutex_lock(&bridge_lock);
	if (registered) { ret = -EALREADY; goto out; }
	registered = true;
	registration_router = router;
	WRITE_ONCE(registration_window, true);
	ret = platform_driver_register(&x1_bridge_driver);
	WRITE_ONCE(registration_window, false);
	if (ret) goto out;
	ret = bridge_owner ? (bridge_owner->ready ? 0 : bridge_owner->error) : -ENODEV;
out:
	mutex_unlock(&bridge_lock);
	return ret;
}

/* Native CM calls only after private approval and rechecking live PAN/UID. */
int x1_pcie_train(void)
{
	struct x1_bridge *b;
	int ret;
	mutex_lock(&bridge_lock);
	b = bridge_owner;
	if (!b || !b->ready || b->error || b->train_attempted || b->downstream) {
		ret = -EPERM;
		goto out;
	}
	b->train_attempted = true;
	dev_emerg(&b->pdev->dev, "PCI0 TRAIN BEGIN\n");
	ret = x1_pci0_train(&b->io);
	dev_emerg(&b->pdev->dev, "PCI0 TRAIN END errno=%d\n", ret);
	if (!ret) {
		dev_emerg(&b->pdev->dev, "PCI0 WINDOWS BEGIN\n");
		ret = x1_pci0_windows(&b->io);
	}
	dev_emerg(&b->pdev->dev, "PCI0 WINDOWS END errno=%d\n", ret);
	if (ret) b->error = ret;
	else b->trained = true;
out:
	mutex_unlock(&bridge_lock);
	return ret;
}

static void x1_bridge_msi_audit(struct x1_bridge *b)
{
	u32 v[X1_MSI_AUDIT_WORDS];

	/* Called only in the existing successful, single-shot scan branch.
	 * The endpoint still has no driver. Retain all original MSI-X routing.
	 */
	dev_emerg(&b->pdev->dev, "V37 RECEIVER BEGIN: PCI0 only, before NVMe; nine reads, no writes\n");
	x1_pci0_msi_audit(&b->io, v);
	dev_emerg(&b->pdev->dev, "V37 RECEIVER DWC addr=%08x:%08x enable=%08x mask=%08x status=%08x\n",
		  v[1], v[0], v[2], v[3], v[4]);
	dev_emerg(&b->pdev->dev, "V37 RECEIVER PARF halt=%08x sid_offset=%08x bdf_translate=%08x bdf_to_sid=%08x\n",
		  v[5], v[6], v[7], v[8]);
	dev_emerg(&b->pdev->dev, "V37 RECEIVER END: raw configuration only; not ITS delivery or a routing fix\n");
}

struct endpoint_scan { struct x1_bridge *b; unsigned int nvmes, other; };
static int find_endpoint(struct pci_dev *pdev, void *data)
{
	struct endpoint_scan *s = data;
	if ((pdev->class >> 8) == PCI_CLASS_BRIDGE_PCI) return 0;
	if (pdev->class != PCI_CLASS_STORAGE_EXPRESS || pdev->driver ||
	    pci_domain_nr(pdev->bus) != 0) {
		s->other++;
		return 0;
	}
	s->nvmes++;
	if (!s->b->endpoint) s->b->endpoint = pci_dev_get(pdev);
	return 0;
}

int x1_pcie_scan(void)
{
	struct x1_bridge *b;
	struct endpoint_scan scan;
	int ret;
	mutex_lock(&bridge_lock);
	b = bridge_owner;
	if (!b || !b->ready || !b->trained || b->error || b->downstream) { ret = -EPERM; goto out; }
	WRITE_ONCE(b->downstream, true);
	dev_emerg(&b->pdev->dev, "PCI0 RESCAN BEGIN: verified USB4 tunnel, external segment only\n");
	pci_lock_rescan_remove();
	pci_rescan_bus(b->bridge->bus);
	scan = (struct endpoint_scan) { .b = b };
	pci_walk_bus(b->bridge->bus, find_endpoint, &scan);
	pci_unlock_rescan_remove();
	ret = scan.nvmes == 1 && !scan.other ? 0 : -ENODEV;
	if (ret) {
		b->error = ret;
		dev_emerg(&b->pdev->dev, "PCI0 endpoint refused nvmes=%u unexpected=%u; no storage driver\n", scan.nvmes, scan.other);
	} else {
		x1_bridge_msi_audit(b);
		smp_store_release(&b->endpoint_validated, true);
		dev_emerg(&b->pdev->dev, "PCI0 EXTERNAL NVME ENUMERATED bdf=%s vendor=%04x device=%04x; not a disk read\n",
			pci_name(b->endpoint), b->endpoint->vendor, b->endpoint->device);
	}
out:
	mutex_unlock(&bridge_lock);
	return ret;
}
