/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QCOM_USB4_X1_PCIE_H
#define _QCOM_USB4_X1_PCIE_H

#include <linux/io.h>
#include <linux/types.h>
#include "x1-imsi.h"
#include "x1-pci0-mem.h"

struct device;
struct x1_train_state;
struct x1_pcie_inventory;

struct x1_pcie_state {
	bool attempted, prepared;
	int error;
	u32 root_id, command_status, class_revision, header, pcie_cap;
	const char *step;
	/* Optional reads in the same admitted mode-only attempt, never afterward. */
	bool inspect_requested, inspect_attempted, inspect_complete;
	u32 inspect_reads;
	u32 inspect_root[3]; /* DWC VERSION, TYPE, ATU_VIEWPORT */
	u32 inspect_parf[10]; /* SYS_CTRL, MHI, WR_HALT, LTSSM, DBI/ATU/aperture */
	/* Combined local preparation only. No downstream configuration access. */
	bool cfg0_requested, cfg0_attempted, decoder_prepared, cfg0_complete;
	bool cfg0_enable_issued;
	u32 decoder_writes, cfg0_writes, cfg0_alignment, cfg0_upper_limit_mask;
	int decoder_error, cfg0_error;
	const char *decoder_step, *cfg0_step;
	/* Cached from existing decoder reads only; never refreshed by sysfs. */
	const char *decoder_check, *decoder_region;
	u32 decoder_offset, decoder_value, decoder_write_offset, decoder_write_value;
	/* write_returned means success; an error cannot prove no write arrived. */
	bool decoder_valid, decoder_write_issued, decoder_write_returned, decoder_write_readback;
	/* Separate batch resource profile; never set by the old preparation gate. */
	bool inventory_requested, inventory_attempted;
	/* Private receiver-only continuation. Bridge remains unpublished. */
	bool receiver_requested, bridge_allocated;
	struct x1_imsi_receipt receiver;
	u32 receiver_mask_before, receiver_mask_after;
	u32 receiver_status_before, receiver_status_after;
	/* Separately armed private read test; no namespace-write permission. */
	bool nvme_requested, host_attempted, host_published, host_prepared;
	bool nvme_bind_attempted, nvme_bound;
	/* Monotonic admission fence, serialized with every PCI config callback.
	 * A closed fence is not proof that DMA/IRQs or object lifetimes ended.
	 */
	bool config_fenced;
	bool namespace_retire_attempted, namespace_retired;
	int namespace_retire_error;
	bool pci_retire_attempted, nvme_resources_retired, pci_retired;
	int pci_retire_error;
	bool platform_retire_attempted, platform_reset, platform_retired;
	u32 platform_clocks_released;
	bool platform_icc_released, platform_runtime_released;
	int platform_retire_error;
	int host_error, cfg_error;
	u32 cfg_error_offset, cfg_error_value;
	u8 cfg_error_bus, cfg_error_size;
	bool cfg_error_write;
	const char *host_step;
	struct x1_mem_state mem;
};

/*
 * Development-only, one mode transition with an empty, nopcie router domain.
 * Caller holds device, lifecycle, Type-C and domain locks and pins the module.
 * After an attempt the caller must prohibit connection commands and retain
 * shared router/PHY/clock/reset resources until full cold power-off, even if
 * this function fails. This is not a host bridge or endpoint admission API.
 * inspect_requested must be set before entry and separately armed by caller;
 * it adds only local root/PARF reads after the same mode transition, no writes.
 * cfg0_requested also requires inspection and the 8 KiB root resource profile;
 * it prepares decoder/OB0 while keeping root command bits and LTSSM disabled.
 */
int qcom_usb4_x1_pcie_prepare(struct device *owner, void __iomem *router,
			    struct x1_pcie_state *state);

/* New batch only. These do not reopen a terminal old preparation attempt. */
int qcom_usb4_x1_batch_prepare(struct device *owner, void __iomem *router,
			     struct x1_pcie_state *state);
int qcom_usb4_x1_batch_inventory(struct device *owner, struct x1_pcie_state *state,
		int (*check_live)(void *), void *context, u16 vendor, u16 device,
		struct x1_train_state *training, struct x1_pcie_inventory *inventory);

/* After successful armed scan, caller freezes CM/Type-C control, retains all
 * NHI/router/PHY/PCI resources through shutdown, and releases ALL device,
 * lifecycle, Type-C and TB locks before calling this exactly once. Binding
 * enables normal DMA/IRQ setup; return 0 proves neither an IRQ nor a read.
 * No hotplug, recovery, teardown, module unload or namespace writes admitted.
 */
int qcom_usb4_x1_nvme_bind(struct device *owner, struct x1_pcie_state *state);

/* After kernel-owned endpoint-quiesce proof, retire endpoint IRQs while
 * hardware is live, close and synchronize ALL PCI configuration callbacks,
 * then stop receiver IRQs before the retained-only tunnel-stop action.
 * Fence remains closed even if the action fails. Caller serializes the
 * frontend lifecycle and holds Type-C/CM locks. No callback may reenter
 * PCI/NVMe or release/rearm any object. The fence itself samples no MMIO;
 * the ordered IRQ-retirement steps do access live endpoint/receiver hardware.
 */
int qcom_usb4_x1_fence_and_quiesce(struct device *owner, struct x1_pcie_state *state,
				 int (*action)(void *), void *context);

/* Namespace visibility removal only. Caller holds device/bind/lifecycle
 * locks, but NOT Type-C/CM locks. The callback checks the kernel-owned stop
 * receipts without transport access or acquiring these locks in reverse.
 * Controller/PCI/MSI/NHI allocations remain owned after this operation.
 */
int qcom_usb4_x1_retire_namespaces(struct device *owner, struct x1_pcie_state *state,
				 int (*check_stopped)(void *), void *context);

/* After namespace retirement: free proven-stopped NVMe resources while its
 * PCI DMA configuration is still alive, then remove the exact old PCI bus.
 * Holds the PCI rescan/remove lock; caller must not hold Type-C/CM locks.
 * Then release the empty receiver domain and its isolated target devres group.
 * Bridge/context allocations and platform providers remain retained. Success
 * does not admit re-probe or release those remaining owners.
 */
int qcom_usb4_x1_retire_pci(struct device *owner, struct x1_pcie_state *state,
			  int (*check_stopped)(void *), void *context);
/* Final exact-owner provider/context retirement, after domain release. */
int qcom_usb4_x1_retire_platform(struct device *owner, struct x1_pcie_state *state,
			       int (*check_stopped)(void *), void *context);

#endif
