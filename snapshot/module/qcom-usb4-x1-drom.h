/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_USB4_X1_DROM_H
#define QCOM_USB4_X1_DROM_H
#include <linux/types.h>

/*
 * Offline X1 reference-profile candidate. No board binding or hardware access.
 * serial must be a validated SoC serial before any eventual hardware use.
 * This descriptor does not establish PHY, firmware or retimer compatibility.
 */
int qcom_usb4_x1_drom_build(u8 *data, size_t size, u32 serial, u8 router);
#endif
