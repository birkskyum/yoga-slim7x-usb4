// SPDX-License-Identifier: GPL-2.0-only
/*
 * X1 PCI0 empty-port local preparation experiment. The mode sequence is derived from
 * the separately recorded x1-pcie-init.h diagnostic, not a general PCI driver.
 * Legacy fixed CFG0 local preparation never maps/accesses a downstream page.
 * A distinct batch profile adds one fixed page, bounded training and read-only
 * inventory under caller-owned exact CM tunnel/lifecycle guards. A separately
 * armed private read-test continuation publishes one exact PCI topology and
 * binds only its NVMe endpoint after frontend locks have been released.
 * Provider leases and a successfully attached driver are deliberately retained
 * after every admitted result. Only full cold power-off ends this experiment.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interconnect.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <dt-bindings/clock/qcom,x1e80100-gcc.h>
#include <dt-bindings/interconnect/qcom,x1e80100-rpmh.h>
#include <dt-bindings/interconnect/qcom,icc.h>

#include "qcom-usb4-x1-pcie.h"
#include "qcom-usb4-x1-decoder.h"
#include "qcom-usb4-x1-cfg0.h"
/* Only the separately armed complete inventory batch can call these. */
#include <linux/nvme-x1-private.h>
#include "qcom-usb4-x1-train.h"
#include "qcom-usb4-x1-inventory.h"
#include "x1-imsi-policy.h"

#define X1_PCIE_NODE "/soc@0/pcie-imsi@400000000"
#define X1_PCIE_COMPAT "birk,yoga-x1-pci0-imsi-local"
#define X1_GCC_NODE "/soc@0/clock-controller@100000"
#define X1_PCIE_GATE 0x82000
#define X1_PCIE_LTSSM 0x1b0

static const char *const x1_pcie_clock_names[] = {
	"usb4-pcie", "cnoc", "pipe", "cfg", "aux", "slv-q2a", "slv", "master",
};
static const u32 x1_pcie_clock_ids[] = {
	GCC_USB4_0_PHY_PCIE_PIPE_CLK, GCC_CNOC_PCIE_TUNNEL_CLK,
	GCC_PCIE_0_PIPE_CLK, GCC_PCIE_0_CFG_AHB_CLK, GCC_PCIE_0_AUX_CLK,
	GCC_PCIE_0_SLV_Q2A_AXI_CLK, GCC_PCIE_0_SLV_AXI_CLK, GCC_PCIE_0_MSTR_AXI_CLK,
};

struct x1_pcie_context {
	struct device *owner;
	struct platform_device *pdev;
	struct x1_pcie_state *state;
	void __iomem *router, *root, *parf;
	struct clk *clocks[ARRAY_SIZE(x1_pcie_clock_names)];
	struct reset_control *reset;
	/* Renewed generations acquire this before probe, outside its devres. */
	bool reset_early;
	struct icc_path *mem, *cfg;
	bool window, entered;
	u32 root_bytes;
	void __iomem *config;
	struct x1_cfg0_state prepared_cfg0;
	struct x1_train_state *training;
	int (*batch_live)(void *);
	void *batch_context;
	bool batch_active;
	struct x1_pcie_inventory *inventory;
	struct pci_host_bridge *bridge;
	struct x1_imsi *receiver;
	struct x1_imsi_policy policy;
	bool receiver_retained;
	bool driver_registered, runtime_enabled, runtime_held;
	u32 clocks_enabled;
	u64 nvme_session;
	struct pci_dev *root_port, *endpoint;
	u16 root_msi, root_msix, root_pm, endpoint_pm;
	/* Config callbacks cannot sleep or call the CM. Phase is permanently
	 * retained; errors latch, and only the one bind call opens DMA/MSI writes.
	 */
	bool config_open, scan_writes, bind_writes;
};

static DEFINE_MUTEX(x1_pcie_lock);
static DEFINE_RAW_SPINLOCK(x1_config_lock);
static struct x1_pcie_context *x1_pcie;
/* Physical reset remains asserted between successfully retired generations. */
static bool x1_pcie_reset_held;

static int x1_pcie_mask(void __iomem *base, u32 offset, u32 mask, u32 value)
{
	u32 old = readl(base + offset);

	if (old == U32_MAX)
		return -EIO;
	writel((old & ~mask) | (value & mask), base + offset);
	return (readl(base + offset) & mask) == (value & mask) ? 0 : -EIO;
}

static void x1_pcie_snapshot(struct x1_pcie_context *ctx)
{
	struct x1_pcie_state *s = ctx->state;

	s->root_id = readl(ctx->root + PCI_VENDOR_ID);
	s->command_status = readl(ctx->root + PCI_COMMAND);
	s->class_revision = readl(ctx->root + PCI_CLASS_REVISION);
	s->header = readl(ctx->root + PCI_CACHE_LINE_SIZE);
}

static int x1_pcie_disabled(struct x1_pcie_context *ctx)
{
	u32 control = readl(ctx->parf + X1_PCIE_LTSSM);

	if (control == U32_MAX || control & BIT(8) ||
	    readl(ctx->root + PCI_COMMAND) &
				(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
		return -EBUSY;
	return 0;
}

static int x1_pcie_quiescent(struct x1_pcie_context *ctx)
{
	if (readl(ctx->router + X1_PCIE_GATE) != 0)
		return -EBUSY;
	return x1_pcie_disabled(ctx);
}

/* Validate physical configuration data; no class-code fixup is performed. */
static int x1_pcie_root_identity(struct x1_pcie_context *ctx)
{
	struct x1_pcie_state *s = ctx->state;
	u64 seen = 0;
	u32 ptr, cap, flags, pcie = 0;
	unsigned int count;

	x1_pcie_snapshot(ctx);
	if (s->root_id != 0x011117cb ||
	    (s->header >> 16 & 0xff) != PCI_HEADER_TYPE_BRIDGE ||
	    !(s->command_status & (PCI_STATUS_CAP_LIST << 16)) ||
	    s->command_status & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
		return -ENODEV;
	ptr = readl(ctx->root + PCI_CAPABILITY_LIST) & 0xff;
	for (count = 0; ptr && count < 48; count++) {
		if (ptr < 0x40 || ptr > 0xfc || ptr & 3 ||
		    seen & BIT_ULL(ptr / 4 - 16))
			return -EINVAL;
		seen |= BIT_ULL(ptr / 4 - 16);
		cap = readl(ctx->root + ptr);
		if (!(cap & 0xff) || (cap & 0xff) == 0xff)
			return -ENODEV;
		if ((cap & 0xff) == PCI_CAP_ID_MSI)
			ctx->root_msi = ptr;
		if ((cap & 0xff) == PCI_CAP_ID_MSIX)
			ctx->root_msix = ptr;
		if ((cap & 0xff) == PCI_CAP_ID_PM)
			ctx->root_pm = ptr;
		if (((cap & 0xff) == PCI_CAP_ID_MSI &&
		     (cap >> 16 & PCI_MSI_FLAGS_ENABLE)) ||
		    ((cap & 0xff) == PCI_CAP_ID_MSIX &&
		     (cap >> 16 & PCI_MSIX_FLAGS_ENABLE)))
			return -EBUSY;
		if ((cap & 0xff) == PCI_CAP_ID_EXP) {
			flags = cap >> 16;
			if (pcie || ptr > 0xc4 ||
			    (flags & PCI_EXP_FLAGS_TYPE) != (PCI_EXP_TYPE_ROOT_PORT << 4) ||
			    !(flags & PCI_EXP_FLAGS_VERS) ||
			    (flags & PCI_EXP_FLAGS_VERS) > 2)
				return -EINVAL;
			pcie = ptr;
		}
		ptr = cap >> 8 & 0xff;
	}
	if (ptr || !pcie)
		return -ENODEV;
	s->pcie_cap = pcie;
	return 0;
}

static int x1_pcie_root_validate(struct x1_pcie_context *ctx)
{
	int ret = x1_pcie_root_identity(ctx);

	return ret ?: x1_pcie_quiescent(ctx);
}

/*
 * Observations only, within the already admitted 4 KiB root / 32 KiB PARF.
 * No iATU/configuration aperture access or internal-host reads. The standard
 * DWC viewport all-ones value denotes unrolled iATU; other raw values are not
 * interpreted as proof of decoder readiness or interrupt routing. A finite
 * read list does not contain a hardware SError/hung MMIO transaction.
 */
static int x1_pcie_inspect(struct x1_pcie_context *ctx)
{
	static const u32 root_offsets[] = { 0x8f8, 0x8fc, 0x900 };
	static const u32 parf_offsets[] = {
		0, 0x174, 0x1a8, 0x1b0, 0x350, 0x354, 0x634, 0x638, 0x358, 0x35c,
	};
	struct x1_pcie_state *s = ctx->state;
	unsigned int i;
	int ret;

	if (!s->inspect_requested || s->inspect_attempted)
		return -EPERM;
	s->inspect_attempted = true;
	s->inspect_complete = false;
	s->step = "inspect-root-fence";
	ret = x1_pcie_root_validate(ctx);
	if (ret)
		return ret;
	s->step = "inspect-root-registers";
	for (i = 0; i < ARRAY_SIZE(root_offsets); i++) {
		ret = x1_pcie_quiescent(ctx);
		if (ret)
			return ret;
		s->inspect_root[i] = readl(ctx->root + root_offsets[i]);
		s->inspect_reads++;
	}
	s->step = "inspect-parf-registers";
	for (i = 0; i < ARRAY_SIZE(parf_offsets); i++) {
		ret = x1_pcie_quiescent(ctx);
		if (ret)
			return ret;
		s->inspect_parf[i] = readl(ctx->parf + parf_offsets[i]);
		s->inspect_reads++;
	}
	s->step = "inspect-final-fence";
	ret = x1_pcie_root_validate(ctx);
	if (ret)
		return ret;
	s->inspect_complete = true;
	return 0;
}

static int x1_pcie_mode(struct x1_pcie_context *ctx)
{
	struct x1_pcie_state *s = ctx->state;
	int ret;

	s->step = "quiescent-root";
	x1_pcie_snapshot(ctx);
	if (s->root_id != 0x011117cb || s->command_status != 0x00100000 ||
	    s->class_revision != 0xff000001 || s->header != 0)
		return -ENODEV;
	ret = x1_pcie_quiescent(ctx);
	if (ret)
		return ret;
	s->step = "ltssm-disable";
	ret = x1_pcie_mask(ctx->parf, X1_PCIE_LTSSM, BIT(8), 0);
	if (ret)
		return ret;
	s->step = "gate-set";
	writel(1, ctx->router + X1_PCIE_GATE);
	if (readl(ctx->router + X1_PCIE_GATE) != 1)
		return -EIO;
	s->step = "reset-assert";
	ret = reset_control_assert(ctx->reset);
	if (ret)
		return ret;
	msleep(5);
	s->step = "reset-deassert";
	ret = reset_control_deassert(ctx->reset);
	if (ret)
		return ret;
	msleep(5);
	s->step = "reset-quiescent";
	ret = x1_pcie_disabled(ctx);
	if (ret)
		return ret;
	s->step = "root-mode";
	ret = x1_pcie_mask(ctx->parf, 0x1000, 0xf, 4);
	if (ret)
		return ret;
	s->step = "mode-quiescent";
	ret = x1_pcie_disabled(ctx);
	if (ret)
		return ret;
	s->step = "gate-clear";
	writel(0, ctx->router + X1_PCIE_GATE);
	if (readl(ctx->router + X1_PCIE_GATE))
		return -EIO;
	msleep(5);
	s->step = "physical-root-port";
	return x1_pcie_root_validate(ctx);
}

/* The caller retains every provider and holds all frontend/domain locks.
 * No first-page read can guarantee that later MMIO will not hang or SError.
 */
static int x1_pcie_local_live(void *context)
{
	struct x1_pcie_context *ctx = context;

	if (!ctx->window || !ctx->entered || !ctx->owner || !ctx->state ||
	    !ctx->state->attempted || !ctx->state->cfg0_requested)
		return -EPERM;
	return x1_pcie_quiescent(ctx);
}

/* Boundary keeps the full quiescent root check. BASE_BATCH deliberately uses
 * only stable router/PARF guards: DBI may be undecodable between CPU-base
 * and mirror-period writes. No indirect DBI access is permitted there. The
 * caller retains all lifecycle/Type-C/domain locks and providers throughout
 * this terminal batch.
 * Never read a later guard after a refusal, nor probe to explain a failure.
 */
static int x1_pcie_decoder_live(void *context, enum x1_decoder_phase phase,
				struct x1_decoder_observation *o)
{
	struct x1_pcie_context *ctx = context;
	u32 value;

	if (phase != X1_DECODER_BOUNDARY && phase != X1_DECODER_BASE_BATCH)
		return -EINVAL;
	if (!ctx->window || !ctx->entered || !ctx->owner || !ctx->state ||
	    !ctx->state->attempted || !ctx->state->cfg0_requested)
		return -EPERM;
	x1_decoder_observe_begin(o, "live-router-gate", "router", X1_PCIE_GATE);
	value = readl(ctx->router + X1_PCIE_GATE);
	o->value = value;
	o->valid = true;
	if (value != 0)
		return -EBUSY;
	x1_decoder_observe_begin(o, "live-ltssm", "parf", X1_PCIE_LTSSM);
	value = readl(ctx->parf + X1_PCIE_LTSSM);
	o->value = value;
	o->valid = true;
	if (phase == X1_DECODER_BASE_BATCH)
		return value ? -EBUSY : 0;
	if (value == U32_MAX || value & BIT(8))
		return -EBUSY;
	x1_decoder_observe_begin(o, "live-command", "root", PCI_COMMAND);
	value = readl(ctx->root + PCI_COMMAND);
	o->value = value;
	o->valid = true;
	return value & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER) ?
		-EBUSY : 0;
}

static int x1_pcie_decoder_read(void *context, enum x1_decoder_region region,
				u32 offset, u32 *value)
{
	struct x1_pcie_context *ctx = context;

	if ((region != X1_DECODER_ROOT && region != X1_DECODER_PARF) ||
	    offset & 3 || offset > (region == X1_DECODER_ROOT ? 0xffc : 0x7ffc))
		return -EINVAL;
	*value = readl((region == X1_DECODER_ROOT ? ctx->root : ctx->parf) + offset);
	return 0;
}

static int x1_pcie_decoder_write(void *context, enum x1_decoder_region region,
				 u32 offset, u32 value)
{
	struct x1_pcie_context *ctx = context;

	if (region != X1_DECODER_PARF ||
	    !((offset == 0x350 && value == 0) || (offset == 0x354 && value == 4) ||
	      (offset == 0x634 && value == 0x1000) || (offset == 0x638 && value == 4) ||
	      (offset == 0x358 && value == 0) || (offset == 0x35c && value == 0x80000000)))
		return -EINVAL;
	writel(value, ctx->parf + offset);
	return 0;
}

static int x1_pcie_cfg0_read(void *context, enum x1_cfg0_region region,
			     u32 offset, u32 *value)
{
	struct x1_pcie_context *ctx = context;

	if ((region != X1_CFG0_ROOT && region != X1_CFG0_PARF) || offset & 3 ||
	    offset > (region == X1_CFG0_ROOT ? 0x1ffc : 0x7ffc))
		return -EINVAL;
	*value = readl((region == X1_CFG0_ROOT ? ctx->root : ctx->parf) + offset);
	return 0;
}

static int x1_pcie_cfg0_write(void *context, enum x1_cfg0_region region,
			      u32 offset, u32 value)
{
	struct x1_pcie_context *ctx = context;

	if (!x1_cfg0_write_allowed(region, offset))
		return -EINVAL;
	ctx->state->cfg0_writes++;
	writel(value, ctx->root + offset);
	return 0;
}

static int x1_pcie_cfg0_delay(void *context, unsigned int ms)
{
	int ret = x1_pcie_local_live(context);

	if (ret)
		return ret;
	if (ms != 10)
		return -EINVAL;
	msleep(ms);
	return 0;
}

static int x1_pcie_local_cfg0(struct x1_pcie_context *ctx)
{
	struct x1_pcie_state *s = ctx->state;
	struct x1_decoder_state decoder = {};
	struct x1_cfg0_state cfg0 = {};
	struct x1_decoder_io decoder_io = {
		.context = ctx, .read = x1_pcie_decoder_read, .write = x1_pcie_decoder_write,
		.check_live = x1_pcie_decoder_live, .root_bytes = 0x1000, .parf_bytes = 0x8000,
	};
	struct x1_cfg0_io cfg0_io = {
		.context = ctx, .read = x1_pcie_cfg0_read, .write = x1_pcie_cfg0_write,
		.delay_ms = x1_pcie_cfg0_delay,
		.check_live = x1_pcie_local_live, .root_bytes = 0x2000, .parf_bytes = 0x8000,
	};
	int ret;

	if (!s->cfg0_requested || s->cfg0_attempted || !s->inspect_complete ||
	    ctx->root_bytes != 0x2000)
		return -EPERM;
	s->cfg0_attempted = true;
	s->step = "local-decoder";
	ret = x1_decoder_prepare(&decoder_io, &decoder);
	s->decoder_prepared = decoder.prepared;
	s->decoder_writes = decoder.writes_issued;
	s->decoder_step = decoder.step;
	s->decoder_error = ret;
	s->decoder_check = decoder.observation.check;
	s->decoder_region = decoder.observation.region;
	s->decoder_offset = decoder.observation.offset;
	s->decoder_value = decoder.observation.value;
	s->decoder_valid = decoder.observation.valid;
	s->decoder_write_offset = decoder.write_offset;
	s->decoder_write_value = decoder.write_value;
	s->decoder_write_issued = decoder.write_issued;
	s->decoder_write_returned = decoder.write_returned;
	s->decoder_write_readback = decoder.write_readback;
	if (ret)
		return ret;
	s->step = "local-cfg0";
	ret = x1_cfg0_prepare(&cfg0_io, &cfg0);
	ctx->prepared_cfg0 = cfg0;
	s->cfg0_complete = cfg0.prepared;
	s->cfg0_enable_issued = cfg0.enable_issued;
	s->cfg0_step = cfg0.step;
	s->cfg0_error = ret;
	s->cfg0_alignment = cfg0.alignment;
	s->cfg0_upper_limit_mask = cfg0.upper_limit_mask;
	if (ret)
		return ret;
	/* Repeat root-port/capability and LTSSM/command fences after both phases. */
	s->step = "local-final-fence";
	ret = x1_pcie_root_validate(ctx);
	if (ret) {
		s->cfg0_complete = false;
		s->cfg0_error = ret;
		s->cfg0_step = "root-final-fence";
	}
	return ret;
}

/*
 * FDT nodes store a unit name in full_name, not an absolute tree path.
 * Resolve the exact path and compare identity; never match a leaf name.
 */
static bool x1_pcie_node_at(const struct device_node *np, const char *path)
{
	struct device_node *expected = of_find_node_by_path(path);
	bool valid = expected && np == expected;

	of_node_put(expected);
	return valid;
}

static bool x1_pcie_cells(struct device_node *np, const char *property,
			  const char *cells, unsigned int index,
			  const char *path, u32 first, u32 second, int nargs)
{
	struct of_phandle_args args;
	bool valid;

	if (of_parse_phandle_with_args(np, property, cells, index, &args))
		return false;
	valid = x1_pcie_node_at(args.np, path) &&
		of_device_is_available(args.np) && args.args_count == nargs &&
		args.args[0] == first && (nargs == 1 || args.args[1] == second);
	of_node_put(args.np);
	return valid;
}

static bool x1_pcie_baseline_active(const char *path)
{
	struct device_node *np = of_find_node_by_path(path);
	struct platform_device *pdev;
	bool active = false;

	if (!np || !of_device_is_available(np) ||
	    !of_device_is_compatible(np, "qcom,pcie-x1e80100"))
		goto out_node;
	pdev = of_find_device_by_node(np);
	if (pdev) {
		/* Observe, never wake or change the working storage/Wi-Fi hosts. */
		active = device_is_bound(&pdev->dev) && pm_runtime_active(&pdev->dev);
		put_device(&pdev->dev);
	}
out_node:
	of_node_put(np);
	return active;
}

/* Strict private DT shape before platform PM/provider operations. The range is
 * reserved for a future host; no ATU MEM programming or BAR assignment occurs.
 */
static bool x1_pcie_u32s(struct device_node *np, const char *name,
			   const u32 *expected, unsigned int count)
{
	u32 values[40];
	unsigned int i;

	if (count > ARRAY_SIZE(values) || of_property_count_u32_elems(np, name) != count ||
	    of_property_read_u32_array(np, name, values, count))
		return false;
	for (i = 0; i < count; i++)
		if (values[i] != expected[i])
			return false;
	return true;
}

static bool x1_pcie_receiver_shape(struct device_node *np)
{
	static const u32 spis[] = { 655, 657, 664, 668, 348, 349, 351, 702, 167 };
	static const u32 intx[] = { 703, 708, 714, 716 };
	static const u32 range[] = { 0x02000000, 0, 0x40000000, 0, 0x40000000, 0, 0x10000000 };
	struct device_node *intc, *parent;
	u32 map[40], irqs[27];
	unsigned int i;
	bool valid;

	if (!x1_pcie->state->inventory_requested || !x1_pcie->state->receiver_requested ||
	    !of_node_is_type(np, "pci") || !of_property_present(np, "dma-coherent") ||
	    !x1_pcie_u32s(np, "#address-cells", (u32[]){3}, 1) ||
	    !x1_pcie_u32s(np, "#size-cells", (u32[]){2}, 1) ||
	    !x1_pcie_u32s(np, "#interrupt-cells", (u32[]){1}, 1) ||
	    !x1_pcie_u32s(np, "bus-range", (u32[]){0, 1}, 2) ||
	    !x1_pcie_u32s(np, "linux,pci-domain", (u32[]){0}, 1) ||
	    !x1_pcie_u32s(np, "ranges", range, ARRAY_SIZE(range)) ||
	    !x1_pcie_u32s(np, "interrupt-map-mask", (u32[]){0, 0, 0, 7}, 4) ||
	    of_property_count_strings(np, "interrupt-names") != 9 || of_irq_count(np) != 9)
		return false;
	intc = of_find_node_by_path("/soc@0/interrupt-controller@17000000");
	parent = of_parse_phandle(np, "interrupt-parent", 0);
	valid = intc && intc == parent;
	of_node_put(parent);
	if (!valid)
		goto out;
	for (i = 0; i < ARRAY_SIZE(spis); i++) {
		char name[] = "msi0";

		name[3] += i;
		if (of_property_match_string(np, "interrupt-names", i == 8 ? "global" : name) != i) {
			valid = false;
			goto out;
		}
		irqs[i * 3] = 0;
		irqs[i * 3 + 1] = spis[i];
		irqs[i * 3 + 2] = IRQ_TYPE_LEVEL_HIGH;
	}
	for (i = 0; i < 4; i++) {
		u32 row[] = { 0, 0, 0, i + 1, intc->phandle, 0, 0, 0,
			      intx[i], IRQ_TYPE_LEVEL_HIGH };

		memcpy(map + i * 10, row, sizeof(row));
	}
	valid = x1_pcie_u32s(np, "interrupts", irqs, ARRAY_SIZE(irqs)) &&
		x1_pcie_u32s(np, "interrupt-map", map, ARRAY_SIZE(map));
out:
	of_node_put(intc);
	return valid;
}

/* All DT checks precede registration: generic platform PM attach precedes probe. */
static int x1_pcie_admit(struct device *owner, struct platform_device **target)
{
	static const char *const forbidden[] = {
		"iommus", "iommu-map", "iommu-map-mask", "msi-map", "msi-map-mask", "msi-parent",
		"interrupts-extended", "dma-ranges", "assigned-clocks",
		"assigned-clock-parents", "assigned-clock-rates", "phys",
	};
	struct device_node *np, *other, *smmu;
	struct platform_device *pdev;
	struct resource res;
	const char *status;
	unsigned int i, count = 0;
	int ret = -ENODEV;

	if (!of_machine_is_compatible("lenovo,yoga-slim7x") || !owner->of_node ||
	    !x1_pcie_node_at(owner->of_node, "/soc@0/usb4@15600000") ||
	    !of_device_is_compatible(owner->of_node, "qcom,x1e80100-usb4") ||
	    !x1_pcie_baseline_active("/soc@0/pcie@1c08000") ||
	    !x1_pcie_baseline_active("/soc@0/pcie@1bf8000"))
		return -ENODEV;
	smmu = of_find_node_by_path("/soc@0/iommu@15400000");
	if (!smmu)
		return -ENODEV;
	ret = of_property_read_string(smmu, "status", &status);
	if (!ret && (strcmp(status, "reserved") ||
		     !of_device_is_compatible(smmu, "arm,smmu-v3")))
		ret = -ENODEV;
	of_node_put(smmu);
	if (ret)
		return ret;
	for_each_compatible_node(other, NULL, X1_PCIE_COMPAT)
		count++;
	if (count != 1)
		return -ENODEV;
	np = of_find_node_by_path(X1_PCIE_NODE);
	if (!np)
		return -ENODEV;
	ret = -EINVAL;
	if (!of_device_is_available(np) || !of_device_is_compatible(np, X1_PCIE_COMPAT))
		goto out;
	for (i = 0; i < ARRAY_SIZE(forbidden); i++)
		if (of_property_present(np, forbidden[i]))
			goto out;
	if (!x1_pcie_receiver_shape(np))
		goto out;
	if (x1_pcie->state->nvme_requested &&
	    !of_property_read_bool(np, "qcom,x1-private-nvme-read-test"))
		goto out;
	if (of_property_count_strings(np, "reg-names") !=
			(x1_pcie->state->inventory_requested ? 3 : 2) ||
	    of_property_match_string(np, "reg-names", "root") != 0 ||
	    of_property_match_string(np, "reg-names", "parf") != 1 ||
	    of_address_to_resource(np, 0, &res) || res.start != 0x400000000ULL ||
	    (resource_size(&res) != 0x1000 && resource_size(&res) != 0x2000))
		goto out;
	x1_pcie->root_bytes = resource_size(&res);
	if ((x1_pcie->state->cfg0_requested && x1_pcie->root_bytes != 0x2000) ||
	    of_address_to_resource(np, 1, &res) || res.start != 0x1c28000 ||
	    resource_size(&res) != 0x8000)
		goto out;
	if (x1_pcie->state->inventory_requested) {
		if (of_property_match_string(np, "reg-names", "config") != 2 ||
		    of_address_to_resource(np, 2, &res) ||
		    res.start != 0x400100000ULL || resource_size(&res) != 0x1000 ||
		    !of_address_to_resource(np, 3, &res))
			goto out;
	} else if (!of_address_to_resource(np, 2, &res)) {
		goto out;
	}
	if (of_property_count_strings(np, "clock-names") != ARRAY_SIZE(x1_pcie_clock_names) ||
	    of_count_phandle_with_args(np, "clocks", "#clock-cells") != ARRAY_SIZE(x1_pcie_clock_names))
		goto out;
	for (i = 0; i < ARRAY_SIZE(x1_pcie_clock_names); i++)
		if (of_property_match_string(np, "clock-names", x1_pcie_clock_names[i]) != i ||
		    !x1_pcie_cells(np, "clocks", "#clock-cells", i, X1_GCC_NODE,
				   x1_pcie_clock_ids[i], 0, 1))
			goto out;
	if (of_property_count_strings(np, "reset-names") != 1 ||
	    of_property_match_string(np, "reset-names", "tunnel") != 0 ||
	    of_count_phandle_with_args(np, "resets", "#reset-cells") != 1 ||
	    !x1_pcie_cells(np, "resets", "#reset-cells", 0, X1_GCC_NODE,
			   GCC_PCIE_0_TUNNEL_BCR, 0, 1) ||
	    of_count_phandle_with_args(np, "power-domains", "#power-domain-cells") != 1 ||
	    !x1_pcie_cells(np, "power-domains", "#power-domain-cells", 0, X1_GCC_NODE,
			   GCC_PCIE_0_TUNNEL_GDSC, 0, 1))
		goto out;
	if (of_property_count_strings(np, "interconnect-names") != 2 ||
	    of_property_match_string(np, "interconnect-names", "pcie-mem") != 0 ||
	    of_property_match_string(np, "interconnect-names", "cpu-pcie") != 1 ||
	    of_count_phandle_with_args(np, "interconnects", "#interconnect-cells") != 4 ||
	    !x1_pcie_cells(np, "interconnects", "#interconnect-cells", 0,
		"/soc@0/interconnect@16c0000", MASTER_PCIE_0, QCOM_ICC_TAG_ALWAYS, 2) ||
	    !x1_pcie_cells(np, "interconnects", "#interconnect-cells", 1,
		"/interconnect-1", SLAVE_EBI1, QCOM_ICC_TAG_ALWAYS, 2) ||
	    !x1_pcie_cells(np, "interconnects", "#interconnect-cells", 2,
		"/soc@0/interconnect@26400000", MASTER_APPSS_PROC, QCOM_ICC_TAG_ACTIVE_ONLY, 2) ||
	    !x1_pcie_cells(np, "interconnects", "#interconnect-cells", 3,
		"/soc@0/interconnect@1600000", SLAVE_PCIE_0_CFG, QCOM_ICC_TAG_ACTIVE_ONLY, 2))
		goto out;
	pdev = of_find_device_by_node(np);
	if (!pdev) {
		ret = -ENODEV;
		goto out;
	}
	if (pdev->dev.driver || pdev->num_resources !=
			(x1_pcie->state->inventory_requested ? 3 : 2)) {
		put_device(&pdev->dev);
		ret = -EBUSY;
		goto out;
	}
	*target = pdev; /* Retained until cold power-off; no devm owner coupling. */
	ret = 0;
out:
	of_node_put(np);
	return ret;
}

/* Generic platform probe powers GDSC before calling our probe. Retirement
 * deliberately held the tunnel BCR; release it through the admitted GCC
 * provider before that power-on, not from the otherwise unreachable probe.
 * Caller holds owner/lifecycle exclusion and x1_pcie_lock. A prior context
 * must already be fully retired before prepare_once can admit a fresh one.
 * This non-devm lease survives a failed generic probe; retain it and the
 * terminal context until cold-off, never retry or unwind a partial release.
 */
static int x1_pcie_release_retired_reset(struct x1_pcie_context *ctx)
{
	int ret;

	if (!x1_pcie_reset_held)
		return 0;
	ctx->state->step = "fresh-generation-reset-get";
	ctx->reset = reset_control_get_exclusive(&ctx->pdev->dev, "tunnel");
	if (IS_ERR_OR_NULL(ctx->reset))
		return ctx->reset ? PTR_ERR(ctx->reset) : -ENODEV;
	ctx->reset_early = true;
	ctx->state->step = "fresh-generation-reset-release-before-genpd";
	ret = reset_control_deassert(ctx->reset);
	if (ret)
		return ret;
	msleep(5);
	x1_pcie_reset_held = false;
	return 0;
}

static int x1_pcie_prepare_probe(struct platform_device *pdev)
{
	struct x1_pcie_context *ctx = x1_pcie;
	struct x1_pcie_state *s = ctx->state;
	struct device *dev = &pdev->dev;
	unsigned int i;
	int ret;

	if (!ctx->window || !ctx->owner || ctx->pdev != pdev || !s || ctx->entered)
		return -EPERM;
	ctx->entered = true;
	/* From here even failure binds successfully: retain acquired resources.
	 * Acquire the runtime-active lease before any other provider/MMIO work;
	 * binding alone would not stop genpd's post-probe power-off synchronization.
	 * A failed resume has acquired no such lease and performs no mode MMIO.
	 */
	s->step = "runtime-power";
	pm_runtime_enable(dev);
	ctx->runtime_enabled = true;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto done;
	ctx->runtime_held = true;
	pm_runtime_forbid(dev);
	s->step = "map-root";
	ctx->root = devm_platform_ioremap_resource_byname(pdev, "root");
	if (IS_ERR(ctx->root)) {
		ret = PTR_ERR(ctx->root);
		goto done;
	}
	s->step = "map-parf";
	ctx->parf = devm_platform_ioremap_resource_byname(pdev, "parf");
	if (IS_ERR(ctx->parf)) {
		ret = PTR_ERR(ctx->parf);
		goto done;
	}
	if (s->inventory_requested) {
		s->step = "map-fixed-config";
		ctx->config = devm_platform_ioremap_resource_byname(pdev, "config");
		if (IS_ERR(ctx->config)) {
			ret = PTR_ERR(ctx->config);
			goto done;
		}
	}
	s->step = "get-reset";
	if (!ctx->reset_early)
		ctx->reset = devm_reset_control_get_exclusive(dev, "tunnel");
	if (IS_ERR_OR_NULL(ctx->reset)) {
		ret = ctx->reset ? PTR_ERR(ctx->reset) : -ENODEV;
		goto done;
	}
	s->step = "get-clocks";
	for (i = 0; i < ARRAY_SIZE(ctx->clocks); i++) {
		ctx->clocks[i] = devm_clk_get(dev, x1_pcie_clock_names[i]);
		if (IS_ERR_OR_NULL(ctx->clocks[i])) {
			ret = ctx->clocks[i] ? PTR_ERR(ctx->clocks[i]) : -ENODEV;
			goto done;
		}
	}
	s->step = "get-memory-icc";
	ctx->mem = devm_of_icc_get(dev, "pcie-mem");
	if (IS_ERR_OR_NULL(ctx->mem)) {
		ret = ctx->mem ? PTR_ERR(ctx->mem) : -ENODEV;
		goto done;
	}
	s->step = "get-config-icc";
	ctx->cfg = devm_of_icc_get(dev, "cpu-pcie");
	if (IS_ERR_OR_NULL(ctx->cfg)) {
		ret = ctx->cfg ? PTR_ERR(ctx->cfg) : -ENODEV;
		goto done;
	}
	s->step = "enable-clocks";
	/* Do not unwind an enabled prefix on failure in this retained experiment. */
	for (i = 0; i < ARRAY_SIZE(ctx->clocks); i++) {
		ret = clk_prepare_enable(ctx->clocks[i]);
		if (ret)
			goto done;
		ctx->clocks_enabled++;
	}
	s->step = "memory-bandwidth";
	ret = icc_set_bw(ctx->mem, 2000000, 2000000);
	if (ret)
		goto done;
	s->step = "config-bandwidth";
	ret = icc_set_bw(ctx->cfg, 75000, 0);
	if (ret)
		goto done;
	ret = x1_pcie_mode(ctx);
	if (!ret && s->inspect_requested)
		ret = x1_pcie_inspect(ctx);
	if (!ret && s->cfg0_requested)
		ret = x1_pcie_local_cfg0(ctx);
done:
	s->error = ret;
	s->prepared = !ret;
	if (!ret)
		s->step = s->cfg0_requested ? "root-cfg0-complete" :
			s->inspect_requested ? "root-inspect-complete" : "mode-only-complete";
	return 0;
}

static const struct of_device_id x1_pcie_match[] = {
	{ .compatible = X1_PCIE_COMPAT },
	{ }
};

static struct platform_driver x1_pcie_driver = {
	.prevent_deferred_probe = true,
	.driver = {
		.name = "qcom-x1-usb4-pcie-mode-only",
		.of_match_table = x1_pcie_match,
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
		.suppress_bind_attrs = true,
	},
};

static int x1_pcie_prepare_once(struct device *owner, void __iomem *router,
			       struct x1_pcie_state *state, bool inventory)
{
	int ret;

	if (!owner || !router || !state)
		return -EINVAL;
	/* This private DT/profile is only entered by the double-armed batch. */
	if (!inventory || !state->receiver_requested)
		return -EPERM;
	mutex_lock(&x1_pcie_lock);
	if (state->attempted || x1_pcie) {
		ret = -EALREADY;
		goto out;
	}
	state->attempted = true;
	x1_pcie = kzalloc(sizeof(*x1_pcie), GFP_KERNEL);
	if (!x1_pcie) {
		ret = -ENOMEM;
		goto terminal;
	}
	state->inventory_requested = inventory;
	if (inventory) {
		state->inspect_requested = true;
		state->cfg0_requested = true;
	}
	state->prepared = false;
	state->step = "admission";
	/* Caller has already pinned this module and holds the owner device lock. */
	x1_pcie->owner = get_device(owner);
	x1_pcie->router = router;
	x1_pcie->state = state;
	ret = x1_pcie_admit(owner, &x1_pcie->pdev);
	if (ret)
		goto terminal;
	ret = x1_pcie_release_retired_reset(x1_pcie);
	if (ret)
		goto terminal;
	state->step = "driver-register-or-genpd-attach";
	/* Establish the complete owner/window before platform core powers genpd. */
	x1_pcie->window = true;
	/* The run-once API installs platform_probe_fail before returning, which
	 * rejects even a supplier-deferred probe before generic genpd attachment.
	 * CONFIG_MODULES keeps this runtime API resident for built-in users too.
	 */
	ret = platform_driver_probe(&x1_pcie_driver, x1_pcie_prepare_probe);
	x1_pcie->window = false;
	if (!ret)
		x1_pcie->driver_registered = true;
	if (!ret)
		ret = x1_pcie->entered ? state->error : -ENODEV;
	/* A genpd attach failure before probe can be unwound by platform core.
	 * No MMIO helper ran in that case; do not claim an acquired genpd lease.
	 */
terminal:
	state->error = ret;
out:
	mutex_unlock(&x1_pcie_lock);
	return ret;
}

int qcom_usb4_x1_pcie_prepare(struct device *owner, void __iomem *router,
			    struct x1_pcie_state *state)
{
	return x1_pcie_prepare_once(owner, router, state, false);
}

int qcom_usb4_x1_batch_prepare(struct device *owner, void __iomem *router,
			     struct x1_pcie_state *state)
{
	return x1_pcie_prepare_once(owner, router, state, true);
}

/* Device/lifecycle/Type-C/domain exclusion stays held by the frontend.
 * Every callback rechecks its deadline and exact live CM-owned tunnel first.
 * Initial-only disabled-LTSSM validators are deliberately not used here.
 */
static int x1_pcie_batch_live(void *context)
{
	struct x1_pcie_context *ctx = context;
	int ret;

	if (!ctx->batch_active || !ctx->batch_live || !ctx->state->inventory_requested ||
	    !ctx->state->inventory_attempted || !ctx->state->prepared || ctx->state->error)
		return -EPERM;
	ret = ctx->batch_live(ctx->batch_context);
	if (ret)
		return ret;
	if (readl(ctx->router + X1_PCIE_GATE))
		return -EBUSY;
	return x1_pcie_root_identity(ctx);
}

static void __iomem *x1_pcie_train_base(struct x1_pcie_context *ctx,
				      enum x1_train_region region)
{
	return region == X1_TRAIN_ROOT ? ctx->root :
		region == X1_TRAIN_PARF ? ctx->parf : ctx->router;
}

static int x1_pcie_train_read(void *context, enum x1_train_region region,
			    u32 offset, u32 *value)
{
	struct x1_pcie_context *ctx = context;
	int ret;

	if (!x1_train_read_allowed(region, offset))
		return -EINVAL;
	ret = x1_pcie_batch_live(ctx);
	if (ret)
		return ret;
	*value = readl(x1_pcie_train_base(ctx, region) + offset);
	return 0;
}

static int x1_pcie_train_write(void *context, enum x1_train_region region,
			     u32 offset, u32 value)
{
	struct x1_pcie_context *ctx = context;
	int ret;

	if (!x1_train_write_allowed(region, offset))
		return -EINVAL;
	ret = x1_pcie_batch_live(ctx);
	if (ret)
		return ret;
	writel(value, x1_pcie_train_base(ctx, region) + offset);
	return 0;
}

static int x1_pcie_train_delay(void *context, unsigned int ms)
{
	int ret = x1_pcie_batch_live(context);

	if (ret)
		return ret;
	if (ms != 1 && ms != 20)
		return -EINVAL;
	msleep(ms);
	return 0;
}

static int x1_pcie_window_survived(struct x1_pcie_context *ctx, bool trained)
{
	struct x1_cfg0_io io = {
		.context = ctx, .read = x1_pcie_cfg0_read,
		.check_live = x1_pcie_batch_live, .root_bytes = 0x2000, .parf_bytes = 0x8000,
	};

	return x1_cfg0_validate_prepared(&io, &ctx->prepared_cfg0, trained,
				       trained ? ctx->training->elbi_after : 0);
}

static int x1_pcie_inventory_read(void *context, u16 offset, u32 *value)
{
	struct x1_pcie_context *ctx = context;
	int ret;

	if (offset & 3 || offset > 0xffc || !ctx->training->complete)
		return -EINVAL;
	ret = x1_pcie_window_survived(ctx, true);
	if (ret)
		return ret;
	/* No write adapter exists for this page. No bus scan or driver binding. */
	*value = readl(ctx->config + offset);
	return 0;
}

/* No PCI core scan/config-write adapter exists in this checkpoint. Repeat the
 * live read-only identity/command/MSI-off fence at every receiver boundary.
 */
static int x1_pcie_receiver_live(void *context)
{
	struct x1_pcie_context *ctx = context;
	struct x1_pcie_inventory *inv = ctx->inventory;
	u32 value;
	int ret;

	if (!ctx->batch_active || !inv || !inv->complete || inv->error ||
	    !ctx->state->receiver_requested || !inv->msix_offset ||
	    !ctx->bridge || ctx->bridge->bus)
		return -EPERM;
	ret = x1_pcie_inventory_read(ctx, PCI_VENDOR_ID, &value);
	if (ret || value != inv->identity)
		return ret > 0 ? -EIO : ret ?: -ENODEV;
	ret = x1_pcie_inventory_read(ctx, PCI_COMMAND, &value);
	if (ret || value & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
		return ret > 0 ? -EIO : ret ?: -EBUSY;
	ret = x1_pcie_inventory_read(ctx, inv->msix_offset, &value);
	if (ret || value != inv->msix_header || (value >> 16 & PCI_MSIX_FLAGS_ENABLE))
		return ret > 0 ? -EIO : ret ?: -EBUSY;
	if (inv->msi_offset) {
		ret = x1_pcie_inventory_read(ctx, inv->msi_offset, &value);
		if (ret || value != inv->msi_header || (value >> 16 & PCI_MSI_FLAGS_ENABLE))
			return ret > 0 ? -EIO : ret ?: -EBUSY;
	}
	ret = x1_pcie_batch_live(ctx);
	return ret > 0 ? -EIO : ret;
}

static int x1_pcie_receiver_retain(void *context)
{
	struct x1_pcie_context *ctx = context;
	int ret = x1_pcie_receiver_live(ctx);

	if (ret)
		return ret;
	if (ctx->receiver_retained || ctx->pdev->dev.driver != &x1_pcie_driver.driver ||
	    !pm_runtime_active(&ctx->pdev->dev) || !pm_runtime_active(ctx->owner))
		return -EBUSY;
	/* Both drivers suppress unbind; no remove/unregister path or PM release.
	 * Frontend denies system suspend after activation and retains all shared
	 * clocks/PHY through shutdown. Pin this shared module again before IRQs.
	 */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	pm_runtime_forbid(ctx->owner);
	pm_runtime_forbid(&ctx->pdev->dev);
	ctx->receiver_retained = true;
	return 0;
}

static int x1_pcie_receiver_global(void *context, int irq)
{
	struct x1_pcie_context *ctx = context;

	return x1_imsi_policy_global(&ctx->policy, irq);
}

static int x1_pcie_receiver_target(void *context, u64 target)
{
	struct x1_pcie_context *ctx = context;

	return x1_imsi_policy_target(&ctx->policy, target);
}

static int x1_pcie_receiver_prepare(struct x1_pcie_context *ctx)
{
	struct x1_imsi_request request = {
		.owner = ctx->owner, .pdev = ctx->pdev, .dbi = ctx->root, .parf = ctx->parf,
		.context = ctx, .check_live = x1_pcie_receiver_live,
		.retain = x1_pcie_receiver_retain, .global_route = x1_pcie_receiver_global,
		.target_check = x1_pcie_receiver_target,
	};
	int ret;

	if (ctx->bridge || ctx->receiver)
		return -EALREADY;
	ret = x1_pcie_window_survived(ctx, true);
	if (ret)
		return ret > 0 ? -EIO : ret;
	/* Receiver-only stage parses/reserves the exact DT ranges, without bus
	 * registration, MEM programming, BAR assignment, endpoint probe or DMA.
	 * The separate nvme_requested continuation may subsequently perform the
	 * guarded MEM/scan stages and, outside frontend locks, bind the endpoint.
	 */
	ctx->bridge = devm_pci_alloc_host_bridge(&ctx->pdev->dev, 0);
	if (!ctx->bridge)
		return -ENOMEM;
	ctx->state->bridge_allocated = true;
	request.bridge = ctx->bridge;
	ctx->policy = (struct x1_imsi_policy) {
		.owner = ctx->owner, .pdev = ctx->pdev, .bridge = ctx->bridge,
		.parf = ctx->parf, .context = ctx, .check_live = x1_pcie_receiver_live,
	};
	ret = x1_imsi_prepare(&request, &ctx->receiver);
	x1_imsi_cached(ctx->receiver, &ctx->state->receiver);
	ctx->state->receiver_mask_before = ctx->policy.mask_before;
	ctx->state->receiver_mask_after = ctx->policy.mask_after;
	ctx->state->receiver_status_before = ctx->policy.status_before;
	ctx->state->receiver_status_after = ctx->policy.status_after;
	if (!ret)
		ret = x1_pcie_receiver_live(ctx);
	return ret > 0 ? -EIO : ret;
}

/* The full CM/window fence is performed on either side of MEM programming.
 * Inside that locked batch use only retained-local checks, not repeated TB
 * control transfers. These checks are also safe inside PCI's raw config lock.
 */
static int x1_pcie_host_local(struct x1_pcie_context *ctx)
{
	u32 command;

	if (ctx != x1_pcie || !READ_ONCE(ctx->state->nvme_requested) ||
	    !ctx->receiver_retained || !ctx->state->receiver.ready ||
	    !ctx->training || !ctx->training->complete ||
	    READ_ONCE(ctx->state->error) || READ_ONCE(ctx->state->host_error) ||
	    READ_ONCE(ctx->state->cfg_error) ||
	    ctx->pdev->dev.driver != &x1_pcie_driver.driver)
		return -EPERM;
	if (readl(ctx->router + X1_PCIE_GATE) ||
	    readl(ctx->root + PCI_VENDOR_ID) != 0x011117cb ||
	    readl(ctx->config + PCI_VENDOR_ID) != ctx->inventory->identity)
		return -ENODEV;
	command = readw(ctx->root + PCI_COMMAND);
	if (command == 0xffff || command & PCI_COMMAND_IO ||
	    (!READ_ONCE(ctx->bind_writes) && command & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)))
		return -EBUSY;
	command = readw(ctx->config + PCI_COMMAND);
	if (command == 0xffff || command & PCI_COMMAND_IO ||
	    (!READ_ONCE(ctx->bind_writes) && command & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)))
		return -EBUSY;
	return 0;
}

static int x1_pcie_mem_check(void *context)
{
	struct x1_pcie_context *ctx = context;

	if (!ctx->batch_active || ctx->bridge->bus || READ_ONCE(ctx->config_open))
		return -EPERM;
	return x1_pcie_host_local(ctx);
}

static int x1_pcie_mem_read(void *context, u32 offset, u32 *value)
{
	struct x1_pcie_context *ctx = context;

	if (!x1_mem_offset(offset))
		return -EINVAL;
	*value = readl(ctx->root + offset);
	return 0;
}

static int x1_pcie_mem_write(void *context, u32 offset, u32 value)
{
	struct x1_pcie_context *ctx = context;

	if (!x1_mem_offset(offset))
		return -EINVAL;
	writel(value, ctx->root + offset);
	return 0;
}

static int x1_pcie_mem_delay(void *context, unsigned int ms)
{
	int ret = x1_pcie_mem_check(context);

	if (ret || ms != 10)
		return ret ?: -EINVAL;
	msleep(ms);
	return x1_pcie_mem_check(context);
}

static void __iomem *x1_pcie_config_base(struct pci_bus *bus, unsigned int devfn)
{
	struct x1_pcie_context *ctx = bus->sysdata;

	if (!ctx || ctx != x1_pcie || !ctx->bridge || !READ_ONCE(ctx->config_open) ||
	    pci_domain_nr(bus) || devfn || !ctx->bridge->bus)
		return NULL;
	if (bus == ctx->bridge->bus && !bus->number && !bus->parent)
		return ctx->root;
	if (bus->number == 1 && bus->parent == ctx->bridge->bus)
		return ctx->config;
	return NULL;
}

static u32 x1_pcie_config_read_width(void __iomem *addr, int size)
{
	return size == 1 ? readb(addr) : size == 2 ? readw(addr) : readl(addr);
}

/* Determine a control word's proposed contents, without doing a dword RMW
 * on hardware: actual writes retain their width, so status W1C bits are not
 * accidentally acknowledged by our filtering.
 */
static u16 x1_pcie_config_word(void __iomem *base, int where, int size,
			      u32 value, unsigned int offset)
{
	u16 word = readw(base + offset);
	unsigned int byte;

	for (byte = 0; byte < size; byte++) {
		if (where + byte < offset || where + byte >= offset + 2)
			continue;
		word &= ~(0xffU << (8 * (where + byte - offset)));
		word |= ((value >> (8 * byte)) & 0xff) <<
			(8 * (where + byte - offset));
	}
	return word;
}

static bool x1_pcie_config_touches(int where, int size, unsigned int word)
{
	return where < word + 2 && where + size > word;
}

static int x1_pcie_config_write_allowed(struct x1_pcie_context *ctx,
		void __iomem *base, int where, int size, u32 value)
{
	bool root = base == ctx->root;
	u16 pcie = root ? ctx->state->pcie_cap : ctx->inventory->pcie_offset;
	u16 msi = root ? ctx->root_msi : ctx->inventory->msi_offset;
	u16 msix = root ? ctx->root_msix : ctx->inventory->msix_offset;
	u16 pm = root ? ctx->root_pm : ctx->endpoint_pm;
	u16 word;

	if (where < 4 || (where < PCI_CLASS_REVISION + 4 &&
			 where + size > PCI_CLASS_REVISION))
		return -EPERM;
	if (x1_pcie_config_touches(where, size, PCI_COMMAND)) {
		word = x1_pcie_config_word(base, where, size, value, PCI_COMMAND);
		if (word & PCI_COMMAND_IO ||
		    (!READ_ONCE(ctx->bind_writes) && word & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)))
			return -EPERM;
	}
	if ((root && x1_pcie_config_touches(where, size, PCI_BRIDGE_CONTROL) &&
	     (x1_pcie_config_word(base, where, size, value, PCI_BRIDGE_CONTROL) &
		PCI_BRIDGE_CTL_BUS_RESET)) ||
	    (x1_pcie_config_touches(where, size, pcie + PCI_EXP_DEVCTL) &&
	     (x1_pcie_config_word(base, where, size, value, pcie + PCI_EXP_DEVCTL) &
		PCI_EXP_DEVCTL_BCR_FLR)) ||
	    (x1_pcie_config_touches(where, size, pcie + PCI_EXP_LNKCTL) &&
	     (x1_pcie_config_word(base, where, size, value, pcie + PCI_EXP_LNKCTL) &
		(PCI_EXP_LNKCTL_RL | PCI_EXP_LNKCTL_LD))))
		return -EPERM;
	if (pm && x1_pcie_config_touches(where, size, pm + PCI_PM_CTRL) &&
	    (x1_pcie_config_word(base, where, size, value, pm + PCI_PM_CTRL) &
		PCI_PM_CTRL_STATE_MASK))
		return -EPERM;
	if (root || !READ_ONCE(ctx->bind_writes)) {
		if (msi && x1_pcie_config_touches(where, size, msi + PCI_MSI_FLAGS) &&
		    (x1_pcie_config_word(base, where, size, value, msi + PCI_MSI_FLAGS) &
			PCI_MSI_FLAGS_ENABLE))
			return -EPERM;
		if (msix && x1_pcie_config_touches(where, size, msix + PCI_MSIX_FLAGS) &&
		    (x1_pcie_config_word(base, where, size, value, msix + PCI_MSIX_FLAGS) &
			PCI_MSIX_FLAGS_ENABLE))
			return -EPERM;
	}
	/* Fixed CFG0 addresses bus1 only; never allow PCI core to renumber it. */
	if (root && where < PCI_PRIMARY_BUS + 3 && where + size > PCI_PRIMARY_BUS) {
		u32 buses = readl(base + PCI_PRIMARY_BUS), proposed = buses;
		unsigned int i;

		for (i = 0; i < size; i++) {
			if (where + i < PCI_PRIMARY_BUS || where + i >= PCI_PRIMARY_BUS + 3)
				continue;
			proposed &= ~(0xffU << (8 * (where + i - PCI_PRIMARY_BUS)));
			proposed |= ((value >> (i * 8)) & 0xff) <<
				(8 * (where + i - PCI_PRIMARY_BUS));
		}
		if ((proposed & 0xffffff) != 0x010100)
			return -EPERM;
	}
	/* BAR/window sizing and assignment exist only during the held scan. */
	if (!READ_ONCE(ctx->scan_writes) && where < (root ? 0x3c : 0x28) &&
	    where + size > PCI_BASE_ADDRESS_0 &&
	    x1_pcie_config_read_width(base + where, size) != value)
		return -EPERM;
	return 0;
}

static int x1_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *value)
{
	struct x1_pcie_context *ctx;
	void __iomem *base;
	unsigned long flags;
	int ret;

	*value = ~0U;
	if ((size != 1 && size != 2 && size != 4) || where < 0 ||
	    where + size > 4096 || where & (size - 1))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	raw_spin_lock_irqsave(&x1_config_lock, flags);
	ctx = x1_pcie;
	base = x1_pcie_config_base(bus, devfn);
	if (!base) {
		ret = PCIBIOS_DEVICE_NOT_FOUND; /* Expected absent-slot scan, no MMIO. */
		goto out;
	}
	ret = x1_pcie_host_local(ctx);
	if (ret) {
		if (!READ_ONCE(ctx->state->cfg_error)) {
			ctx->state->cfg_error_offset = where;
			ctx->state->cfg_error_bus = bus->number;
			ctx->state->cfg_error_size = size;
			/* First-writer arbitration is under x1_config_lock. Publish
			 * the immutable multi-field receipt only after it is filled.
			 */
			smp_store_release(&ctx->state->cfg_error, ret);
		}
		ret = PCIBIOS_DEVICE_NOT_FOUND;
		goto out;
	}
	*value = x1_pcie_config_read_width(base + where, size);
	ret = PCIBIOS_SUCCESSFUL;
out:
	raw_spin_unlock_irqrestore(&x1_config_lock, flags);
	return ret;
}

static int x1_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 value)
{
	struct x1_pcie_context *ctx;
	void __iomem *base;
	unsigned long flags;
	int ret;

	if ((size != 1 && size != 2 && size != 4) || where < 0 ||
	    where + size > 4096 || where & (size - 1))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	raw_spin_lock_irqsave(&x1_config_lock, flags);
	ctx = x1_pcie;
	base = x1_pcie_config_base(bus, devfn);
	if (!base) {
		ret = PCIBIOS_DEVICE_NOT_FOUND;
		goto out;
	}
	ret = x1_pcie_host_local(ctx);
	if (!ret)
		ret = x1_pcie_config_write_allowed(ctx, base, where, size, value);
	if (ret) {
		if (!READ_ONCE(ctx->state->cfg_error)) {
			ctx->state->cfg_error_offset = where;
			ctx->state->cfg_error_value = value;
			ctx->state->cfg_error_bus = bus->number;
			ctx->state->cfg_error_size = size;
			ctx->state->cfg_error_write = true;
			smp_store_release(&ctx->state->cfg_error, ret);
		}
		ret = PCIBIOS_SET_FAILED;
		goto out;
	}
	if (size == 1)
		writeb(value, base + where);
	else if (size == 2)
		writew(value, base + where);
	else
		writel(value, base + where);
	ret = PCIBIOS_SUCCESSFUL;
out:
	raw_spin_unlock_irqrestore(&x1_config_lock, flags);
	return ret;
}

static struct pci_ops x1_pcie_ops = {
	.read = x1_pcie_config_read,
	.write = x1_pcie_config_write,
};

static int x1_pcie_enable_device(struct pci_host_bridge *bridge, struct pci_dev *dev)
{
	struct x1_pcie_context *ctx;
	unsigned long flags;
	int ret = -EPERM;

	raw_spin_lock_irqsave(&x1_config_lock, flags);
	ctx = x1_pcie;
	if (!ctx || bridge != ctx->bridge || !READ_ONCE(ctx->bind_writes) ||
	    !READ_ONCE(ctx->state->host_prepared) ||
	    (dev != ctx->root_port && dev != ctx->endpoint))
		goto out;
	ret = x1_pcie_host_local(ctx);
out:
	raw_spin_unlock_irqrestore(&x1_config_lock, flags);
	return ret;
}

static int x1_pcie_no_root_reset(struct pci_host_bridge *bridge, struct pci_dev *dev)
{
	return -EPERM;
}

static int x1_pcie_scan_identity(struct x1_pcie_context *ctx)
{
	struct pci_dev *root = NULL, *ep = NULL, *dev;
	struct pci_bus *bus = ctx->bridge->bus;
	struct x1_pcie_inventory *inv = ctx->inventory;
	unsigned int roots = 0, endpoints = 0;

	if (!bus || bus->number || pci_domain_nr(bus) || bus->parent)
		return -ENODEV;
	list_for_each_entry(dev, &bus->devices, bus_list) {
		root = dev;
		roots++;
	}
	if (roots != 1 || !root || root->devfn || root->vendor != 0x17cb ||
	    root->device != 0x0111 || root->hdr_type != PCI_HEADER_TYPE_BRIDGE ||
	    root->class != PCI_CLASS_BRIDGE_PCI_NORMAL || !root->no_msi ||
	    root->multifunction || !pci_is_pcie(root) ||
	    pci_pcie_type(root) != PCI_EXP_TYPE_ROOT_PORT || root->dev.driver ||
	    !root->subordinate || root->subordinate->number != 1 ||
	    root->subordinate->parent != bus ||
	    !list_is_singular(&bus->children) ||
	    !list_empty(&root->subordinate->children))
		return -ENODEV;
	list_for_each_entry(dev, &root->subordinate->devices, bus_list) {
		ep = dev;
		endpoints++;
	}
	if (endpoints != 1 || !ep || ep->devfn || ep->subordinate ||
	    ep->vendor != (inv->identity & 0xffff) || ep->device != (inv->identity >> 16) ||
	    ep->class != (inv->class_revision >> 8) || ep->revision != (inv->class_revision & 0xff) ||
	    ep->hdr_type != PCI_HEADER_TYPE_NORMAL || ep->multifunction ||
	    ep->dev.driver || !pci_is_pcie(ep) || ep->msix_cap != inv->msix_offset ||
	    ep->msi_cap != inv->msi_offset || ep->no_msi ||
	    !dev_get_msi_domain(&ctx->bridge->dev) ||
	    dev_get_msi_domain(&ep->dev) != dev_get_msi_domain(&ctx->bridge->dev))
		return -ENODEV;
	/* List iteration borrows PCI core's references. Keep our own pair until
	 * retirement's post-remove checks and session close have finished. This
	 * identity check runs after scan, after publication and before bind; do
	 * not acquire another pair on each check, or replace an owned identity.
	 */
	if (ctx->root_port || ctx->endpoint)
		return ctx->root_port == root && ctx->endpoint == ep ? 0 : -ENODEV;
	ctx->root_port = pci_dev_get(root);
	ctx->endpoint = pci_dev_get(ep);
	return 0;
}

static int x1_pcie_assigned_bars(struct x1_pcie_context *ctx)
{
	struct x1_pcie_inventory *inv = ctx->inventory;
	struct pci_dev *devices[] = { ctx->root_port, ctx->endpoint };
	struct resource *r;
	void __iomem *base;
	u64 address;
	u32 low;
	unsigned int d, bar;

	for (d = 0; d < ARRAY_SIZE(devices); d++) {
		base = d ? ctx->config : ctx->root;
		for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
			r = &devices[d]->resource[bar];
			if (!r->flags)
				continue;
			if (!(r->flags & IORESOURCE_MEM) ||
			    r->flags & (IORESOURCE_IO | IORESOURCE_UNSET | IORESOURCE_STARTALIGN) ||
			    !r->parent || r->start < 0x40000000 || r->end > 0x4fffffff ||
			    r->end < r->start)
				return -ERANGE;
			low = readl(base + PCI_BASE_ADDRESS_0 + bar * 4);
			if (low == U32_MAX || low & PCI_BASE_ADDRESS_SPACE_IO)
				return -EIO;
			address = low & PCI_BASE_ADDRESS_MEM_MASK;
			if ((low & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64) {
				if (bar == PCI_STD_NUM_BARS - 1)
					return -EINVAL;
				address |= (u64)readl(base + PCI_BASE_ADDRESS_0 + (bar + 1) * 4) << 32;
			}
			if (address != r->start)
				return -EIO;
		}
		if (devices[d]->resource[PCI_ROM_RESOURCE].flags)
			return -EOPNOTSUPP;
	}
	r = &ctx->endpoint->resource[0];
	if (!(r->flags & IORESOURCE_MEM) || resource_size(r) < 0x2000)
		return -ENODEV;
	if (readl(ctx->config + inv->msix_offset + PCI_MSIX_TABLE) != inv->msix_table ||
	    readl(ctx->config + inv->msix_offset + PCI_MSIX_PBA) != inv->msix_pba)
		return -ENODEV;
	r = &ctx->endpoint->resource[inv->table_bir];
	if (!(r->flags & IORESOURCE_MEM) ||
	    (u64)inv->table_offset + inv->table_bytes > resource_size(r))
		return -ERANGE;
	r = &ctx->endpoint->resource[inv->pba_bir];
	if (!(r->flags & IORESOURCE_MEM) ||
	    (u64)inv->pba_offset + inv->pba_bytes > resource_size(r))
		return -ERANGE;
	r = &ctx->root_port->resource[PCI_BRIDGE_MEM_WINDOW];
	if (!(r->flags & IORESOURCE_MEM) || !r->parent ||
	    r->start < 0x40000000 || r->end > 0x4fffffff || r->end < r->start)
		return -ERANGE;
	return 0;
}

static int x1_pcie_host_scan(struct x1_pcie_context *ctx)
{
	struct x1_pcie_state *s = ctx->state;
	struct x1_mem_io io = {
		.context = ctx, .check = x1_pcie_mem_check, .read = x1_pcie_mem_read,
		.write = x1_pcie_mem_write, .delay = x1_pcie_mem_delay,
	};
	struct pci_host_bridge *bridge = ctx->bridge;
	unsigned int i;
	int ret;

	if (s->host_attempted)
		return -EALREADY;
	s->host_attempted = true;
	s->host_step = "mem-window";
	ret = x1_pcie_receiver_live(ctx);
	if (!ret)
		ret = x1_mem_prepare(&io, &s->mem);
	if (!ret)
		ret = x1_pcie_receiver_live(ctx);
	if (ret)
		goto done;
	for (i = 0; i < ctx->inventory->standard_count; i++)
		if (ctx->inventory->standard_ids[i] == PCI_CAP_ID_PM)
			ctx->endpoint_pm = ctx->inventory->standard_offsets[i];
	bridge->sysdata = ctx;
	bridge->ops = &x1_pcie_ops;
	bridge->child_ops = &x1_pcie_ops;
	bridge->busnr = 0;
	bridge->enable_device = x1_pcie_enable_device;
	bridge->reset_root_port = x1_pcie_no_root_reset;
	/* Root driver remains held; neither root services nor INTx fallback are
	 * admitted by this one-endpoint MSI-X experiment.
	 */
	bridge->native_aer = false;
	bridge->native_pcie_hotplug = false;
	bridge->native_shpc_hotplug = false;
	bridge->native_pme = false;
	bridge->native_ltr = false;
	bridge->native_dpc = false;
	bridge->map_irq = NULL;
	bridge->swizzle_irq = NULL;
	WRITE_ONCE(ctx->config_open, true);
	WRITE_ONCE(ctx->scan_writes, true);
	s->host_step = "pci-scan-config-writes-no-binding";
	pci_lock_rescan_remove();
	/* PCI core sizes BARs and configures capabilities here. This is NOT a
	 * read-only inventory, but PCI_DEV_ALLOW_BINDING is still clear.
	 */
	ret = pci_scan_root_bus_bridge(bridge);
	s->host_published = !!bridge->bus;
	if (!ret)
		ret = READ_ONCE(s->cfg_error) ?: x1_pcie_scan_identity(ctx);
	if (ret)
		goto unlock;
	ret = device_set_driver_override(&ctx->root_port->dev, "x1-private-held");
	if (!ret)
		ret = device_set_driver_override(&ctx->endpoint->dev, "x1-private-held");
	if (ret)
		goto unlock;
	s->host_step = "pci-bar-assignment-no-binding";
	pci_bus_size_bridges(bridge->bus);
	pci_bus_assign_resources(bridge->bus);
	ret = READ_ONCE(s->cfg_error) ?: x1_pcie_assigned_bars(ctx);
	if (!ret)
		ret = x1_pcie_batch_live(ctx);
	if (ret)
		goto unlock;
	WRITE_ONCE(ctx->scan_writes, false);
	s->host_step = "pci-add-devices-both-drivers-held";
	/* Forbid runtime PM before add_devices enables it, not after a race. */
	pm_runtime_forbid(&ctx->root_port->dev);
	pm_runtime_forbid(&ctx->endpoint->dev);
	pci_bus_add_devices(bridge->bus);
	ret = READ_ONCE(s->cfg_error) ?: x1_pcie_scan_identity(ctx);
	if (!ret)
		ret = x1_pcie_assigned_bars(ctx);
	if (!ret)
		ret = x1_pcie_batch_live(ctx);
	if (!ret) {
		WRITE_ONCE(s->host_prepared, true);
		s->host_step = "pci-ready-both-drivers-held-no-endpoint-dma";
	}
unlock:
	pci_unlock_rescan_remove();
done:
	WRITE_ONCE(ctx->scan_writes, false);
	WRITE_ONCE(s->host_error, ret);
	return ret;
}

int qcom_usb4_x1_nvme_bind(struct device *owner, struct x1_pcie_state *state)
{
	struct x1_pcie_context *ctx;
	int ret = -EPERM;

	if (!owner || !state)
		return -EINVAL;
	/* No frontend/TB lock may be held. Normal PCI DMA configuration uses
	 * bridge->dev.parent == PCI0 pdev; no NHI dma_ops/fwspec is copied, and
	 * firmware-owned PCIe SMMU/no iommu-map is not an isolation assertion.
	 */
	mutex_lock(&x1_pcie_lock);
	ctx = x1_pcie;
	if (!ctx || ctx->owner != owner || ctx->state != state || !state->nvme_requested ||
	    !state->host_prepared || !state->host_published || state->error ||
	    ctx->batch_active || !ctx->endpoint || !ctx->root_port)
		goto out;
	if (state->nvme_bind_attempted) {
		ret = -EALREADY;
		goto out;
	}
	state->nvme_bind_attempted = true;
	state->host_step = "nvme-bind-once-not-irq-proof";
	ret = x1_pcie_host_local(ctx);
	if (!ret)
		ret = x1_pcie_scan_identity(ctx);
	if (!ret)
		ret = x1_pcie_assigned_bars(ctx);
	if (!ret)
		ret = nvme_x1_open_session(ctx->endpoint, &ctx->nvme_session);
	if (!ret)
		ret = device_set_driver_override(&ctx->endpoint->dev, "nvme");
	if (ret)
		goto terminal;
	/* Driver-specific read-only/timeout/IRQ instrumentation is separately
	 * required by the private DT opt-in. This permits controller setup and
	 * DMA, not namespace writes, hotplug, reset recovery or unbounded tests.
	 */
	WRITE_ONCE(ctx->bind_writes, true);
	ret = device_attach(&ctx->endpoint->dev);
	if (ret != 1 || !ctx->endpoint->dev.driver ||
	    strcmp(ctx->endpoint->dev.driver->name, "nvme"))
		ret = ret < 0 ? ret : -ENODEV;
	else
		ret = READ_ONCE(state->cfg_error);
	if (!ret) {
		state->nvme_bound = true;
		state->host_step = "nvme-bound-controller-readiness-unproven";
	}
terminal:
	WRITE_ONCE(state->host_error, ret);
	if (ret)
		WRITE_ONCE(state->error, ret);
	/* No remove, reset, DMA free or receiver teardown on any admitted result.
	 * The driver can still be completing asynchronous controller setup.
	 */
out:
	mutex_unlock(&x1_pcie_lock);
	return ret;
}

struct x1_pcie_fenced_action {
	struct x1_pcie_context *pcie;
	int (*action)(void *);
	void *context;
};

/* Called with the actual NVMe endpoint's successful quiesce proof locked.
 * Acquire the SAME raw lock as both config callbacks: waiting here drains
 * an in-flight access, and later callbacks return absent without MMIO.
 * Never hold this spinlock over a tunnel command, IRQ sync or sleeping work.
 */
static int x1_pcie_fence_action(void *context)
{
	struct x1_pcie_fenced_action *call = context;
	struct x1_pcie_context *ctx = call->pcie;
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&x1_config_lock, flags);
	if (ctx->state->config_fenced) {
		ret = -EALREADY;
		goto out;
	}
	/* Recheck errors under the config lock; an access may have failed since
	 * the outer admission check. Still close permanently on that failure.
	 */
	ret = READ_ONCE(ctx->state->cfg_error) ?: READ_ONCE(ctx->state->host_error);
	if (!ret)
		ret = READ_ONCE(ctx->state->error);
	if (!ctx->config_open && !ret)
		ret = -EPERM;
	WRITE_ONCE(ctx->config_open, false);
	WRITE_ONCE(ctx->scan_writes, false);
	WRITE_ONCE(ctx->bind_writes, false);
	WRITE_ONCE(ctx->state->config_fenced, true);
out:
	raw_spin_unlock_irqrestore(&x1_config_lock, flags);
	if (!ret) {
		ret = x1_imsi_stop_retained(ctx->receiver);
		x1_imsi_cached(ctx->receiver, &ctx->state->receiver);
	}
	return ret ?: call->action(call->context);
}

int qcom_usb4_x1_fence_and_quiesce(struct device *owner, struct x1_pcie_state *state,
				 int (*action)(void *), void *context)
{
	struct x1_pcie_context *ctx;
	struct x1_pcie_fenced_action call = {
		.action = action, .context = context,
	};
	int ret = -EPERM;

	if (!owner || !state || !action)
		return -EINVAL;
	mutex_lock(&x1_pcie_lock);
	ctx = x1_pcie;
	call.pcie = ctx;
	if (!ctx || ctx->owner != owner || ctx->state != state || !state->nvme_requested ||
	    !state->host_prepared || !state->host_published || !state->nvme_bound ||
	    !state->nvme_bind_attempted || state->error || state->host_error ||
	    READ_ONCE(state->cfg_error) || ctx->batch_active || !ctx->endpoint ||
	    !ctx->root_port || !ctx->receiver_retained || !state->receiver.ready)
		goto out;
	ret = nvme_x1_retire_irqs_and_action(ctx->endpoint, x1_pcie_fence_action, &call);
out:
	mutex_unlock(&x1_pcie_lock);
	return ret;
}

int qcom_usb4_x1_retire_namespaces(struct device *owner, struct x1_pcie_state *state,
				 int (*check_stopped)(void *), void *context)
{
	struct x1_pcie_context *ctx;
	int ret = -EPERM;

	if (!owner || !state || !check_stopped)
		return -EINVAL;
	mutex_lock(&x1_pcie_lock);
	ctx = x1_pcie;
	if (!ctx || ctx->owner != owner || ctx->state != state || !state->nvme_requested ||
	    !state->host_prepared || !state->host_published || !state->nvme_bound ||
	    !state->nvme_bind_attempted || state->error || state->host_error ||
	    READ_ONCE(state->cfg_error) || ctx->batch_active || !ctx->endpoint ||
	    !ctx->root_port || !ctx->receiver_retained || !state->receiver.ready ||
	    !state->receiver.stop.finished || state->receiver.stop.error ||
	    state->receiver.stop.banks_disabled != 8 || state->receiver.stop.parents_detached != 8 ||
	    !READ_ONCE(state->config_fenced) || READ_ONCE(ctx->config_open) ||
	    READ_ONCE(ctx->scan_writes) || READ_ONCE(ctx->bind_writes))
		goto out;
	if (state->namespace_retire_attempted) {
		ret = -EALREADY;
		goto out;
	}
	state->namespace_retire_attempted = true;
	/* No config reads or endpoint MMIO behind the stopped/reset tunnel. */
	ret = nvme_x1_retire_namespaces(ctx->endpoint, check_stopped, context);
	state->namespace_retire_error = ret;
	state->namespace_retired = !ret;
out:
	mutex_unlock(&x1_pcie_lock);
	return ret;
}

/* Check only software identity under the rescan/remove lock. Config is
 * fenced and hardware is off; do not reuse the live inventory/MMIO helpers.
 * Refuse any additional device rather than extending the removal scope.
 */
static bool x1_pcie_retire_shape(struct x1_pcie_context *ctx)
{
	struct pci_bus *bus = ctx->bridge->bus;
	struct pci_dev *root = ctx->root_port, *endpoint = ctx->endpoint;

	if (!bus || !root || !endpoint || bus->sysdata != ctx ||
	    !pci_is_root_bus(bus) || pci_domain_nr(bus) || bus->number ||
	    root->bus != bus || root->devfn || root->vendor != 0x17cb ||
	    root->device != 0x0111 || root->dev.driver ||
	    !root->subordinate || root->subordinate->parent != bus ||
	    endpoint->bus != root->subordinate || endpoint->bus->number != 1 ||
	    endpoint->devfn || endpoint->vendor != 0x1c19 || endpoint->device != 0x102b ||
	    endpoint->subordinate || !list_is_singular(&bus->devices) ||
	    !list_is_singular(&endpoint->bus->devices) ||
	    list_first_entry(&bus->devices, struct pci_dev, bus_list) != root ||
	    list_first_entry(&endpoint->bus->devices, struct pci_dev, bus_list) != endpoint)
		return false;
	return true;
}

static int x1_pcie_receiver_release_proof(void *context)
{
	struct x1_pcie_fenced_action *call = context;
	struct x1_pcie_context *ctx = call->pcie;
	struct x1_pcie_state *s = ctx->state;

	lockdep_assert_held(&x1_pcie_lock);
	if (ctx != x1_pcie || !ctx->owner || !ctx->receiver_retained ||
	    !s->config_fenced || ctx->config_open || ctx->scan_writes || ctx->bind_writes ||
	    !s->pci_retire_attempted || !s->nvme_resources_retired || !s->pci_retired ||
	    s->pci_retire_error || ctx->endpoint || ctx->root_port || !ctx->bridge ||
	    ctx->bridge->bus || s->nvme_bound || s->host_published || s->host_prepared)
		return -EPERM;
	return call->action(call->context);
}

int qcom_usb4_x1_retire_pci(struct device *owner, struct x1_pcie_state *state,
			  int (*check_stopped)(void *), void *context)
{
	struct x1_pcie_context *ctx;
	struct x1_pcie_fenced_action call = {
		.action = check_stopped, .context = context,
	};
	int ret = -EPERM;

	if (!owner || !state || !check_stopped)
		return -EINVAL;
	mutex_lock(&x1_pcie_lock);
	ctx = x1_pcie;
	call.pcie = ctx;
	if (!ctx || ctx->owner != owner || ctx->state != state || !ctx->bridge ||
	    !ctx->endpoint || !ctx->root_port || !ctx->receiver_retained ||
	    ctx->batch_active || !state->nvme_bound || !state->host_prepared ||
	    !state->host_published || state->error || state->host_error || state->cfg_error ||
	    !state->namespace_retire_attempted || !state->namespace_retired ||
	    state->namespace_retire_error || !state->config_fenced ||
	    ctx->config_open || ctx->scan_writes || ctx->bind_writes ||
	    !state->receiver.stop.finished || state->receiver.stop.error ||
	    state->receiver.stop.banks_disabled != 8 || state->receiver.stop.parents_detached != 8)
		goto out;
	if (state->pci_retire_attempted) {
		ret = -EALREADY;
		goto out;
	}
	pci_lock_rescan_remove();
	if (!x1_pcie_retire_shape(ctx))
		goto unlock_bus;
	ret = check_stopped(context);
	if (ret)
		goto unlock_bus;
	state->pci_retire_attempted = true;
	state->host_step = "retire-stopped-nvme-resources";
	ret = nvme_x1_retire_resources(ctx->endpoint, check_stopped, context);
	if (ret)
		goto failed;
	state->nvme_resources_retired = true;
	state->host_step = "remove-old-pci-bus";
	/* Normal driver removal now only balances the retained controller ref.
	 * Its DMA buffers are gone before PCI core tears down DMA configuration.
	 * Root-port services were never bound. Any generic PCI config access is
	 * denied by the earlier synchronized fence, not issued to powered-off HW.
	 */
	pci_stop_root_bus(ctx->bridge->bus);
	if (ctx->endpoint->dev.driver || pci_get_drvdata(ctx->endpoint) ||
	    ctx->endpoint->dev.msi.data || ctx->root_port->dev.driver) {
		ret = -EIO;
		goto failed;
	}
	pci_remove_root_bus(ctx->bridge->bus);
	if (ctx->bridge->bus || device_is_registered(&ctx->endpoint->dev) ||
	    device_is_registered(&ctx->root_port->dev) ||
	    device_is_registered(&ctx->bridge->dev)) {
		ret = -EIO;
		goto failed;
	}
	ret = nvme_x1_close_session(ctx->endpoint, ctx->nvme_session);
	if (ret)
		goto failed;
	ctx->nvme_session = 0;
	pci_dev_put(ctx->endpoint);
	pci_dev_put(ctx->root_port);
	ctx->endpoint = NULL;
	ctx->root_port = NULL;
	state->nvme_bound = false;
	state->host_published = false;
	state->host_prepared = false;
	state->pci_retired = true;
	state->host_step = "release-stopped-receiver-domain-target";
	ret = x1_imsi_release_stopped(ctx->receiver, x1_pcie_receiver_release_proof, &call);
	x1_imsi_cached(ctx->receiver, &state->receiver);
	if (!ret)
		state->host_step = "old-pci-domain-target-removed-platform-retained";
failed:
	state->pci_retire_error = ret;
unlock_bus:
	pci_unlock_rescan_remove();
out:
	mutex_unlock(&x1_pcie_lock);
	return ret;
}

int qcom_usb4_x1_retire_platform(struct device *owner, struct x1_pcie_state *state,
			       int (*check_stopped)(void *), void *context)
{
	struct x1_pcie_context *ctx;
	unsigned long flags;
	unsigned int i;
	int ret = -EPERM;

	if (!owner || !state || !check_stopped)
		return -EINVAL;
	mutex_lock(&x1_pcie_lock);
	ctx = x1_pcie;
	if (!ctx || ctx->owner != owner || ctx->state != state ||
	    !state->pci_retired || state->pci_retire_error ||
	    !state->receiver.released || state->receiver.release_error ||
	    !state->config_fenced || ctx->config_open || ctx->scan_writes ||
	    ctx->bind_writes || ctx->batch_active || ctx->window ||
	    ctx->endpoint || ctx->root_port || ctx->nvme_session ||
	    !ctx->bridge || ctx->bridge->bus || device_is_registered(&ctx->bridge->dev) ||
	    !ctx->driver_registered || !ctx->receiver_retained ||
	    !ctx->runtime_enabled || !ctx->runtime_held ||
	    ctx->clocks_enabled != ARRAY_SIZE(ctx->clocks) ||
	    ctx->pdev->dev.driver != &x1_pcie_driver.driver)
		goto out;
	if (state->platform_retire_attempted) {
		ret = -EALREADY;
		goto out;
	}
	ret = check_stopped(context);
	if (ret)
		goto out;
	state->platform_retire_attempted = true;
	/* Router/endpoint/control DMA is already retired. GCC reset accesses
	 * the live reset provider, never the powered-off DBI or router aperture. */
	ret = reset_control_assert(ctx->reset);
	if (ret)
		goto failed;
	state->platform_reset = true;
	x1_pcie_reset_held = true;
	ret = icc_set_bw(ctx->cfg, 0, 0);
	if (!ret)
		ret = icc_set_bw(ctx->mem, 0, 0);
	if (ret)
		goto failed;
	state->platform_icc_released = true;
	for (i = ARRAY_SIZE(ctx->clocks); i; i--) {
		clk_disable_unprepare(ctx->clocks[i - 1]);
		ctx->clocks_enabled--;
		state->platform_clocks_released++;
	}
	/* Balance both the explicit active lease and forbid's implicit vote.
	 * No private PCI PM callback can touch MMIO. Hold owner device/lifecycle
	 * exclusion throughout, and disable runtime PM before devres removal. */
	pm_runtime_allow(&ctx->pdev->dev);
	ret = pm_runtime_put_sync(&ctx->pdev->dev);
	ctx->runtime_held = false;
	if (ret < 0)
		goto failed;
	pm_runtime_barrier(&ctx->pdev->dev);
	pm_runtime_disable(&ctx->pdev->dev);
	ctx->runtime_enabled = false;
	pm_runtime_allow(owner);
	pm_runtime_barrier(owner);
	state->platform_runtime_released = true;
	ret = x1_imsi_retire_context(ctx->receiver, check_stopped, context);
	if (ret)
		goto failed;
	/* Config readers synchronize against this same lock before they inspect
	 * any context field. A stale bus->sysdata must not reach freed memory. */
	raw_spin_lock_irqsave(&x1_config_lock, flags);
	x1_pcie = NULL;
	raw_spin_unlock_irqrestore(&x1_config_lock, flags);
	platform_driver_unregister(&x1_pcie_driver);
	/* The driver has no hardware remove callback. Core released only its
	 * proven-dead bridge/receiver/devres/provider handles. */
	if (ctx->reset_early)
		reset_control_put(ctx->reset); /* Held asserted; put never deasserts. */
	state->platform_retired = true;
	state->host_step = "old-platform-provider-context-retired";
	put_device(&ctx->pdev->dev);
	put_device(ctx->owner);
	module_put(THIS_MODULE); /* persistent physical-port module pin remains */
	kfree(ctx);
	ret = 0;
	goto out;
failed:
	state->platform_retire_error = ret;
out:
	mutex_unlock(&x1_pcie_lock);
	return ret;
}

int qcom_usb4_x1_batch_inventory(struct device *owner, struct x1_pcie_state *state,
		int (*check_live)(void *), void *context, u16 vendor, u16 device,
		struct x1_train_state *training, struct x1_pcie_inventory *inventory)
{
	struct x1_pcie_context *ctx;
	struct x1_train_io io = {
		.read = x1_pcie_train_read, .write = x1_pcie_train_write,
		.delay_ms = x1_pcie_train_delay, .check_live = x1_pcie_batch_live,
		.root_bytes = 0x2000, .parf_bytes = 0x8000, .router_bytes = 0x100000,
	};
	int ret = -EPERM;

	if (!state || !training || !inventory || !check_live)
		return -EINVAL;
	mutex_lock(&x1_pcie_lock);
	ctx = x1_pcie;
	io.context = ctx;
	if (!ctx || ctx->owner != owner || ctx->state != state || !state->inventory_requested ||
	    !state->prepared || state->error || !state->cfg0_complete || ctx->window ||
	    !ctx->config || IS_ERR(ctx->config))
		goto out;
	if (state->inventory_attempted) {
		ret = -EALREADY;
		goto out;
	}
	state->inventory_attempted = true;
	ctx->batch_active = true;
	ctx->batch_live = check_live;
	ctx->batch_context = context;
	ctx->training = training;
	ctx->inventory = inventory;
	state->step = "batch-pretraining-window";
	ret = x1_pcie_window_survived(ctx, false);
	if (ret)
		goto terminal;
	state->step = "batch-training";
	ret = x1_pcie_train(&io, training);
	if (ret)
		goto terminal;
	state->step = "batch-window-survival";
	ret = x1_pcie_window_survived(ctx, true);
	if (ret)
		goto terminal;
	state->step = "batch-endpoint-inventory";
	ret = qcom_usb4_x1_inventory(x1_pcie_inventory_read, ctx, vendor, device, inventory);
	if (!ret)
		ret = x1_pcie_window_survived(ctx, true);
	if (!ret) {
		state->step = "batch-masked-receiver";
		ret = x1_pcie_receiver_prepare(ctx);
	}
	if (!ret && state->nvme_requested) {
		state->step = "batch-private-pci-host";
		ret = x1_pcie_host_scan(ctx);
	}
terminal:
	/* No MMIO cleanup, reset, retry or resource release follows any result. */
	ctx->batch_active = false;
	ctx->batch_live = NULL;
	ctx->batch_context = NULL;
	state->error = ret;
	if (!ret)
		state->step = state->nvme_requested ? "batch-pci-ready-drivers-held" :
			"batch-receiver-complete-no-dma";
out:
	mutex_unlock(&x1_pcie_lock);
	return ret;
}
