// SPDX-License-Identifier: GPL-2.0-only
/*
 * Yoga Slim 7x X1E80100 router-0 development frontend.
 *
 * Derived from Jim Martin's Glymur frontend and the separately recorded X1
 * startup experiment. Common MCU, NHI and Type-C helpers retain their original
 * implementation. This is a control-domain integration, NOT a PCI0/MSI fix.
 *
 * One cold-start attempt per boot. Activation pins the module; unbind and
 * suspend are disabled until recovery/lifecycle has been hardware validated.
 * No MCU event IRQ is guessed and no raw register/mailbox interface exists.
 * A separately armed one-shot inventory batch may authorize one exact private
 * tunnel and read its endpoint configuration, with endpoint DMA/MSI disabled.
 */
#include <crypto/sha2.h>
#include <linux/capability.h>
#include <linux/clk.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/hex.h>
#include <linux/interconnect.h>
#include <linux/iommu.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-thunderbolt.h>
#include <linux/phy/phy-qcom-x1-usb4.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/rcupdate.h>
#include <linux/reset.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/socinfo.h>
#include <linux/soc/qcom/x1-usb4.h>
#include <linux/unaligned.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_tbt.h>
#include <dt-bindings/phy/phy-qcom-qmp.h>

#include "nhi.h"
#include "nhi_regs.h"
#include "tb.h"
#include "tunnel.h"
#include "qcom-usb4-drom.h"
#include "qcom-usb4-fw.h"
#include "qcom-usb4-mcu.h"
#include "qcom-usb4-nhi.h"
#include "qcom-usb4-typec.h"
#include "qcom-usb4-x1-drom.h"
#include "qcom-usb4-x1-startup.h"
#include "qcom-usb4-x1-pcie.h"
#include "qcom-usb4-x1-train.h"
#include "qcom-usb4-x1-inventory.h"

#define X1_ROUTER_SIZE 0x100000
#define X1_NHI_OFFSET 0x3f000
#define X1_RAM_SIZE 0xf000
#define X1_FIRMWARE "qcom/x1e80100/usb4-router-legacy.bin"
#define X1_PIPE_RESET 3
#define X1_COMMAND_TIMEOUT_US 10000

static bool x1_startup;
module_param(x1_startup, bool, 0444);
MODULE_PARM_DESC(x1_startup, "Opt in to one cold X1 router-0 startup (default false)");

static bool x1_runtime_startup;
module_param(x1_runtime_startup, bool, 0444);
MODULE_PARM_DESC(x1_runtime_startup, "Arm the root-only one-shot cold-start gate (default false)");

static bool x1_pcie_prepare;
module_param(x1_pcie_prepare, bool, 0444);
MODULE_PARM_DESC(x1_pcie_prepare, "Arm one empty-port PCI0 mode-only test; no scan or DMA (default false)");
static bool x1_pcie_inspect;
module_param(x1_pcie_inspect, bool, 0444);
MODULE_PARM_DESC(x1_pcie_inspect, "Allow root-inspect in the same PCI0 mode-only attempt; local reads only (default false)");
static bool x1_pcie_cfg0;
module_param(x1_pcie_cfg0, bool, 0444);
MODULE_PARM_DESC(x1_pcie_cfg0, "Allow empty-port root-cfg0 local preparation; no downstream access, training or DMA (default false)");
static bool x1_inventory_batch;
module_param(x1_inventory_batch, bool, 0444);
MODULE_PARM_DESC(x1_inventory_batch, "Arm one cold tunnel/training/read-only inventory batch; no endpoint enable or DMA (default false)");
static bool x1_imsi_receiver;
module_param(x1_imsi_receiver, bool, 0444);
MODULE_PARM_DESC(x1_imsi_receiver, "Private masked-receiver continuation; requires inventory batch, no PCI bus publication or endpoint DMA (default false)");
static bool x1_nvme_read;
module_param(x1_nvme_read, bool, 0444);
MODULE_PARM_DESC(x1_nvme_read, "Private exact-device read-only NVMe continuation; requires inventory and receiver arms (default false)");

static bool x1_tunnel_quiesce;
module_param(x1_tunnel_quiesce, bool, 0444);
MODULE_PARM_DESC(x1_tunnel_quiesce, "Private checked tunnel stop after endpoint quiesce; all resources retained (default false)");

static bool x1_host_quiesce;
module_param(x1_host_quiesce, bool, 0444);
MODULE_PARM_DESC(x1_host_quiesce, "Private checked control-ring and parent-IRQ stop after retained tunnel stop (default false)");

static bool x1_power_quiesce;
module_param(x1_power_quiesce, bool, 0444);
MODULE_PARM_DESC(x1_power_quiesce, "Private checked router reset and provider-vote release after host stop; no free/rearm (default false)");

static bool x1_namespace_retire;
module_param(x1_namespace_retire, bool, 0444);
MODULE_PARM_DESC(x1_namespace_retire, "Private namespace removal after checked power stop; controller DMA remains retained (default false)");
static bool x1_pci_retire;
module_param(x1_pci_retire, bool, 0444);
MODULE_PARM_DESC(x1_pci_retire, "Private stopped NVMe/PCI removal after namespace retirement; no reconnect (default false)");
static bool x1_control_retire;
module_param(x1_control_retire, bool, 0444);
MODULE_PARM_DESC(x1_control_retire, "Private stopped control IRQ/DMA retirement after PCI removal; CM shell retained (default false)");
static bool x1_cm_retire;
module_param(x1_cm_retire, bool, 0444);
MODULE_PARM_DESC(x1_cm_retire, "Private powered-off CM/domain retirement after control DMA retirement (default false)");
static bool x1_managed;
module_param(x1_managed, bool, 0444);
MODULE_PARM_DESC(x1_managed, "Private managed empty-port sessions and checked clean-eject reconnect (default false)");

struct x1_power_stop {
	bool attempted, finished, mcu_request_cleared, rx_reference;
	bool phy_off, phy_exited, route_cleared, memory_force_cleared;
	bool icc_released, gcc_released, clocks_released, runtime_released;
	u32 mcu_control, host_control, reset_requests;
	unsigned int resets_asserted;
	const char *stage;
	int error;
};

static const char *const x1_pin_names[] = {
	"rx-prepare", "txb-prepare", "txa-prepare",
	"rx-active", "txb-active", "txa-active",
};
static const char *const x1_reset_names[] = {
	"sys", "rx0", "rx1", "usb-pipe", "pcie-pipe", "tmu",
	"sb-if", "hia", "ahb", "dp0", "dp1",
};
static const char *const x1_clock_names[] = {
	"ahb", "sys", "master", "axi", "tmu", "sb_if",
};

struct x1_host;
/* Persistent physical-port owner. Runtime objects below are per attachment. */
struct x1_port {
	struct device *dev;
	struct x1_host __rcu *session;
	struct mutex generation_lock;
	spinlock_t event_lock;
	struct typec_switch_dev *sw;
	struct typec_mux_dev *mux;
	enum typec_orientation orientation;
	u64 event_sequence, disconnect_sequence, next_generation;
	bool module_held, runtime_enabled;
};

struct x1_host {
	struct kref ref;
	struct x1_port *port;
	u64 generation, initial_event_sequence;
	bool fully_retired;
	struct device *dev;
	void __iomem *router;
	struct qcom_usb4_mcu mcu;
	struct qcom_usb4_nhi qnhi;
	struct tb *tb;
	struct x1_state state;
	struct x1_pcie_state pcie;
	struct x1_gcc_usb4 *gcc;
	struct regmap *tcsr;
	struct phy *phy;
	struct clk_bulk_data clocks[ARRAY_SIZE(x1_clock_names)];
	struct reset_control_bulk_data resets[ARRAY_SIZE(x1_reset_names)];
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins[ARRAY_SIZE(x1_pin_names)];
	struct icc_path *ddr, *apps;
	struct typec_switch_dev *sw;
	struct typec_mux_dev *mux;
	const struct firmware *fw;
	/* Device lock -> lifecycle_lock -> short Type-C lock; never reversed. */
	struct mutex lifecycle_lock;
	/* Only binds versus shutdown; never taken by CM or IRQ callbacks. */
	struct mutex bind_lock;
	struct mutex lock;
	enum typec_orientation orientation;
	u32 serial, shared, pending_command, connected_command;
	int irq, error;
	bool runtime_held, clocks_on, phy_initialized, phy_on;
	bool activated, cold_owned, ready, stopping, command_attempted;
	bool startup_attempted, suspended;
	/* Fresh-session reset restoration; never copied from the retired owner. */
	bool reset_restore_attempted, reset_restore_complete;
	u32 reset_restore_state;
	int reset_restore_error, reset_hold_error;
	/* Separate one-shot lifecycle, never reopens an earlier terminal gate. */
	bool batch_attempted, batch_waiting, batch_terminal, batch_complete;
	enum typec_orientation batch_orientation;
	unsigned long batch_deadline;
	const char *batch_step;
	int batch_error;
	u64 batch_uid; /* Privileged admission only; never emitted in sysfs/logs. */
	u16 batch_vendor, batch_device;
	struct tb_switch *batch_switch;
	struct x1_train_state training;
	struct x1_pcie_inventory inventory;
	/* Separate, default-off retained teardown. Never rearms the batch. */
	bool tunnel_held, tunnel_stop_attempted, tunnel_stop_complete;
	/* Published only with a permanent device reference after successful
	 * retained CM hold. Early domain-add failures cannot reach this pointer.
	 */
	struct tb __rcu *retained_tb;
	int tunnel_stop_error;
	struct tb_x1_tunnel_stop tunnel_stop;
	bool host_stop_attempted;
	struct qcom_usb4_nhi_stop host_stop;
	struct x1_power_stop power_stop;
	struct qcom_usb4_nhi_retire control_retire;
	struct tb_x1_cm_retire cm_retire;
	bool domain_retire_attempted;
	bool domain_unpublished;
	bool domain_unregistered;
	bool domain_retired;
	int domain_retire_error;
};

static void x1_session_release(struct kref *ref)
{
	struct x1_host *host = container_of(ref, struct x1_host, ref);

	/* No live session loses its port-owner reference. Failed attempts stay
	 * pinned; only a pristine or fully retired allocation can reach release.
	 */
	if (WARN_ON(host->startup_attempted && !host->fully_retired))
		return;
	if (host->fw)
		release_firmware(host->fw);
	mutex_destroy(&host->lock);
	mutex_destroy(&host->lifecycle_lock);
	mutex_destroy(&host->bind_lock);
	kfree(host);
}

static void x1_session_put(struct x1_host *host)
{
	if (host)
		kref_put(&host->ref, x1_session_release);
}
DEFINE_FREE(x1_session, struct x1_host *, if (_T) x1_session_put(_T));

/* Pin before any caller waits on session-local locks. A stale sysfs/cable
 * callback may finish against its closed old session, never the new one.
 */
static struct x1_host *x1_session_get(struct device *dev)
{
	struct x1_port *port = dev_get_drvdata(dev);
	struct x1_host *host;

	if (!port)
		return NULL;
	rcu_read_lock();
	host = rcu_dereference(port->session);
	if (host && !kref_get_unless_zero(&host->ref))
		host = NULL;
	rcu_read_unlock();
	return host;
}

/* Selection and the physical event record share renewal's publication lock.
 * No old callback can record a new cable event after pinning the wrong session. */
static struct x1_host *x1_orientation_session(struct x1_port *port,
					    enum typec_orientation orientation)
{
	struct x1_host *host;
	unsigned long flags;

	rcu_read_lock();
	spin_lock_irqsave(&port->event_lock, flags);
	host = rcu_dereference(port->session);
	if (host && !kref_get_unless_zero(&host->ref))
		host = NULL;
	port->orientation = orientation;
	port->event_sequence++;
	if (orientation == TYPEC_ORIENTATION_NONE)
		port->disconnect_sequence = port->event_sequence;
	spin_unlock_irqrestore(&port->event_lock, flags);
	rcu_read_unlock();
	return host;
}

static struct x1_host *x1_session_alloc(struct x1_port *port)
{
	struct x1_host *host = kzalloc(sizeof(*host), GFP_KERNEL);

	if (!host)
		return NULL;
	host->dev = port->dev;
	host->port = port;
	host->generation = ++port->next_generation;
	kref_init(&host->ref);
	mutex_init(&host->lifecycle_lock);
	mutex_init(&host->bind_lock);
	mutex_init(&host->lock);
	return host;
}

/* Type-C callback lock held. The first attachment is allowed only after local
 * preparation; every subsequent orientation/disconnect is terminal, including
 * before the MCU command. Duplicate notifications are not new attachments.
 */
static int x1_batch_orientation(struct x1_host *host, enum typec_orientation orientation)
{
	if (!host->batch_attempted)
		return 0;
	if (host->batch_terminal || host->error)
		return host->error ?: -ESHUTDOWN;
	if (orientation == host->orientation)
		return 0;
	if (!host->batch_waiting || host->batch_orientation != TYPEC_ORIENTATION_NONE ||
	    (orientation != TYPEC_ORIENTATION_NORMAL && orientation != TYPEC_ORIENTATION_REVERSE)) {
		host->error = -ESHUTDOWN;
		return host->error;
	}
	host->batch_orientation = orientation;
	return 0;
}

static int x1_read(void *ctx, u32 off, u32 *value)
{
	struct x1_host *host = ctx;

	if (off & 3 || off > X1_ROUTER_SIZE - 4)
		return -EINVAL;
	*value = readl(host->router + off);
	return 0;
}

static int x1_update(void *ctx, u32 off, u32 mask, u32 value)
{
	struct x1_host *host = ctx;
	u32 before;
	int ret = x1_read(ctx, off, &before);

	if (ret)
		return ret;
	writel((before & ~mask) | (value & mask), host->router + off);
	return 0;
}

static void x1_delay(void *ctx, unsigned int us)
{
	if (us < 20)
		udelay(us);
	else
		usleep_range(us, us + us / 10 + 10);
}

static int x1_owner(void *ctx, const struct x1_config *cfg)
{
	struct x1_host *host = ctx;

	if (cfg->firmware != host->fw->data || host->phy_initialized)
		return -EINVAL;
	/* No startup on live, pending, or inherited MCU state. */
	if (readl(host->router + 0x22000) || readl(host->router + 0x18) ||
	    readl(host->router + 0x22010) || readl(host->router + 0x2201c))
		return -EBUSY;
	if (readl(host->router + 0x78640) != 3)
		return -ENODEV;
	host->cold_owned = true;
	return 0;
}

static int x1_store_drom(struct x1_host *host)
{
	u8 drom[QCOM_USB4_DROM_SIZE];
	unsigned int i;
	int ret;

	ret = qcom_usb4_x1_drom_build(drom, sizeof(drom), host->serial, 0);
	if (ret)
		return ret;
	if (host->mcu.drom_offset != 0xdf20 ||
	    sizeof(drom) > host->mcu.ram_size - 0xdf20 ||
	    readl(host->router + host->mcu.control_offset))
		return -EINVAL;
	writel(get_unaligned_le32(drom + 5), host->router + 0x801c);
	writel(get_unaligned_le32(drom + 1), host->router + 0x8020);
	for (i = 0; i < sizeof(drom); i += 4)
		writel(get_unaligned_le32(drom + i), host->router + 0x20f20 + i);
	for (i = 0; i < sizeof(drom); i += 4)
		if (readl(host->router + 0x20f20 + i) != get_unaligned_le32(drom + i))
			return -EIO;
	return 0;
}

static int x1_step(void *ctx, enum x1_step step, const struct x1_config *cfg)
{
	struct x1_host *host = ctx;
	u32 command;
	int ret;

	switch (step) {
	case X1_PHY_PREPARE:
		ret = phy_set_mode_ext(host->phy, PHY_MODE_TBT, PHY_SUBMODE_USB4);
		if (!ret) {
			ret = phy_init(host->phy);
			host->phy_initialized = !ret;
		}
		return ret;
	case X1_RX_PREPARE: case X1_TXB_PREPARE: case X1_TXA_PREPARE:
		return pinctrl_select_state(host->pinctrl, host->pins[step - X1_RX_PREPARE]);
	case X1_RX_ACTIVE: case X1_TXB_ACTIVE: case X1_TXA_ACTIVE:
		return pinctrl_select_state(host->pinctrl, host->pins[step - X1_RX_ACTIVE + 3]);
	case X1_RX_CLOCK_REFERENCE:
		return x1_gcc_usb4_rx_select(host->gcc, false);
	case X1_RX_CLOCK_PHY:
		return x1_gcc_usb4_rx_select(host->gcc, true);
	case X1_MISC_RESET_ASSERT:
		return reset_control_bulk_assert(ARRAY_SIZE(host->resets), host->resets);
	case X1_MISC_RESET_CLEAR:
		return reset_control_bulk_deassert(ARRAY_SIZE(host->resets), host->resets);
	case X1_EXTRA_RESET_ASSERT: case X1_EXTRA_RESET_CLEAR:
		return x1_gcc_usb4_ctrl_bit(host->gcc, step == X1_EXTRA_RESET_ASSERT);
	case X1_DP_RESET_ASSERT: case X1_DP_RESET_CLEAR:
		return qcom_x1_usb4_phy_set_dp_reset(host->phy, step == X1_DP_RESET_ASSERT);
	case X1_PIPE_RESET_ASSERT:
		return reset_control_assert(host->resets[X1_PIPE_RESET].rstc);
	case X1_PIPE_RESET_CLEAR:
		return reset_control_deassert(host->resets[X1_PIPE_RESET].rstc);
	case X1_SYS_READY:
		return x1_gcc_usb4_sys_ready(host->gcc, 100000);
	case X1_UPLOAD:
		return qcom_usb4_mcu_upload(&host->mcu, cfg->firmware, cfg->firmware_size);
	case X1_SYS_MEMORY_FORCE:
		return x1_gcc_usb4_sys_force_mem(host->gcc, true);
	case X1_TCSR_ROUTE:
		return regmap_update_bits(host->tcsr, 0x1b000, BIT(0), BIT(0));
	case X1_PIPE_HWCG_OFF:
		return x1_gcc_usb4_pipe_hwcg(host->gcc, false);
	case X1_DROM:
		return x1_store_drom(host);
	case X1_PHY_COMPOSITE:
		ret = phy_power_on(host->phy);
		host->phy_on = !ret;
		return ret;
	case X1_MCU_START:
		return qcom_usb4_mcu_start(&host->mcu, &host->shared);
	case X1_PRESET:
		ret = qcom_usb4_typec_preset_command(cfg->mcu_preset, &command);
		return ret ?: qcom_usb4_mcu_command(&host->mcu, command, X1_COMMAND_TIMEOUT_US);
	default:
		return -EINVAL;
	}
}

static const struct x1_ops x1_start_ops = {
	.owner = x1_owner, .step = x1_step, .read = x1_read,
	.update = x1_update, .delay_us = x1_delay,
};

static int x1_apply_link(struct x1_host *host)
{
	int ret;

	lockdep_assert_held(&host->lock);
	if (!host->ready || !host->pending_command || host->stopping)
		return 0;
	if (host->error)
		return host->error;
	/* Duplicated notifications are not another firmware command. */
	if (host->pending_command == host->connected_command)
		return 0;
	/* The first integration is deliberately one attachment, not hotplug. */
	if (host->command_attempted) {
		host->error = -EOPNOTSUPP;
		return host->error;
	}
	host->command_attempted = true;
	ret = qcom_usb4_mcu_command(&host->mcu, host->pending_command,
				    X1_COMMAND_TIMEOUT_US);
	if (ret) {
		host->error = ret;
		dev_err(host->dev, "Type-C connect failed: %d; no retry, cold shutdown required\n", ret);
	} else {
		host->connected_command = host->pending_command;
		dev_info(host->dev, "Type-C connect acknowledged; not control-IRQ or PCIe/MSI proof\n");
	}
	return ret;
}

/* RCU covers the pointer AND its atomic invalidation; it ends before waiting
 * on the Type-C mutex. The owner unpublishes, drains readers, then drops its
 * extra domain reference. A load-acquire alone would not pin the old object.
 */
static void x1_invalidate_retained(struct x1_host *host)
{
	struct tb *tb;

	rcu_read_lock();
	tb = rcu_dereference(host->retained_tb);
	if (tb)
		tb_x1_invalidate_pcie(tb);
	rcu_read_unlock();
}

static int x1_switch_set(struct typec_switch_dev *sw, enum typec_orientation orientation)
{
	struct x1_port *port = typec_switch_get_drvdata(sw);
	struct x1_host *host __free(x1_session) = x1_orientation_session(port, orientation);
	int ret;

	if (!host)
		return -ENODEV;
	/* Invalidate before waiting: teardown may hold the Type-C lock. Also
	 * repeat under it, to catch a callback queued before CM hold began.
	 * This conservative private checkpoint refuses even post-hold duplicate
	 * notifications; it does not claim reusable hotplug yet.
	 */
	x1_invalidate_retained(host);
	mutex_lock(&host->lock);
	x1_invalidate_retained(host);
	if (host->stopping) {
		ret = 0; /* Cache physical state only; never touch the retired HW. */
		goto out;
	}
	ret = x1_batch_orientation(host, orientation);
	if (ret)
		goto out;
	if (host->orientation != orientation || orientation == TYPEC_ORIENTATION_NONE) {
		host->pending_command = 0;
		if (host->command_attempted)
			host->error = -ESHUTDOWN;
	}
	host->orientation = orientation;
out:
	mutex_unlock(&host->lock);
	return ret;
}

static int x1_mux_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct x1_port *port = typec_mux_get_drvdata(mux);
	struct x1_host *host __free(x1_session) = x1_session_get(port->dev);
	u32 command;
	int ret = 0;

	if (!host)
		return -ENODEV;
	x1_invalidate_retained(host);
	mutex_lock(&host->lock);
	x1_invalidate_retained(host);
	if (host->stopping)
		goto out;
	if (host->batch_attempted && (host->batch_terminal || host->error)) {
		ret = host->error ?: -ESHUTDOWN;
		goto out;
	}
	if (!state->alt && state->mode == TYPEC_MODE_USB4 && state->data) {
		if (host->batch_attempted && !host->batch_waiting) {
			ret = host->error = -ESHUTDOWN;
			goto out;
		}
		host->pending_command = 0;
		ret = qcom_usb4_typec_usb4_command(state->data, host->orientation, 1, &command);
		if (ret && (host->command_attempted || host->batch_attempted))
			host->error = ret;
		if (!ret) {
			host->pending_command = command;
			ret = x1_apply_link(host);
		}
	} else {
		host->pending_command = 0;
		/*
		 * This provider is downstream of PS8830, not the UCSI connector.
		 * Plain USB here is a PAN downgrade after retimer programming;
		 * UCSI's direct connector mux notification cannot reach this edge.
		 */
		if (host->command_attempted) {
			/* No guessed MCU stop-event IRQ or reattachment sequence. */
			host->error = -ESHUTDOWN;
			dev_warn(host->dev, "connection changed; no reattachment, cold shutdown required\n");
		}
	}
out:
	mutex_unlock(&host->lock);
	return ret;
}

static void x1_typec_unregister(void *data)
{
	struct x1_port *port = data;

	typec_mux_unregister(port->mux);
	typec_switch_unregister(port->sw);
}

static int x1_typec_register(struct x1_host *host)
{
	struct x1_port *port = host->port;
	struct typec_switch_desc sw = {
		.fwnode = dev_fwnode(host->dev), .set = x1_switch_set, .drvdata = port,
	};
	struct typec_mux_desc mux = {
		.fwnode = dev_fwnode(host->dev), .set = x1_mux_set, .drvdata = port,
	};

	port->sw = typec_switch_register(host->dev, &sw);
	if (IS_ERR(port->sw))
		return PTR_ERR(port->sw);
	port->mux = typec_mux_register(host->dev, &mux);
	if (IS_ERR(port->mux)) {
		typec_switch_unregister(port->sw);
		return PTR_ERR(port->mux);
	}
	return devm_add_action_or_reset(host->dev, x1_typec_unregister, port);
}

static const struct tb_nhi_ops x1_nhi_ops = {
	.init_interrupts = qcom_usb4_nhi_init_interrupts,
	.request_ring_irq = qcom_usb4_nhi_request_ring_irq,
	.release_ring_irq = qcom_usb4_nhi_release_ring_irq,
	.ring_interrupt_mask = qcom_usb4_nhi_ring_interrupt_mask,
	.disable_interrupts = qcom_usb4_nhi_disable_interrupts,
	.shutdown = qcom_usb4_nhi_shutdown,
};

static int x1_start_domain(struct x1_host *host)
{
	struct tb_nhi *nhi = &host->qnhi.nhi;
	unsigned int i;
	int ret;

	nhi->dev = host->dev;
	nhi->dma_dev = host->dev;
	nhi->iobase = host->router + X1_NHI_OFFSET;
	nhi->ops = &x1_nhi_ops;
	nhi->hop_count = 3;
	spin_lock_init(&nhi->lock);
	init_completion(&nhi->domain_released);
	if (readl(nhi->iobase + REG_CAPS) != 3)
		return -ENODEV;
	for (i = 0; i < nhi->hop_count; i++)
		if ((readl(nhi->iobase + REG_TX_OPTIONS_BASE + i * 32) |
		     readl(nhi->iobase + REG_RX_OPTIONS_BASE + i * 32)) & RING_FLAG_ENABLE)
			return -EBUSY;
	nhi->tx_rings = devm_kcalloc(host->dev, 3, sizeof(*nhi->tx_rings), GFP_KERNEL);
	nhi->rx_rings = devm_kcalloc(host->dev, 3, sizeof(*nhi->rx_rings), GFP_KERNEL);
	if (!nhi->tx_rings || !nhi->rx_rings)
		return -ENOMEM;
	ret = qcom_usb4_nhi_prepare(&host->qnhi, X1_ROUTER_SIZE - X1_NHI_OFFSET,
				    3, QCOM_USB4_IRQ_32, host->irq);
	if (ret)
		return ret;
	ret = qcom_usb4_nhi_init_interrupts(nhi);
	if (ret)
		goto shutdown;
	/*
	 * Explicit cold native domain: do not issue a second generic host reset,
	 * replace platform drvdata, or inherit PCI NHI PM callbacks. PHY/clock
	 * ownership stays with this frontend. No completion-polling fallback.
	 */
	host->tb = tb_probe(nhi);
	if (!host->tb) {
		ret = -ENOMEM;
		goto shutdown;
	}
	host->tb->security_level = TB_SECURITY_NOPCIE;
	ret = tb_domain_add(host->tb, false);
	if (ret) {
		tb_domain_put(host->tb);
		wait_for_completion(&nhi->domain_released);
		host->tb = NULL;
		goto shutdown;
	}
	pm_runtime_forbid(&host->tb->dev);
	return 0;
shutdown:
	qcom_usb4_nhi_shutdown(nhi);
	return ret;
}

static int x1_group_member(struct device *dev, void *data)
{
	return dev == data ? 0 : -EBUSY;
}

static int x1_dma_check(struct x1_host *host)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(host->dev);
	struct iommu_group *group;
	int ret;

	if (!domain || (domain->type != IOMMU_DOMAIN_DMA &&
			domain->type != IOMMU_DOMAIN_DMA_FQ))
		return -EPERM;
	group = iommu_group_get(host->dev);
	if (!group)
		return -EPERM;
	ret = iommu_group_for_each_dev(group, host->dev, x1_group_member);
	iommu_group_put(group);
	return ret ?: dma_set_mask_and_coherent(host->dev, DMA_BIT_MASK(32));
}

static int x1_firmware(struct x1_host *host)
{
	static const u8 expected[SHA256_DIGEST_SIZE] = {
		0xcd, 0x4f, 0x59, 0x29, 0xb5, 0x1f, 0x2d, 0xbb,
		0x0b, 0x58, 0x36, 0x93, 0xff, 0x8d, 0x02, 0x45,
		0x21, 0xc8, 0x7f, 0x0d, 0x2e, 0x45, 0xc7, 0xad,
		0xc1, 0x42, 0xfe, 0xd9, 0x76, 0x65, 0x0b, 0x99,
	};
	u8 digest[SHA256_DIGEST_SIZE];
	int ret = request_firmware(&host->fw, X1_FIRMWARE, host->dev);

	if (ret)
		return ret;
	if (host->fw->size != 40816)
		return -EINVAL;
	sha256(host->fw->data, host->fw->size, digest);
	if (memcmp(digest, expected, sizeof(digest)))
		return -EKEYREJECTED;
	return qcom_usb4_fw_validate(host->fw->data, host->fw->size, X1_RAM_SIZE);
}

static int x1_resources(struct x1_host *host, struct platform_device *pdev)
{
	struct device *dev = host->dev;
	struct irq_data *irqd;
	struct socinfo *info;
	struct resource *res;
	const char *firmware;
	size_t size;
	u32 port;
	unsigned int i;
	int ret;

	if (!of_machine_is_compatible("lenovo,yoga-slim7x"))
		return -ENODEV;
	if (of_property_read_u32(dev->of_node, "qcom,router-index", &port) || port)
		return -EINVAL;
	if (of_property_read_string(dev->of_node, "firmware-name", &firmware) ||
	    strcmp(firmware, X1_FIRMWARE))
		return -EINVAL;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != 0x15600000 || resource_size(res) != X1_ROUTER_SIZE)
		return -EINVAL;
	host->router = devm_ioremap_resource(dev, res);
	if (IS_ERR(host->router))
		return PTR_ERR(host->router);
	host->irq = platform_get_irq_byname(pdev, "nhi");
	if (host->irq < 0)
		return host->irq;
	irqd = irq_get_irq_data(host->irq);
	/* DT GIC SPI 472 corresponds to architectural INTID 504. */
	if (!irqd || irqd->hwirq != 504 || irq_get_trigger_type(host->irq) != IRQ_TYPE_LEVEL_HIGH)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(host->clocks); i++)
		host->clocks[i].id = x1_clock_names[i];
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(host->clocks), host->clocks);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(host->resets); i++)
		host->resets[i].id = x1_reset_names[i];
	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(host->resets), host->resets);
	if (ret)
		return ret;
	host->phy = devm_phy_get(dev, "usb4");
	if (IS_ERR(host->phy))
		return PTR_ERR(host->phy);
	host->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(host->pinctrl))
		return PTR_ERR(host->pinctrl);
	for (i = 0; i < ARRAY_SIZE(host->pins); i++) {
		host->pins[i] = pinctrl_lookup_state(host->pinctrl, x1_pin_names[i]);
		if (IS_ERR(host->pins[i]))
			return PTR_ERR(host->pins[i]);
	}
	host->tcsr = syscon_regmap_lookup_by_phandle(dev->of_node, "qcom,tcsr");
	if (IS_ERR(host->tcsr))
		return PTR_ERR(host->tcsr);
	host->ddr = devm_of_icc_get(dev, "usb4-ddr");
	if (IS_ERR(host->ddr))
		return PTR_ERR(host->ddr);
	host->apps = devm_of_icc_get(dev, "apps-usb4");
	if (IS_ERR(host->apps))
		return PTR_ERR(host->apps);
	info = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID, &size);
	if (IS_ERR(info))
		return PTR_ERR(info);
	if (offsetofend(struct socinfo, serial_num) > size)
		return -ENODEV;
	host->serial = le32_to_cpu(info->serial_num);
	if (!host->serial)
		return -ENODEV;
	return qcom_usb4_mcu_init(&host->mcu, host->router, X1_ROUTER_SIZE,
				  X1_RAM_SIZE, QCOM_USB4_MCU_LEGACY);
}

static ssize_t startup_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	ssize_t len;

	/* Startup mutates phase/step outside the short Type-C lock. */
	if (!mutex_trylock(&host->lifecycle_lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	mutex_lock(&host->lock);
	len = sysfs_emit(buf, "busy=0 attempted=%u activated=%u phase=%u step=%s completed=%s error=%d control_domain=%u connect_ack=%u orientation=%u pending_connect=%u pcie=%s mcu_event_irq=unknown reset_restore_attempted=%u reset_restore_complete=%u reset_restore_state=%x reset_restore_error=%d reset_hold_error=%d\n",
			host->startup_attempted,
			host->activated, host->state.phase, x1_step_name(host->state.active),
			x1_step_name(host->state.completed), host->error, host->ready,
			!!host->connected_command, host->orientation, !!host->pending_command,
			host->batch_attempted ? "private-inventory-batch" : "disabled",
			host->reset_restore_attempted, host->reset_restore_complete,
			host->reset_restore_state, host->reset_restore_error,
			host->reset_hold_error);
	mutex_unlock(&host->lock);
	mutex_unlock(&host->lifecycle_lock);
	return len;
}
static DEVICE_ATTR_RO(startup_state);

/* Only used before any startup write, or after successful router reset. */
static void x1_release_power(struct x1_host *host)
{
	if (host->gcc) {
		x1_gcc_usb4_put(host->gcc);
		host->gcc = NULL;
	}
	if (host->clocks_on) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(host->clocks), host->clocks);
		host->clocks_on = false;
	}
	icc_set_bw(host->apps, 0, 0);
	icc_set_bw(host->ddr, 0, 0);
	if (host->runtime_held) {
		pm_runtime_put_sync(host->dev);
		host->runtime_held = false;
	}
}

/* Full retirement holds every miscellaneous reset asserted. A fresh managed
 * generation is published only after all old owners have retired and physical
 * disconnect has been observed. Restore that inactive reset baseline BEFORE
 * enabling AHB; a held AHB reset otherwise prevents its clock from starting.
 * Only GCC reset-provider access is allowed here, not router MMIO, PHY or DMA.
 * The normal startup owner checks and complete startup sequence still follow.
 */
static int x1_restore_resets(struct x1_host *host)
{
	unsigned int i;
	int ret;

	lockdep_assert_held(&host->lifecycle_lock);
	if (!x1_managed || host->generation <= 1 || !host->gcc ||
	    !host->runtime_held || host->clocks_on || host->activated ||
	    host->reset_restore_attempted)
		return -EPERM;
	ret = x1_gcc_usb4_reset_state(host->gcc, &host->reset_restore_state);
	if (ret || host->reset_restore_state != GENMASK(10, 0)) {
		ret = ret ?: -EIO;
		goto fail;
	}
	host->reset_restore_attempted = true;
	for (i = 0; i < ARRAY_SIZE(host->resets); i++) {
		ret = reset_control_deassert(host->resets[i].rstc);
		if (ret)
			goto fail;
	}
	usleep_range(100, 120);
	ret = x1_gcc_usb4_reset_state(host->gcc, &host->reset_restore_state);
	if (ret || host->reset_restore_state) {
		ret = ret ?: -EIO;
		goto fail;
	}
	host->reset_restore_complete = true;
	return 0;
fail:
	host->reset_restore_error = ret;
	return ret;
}

/* Pre-startup failure only: try each reset once, never bulk-assert (its rollback
 * would deassert blocks). Do not release provider votes unless all asserts and
 * the independent GCC readback succeed. Failure stays terminal until cold-off.
 */
static int x1_rehold_resets(struct x1_host *host)
{
	unsigned int i;
	int ret, first = 0;

	lockdep_assert_held(&host->lifecycle_lock);
	for (i = 0; i < ARRAY_SIZE(host->resets); i++) {
		ret = reset_control_assert(host->resets[i].rstc);
		if (ret && !first)
			first = ret;
	}
	usleep_range(100, 120);
	ret = x1_gcc_usb4_reset_state(host->gcc, &host->reset_restore_state);
	if (!first)
		first = ret ?: (host->reset_restore_state != GENMASK(10, 0) ? -EIO : 0);
	host->reset_hold_error = first;
	return first;
}

/* Caller holds lifecycle_lock. Every accepted attempt is terminal, even if
 * admission fails before writes. Type-C callbacks may only cache state until
 * ready is published under their separate lock.
 */
static int x1_activate(struct x1_host *host)
{
	struct device *dev = host->dev;
	struct x1_config cfg = {
		.router = 0, .phy_rx_eq = 0x6f, .mcu_preset = 0x0c63, .reset_release_us = 100,
	};
	int ret;

	lockdep_assert_held(&host->lifecycle_lock);
	if (host->startup_attempted)
		return -EALREADY;
	if (host->stopping)
		return -ESHUTDOWN;
	if (host->suspended)
		return -EBUSY;
	host->startup_attempted = true;
	/* Pin before PM, interconnect, clock or any other provider operation. */
	if (!host->port->module_held && !try_module_get(THIS_MODULE)) {
		ret = -ENODEV;
		goto stopped;
	}
	host->port->module_held = true;
	ret = x1_dma_check(host);
	if (ret)
		goto stopped;
	ret = x1_firmware(host);
	if (ret)
		goto stopped;
	if (!host->port->runtime_enabled) {
		ret = devm_pm_runtime_enable(dev);
		if (ret)
			goto stopped;
		host->port->runtime_enabled = true;
	}
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto stopped;
	host->runtime_held = true;
	ret = icc_set_bw(host->apps, 0, MBps_to_icc(40));
	if (ret)
		goto release;
	ret = icc_set_bw(host->ddr, 0, MBps_to_icc(5000));
	if (ret)
		goto release;
	if (host->generation > 1) {
		/* A renewed owner starts from our asserted stop baseline, not the
		 * first cold boot's firmware baseline. The lease itself enables no
		 * RX clock and accesses no router register.
		 */
		host->gcc = x1_gcc_usb4_get(dev, 0);
		if (IS_ERR(host->gcc)) {
			ret = PTR_ERR(host->gcc);
			host->gcc = NULL;
			goto release;
		}
		ret = x1_restore_resets(host);
		if (ret)
			goto release;
	}
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(host->clocks), host->clocks);
	if (ret)
		goto release;
	host->clocks_on = true;
	if (!host->gcc) {
		host->gcc = x1_gcc_usb4_get(dev, 0);
		if (IS_ERR(host->gcc)) {
			ret = PTR_ERR(host->gcc);
			host->gcc = NULL;
			goto release;
		}
	}
	cfg.firmware = host->fw->data;
	cfg.firmware_size = host->fw->size;
	cfg.serial = host->serial;
	host->activated = true;
	ret = x1_startup_run(&host->state, &cfg, &x1_start_ops, host);
	if (ret)
		goto stopped;
	ret = x1_start_domain(host);
	if (ret)
		goto stopped;
	mutex_lock(&host->lock);
	host->ready = true;
	ret = x1_apply_link(host);
	mutex_unlock(&host->lock);
	if (ret)
		goto stopped;
	dev_info(dev, "X1 control domain ready; PCIe disabled, genuine IRQ evidence still required\n");
	return 0;
release:
	if (host->reset_restore_attempted && x1_rehold_resets(host)) {
		dev_err(dev, "renewed reset hold failed (%d); retaining provider votes until cold-off\n",
			host->reset_hold_error);
		goto stopped;
	}
	x1_release_power(host);
stopped:
	mutex_lock(&host->lock);
	host->error = ret;
	mutex_unlock(&host->lock);
	if (host->fw) {
		release_firmware(host->fw);
		host->fw = NULL;
	}
	dev_err(dev, "startup stopped (%d) at %s; activated=%u; no automatic retry\n",
		ret, x1_step_name(host->state.active), host->activated);
	return ret;
}

static ssize_t activate_once_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	int ret;

	if (!capable(CAP_SYS_ADMIN) || !x1_runtime_startup)
		return -EPERM;
	if (!((count == 10 && !memcmp(buf, "cold-start", 10)) ||
	      (count == 11 && !memcmp(buf, "cold-start\n", 11))))
		return -EINVAL;
	/* A blocking lock here deadlocks removal's drain of active sysfs calls. */
	if (!device_trylock(dev))
		return -EBUSY;
	mutex_lock(&host->lifecycle_lock);
	mutex_lock(&host->lock);
	ret = host->batch_attempted ? -EALREADY :
		host->orientation != TYPEC_ORIENTATION_NONE || host->pending_command ?
		-EBUSY : 0;
	mutex_unlock(&host->lock);
	if (!ret)
		ret = x1_activate(host);
	mutex_unlock(&host->lifecycle_lock);
	device_unlock(dev);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(activate_once);

static bool x1_pcie_empty_domain(struct tb *tb)
{
	struct tb_port *port;

	lockdep_assert_held(&tb->lock);
	if (!tb->root_switch || tb->root_switch->is_unplugged)
		return false;
	tb_switch_for_each_port(tb->root_switch, port)
		if (port->remote || port->xdomain)
			return false;
	return true;
}

/* This is not PCIe tunnel admission. Keep nopcie throughout, and do not
 * publish a PCI bus, map an endpoint, or acquire an unverified MSI route.
 * Lock order: device -> lifecycle -> Type-C -> TB domain -> PCI0 helper.
 * Holding Type-C while the bounded helper runs prevents a connect command
 * interleaving with the shared router mode gate. No helper calls Type-C.
 */
static ssize_t pcie_prepare_once_store(struct device *dev, struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	bool inspect, cfg0;
	int ret;

	if (!capable(CAP_SYS_ADMIN) || !x1_pcie_prepare)
		return -EPERM;
	inspect = (count == 12 && !memcmp(buf, "root-inspect", 12)) ||
		  (count == 13 && !memcmp(buf, "root-inspect\n", 13));
	cfg0 = (count == 9 && !memcmp(buf, "root-cfg0", 9)) ||
		(count == 10 && !memcmp(buf, "root-cfg0\n", 10));
	if (!inspect && !cfg0 && !((count == 9 && !memcmp(buf, "root-mode", 9)) ||
			 (count == 10 && !memcmp(buf, "root-mode\n", 10))))
		return -EINVAL;
	if (((inspect || cfg0) && !x1_pcie_inspect) || (cfg0 && !x1_pcie_cfg0))
		return -EPERM;
	if (!device_trylock(dev))
		return -EBUSY;
	mutex_lock(&host->lifecycle_lock);
	mutex_lock(&host->lock);
	if (host->pcie.attempted || host->batch_attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (!host->startup_attempted || !host->activated || !host->cold_owned ||
	    !host->ready || !host->tb || host->error || host->stopping || host->suspended) {
		ret = -EPERM;
		goto out;
	}
	if (host->orientation != TYPEC_ORIENTATION_NONE || host->pending_command ||
	    host->connected_command || host->command_attempted) {
		ret = -EBUSY;
		goto out;
	}
	mutex_lock(&host->tb->lock);
	if (host->tb->security_level != TB_SECURITY_NOPCIE ||
	    !x1_pcie_empty_domain(host->tb)) {
		ret = -EPERM;
	} else {
		host->pcie.inspect_requested = inspect || cfg0;
		host->pcie.cfg0_requested = cfg0;
		ret = qcom_usb4_x1_pcie_prepare(dev, host->router, &host->pcie);
		/* No attachment is allowed after this isolated mode-only test,
		 * even on success. A later candidate must own full PCI0 lifecycle.
		 */
		if (host->pcie.attempted)
			host->error = ret ?: -ESHUTDOWN;
	}
	mutex_unlock(&host->tb->lock);
out:
	mutex_unlock(&host->lock);
	mutex_unlock(&host->lifecycle_lock);
	device_unlock(dev);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(pcie_prepare_once);

/* Exact bounded private record, not a command line/module parameter. No
 * identifier is logged or echoed. A malformed record consumes no attempt.
 */
static int x1_batch_parse(const char *buf, size_t count, u64 *uid, u16 *vendor, u16 *device)
{
	u8 identity[8], ids[4];

	if ((count != 36 && (count != 37 || buf[36] != '\n')) ||
	    memcmp(buf, "inventory ", 10) || buf[26] != ' ' || buf[31] != ':')
		return -EINVAL;
	if (hex2bin(identity, buf + 10, 8) || hex2bin(ids, buf + 27, 2) ||
	    hex2bin(ids + 2, buf + 32, 2))
		return -EINVAL;
	*uid = get_unaligned_be64(identity);
	*vendor = get_unaligned_be16(ids);
	*device = get_unaligned_be16(ids + 2);
	return !*uid || *uid == U64_MAX || !*vendor || *vendor == U16_MAX ||
		!*device || *device == U16_MAX ? -EINVAL : 0;
}

/* Device/lifecycle held throughout; Type-C/domain locks only while observing.
 * The cold-owned frontend and native CM handle the one physical attachment.
 * No polling of downstream MMIO, tunnel approval or reconnect occurs here.
 */
static int x1_batch_wait(struct x1_host *host)
{
	int ret;

	for (;;) {
		mutex_lock(&host->lock);
		ret = host->error;
		if (!ret && time_after_eq(jiffies, host->batch_deadline))
			ret = -ETIMEDOUT;
		if (!ret && (host->stopping || host->suspended || !host->ready || !host->tb))
			ret = -ESHUTDOWN;
		if (!ret && host->connected_command) {
			if (!host->batch_waiting || !host->command_attempted ||
			    host->orientation == TYPEC_ORIENTATION_NONE ||
			    host->orientation != host->batch_orientation ||
			    host->pending_command != host->connected_command) {
				ret = -ENOLINK;
			} else {
				mutex_lock(&host->tb->lock);
				host->batch_switch = tb_switch_find_by_route(host->tb, 2);
				if (host->batch_switch &&
				    host->batch_switch->uid != host->batch_uid)
					ret = -ENODEV;
				if (!ret && time_after_eq(jiffies, host->batch_deadline))
					ret = -ETIMEDOUT;
				mutex_unlock(&host->tb->lock);
			}
		}
		mutex_unlock(&host->lock);
		if (ret || host->batch_switch)
			return ret;
		msleep(50);
	}
}

/* All five caller locks remain held once CM approval begins. A live control
 * request may take time; check the wall-clock deadline both before and after.
 * Neither this deadline nor a recent successful sample contains stuck MMIO.
 */
static int x1_batch_live(void *context)
{
	struct x1_host *host = context;
	int ret;

	device_lock_assert(host->dev);
	lockdep_assert_held(&host->lifecycle_lock);
	lockdep_assert_held(&host->lock);
	lockdep_assert_held(&host->tb->lock);
	if (host->error || host->stopping || host->suspended || !host->ready ||
	    !host->cold_owned || !host->batch_attempted || host->batch_terminal ||
	    host->batch_waiting || !host->batch_switch || !host->command_attempted ||
	    !host->connected_command || host->pending_command != host->connected_command ||
	    host->orientation == TYPEC_ORIENTATION_NONE ||
	    host->orientation != host->batch_orientation)
		return host->error ?: -ESHUTDOWN;
	if (time_after_eq(jiffies, host->batch_deadline))
		return -ETIMEDOUT;
	ret = tb_x1_validate_pcie(host->tb, host->batch_switch, host->batch_uid);
	if (ret)
		return ret;
	return time_after_eq(jiffies, host->batch_deadline) ? -ETIMEDOUT : 0;
}

static ssize_t inventory_once_store(struct device *dev, struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	u64 uid;
	u16 vendor, device;
	bool bind = false;
	int ret;

	if (!capable(CAP_SYS_ADMIN) || !x1_inventory_batch || !x1_imsi_receiver || x1_startup ||
	    x1_runtime_startup || x1_pcie_prepare || x1_pcie_inspect || x1_pcie_cfg0 ||
	    (x1_tunnel_quiesce && !x1_nvme_read))
		return -EPERM;
	ret = x1_batch_parse(buf, count, &uid, &vendor, &device);
	if (ret)
		return ret;
	if (!device_trylock(dev))
		return -EBUSY;
	mutex_lock(&host->lifecycle_lock);
	mutex_lock(&host->lock);
	/* Managed mode separates empty-port preparation from attachment. Idle
	 * waiting holds no device/lifecycle mutex and has no 90/120s deadline. */
	if (x1_managed && host->batch_attempted && host->batch_waiting &&
	    !host->batch_terminal && !host->error && host->ready &&
	    host->pcie.prepared && !host->stopping && !host->suspended &&
	    uid == host->batch_uid && vendor == host->batch_vendor &&
	    device == host->batch_device) {
		if (host->orientation == TYPEC_ORIENTATION_NONE) {
			ret = -EAGAIN;
			goto out;
		}
		host->batch_deadline = jiffies + 120 * HZ;
		goto wait_attachment;
	}
	if (host->startup_attempted || host->pcie.attempted || host->batch_attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (host->stopping || host->suspended || host->error ||
	    host->orientation != TYPEC_ORIENTATION_NONE || host->pending_command ||
	    host->connected_command || host->command_attempted) {
		ret = -EBUSY;
		goto out;
	}
	/* Consume before startup or any provider operation. */
	host->batch_attempted = true;
	host->batch_uid = uid;
	host->batch_vendor = vendor;
	host->batch_device = device;
	host->batch_step = "startup";
	mutex_unlock(&host->lock);
	ret = x1_activate(host);
	mutex_lock(&host->lock);
	if (ret)
		goto terminal;
	if (host->error || host->orientation != TYPEC_ORIENTATION_NONE ||
	    host->pending_command || host->connected_command || host->command_attempted) {
		ret = host->error ?: -EBUSY;
		goto terminal;
	}
	host->batch_step = "local-preparation";
	host->pcie.receiver_requested = true;
	host->pcie.nvme_requested = x1_nvme_read;
	mutex_lock(&host->tb->lock);
	if (host->tb->security_level != TB_SECURITY_NOPCIE || !x1_pcie_empty_domain(host->tb))
		ret = -EPERM;
	else
		ret = qcom_usb4_x1_batch_prepare(dev, host->router, &host->pcie);
	mutex_unlock(&host->tb->lock);
	if (ret)
		goto terminal;
	host->batch_step = "connect-once";
	host->batch_waiting = true;
	host->batch_deadline = jiffies + 120 * HZ;
	if (x1_managed) {
		host->batch_deadline = 0;
		ret = 0;
		goto out;
	}
wait_attachment:
	mutex_unlock(&host->lock);
	dev_info(dev, "inventory batch: local preparation passed; connect admitted device once within 120 seconds\n");
	ret = x1_batch_wait(host);
	mutex_lock(&host->lock);
	host->batch_waiting = false;
	if (ret)
		goto terminal;
	if (time_after_eq(jiffies, host->batch_deadline)) {
		ret = -ETIMEDOUT;
		goto terminal;
	}
	if (host->error || !host->batch_switch || host->stopping || host->suspended ||
	    host->orientation != host->batch_orientation || !host->connected_command ||
	    host->pending_command != host->connected_command) {
		ret = host->error ?: -ENOLINK;
		goto terminal;
	}
	host->batch_step = "approve-tunnel";
	host->batch_deadline = jiffies + 60 * HZ;
	mutex_lock(&host->tb->lock);
	ret = tb_x1_approve_pcie(host->tb, host->batch_switch, host->batch_uid);
	if (!ret)
		ret = x1_batch_live(host);
	if (!ret) {
		host->batch_step = "training-inventory-and-masked-receiver";
		ret = qcom_usb4_x1_batch_inventory(dev, &host->pcie, x1_batch_live, host,
				vendor, device, &host->training, &host->inventory);
	}
	if (!ret && x1_tunnel_quiesce) {
		ret = tb_x1_hold_pcie(host->tb, host->batch_switch, host->batch_uid);
		host->tunnel_held = !ret;
		if (!ret) {
			get_device(&host->tb->dev);
			rcu_assign_pointer(host->retained_tb, host->tb);
		}
	}
	mutex_unlock(&host->tb->lock);
terminal:
	/* Fence frontend/Type-C work before dropping the locks for probe. Native
	 * CM ownership is separately retained by tb_x1_hold_pcie when armed.
	 * A published, held endpoint has not yet been admitted to DMA. The bind
	 * helper has its own single-use receipt; no failed batch can reach it.
	 */
	bind = !ret && x1_nvme_read && host->pcie.host_published;
	host->batch_waiting = false;
	host->batch_terminal = true;
	host->batch_complete = !ret && !bind;
	host->batch_error = ret;
	host->error = ret ?: -ESHUTDOWN;
	if (!ret)
		host->batch_step = bind ? "nvme-bind-pending" : "receiver-complete-no-endpoint-dma";
	if (host->batch_switch) {
		tb_switch_put(host->batch_switch);
		host->batch_switch = NULL;
	}
out:
	mutex_unlock(&host->lock);
	mutex_unlock(&host->lifecycle_lock);
	device_unlock(dev);
	if (bind) {
		/* NVMe setup may sleep and start asynchronous admin DMA. It must
		 * never inherit the frontend, Type-C or Thunderbolt domain locks.
		 */
		mutex_lock(&host->bind_lock);
		mutex_lock(&host->lock);
		ret = host->stopping || host->suspended || !host->ready ? -ESHUTDOWN : 0;
		mutex_unlock(&host->lock);
		if (!ret)
			ret = qcom_usb4_x1_nvme_bind(dev, &host->pcie);
		mutex_lock(&host->lock);
		host->batch_error = ret;
		host->batch_complete = !ret;
		host->batch_step = ret ? "nvme-bind-failed" : "nvme-bound-read-not-proven";
		mutex_unlock(&host->lock);
		mutex_unlock(&host->bind_lock);
	}
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(inventory_once);

/* Only cached software state; can report connect-once while lifecycle is held
 * by the synchronous batch. Never wait on its long critical sections.
 */
static ssize_t inventory_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	ssize_t len;

	if (!mutex_trylock(&host->lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	/* PCI binding updates its receipt outside the frontend lock. Do not read
	 * a partially updated receipt or claim a completed pipeline meanwhile.
	 */
	if (host->batch_step && !strcmp(host->batch_step, "nvme-bind-pending")) {
		mutex_unlock(&host->lock);
		return sysfs_emit(buf, "busy=1 step=nvme-bind-pending snapshot=unavailable\n");
	}
	len = sysfs_emit(buf, "busy=0 attempted=%u waiting=%u terminal=%u complete=%u error=%d step=%s training_attempted=%u trained=%u training_error=%d inventory_attempted=%u inventory_complete=%u inventory_error=%d reads=%u cached=1 endpoint_enable=%s endpoint_dma=%s msi_delivery=unproven cold_shutdown_required=%u\n",
		host->batch_attempted, host->batch_waiting, host->batch_terminal,
		host->batch_complete, host->batch_error, host->batch_step ?: "none",
		host->training.attempted, host->training.complete, host->training.error,
		host->inventory.attempted, host->inventory.complete, host->inventory.error,
		host->inventory.reads, host->pcie.nvme_bind_attempted ? "attempted" : "never",
		host->pcie.nvme_bind_attempted ? "possible" : "disabled", host->batch_attempted);
	len += sysfs_emit_at(buf, len, "receiver_attempted=%u retained=%u domain_ready=%u target_reserved=%u policy_complete=%u associated=%u receiver_ready=%u receiver_error=%d receiver_step=%s banks=%u vectors=%u target=%016llx bridge_allocated=%u bridge_published=%u mask_policy=%s global_mask_before=%08x global_mask_after=%08x global_status_before=%08x global_status_after=%08x\n",
		host->pcie.receiver.attempted, host->pcie.receiver.retained,
		host->pcie.receiver.domain_ready, host->pcie.receiver.target_reserved,
		host->pcie.receiver.global_policy_complete, host->pcie.receiver.associated,
		host->pcie.receiver.ready, host->pcie.receiver.error,
		host->pcie.receiver.step ?: "none", host->pcie.receiver.banks,
		host->pcie.receiver.vectors, host->pcie.receiver.target,
		host->pcie.bridge_allocated, host->pcie.host_published,
		host->pcie.nvme_bind_attempted ? "driver-owned" : "all-masked", host->pcie.receiver_mask_before,
		host->pcie.receiver_mask_after, host->pcie.receiver_status_before,
		host->pcie.receiver_status_after);
	len += sysfs_emit_at(buf, len, "nvme_requested=%u host_attempted=%u host_error=%d cfg_error=%d host_step=%s mem_attempted=%u mem_complete=%u mem_error=%d nvme_bind_attempted=%u nvme_bound=%u writes=prohibited\n",
		host->pcie.nvme_requested, host->pcie.host_attempted, host->pcie.host_error,
		READ_ONCE(host->pcie.cfg_error), host->pcie.host_step ?: "none", host->pcie.mem.attempted,
		host->pcie.mem.complete, host->pcie.mem.error,
		host->pcie.nvme_bind_attempted, host->pcie.nvme_bound);
	/* The first config failure publishes its immutable detail before errno. */
	if (smp_load_acquire(&host->pcie.cfg_error))
		len += sysfs_emit_at(buf, len,
			"cfg_error_bus=%u cfg_error_offset=%03x cfg_error_size=%u cfg_error_write=%u cfg_error_value=%08x\n",
			host->pcie.cfg_error_bus, host->pcie.cfg_error_offset,
			host->pcie.cfg_error_size, host->pcie.cfg_error_write,
			host->pcie.cfg_error_value);
	mutex_unlock(&host->lock);
	return len;
}
static DEVICE_ATTR_RO(inventory_state);

/* Called with PCI0 and the endpoint's removal/shutdown locks held as well as
 * the frontend locks below. No PCI/NVMe reentry, device free or rearm here.
 */
static int x1_stop_tunnel_action(void *context)
{
	struct x1_host *host = context;

	device_lock_assert(host->dev);
	lockdep_assert_held(&host->bind_lock);
	lockdep_assert_held(&host->lifecycle_lock);
	lockdep_assert_held(&host->lock);
	lockdep_assert_held(&host->tb->lock);
	return tb_x1_stop_pcie_retained(host->tb, host->batch_uid, &host->tunnel_stop);
}

static ssize_t tunnel_quiesce_once_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	int ret = -EPERM;

	if (!capable(CAP_SYS_ADMIN) || !x1_tunnel_quiesce ||
	    !sysfs_streq(buf, "stop-retain-v1"))
		return ret;
	if (!device_trylock(dev))
		return -EBUSY;
	mutex_lock(&host->bind_lock);
	mutex_lock(&host->lifecycle_lock);
	mutex_lock(&host->lock);
	if (host->tunnel_stop_attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (!host->tb || !host->tunnel_held || !host->cold_owned ||
	    !host->ready || host->stopping || host->suspended ||
	    !host->batch_attempted || !host->batch_terminal ||
	    !host->batch_complete || host->batch_waiting || host->batch_error ||
	    host->error != -ESHUTDOWN || !host->command_attempted ||
	    !host->connected_command || host->pending_command != host->connected_command ||
	    host->orientation == TYPEC_ORIENTATION_NONE ||
	    host->orientation != host->batch_orientation)
		goto out;
	/* One admitted call only, including endpoint-proof or CM refusal. */
	host->tunnel_stop_attempted = true;
	mutex_lock(&host->tb->lock);
	ret = qcom_usb4_x1_fence_and_quiesce(dev, &host->pcie,
					   x1_stop_tunnel_action, host);
	mutex_unlock(&host->tb->lock);
	host->tunnel_stop_error = ret;
	host->tunnel_stop_complete = !ret;
	dev_info(dev, "retained tunnel stop result=%d complete=%u adapters=%u hops=%u physical_eject=0 reconnect=0\n",
		 ret, host->tunnel_stop_complete, host->tunnel_stop.adapters,
		 host->tunnel_stop.hops);
out:
	mutex_unlock(&host->lock);
	mutex_unlock(&host->lifecycle_lock);
	mutex_unlock(&host->bind_lock);
	device_unlock(dev);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(tunnel_quiesce_once);

static ssize_t tunnel_quiesce_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	const struct tb_x1_tunnel_stop *s = &host->tunnel_stop;
	ssize_t len;

	if (!mutex_trylock(&host->lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	len = sysfs_emit(buf,
		"busy=0 enabled=%u held=%u attempted=%u complete=%u error=%d cm_attempted=%u cm_finished=%u cm_error=%d stage=%s adapters=%u hops=%u retained=1 physical_eject=0 reconnect=0 cached=1\n",
		x1_tunnel_quiesce, host->tunnel_held, host->tunnel_stop_attempted,
		host->tunnel_stop_complete, host->tunnel_stop_error, s->attempted,
		s->finished, s->error, s->stage ?: "none", s->adapters, s->hops);
	mutex_unlock(&host->lock);
	return len;
}
static DEVICE_ATTR_RO(tunnel_quiesce_state);

static ssize_t host_quiesce_once_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	struct qcom_usb4_nhi_stop *s = &host->host_stop;
	int ret = -EPERM;

	if (!capable(CAP_SYS_ADMIN) || !x1_host_quiesce || !x1_tunnel_quiesce ||
	    !sysfs_streq(buf, "stop-host-retain-v1"))
		return ret;
	if (!device_trylock(dev))
		return -EBUSY;
	mutex_lock(&host->bind_lock);
	mutex_lock(&host->lifecycle_lock);
	mutex_lock(&host->lock);
	if (host->host_stop_attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (!host->tb || !host->tunnel_held || !host->pcie.config_fenced ||
	    !host->pcie.receiver.stop.finished || host->pcie.receiver.stop.error ||
	    host->pcie.receiver.stop.banks_disabled != 8 ||
	    host->pcie.receiver.stop.parents_detached != 8 ||
	    !host->tunnel_stop_attempted ||
	    !host->tunnel_stop_complete || host->tunnel_stop_error ||
	    !host->tunnel_stop.finished || host->tunnel_stop.error ||
	    host->tunnel_stop.adapters != 2 || host->tunnel_stop.hops != 4 ||
	    !host->cold_owned || !host->ready || host->stopping || host->suspended ||
	    !host->batch_complete || !host->batch_terminal || host->batch_waiting ||
	    host->batch_error || host->error != -ESHUTDOWN ||
	    host->orientation == TYPEC_ORIENTATION_NONE ||
	    host->orientation != host->batch_orientation ||
	    !host->connected_command || host->pending_command != host->connected_command)
		goto out;
	/* Never re-read endpoint registers behind the already stopped tunnel.
	 * Completion is a kernel-owned, monotonic receipt; all objects stay pinned.
	 */
	host->host_stop_attempted = true;
	s->stage = "cm-before-host-stop";
	mutex_lock(&host->tb->lock);
	ret = tb_x1_pcie_stopped(host->tb, host->batch_uid);
	if (!ret)
		ret = tb_ctl_stop_x1_retained(host->tb->ctl, &host->qnhi, s);
	if (!ret) {
		s->stage = "cm-after-host-stop";
		ret = tb_x1_pcie_stopped(host->tb, host->batch_uid);
	}
	if (!ret && (!s->attempted || !s->ctl_closed || !s->irq_disabled ||
		     !s->callbacks_drained || s->rings_disabled != 2 ||
		     s->rx_options || s->tx_options))
		ret = -EIO;
	s->error = ret;
	s->finished = !ret;
	if (!ret) {
		s->stage = "control-stopped-retained";
		host->ready = false;
	}
	mutex_unlock(&host->tb->lock);
	dev_info(dev, "retained host stop result=%d complete=%u rings=%u irq_disabled=%u callbacks_drained=%u physical_eject=0 reconnect=0\n",
		 ret, s->finished, s->rings_disabled, s->irq_disabled, s->callbacks_drained);
out:
	mutex_unlock(&host->lock);
	mutex_unlock(&host->lifecycle_lock);
	mutex_unlock(&host->bind_lock);
	device_unlock(dev);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(host_quiesce_once);

static ssize_t host_quiesce_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	const struct qcom_usb4_nhi_stop *s = &host->host_stop;
	ssize_t len;

	if (!mutex_trylock(&host->lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	len = sysfs_emit(buf,
		"busy=0 enabled=%u attempted=%u complete=%u error=%d stage=%s ctl_closed=%u rings=%u irq_disabled=%u callbacks_drained=%u rx_options=%08x tx_options=%08x retained=1 physical_eject=0 reconnect=0 cached=1\n",
		x1_host_quiesce, host->host_stop_attempted, s->finished, s->error,
		s->stage ?: "none", s->ctl_closed, s->rings_disabled, s->irq_disabled,
		s->callbacks_drained, s->rx_options, s->tx_options);
	mutex_unlock(&host->lock);
	return len;
}
static DEVICE_ATTR_RO(host_quiesce_state);

/* Lifecycle/Type-C locks held. All endpoint, tunnel and ring admission has
 * already closed permanently. Do not read router/endpoint MMIO after reset.
 * Request-bit readback is not an MCU halt or global DMA-idle acknowledgement:
 * every DMA allocation, PCI object and IRQ domain remains owned and pinned.
 */
static int x1_power_stop_run(struct x1_host *host)
{
	struct x1_power_stop *s = &host->power_stop;
	u32 route;
	unsigned int i;
	int ret;

	if (s->attempted)
		return -EALREADY;
	if (!host->cold_owned || !host->activated || !host->pcie.config_fenced ||
	    !host->pcie.receiver.stop.finished || host->pcie.receiver.stop.error ||
	    host->pcie.receiver.stop.banks_disabled != 8 ||
	    host->pcie.receiver.stop.parents_detached != 8 ||
	    host->ready || host->stopping ||
	    host->suspended || !host->gcc || !host->clocks_on || !host->runtime_held ||
	    !host->phy_on || !host->phy_initialized || !host->host_stop_attempted ||
	    !host->host_stop.finished || host->host_stop.error ||
	    !host->host_stop.ctl_closed || !host->host_stop.irq_disabled ||
	    !host->host_stop.callbacks_drained || host->host_stop.rings_disabled != 2 ||
	    host->host_stop.rx_options || host->host_stop.tx_options ||
	    !host->qnhi.retained_stop_attempted || !host->qnhi.retained_irq_disabled ||
	    host->qnhi.accept_rearm || host->mcu.control_offset != 0x22000 ||
	    host->mcu.base != host->router)
		return -EPERM;
	s->attempted = true;
	/* Includes partial failures: system shutdown must not repeat this path. */
	host->stopping = true;
	s->stage = "reset-baseline";
	ret = x1_gcc_usb4_reset_state(host->gcc, &s->reset_requests);
	if (ret || s->reset_requests) {
		ret = ret ?: -EIO;
		goto fail;
	}
	s->stage = "mcu-stop-request";
	qcom_usb4_mcu_stop(&host->mcu);
	s->mcu_control = readl(host->router + 0x22000);
	s->host_control = readl(host->router + 0x8018);
	if ((s->mcu_control & BIT(0)) || (s->host_control & BIT(24))) {
		ret = -EIO;
		goto fail;
	}
	s->mcu_request_cleared = true;
	s->stage = "router-reset";
	/* Never use bulk assert here: its error rollback would restart blocks. */
	for (i = 0; i < ARRAY_SIZE(host->resets); i++) {
		ret = reset_control_assert(host->resets[i].rstc);
		if (ret)
			goto fail;
		s->resets_asserted++;
	}
	usleep_range(100, 120);
	ret = x1_gcc_usb4_reset_state(host->gcc, &s->reset_requests);
	if (ret || s->reset_requests != GENMASK(10, 0)) {
		ret = ret ?: -EIO;
		goto fail;
	}
	/* The RX mux must leave its PHY parent before that parent powers down. */
	s->stage = "rx-reference";
	ret = x1_gcc_usb4_rx_select(host->gcc, false);
	if (ret)
		goto fail;
	s->rx_reference = true;
	s->stage = "phy-power-off";
	ret = phy_power_off(host->phy);
	if (ret)
		goto fail;
	host->phy_on = false;
	s->phy_off = true;
	s->stage = "phy-exit";
	ret = phy_exit(host->phy);
	if (ret)
		goto fail;
	host->phy_initialized = false;
	s->phy_exited = true;
	s->stage = "route-clear";
	ret = regmap_update_bits(host->tcsr, 0x1b000, BIT(0), 0);
	if (!ret)
		ret = regmap_read(host->tcsr, 0x1b000, &route);
	if (ret || (route & BIT(0))) {
		ret = ret ?: -EIO;
		goto fail;
	}
	s->route_cleared = true;
	s->stage = "memory-force-clear";
	ret = x1_gcc_usb4_sys_force_mem(host->gcc, false);
	if (ret)
		goto fail;
	s->memory_force_cleared = true;
	s->stage = "icc-votes-release";
	ret = icc_set_bw(host->apps, 0, 0);
	if (!ret)
		ret = icc_set_bw(host->ddr, 0, 0);
	if (ret)
		goto fail;
	s->icc_released = true;
	s->stage = "clock-votes-release";
	x1_gcc_usb4_put(host->gcc);
	host->gcc = NULL;
	s->gcc_released = true;
	clk_bulk_disable_unprepare(ARRAY_SIZE(host->clocks), host->clocks);
	host->clocks_on = false;
	s->clocks_released = true;
	s->stage = "runtime-vote-release";
	ret = pm_runtime_put_sync(host->dev);
	/* put_sync drops the usage count even if the suspend callback fails. */
	host->runtime_held = false;
	s->runtime_released = true;
	if (ret < 0)
		goto fail;
	s->stage = "provider-votes-released-retained";
	s->finished = true;
	return 0;
fail:
	s->error = ret;
	return ret;
}

/* lifecycle_lock keeps these monotonic receipts and provider ownership
 * stable. Called under PCI/NVMe locks, so do not acquire Type-C/CM locks or
 * issue hardware operations here. CM identity was checked before dropping
 * its lock, after ring/IRQ/callback stop; no new event can be queued by NHI.
 * A later Type-C notification may invalidate the old generation, but cannot
 * undo its completed stop or retarget these pinned PCI/NVMe objects. This
 * permits removal of OLD namespace visibility only, never a new connection.
 */
static int x1_namespace_owner_stopped(void *context)
{
	struct x1_host *host = context;
	const struct x1_power_stop *s = &host->power_stop;

	lockdep_assert_held(&host->lifecycle_lock);
	if (!host->stopping || host->ready || host->suspended ||
	    !host->cold_owned || !host->activated || !host->pcie.config_fenced ||
	    !host->pcie.receiver.stop.finished || host->pcie.receiver.stop.error ||
	    host->pcie.receiver.stop.banks_disabled != 8 ||
	    host->pcie.receiver.stop.parents_detached != 8 ||
	    !host->tunnel_held || !host->tunnel_stop_complete ||
	    host->tunnel_stop_error || !host->tunnel_stop.finished ||
	    host->tunnel_stop.error || host->tunnel_stop.adapters != 2 ||
	    host->tunnel_stop.hops != 4 || !host->host_stop_attempted ||
	    !host->host_stop.finished || host->host_stop.error ||
	    !host->host_stop.ctl_closed || !host->host_stop.irq_disabled ||
	    !host->host_stop.callbacks_drained || host->host_stop.rings_disabled != 2 ||
	    host->host_stop.rx_options || host->host_stop.tx_options ||
	    !host->qnhi.retained_stop_attempted || !host->qnhi.retained_irq_disabled ||
	    host->qnhi.accept_rearm || !s->attempted || !s->finished || s->error ||
	    !s->mcu_request_cleared || s->resets_asserted != ARRAY_SIZE(host->resets) ||
	    s->reset_requests != GENMASK(10, 0) || !s->rx_reference || !s->phy_off ||
	    !s->phy_exited || !s->route_cleared || !s->memory_force_cleared ||
	    !s->icc_released || !s->gcc_released || !s->clocks_released ||
	    !s->runtime_released || host->gcc || host->clocks_on || host->phy_on ||
	    host->phy_initialized || host->runtime_held)
		return -EPERM;
	return 0;
}

static int x1_control_owner_stopped(void *context)
{
	struct x1_host *host = context;
	int ret = x1_namespace_owner_stopped(context);

	if (ret)
		return ret;
	if (!host->tb || host->tb != rcu_access_pointer(host->retained_tb) || !host->tb->ctl ||
	    !host->pcie.namespace_retired || !host->pcie.nvme_resources_retired ||
	    !host->pcie.pci_retire_attempted || host->pcie.pci_retire_error ||
	    !host->pcie.pci_retired || host->pcie.host_prepared ||
	    host->pcie.host_published || host->pcie.nvme_bound ||
	    !host->pcie.receiver.release_attempted || !host->pcie.receiver.released ||
	    host->pcie.receiver.release_error || host->pcie.receiver.ready ||
	    host->pcie.receiver.associated || host->pcie.receiver.domain_ready ||
	    host->pcie.receiver.target_reserved)
		return -EPERM;
	return 0;
}

static int x1_cm_owner_stopped(void *context)
{
	struct x1_host *host = context;
	const struct qcom_usb4_nhi_retire *c = &host->control_retire;
	int ret = x1_control_owner_stopped(context);

	if (ret)
		return ret;
	if (!c->attempted || !c->finished || c->error || !c->irq_released ||
	    !c->rings_detached || c->rings_freed != 2 || c->packets_freed != 10 ||
	    !c->pool_released || !host->qnhi.released || host->qnhi.irq_requested ||
	    !host->qnhi.nhi.going_away ||
	    !tb_ctl_x1_retired(host->tb->ctl, &host->qnhi.nhi))
		return -EPERM;
	return 0;
}

/* Device/bind/lifecycle locks serialize this old session. No Type-C or TB
 * lock may be held across work, RCU, device unregister or release completion.
 * A timeout retains the host/NHI/module: their lifetime includes late device
 * references and the final domain release callback's completion storage.
 */
static int x1_retire_domain(struct x1_host *host)
{
	struct tb *tb;
	int ret;

	lockdep_assert_held(&host->lifecycle_lock);
	if (host->domain_retire_attempted)
		return -EALREADY;
	ret = x1_cm_owner_stopped(host);
	if (ret)
		return ret;
	host->domain_retire_attempted = true;
	tb = host->tb;
	ret = tb_x1_retire_cm(tb, host->batch_uid, &host->cm_retire,
			      x1_cm_owner_stopped, host);
	if (ret)
		goto fail;
	mutex_lock(&host->lock);
	if (host->tb != tb || rcu_access_pointer(host->retained_tb) != tb) {
		mutex_unlock(&host->lock);
		ret = -ESTALE;
		goto fail;
	}
	rcu_assign_pointer(host->retained_tb, NULL);
	host->tb = NULL;
	host->domain_unpublished = true;
	mutex_unlock(&host->lock);
	synchronize_rcu();
	tb_domain_remove(tb);
	host->domain_unregistered = true;
	tb_domain_put(tb); /* extra reference acquired at the original CM hold */
	if (!wait_for_completion_timeout(&host->qnhi.nhi.domain_released,
					 msecs_to_jiffies(5000))) {
		ret = -ETIMEDOUT;
		goto fail;
	}
	host->domain_retired = true;
	return 0;
fail:
	host->domain_retire_error = ret;
	return ret;
}

static int x1_platform_owner_stopped(void *context)
{
	struct x1_host *host = context;
	int ret = x1_namespace_owner_stopped(host);

	if (ret)
		return ret;
	/* domain_retired is latched only after the successful completion wait,
	 * under lifecycle_lock. That wait consumes complete()'s single token;
	 * completion_done() afterwards is false, not evidence of failed release.
	 * A timeout never sets this receipt, even if release completes later.
	 */
	if (!host->domain_retired || host->domain_retire_error ||
	    !host->domain_unregistered || !host->domain_unpublished ||
	    host->tb || rcu_access_pointer(host->retained_tb) ||
	    !host->cm_retire.finished || host->cm_retire.error ||
	    !host->control_retire.finished || host->control_retire.error ||
	    !host->qnhi.released || host->qnhi.irq_requested ||
	    host->qnhi.accept_rearm || !host->qnhi.nhi.going_away)
		return -EPERM;
	return 0;
}

static int x1_retire_session(struct x1_host *host)
{
	struct tb_nhi *nhi = &host->qnhi.nhi;
	int ret = x1_platform_owner_stopped(host);

	if (ret)
		return ret;
	ret = qcom_usb4_x1_retire_platform(host->dev, &host->pcie,
					  x1_platform_owner_stopped, host);
	if (ret)
		return ret;
	/* Final domain release completed: no late ctl/domain callback can see
	 * these ownership arrays or the session's embedded NHI anymore. */
	devm_kfree(host->dev, nhi->tx_rings);
	devm_kfree(host->dev, nhi->rx_rings);
	nhi->tx_rings = NULL;
	nhi->rx_rings = NULL;
	mutex_destroy(&host->qnhi.ownership_lock);
	host->fully_retired = true;
	return 0;
}

static ssize_t power_quiesce_once_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	bool retire = false;
	int ret = -EPERM;

	if (!capable(CAP_SYS_ADMIN) || !x1_power_quiesce || !x1_host_quiesce ||
	    !x1_tunnel_quiesce || !sysfs_streq(buf, "stop-power-retain-v1"))
		return ret;
	if (!device_trylock(dev))
		return -EBUSY;
	mutex_lock(&host->bind_lock);
	mutex_lock(&host->lifecycle_lock);
	mutex_lock(&host->lock);
	if (host->power_stop.attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (!host->tb || !host->tunnel_held || !host->tunnel_stop_complete ||
	    host->tunnel_stop_error || !host->tunnel_stop.finished ||
	    host->tunnel_stop.error || host->tunnel_stop.adapters != 2 ||
	    host->tunnel_stop.hops != 4 || !host->batch_complete ||
	    !host->batch_terminal || host->batch_waiting || host->batch_error ||
	    host->error != -ESHUTDOWN || host->orientation == TYPEC_ORIENTATION_NONE ||
	    host->orientation != host->batch_orientation || !host->connected_command ||
	    host->pending_command != host->connected_command)
		goto out;
	mutex_lock(&host->tb->lock);
	ret = tb_x1_pcie_stopped(host->tb, host->batch_uid);
	if (!ret)
		ret = x1_power_stop_run(host);
	/* Cached identity/generation only, no transaction through reset hardware. */
	if (!ret)
		ret = tb_x1_pcie_stopped(host->tb, host->batch_uid);
	if (ret && host->power_stop.attempted) {
		host->power_stop.error = ret;
		host->power_stop.finished = false;
	}
	mutex_unlock(&host->tb->lock);
	retire = !ret && x1_namespace_retire;
	dev_info(dev, "retained power stop result=%d complete=%u stage=%s resets=%u votes_released=%u dma_retained=1 physical_eject=0 reconnect=0\n",
		 ret, host->power_stop.finished, host->power_stop.stage ?: "none",
		 host->power_stop.resets_asserted, host->power_stop.runtime_released);
out:
	mutex_unlock(&host->lock);
	/* Namespace/core removal can wait for work and acquire disk locks. Do
	 * not hold the short Type-C lock or the native CM lock over that path.
	 */
	if (retire) {
		ret = qcom_usb4_x1_retire_namespaces(dev, &host->pcie,
						   x1_namespace_owner_stopped, host);
		dev_info(dev, "namespace retirement result=%d removed=%u controller_dma_retained=1 reconnect=0\n",
			 ret, host->pcie.namespace_retired);
		if (!ret && x1_pci_retire) {
			ret = qcom_usb4_x1_retire_pci(dev, &host->pcie,
						    x1_namespace_owner_stopped, host);
			dev_info(dev, "PCI retirement result=%d nvme_resources_retired=%u pci_removed=%u receiver_released=%u platform_retained=1 reconnect=0\n",
				 ret, host->pcie.nvme_resources_retired, host->pcie.pci_retired,
				 host->pcie.receiver.released);
			if (!ret && x1_control_retire) {
				ret = tb_ctl_retire_x1_stopped(host->tb->ctl, &host->qnhi,
						&host->host_stop, &host->control_retire,
						x1_control_owner_stopped, host);
				dev_info(dev, "control retirement result=%d irq_released=%u rings_freed=%u packets_freed=%u complete=%u cm_retained=1 reconnect=0\n",
					 ret, host->control_retire.irq_released,
					 host->control_retire.rings_freed,
					 host->control_retire.packets_freed,
					 host->control_retire.finished);
				if (!ret && x1_cm_retire) {
					ret = x1_retire_domain(host);
					dev_info(dev, "CM/domain retirement result=%d cm_removed=%u domain_removed=%u released=%u platform_retained=1 reconnect=0\n",
						 ret, host->cm_retire.finished,
						 host->domain_unregistered, host->domain_retired);
					if (!ret && x1_managed)
						ret = x1_retire_session(host);
				}
			}
		}
	}
	mutex_unlock(&host->lifecycle_lock);
	mutex_unlock(&host->bind_lock);
	device_unlock(dev);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(power_quiesce_once);

static ssize_t power_quiesce_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	const struct x1_power_stop *s = &host->power_stop;
	ssize_t len;

	if (!mutex_trylock(&host->lifecycle_lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	len = sysfs_emit(buf,
		"busy=0 enabled=%u attempted=%u complete=%u error=%d stage=%s mcu_request_cleared=%u mcu_control=%08x host_control=%08x resets=%u reset_requests=%08x rx_reference=%u phy_off=%u phy_exited=%u route_cleared=%u memory_force_cleared=%u icc_released=%u gcc_released=%u clocks_released=%u runtime_released=%u dma_retained=%u physical_eject=0 reconnect=0 cached=1\n",
		x1_power_quiesce, s->attempted, s->finished, s->error, s->stage ?: "none",
		s->mcu_request_cleared, s->mcu_control, s->host_control,
		s->resets_asserted, s->reset_requests, s->rx_reference,
		s->phy_off, s->phy_exited, s->route_cleared, s->memory_force_cleared,
		s->icc_released, s->gcc_released, s->clocks_released, s->runtime_released,
		!(host->pcie.nvme_resources_retired && host->pcie.receiver.released &&
		  host->control_retire.finished));
	mutex_unlock(&host->lifecycle_lock);
	return len;
}
static DEVICE_ATTR_RO(power_quiesce_state);

static ssize_t retire_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	const struct x1_pcie_state *p = &host->pcie;
	const struct qcom_usb4_nhi_retire *c = &host->control_retire;
	const struct tb_x1_cm_retire *cm = &host->cm_retire;
	ssize_t len;

	if (!mutex_trylock(&host->lifecycle_lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	len = sysfs_emit(buf,
		"busy=0 namespace_removed=%u nvme_resources_retired=%u pci_attempted=%u pci_removed=%u pci_error=%d receiver_released=%u receiver_error=%d control_attempted=%u control_complete=%u control_error=%d control_stage=%s control_irq_released=%u rings_detached=%u rings_freed=%u packets_freed=%u pool_released=%u cm_attempted=%u cm_complete=%u cm_error=%d cm_stage=%s domain_unpublished=%u domain_unregistered=%u domain_retired=%u domain_error=%d platform_retained=%u fully_retired=%u cached=1\n",
		p->namespace_retired, p->nvme_resources_retired, p->pci_retire_attempted,
		p->pci_retired, p->pci_retire_error, p->receiver.released,
		p->receiver.release_error, c->attempted, c->finished, c->error,
		c->stage ?: "none", c->irq_released, c->rings_detached,
		c->rings_freed, c->packets_freed, c->pool_released,
		cm->attempted, cm->finished, cm->error, cm->stage ?: "none",
		host->domain_unpublished, host->domain_unregistered,
		host->domain_retired, host->domain_retire_error,
		!p->platform_retired, host->fully_retired);
	mutex_unlock(&host->lifecycle_lock);
	return len;
}
static DEVICE_ATTR_RO(retire_state);

static ssize_t pcie_prepare_state_show(struct device *dev, struct device_attribute *attr,
				       char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	struct x1_pcie_state *state = &host->pcie;
	ssize_t len;

	if (READ_ONCE(host->batch_attempted))
		return inventory_state_show(dev, attr, buf);
	if (!mutex_trylock(&host->lifecycle_lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	len = sysfs_emit(buf, "busy=0 attempted=%u prepared=%u error=%d step=%s id=%08x command_status=%08x class_revision=%08x header=%08x pcie_cap=%08x tunnel=disabled scan=disabled dma=disabled msi=unproven cold_shutdown_required=%u\n",
		state->attempted, state->prepared, state->error, state->step ?: "none",
		state->root_id, state->command_status, state->class_revision,
		state->header, state->pcie_cap, state->attempted);
	mutex_unlock(&host->lifecycle_lock);
	return len;
}
static DEVICE_ATTR_RO(pcie_prepare_state);

static ssize_t pcie_inspect_state_show(struct device *dev, struct device_attribute *attr,
				      char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	struct x1_pcie_state *s = &host->pcie;
	ssize_t len;

	if (!mutex_trylock(&host->lifecycle_lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	len = sysfs_emit(buf, "busy=0 requested=%u attempted=%u complete=%u reads=%u cached=1 dwc_version=%08x dwc_type=%08x atu_viewport=%08x parf_000=%08x parf_174=%08x parf_1a8=%08x parf_1b0=%08x parf_350=%08x parf_354=%08x parf_634=%08x parf_638=%08x parf_358=%08x parf_35c=%08x decoder=unproven msi=unproven\n",
		s->inspect_requested, s->inspect_attempted, s->inspect_complete,
		s->inspect_reads, s->inspect_root[0], s->inspect_root[1], s->inspect_root[2],
		s->inspect_parf[0], s->inspect_parf[1], s->inspect_parf[2], s->inspect_parf[3],
		s->inspect_parf[4], s->inspect_parf[5], s->inspect_parf[6], s->inspect_parf[7],
		s->inspect_parf[8], s->inspect_parf[9]);
	mutex_unlock(&host->lifecycle_lock);
	return len;
}
static DEVICE_ATTR_RO(pcie_inspect_state);

static ssize_t pcie_cfg0_state_show(struct device *dev, struct device_attribute *attr,
				   char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	struct x1_pcie_state *s = &host->pcie;
	ssize_t len;

	if (READ_ONCE(host->batch_attempted))
		return inventory_state_show(dev, attr, buf);
	if (!mutex_trylock(&host->lifecycle_lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	len = sysfs_emit(buf, "busy=0 requested=%u attempted=%u decoder_prepared=%u decoder_writes=%u decoder_error=%d decoder_step=%s complete=%u writes=%u enable_issued=%u error=%d step=%s alignment=%08x upper_limit_mask=%08x cached=1 config_access=never training=disabled tunnel=disabled dma=disabled msi=unproven\n",
		s->cfg0_requested, s->cfg0_attempted, s->decoder_prepared, s->decoder_writes,
		s->decoder_error, s->decoder_step ?: "none", s->cfg0_complete, s->cfg0_writes,
		s->cfg0_enable_issued, s->cfg0_error, s->cfg0_step ?: "none",
		s->cfg0_alignment, s->cfg0_upper_limit_mask);
	mutex_unlock(&host->lifecycle_lock);
	return len;
}
static DEVICE_ATTR_RO(pcie_cfg0_state);

static ssize_t pcie_decoder_state_show(struct device *dev, struct device_attribute *attr,
				      char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	struct x1_pcie_state *s = &host->pcie;
	ssize_t len;

	if (!mutex_trylock(&host->lifecycle_lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	len = sysfs_emit(buf, "busy=0 attempted=%u error=%d step=%s check=%s valid=%u region=%s offset=%08x value=%08x writes=%u write_issued=%u write_returned=%u write_readback=%u write_offset=%08x write_value=%08x cached=1 retry=disabled\n",
		s->cfg0_attempted, s->decoder_error, s->decoder_step ?: "none",
		s->decoder_check ?: "none", s->decoder_valid, s->decoder_region ?: "none",
		s->decoder_offset, s->decoder_value, s->decoder_writes,
		s->decoder_write_issued, s->decoder_write_returned, s->decoder_write_readback,
		s->decoder_write_offset, s->decoder_write_value);
	mutex_unlock(&host->lifecycle_lock);
	return len;
}
static DEVICE_ATTR_RO(pcie_decoder_state);

/* Copy immutable physical provider handles only. No DMA, device, CM, proof,
 * consumed flag, firmware allocation or protocol state crosses generations. */
static void x1_session_providers(struct x1_host *next, const struct x1_host *old)
{
	next->router = old->router;
	next->mcu = old->mcu;
	next->tcsr = old->tcsr;
	next->phy = old->phy;
	memcpy(next->clocks, old->clocks, sizeof(next->clocks));
	memcpy(next->resets, old->resets, sizeof(next->resets));
	next->pinctrl = old->pinctrl;
	memcpy(next->pins, old->pins, sizeof(next->pins));
	next->ddr = old->ddr;
	next->apps = old->apps;
	next->serial = old->serial;
	next->irq = old->irq;
}

static ssize_t session_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	struct x1_port *port = host->port;
	unsigned long flags;
	u64 events, disconnected;
	unsigned int orientation;
	ssize_t len;

	if (!mutex_trylock(&host->lifecycle_lock))
		return sysfs_emit(buf, "busy=1 snapshot=unavailable\n");
	spin_lock_irqsave(&port->event_lock, flags);
	events = port->event_sequence;
	disconnected = port->disconnect_sequence;
	orientation = port->orientation;
	spin_unlock_irqrestore(&port->event_lock, flags);
	len = sysfs_emit(buf, "busy=0 generation=%llu initial_event=%llu events=%llu disconnect_event=%llu orientation=%u managed=%u pristine=%u prepared=%u waiting=%u complete=%u fully_retired=%u platform_retired=%u platform_error=%d cached=1\n",
		host->generation, host->initial_event_sequence, events, disconnected,
		orientation, x1_managed, !host->startup_attempted,
		host->pcie.prepared, host->batch_waiting, host->batch_complete,
		host->fully_retired, host->pcie.platform_retired,
		host->pcie.platform_retire_error);
	mutex_unlock(&host->lifecycle_lock);
	return len;
}
static DEVICE_ATTR_RO(session_state);

static ssize_t renew_session_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct x1_host *old __free(x1_session) = x1_session_get(dev);
	struct x1_port *port = old->port;
	struct x1_host *next = NULL;
	unsigned long flags;
	u64 generation;
	bool published = false;
	int ret;

	if (!capable(CAP_SYS_ADMIN) || !x1_managed)
		return -EPERM;
	ret = kstrtou64(buf, 10, &generation);
	if (ret)
		return ret;
	if (!device_trylock(dev))
		return -EBUSY;
	mutex_lock(&port->generation_lock);
	mutex_lock(&old->bind_lock);
	mutex_lock(&old->lifecycle_lock);
	ret = -EPERM;
	if (rcu_access_pointer(port->session) != old || generation != old->generation ||
	    !old->fully_retired || !old->pcie.platform_retired ||
	    old->pcie.platform_retire_error || x1_platform_owner_stopped(old) ||
	    port->next_generation == U64_MAX)
		goto out;
	next = x1_session_alloc(port);
	if (!next) {
		ret = -ENOMEM;
		goto out;
	}
	x1_session_providers(next, old);
	spin_lock_irqsave(&port->event_lock, flags);
	if (port->orientation != TYPEC_ORIENTATION_NONE ||
	    port->disconnect_sequence <= old->initial_event_sequence) {
		ret = -EBUSY;
	} else {
		next->initial_event_sequence = port->event_sequence;
		rcu_assign_pointer(port->session, next);
		published = true;
		ret = 0;
	}
	spin_unlock_irqrestore(&port->event_lock, flags);
out:
	mutex_unlock(&old->lifecycle_lock);
	mutex_unlock(&old->bind_lock);
	if (published) {
		synchronize_rcu();
		x1_session_put(old); /* old port-owner ref; caller still pins it */
	} else {
		x1_session_put(next);
	}
	mutex_unlock(&port->generation_lock);
	device_unlock(dev);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(renew_session);

static struct attribute *x1_attrs[] = {
	&dev_attr_session_state.attr, &dev_attr_renew_session.attr,
	&dev_attr_startup_state.attr, &dev_attr_activate_once.attr,
	&dev_attr_pcie_prepare_once.attr, &dev_attr_pcie_prepare_state.attr,
	&dev_attr_pcie_inspect_state.attr, &dev_attr_pcie_cfg0_state.attr,
	&dev_attr_pcie_decoder_state.attr,
	&dev_attr_inventory_once.attr, &dev_attr_inventory_state.attr,
	&dev_attr_tunnel_quiesce_once.attr, &dev_attr_tunnel_quiesce_state.attr,
	&dev_attr_host_quiesce_once.attr, &dev_attr_host_quiesce_state.attr,
	&dev_attr_power_quiesce_once.attr, &dev_attr_power_quiesce_state.attr,
	&dev_attr_retire_state.attr, NULL,
};
ATTRIBUTE_GROUPS(x1);

static void x1_port_release(void *data)
{
	struct x1_port *port = data;
	struct x1_host *host = rcu_access_pointer(port->session);

	rcu_assign_pointer(port->session, NULL);
	synchronize_rcu();
	/* A module lease prevents normal unbind once startup is admitted. Never
	 * free an uncertain live session if an exceptional removal reaches here.
	 */
	if (host && (!host->startup_attempted || host->fully_retired))
		x1_session_put(host);
	mutex_destroy(&port->generation_lock);
}

static int x1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct x1_port *port;
	struct x1_host *host;
	int ret;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	port->dev = dev;
	mutex_init(&port->generation_lock);
	spin_lock_init(&port->event_lock);
	platform_set_drvdata(pdev, port);
	host = x1_session_alloc(port);
	if (!host)
		return -ENOMEM;
	rcu_assign_pointer(port->session, host);
	ret = devm_add_action_or_reset(dev, x1_port_release, port);
	if (ret)
		return ret;
	ret = x1_resources(host, pdev);
	if (ret)
		return dev_err_probe(dev, ret, "resource/identity check\n");
	ret = x1_typec_register(host);
	if (ret)
		return dev_err_probe(dev, ret, "Type-C provider registration\n");
	if (!x1_startup) {
		dev_info(dev, "X1 frontend bound, startup disabled; no controller access\n");
		return 0;
	}
	mutex_lock(&host->lifecycle_lock);
	x1_activate(host);
	mutex_unlock(&host->lifecycle_lock);
	/* Keep display providers bound after any failed startup attempt. */
	return 0;
}

static void x1_shutdown(struct platform_device *pdev)
{
	struct x1_host *host __free(x1_session) = x1_session_get(&pdev->dev);
	int ret;

	if (!host)
		return;
	mutex_lock(&host->bind_lock);
	mutex_lock(&host->lifecycle_lock);
	if (host->stopping)
		goto out;
	mutex_lock(&host->lock);
	host->stopping = true;
	host->ready = false;
	mutex_unlock(&host->lock);
	if (!host->activated)
		goto firmware;
	if (!host->cold_owned) {
		/* Failed admission never authorizes stopping an inherited owner. */
		x1_release_power(host);
		goto firmware;
	}
	if (host->pcie.nvme_requested && host->pcie.host_attempted) {
		/* A bind/setup timeout does not establish that endpoint DMA stopped.
		 * Keep the entire tunnel, NHI, MCU and provider lease until power-off;
		 * never invalidate a possible DMA path through frontend teardown.
		 */
		dev_warn(host->dev, "private PCI0 read test consumed; retaining complete tunnel until cold power-off\n");
		goto firmware;
	}
	/* Stop/drain control rings while all hardware and providers stay live. */
	if (host->tb) {
		tb_domain_remove(host->tb);
		wait_for_completion(&host->qnhi.nhi.domain_released);
		host->tb = NULL;
	}
	if (host->qnhi.prepared)
		qcom_usb4_nhi_shutdown(&host->qnhi.nhi);
	qcom_usb4_mcu_stop(&host->mcu);
	if (host->pcie.attempted) {
		/* PCI0 mode/reset was an isolated experiment. Do not improvise a
		 * shared PIPE/PHY teardown; all provider references remain held
		 * until the machine is fully powered off. No endpoint DMA exists.
		 */
		dev_warn(host->dev, "PCI0 mode test consumed; retaining power, full cold shutdown required\n");
		goto firmware;
	}
	ret = reset_control_bulk_assert(ARRAY_SIZE(host->resets), host->resets);
	if (ret) {
		dev_err(host->dev, "shutdown reset failed: %d; retaining power, cold power-off required\n", ret);
		goto out;
	}
	if (host->phy_on) {
		ret = phy_power_off(host->phy);
		if (ret)
			goto out;
		host->phy_on = false;
	}
	if (host->phy_initialized) {
		ret = phy_exit(host->phy);
		if (ret)
			goto out;
		host->phy_initialized = false;
	}
	x1_release_power(host);
firmware:
	if (host->fw) {
		release_firmware(host->fw);
		host->fw = NULL;
	}
out:
	mutex_unlock(&host->lifecycle_lock);
	mutex_unlock(&host->bind_lock);
}

static int x1_suspend(struct device *dev)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);
	int ret;

	mutex_lock(&host->lifecycle_lock);
	ret = host->startup_attempted ? -EBUSY : 0;
	if (!ret)
		host->suspended = true;
	mutex_unlock(&host->lifecycle_lock);
	return ret;
}

static void x1_pm_complete(struct device *dev)
{
	struct x1_host *host __free(x1_session) = x1_session_get(dev);

	mutex_lock(&host->lifecycle_lock);
	host->suspended = false;
	mutex_unlock(&host->lifecycle_lock);
}

static const struct dev_pm_ops x1_pm_ops = {
	/* Block activation throughout prepare -> complete, not just sleep. */
	.prepare = x1_suspend, .complete = x1_pm_complete,
	.suspend = x1_suspend, .freeze = x1_suspend, .poweroff = x1_suspend,
};
static const struct of_device_id x1_match[] = {
	{ .compatible = "qcom,x1e80100-usb4" }, { }
};
MODULE_DEVICE_TABLE(of, x1_match);
MODULE_FIRMWARE(X1_FIRMWARE);

struct platform_driver qcom_usb4_x1_driver = {
	.probe = x1_probe,
	.remove = x1_shutdown,
	.shutdown = x1_shutdown,
	.driver = {
		.name = "qcom-usb4-x1",
		.of_match_table = x1_match,
		.dev_groups = x1_groups,
		.pm = &x1_pm_ops,
		.suppress_bind_attrs = true,
	},
};
