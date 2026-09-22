/* SPDX-License-Identifier: GPL-2.0-only */
/* PRIVATE, dormant first-test policies. No active caller or hardware result. */
#ifndef LOCAL_X1_IMSI_POLICY_H
#define LOCAL_X1_IMSI_POLICY_H

#include <linux/types.h>

struct device;
struct platform_device;
struct pci_host_bridge;

struct x1_imsi_policy {
	struct device *owner;
	struct platform_device *pdev;
	struct pci_host_bridge *bridge;
	void __iomem *parf;
	void *context;
	/* Same retained lifetime and caller locks as x1-imsi.h. Must verify the
	 * exact live owner/tunnel and disabled endpoint MSI/MSI-X on each call.
	 * Receiver initialization has already checked all eight masked banks.
	 */
	int (*check_live)(void *context);
	bool global_attempted, global_ready, target_attempted, target_ready;
	bool mask_write_issued, mask_write_returned;
	u32 mask_before, mask_after, status_before, status_after;
	u64 target;
	unsigned int memory_windows;
	int global_error, target_error;
};

/* Caller serializes these callbacks; do not clear receipts or replace context
 * after an attempt. The receiver's global one-shot latch is authoritative.
 * No IRQ request/enable, status clear, rollback or retry is provided here.
 */
int x1_imsi_policy_global(void *context, int global_irq);
int x1_imsi_policy_target(void *context, u64 target);

#endif
