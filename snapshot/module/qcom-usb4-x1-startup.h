/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_USB4_X1_STARTUP_H
#define QCOM_USB4_X1_STARTUP_H

#include <linux/types.h>

/* Private, unbound Yoga/X1 router-0 experiment. Not a DT ABI or host driver. */
enum x1_step {
	X1_NONE, X1_VALIDATE, X1_OWNER,
	X1_PHY_PREPARE,
	X1_RX_PREPARE, X1_TXB_PREPARE, X1_TXA_PREPARE,
	X1_RX_CLOCK_REFERENCE, X1_MISC_RESET_ASSERT, X1_MISC_ASSERT_DELAY,
	X1_EXTRA_RESET_ASSERT, X1_DP_RESET_ASSERT, X1_RESET_ASSERT_DELAY,
	X1_DP_RESET_CLEAR, X1_EXTRA_RESET_CLEAR, X1_RESET_RELEASE_DELAY,
	X1_MISC_RESET_CLEAR, X1_SYS_READY, X1_RX_CLOCK_PHY, X1_CLEAR_1200C,
	X1_RX_ACTIVE, X1_TXB_ACTIVE, X1_TXA_ACTIVE,
	X1_SET_81000, X1_PIPE_RESET_ASSERT, X1_PIPE_ASSERT_DELAY,
	X1_PIPE_ASSERT_SETTLE, X1_SET_81018, X1_PIPE_RESET_CLEAR,
	X1_PIPE_RELEASE_DELAY, X1_PIPE_RELEASE_SETTLE, X1_CLEAR_81000,
	X1_UPLOAD, X1_SYS_MEMORY_FORCE, X1_TCSR_ROUTE, X1_PIPE_HWCG_OFF,
	X1_SET_81004, X1_TIMER_HIGH, X1_TIMER_LOW, X1_TIMER_8148,
	X1_TIMER_8144, X1_DROM, X1_PHY_COMPOSITE,
	X1_ADAPTER_23, X1_ADAPTER_22, X1_ADAPTER_21,
	X1_CLEAR_D064_6, X1_MCU_START, X1_PRESET,
};

struct x1_config {
	const u8 *firmware;
	size_t firmware_size;
	u32 serial;
	u32 router;
	u32 phy_rx_eq;
	u32 mcu_preset;
	/* Explicit experimental Linux delay, not a recovered Windows value. */
	u32 reset_release_us;
};

enum x1_phase { X1_NEW, X1_RUNNING, X1_FAILED, X1_COMPLETE };
struct x1_state {
	enum x1_phase phase;
	enum x1_step active;
	enum x1_step completed;
	int error;
};

/*
 * No real provider is bound to this sequence yet. The owner MUST establish:
 * exact Yoga RAM-only image, pinned X1 firmware, captured empty PHYC,
 * powered/exclusive router-0, no competing PHY/DP/Type-C users, stopped MCU,
 * and held provider/resource references for the entire attempt and recovery.
 * Configuration and firmware must remain immutable throughout the call.
 *
 * step() implements only provider operations and these bounded MCU helpers:
 * UPLOAD: unchanged validated X1 bytes, MCU held stopped;
 * DROM: X1 descriptor, verified serial, validated RAM bounds;
 * MCU_START: finite readiness poll and checked shared-RAM pointer;
 * PRESET: 0x0c63, finite mailbox poll. Never a connect command.
 *
 * PHY_PREPARE owns the early PHY power write. PHY_COMPOSITE owns common
 * setup, port-0 DP producer/AUX, tables, BIOS RX override and PCS start.
 * EXTRA_RESET is GCC +0x5001c bit 0, NOT the +0x5000c PHY reset.
 * MISC_RESET is +0xad0f8 bits 0..10; PIPE_RESET is its bit 3.
 * TCSR_ROUTE is +0x1b000 bit 0. SYS_READY must fail on a finite timeout.
 * These belong in owning Linux providers, not host raw GCC/TLMM mappings.
 *
 * read/update access only the expanded router aperture. update must preserve
 * bits outside mask. step/read/update return zero or negative errno. Positive
 * status is a protocol error. delay_us honors the minimum before returning.
 */
struct x1_ops {
	int (*owner)(void *ctx, const struct x1_config *cfg);
	int (*step)(void *ctx, enum x1_step step, const struct x1_config *cfg);
	int (*read)(void *ctx, u32 offset, u32 *value);
	int (*update)(void *ctx, u32 offset, u32 mask, u32 value);
	void (*delay_us)(void *ctx, unsigned int usec);
};

/*
 * One attempt per zero-initialized state. No automatic retry or rollback.
 * A failed callback may already have changed hardware; retain ownership and
 * require a cold shutdown. Success is NOT link, SSD, DMA or tunnel validation.
 * No IRQ, NHI, PCIe policy, connection, storage or suspend operations here.
 */
int x1_startup_run(struct x1_state *state, const struct x1_config *cfg,
		   const struct x1_ops *ops, void *ctx);
const char *x1_step_name(enum x1_step step);
#endif
