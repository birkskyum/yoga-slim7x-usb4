/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_USB4_DROM_H
#define QCOM_USB4_DROM_H

#include <linux/types.h>

#define QCOM_USB4_DROM_SIZE 116

/*
 * Build the descriptor format observed on the Surface Glymur router.
 * serial must come from validated SoC information, not a guessed board ID.
 * router is the hardware router index (0..2). This helper does not determine
 * whether the descriptor's adapter topology applies to another chip.
 * On error, data is unchanged. No hardware access is performed.
 */
int qcom_usb4_drom_build(u8 *data, size_t size, u32 serial, u8 router);

#endif
