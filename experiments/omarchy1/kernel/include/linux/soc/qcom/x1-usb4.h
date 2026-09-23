/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_QCOM_X1_USB4_H
#define __LINUX_SOC_QCOM_X1_USB4_H

#include <linux/types.h>

struct device;
struct x1_gcc_usb4;

/*
 * Exclusive router-0 development lease. The consumer must reserve the port's
 * USB3/DP users and hold its power/clocks before applying startup operations.
 * get() acquires software handles only. No register or clock writes occur.
 * A failed startup operation can have partially changed hardware; do not
 * automatically retry or call put() until controller/PHY shutdown is safe.
 * These operations do not claim PCIe, IRQ, firmware or storage ownership.
 */
struct x1_gcc_usb4 *x1_gcc_usb4_get(struct device *consumer, unsigned int port);
void x1_gcc_usb4_put(struct x1_gcc_usb4 *lease);
int x1_gcc_usb4_rx_select(struct x1_gcc_usb4 *lease, bool phy);
int x1_gcc_usb4_sys_ready(struct x1_gcc_usb4 *lease, unsigned int timeout_us);
int x1_gcc_usb4_sys_force_mem(struct x1_gcc_usb4 *lease, bool on);
int x1_gcc_usb4_pipe_hwcg(struct x1_gcc_usb4 *lease, bool enable);
/* GCC +0x5001c bit0: NOT the existing +0x5000c PHY reset. */
int x1_gcc_usb4_ctrl_bit(struct x1_gcc_usb4 *lease, bool set);
/* Readback of router-0's eleven owned MISC reset request bits, not DMA idle. */
int x1_gcc_usb4_reset_state(struct x1_gcc_usb4 *lease, u32 *state);
/* 0 only if gcc_usb4_0_gdsc reports powered up; -ENODEV if it is off. */
int x1_gcc_usb4_power_on(struct x1_gcc_usb4 *lease);

#endif
