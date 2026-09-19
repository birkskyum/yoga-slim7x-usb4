/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_USB4_MCU_H
#define QCOM_USB4_MCU_H

#include <linux/io.h>
#include <linux/types.h>

enum qcom_usb4_mcu_layout {
	QCOM_USB4_MCU_LEGACY,
	QCOM_USB4_MCU_V2,
};

struct qcom_usb4_mcu {
	void __iomem *base;
	size_t ram_size;
	u32 ram_offset;
	u32 control_offset;
	u32 shared_offset_reg;
	u32 drom_offset;
};

/*
 * The caller must verify the router variant and usable RAM size, own the
 * mapping, serialize all operations, and maintain power/clocks throughout.
 * The register mapping base must be aligned to four bytes.
 * Initializing this helper does not touch hardware. It is not a power/reset
 * sequence or a platform driver. Do not modify the fields after initialization.
 */
int qcom_usb4_mcu_init(struct qcom_usb4_mcu *mcu, void __iomem *base,
		       size_t mapping_size, size_t ram_size,
		       enum qcom_usb4_mcu_layout layout);

/*
 * Separate stopped-state operations for platform sequencing. Upload validates
 * the entire image before zeroing control and writing firmware; it does not
 * write UID/DROM or start the MCU. A control readback follows successful
 * upload before returning. The owner can then perform the required
 * provider memory-force and board-extension steps before writing UID/DROM.
 * Both operations reject running firmware without writes. DROM input/bounds
 * validation happens before any MMIO. Neither operation establishes power,
 * image identity, physical halt or successful completion of the other step.
 * Keep exclusive ownership across the entire sequence, including provider work.
 */
int qcom_usb4_mcu_upload(struct qcom_usb4_mcu *mcu, const u8 *data, size_t size);
int qcom_usb4_mcu_write_drom(struct qcom_usb4_mcu *mcu, u32 serial, u8 router);

/*
 * Upload a caller-selected immutable image and generated DROM while stopped.
 * serial and router must be validated hardware identity, as for the DROM
 * builder. Malformed input or running firmware causes no hardware writes.
 * This convenience operation provides no intervening provider step. The Surface
 * platform sequence requires upload and write_drom separately, with memory
 * force and the board extension between them. This does not select an image
 * or establish the board's power/reset state.
 */
int qcom_usb4_mcu_load(struct qcom_usb4_mcu *mcu, const u8 *data, size_t size,
		       u32 serial, u8 router);

/*
 * Start requires a fully uploaded firmware image and DROM. Already running
 * firmware is rejected without modifying hardware. On failure after starting,
 * stop the MCU and leave shared_offset zero. Success validates only the first
 * 12 shared bytes; further accesses require their own bounds checks.
 */
int qcom_usb4_mcu_start(struct qcom_usb4_mcu *mcu, u32 *shared_offset);
void qcom_usb4_mcu_stop(struct qcom_usb4_mcu *mcu);

/*
 * Send a caller-validated, nonzero command to running, ready firmware.
 * Requires sleepable context and exclusive command/lifecycle ownership.
 * timeout_us must be in 1..1000000; it is caller policy, not a discovered
 * hardware limit. Success means only that firmware cleared the mailbox.
 * On timeout the command may still be executing: do not retry blindly. This
 * helper neither clears a pending command nor resets/stops firmware.
 */
int qcom_usb4_mcu_command(struct qcom_usb4_mcu *mcu, u32 command,
			 unsigned int timeout_us);

#endif
