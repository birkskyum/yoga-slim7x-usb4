/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_USB4_FW_H
#define QCOM_USB4_FW_H

#include <linux/types.h>

/*
 * Offsets are relative to MCU RAM, not to the router's MMIO resource.
 * The callback returns zero on success or a negative errno on failure.
 * Keep the image bytes unchanged throughout upload, including callbacks.
 * Transport failure stops further writes but may leave a partial image;
 * the caller must keep the MCU stopped and perform its recovery sequence.
 */
typedef int (*qcom_usb4_fw_write_word_t)(void *context, u32 offset, u32 value);

int qcom_usb4_fw_validate(const u8 *data, size_t size, size_t ram_size);
int qcom_usb4_fw_upload(const u8 *data, size_t size, size_t ram_size,
		       qcom_usb4_fw_write_word_t write_word, void *context);

#endif
