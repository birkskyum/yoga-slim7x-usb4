/* SPDX-License-Identifier: GPL-2.0-only */
/* PRIVATE bring-up adapter. Dev19 retirement changes are not hardware-tested. */
#ifndef LOCAL_X1_IMSI_H
#define LOCAL_X1_IMSI_H

#include <linux/types.h>

struct device;
struct platform_device;
struct pci_host_bridge;
struct x1_imsi;

struct x1_imsi_request {
	struct device *owner;
	struct platform_device *pdev;
	struct pci_host_bridge *bridge;
	void __iomem *dbi;
	void __iomem *parf;
	void *context;
	/*
	 * Caller holds its device/lifecycle/Type-C/tb locks. This validates the
	 * exact retained owner, tunnel/inventory receipt and disabled endpoint
	 * MSI/MSI-X before each setup boundary. It MUST NOT permit driver binding
	 * or endpoint bus mastering merely because receiver setup succeeds.
	 */
	int (*check_live)(void *context);
	/*
	 * Commit a NONREVOCABLE cold-boot lease before any receiver MMIO:
	 * caller module, both device/driver lifetimes, mappings, runtime power,
	 * clocks and firmware ownership. No unbind/remove/suspend until cold off.
	 * A successful callback is retained on every subsequent error. A normal
	 * get_device() reference alone does NOT retain devres or driver binding.
	 */
	int (*retain)(void *context);
	/*
	 * REQUIRED future integration policy, not implemented by this adapter.
	 * Resolve whether the supplied separate global parent needs PARF routing
	 * and/or a handler on THIS USB4 controller. Return -EOPNOTSUPP if unknown.
	 * Called after receiver banks are masked/programmed, before association.
	 * Any requested handler must retain context/power and never dereference an
	 * unpublished bus. NULL refuses before any receiver MMIO. Normal Qualcomm
	 * internal-host global mask programming is not proof for USB4 PCI0.
	 */
	int (*global_route)(void *context, int global_irq);
	/* Check the reserved DMA API address against this host's future BAR/bus
	 * windows and target-address policy. This is not an endpoint DMA check.
	 * NULL refuses before MMIO. Address zero is not inherently invalid.
	 */
	int (*target_check)(void *context, u64 target);
};

struct x1_imsi_stop {
	bool attempted, finished;
	u32 banks_disabled, parents_detached;
	int error;
};

struct x1_imsi_receipt {
	bool attempted, retained, domain_ready, target_reserved, global_policy_complete;
	bool associated, ready;
	u32 banks, vectors;
	u64 target;
	int error;
	const char *step;
	struct x1_imsi_stop stop;
	bool release_attempted, released;
	int release_error;
};

/*
 * One owned context at a time. *out is non-NULL once context allocation
 * succeeds. No retry/reset API is provided. Only a fully stopped, depopulated
 * bus may use the checked release and context-retirement paths below. A new
 * allocation is admitted only after the old owner's complete retirement.
 * An allocation/init failure is not permission to free unknown live state.
 */
int x1_imsi_prepare(const struct x1_imsi_request *request, struct x1_imsi **out);
void x1_imsi_cached(const struct x1_imsi *receiver, struct x1_imsi_receipt *out);
/* After endpoint IRQ handlers/vectors are freed, while DBI is still powered:
 * mask/disable banks, disable/synchronize/detach parent handlers. Keeps the
 * MSI domain, reserved target and host/device allocations. No rearm or retry.
 * Caller holds the PCI/NVMe quiesce proof and has closed PCI configuration.
 */
int x1_imsi_stop_retained(struct x1_imsi *receiver);
/* After checked power stop and complete PCI bus/child-domain removal: remove
 * the empty MSI domain and release only the DWC target's devres group. Does
 * not release platform/context/module ownership or permit another prepare.
 * check_stopped must confirm the exact owner and retired PCI generation.
 */
int x1_imsi_release_stopped(struct x1_imsi *receiver,
			  int (*check_stopped)(void *), void *context);
int x1_imsi_retire_context(struct x1_imsi *receiver,
			   int (*check_stopped)(void *), void *context);

#endif
