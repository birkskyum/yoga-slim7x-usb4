// SPDX-License-Identifier: GPL-2.0-only
/*
 * Separate X1 cold-start candidate. Structure follows Jim Martin's unbound
 * qcom-usb4-startup helper; X1 order comes from the pinned 8380 reference
 * trace in STARTUP-REVIEW.md. No Surface guard or sequence is changed.
 */
#include <linux/errno.h>
#include "qcom-usb4-fw.h"
#include "qcom-usb4-x1-drom.h"
#include "qcom-usb4-drom.h"
#include "qcom-usb4-x1-startup.h"

enum kind { PROVIDER, UPDATE, DELAY, RELEASE_DELAY, CLEAR_1200C };
struct operation {
	enum x1_step step;
	enum kind kind;
	u32 offset, mask, value;
	const char *name;
};
#define P(s) { X1_##s, PROVIDER, 0, 0, 0, #s }
#define U(s, o, m, v) { X1_##s, UPDATE, o, m, v, #s }
#define D(s, us) { X1_##s, DELAY, 0, 0, us, #s }

static const struct operation operations[] = {
	P(PHY_PREPARE),
	P(RX_PREPARE), P(TXB_PREPARE), P(TXA_PREPARE), P(RX_CLOCK_REFERENCE),
	P(MISC_RESET_ASSERT), D(MISC_ASSERT_DELAY, 100),
	P(EXTRA_RESET_ASSERT), P(DP_RESET_ASSERT), D(RESET_ASSERT_DELAY, 1),
	P(DP_RESET_CLEAR), P(EXTRA_RESET_CLEAR),
	{ X1_RESET_RELEASE_DELAY, RELEASE_DELAY, 0, 0, 0, "RESET_RELEASE_DELAY" },
	P(MISC_RESET_CLEAR), P(SYS_READY), P(RX_CLOCK_PHY),
	{ X1_CLEAR_1200C, CLEAR_1200C, 0, 0, 0, "CLEAR_1200C" },
	P(RX_ACTIVE), P(TXB_ACTIVE), P(TXA_ACTIVE),
	U(SET_81000, 0x81000, 1, 1), P(PIPE_RESET_ASSERT),
	D(PIPE_ASSERT_DELAY, 1000), D(PIPE_ASSERT_SETTLE, 100),
	U(SET_81018, 0x81018, 1, 1), P(PIPE_RESET_CLEAR),
	D(PIPE_RELEASE_DELAY, 1000), D(PIPE_RELEASE_SETTLE, 100),
	U(CLEAR_81000, 0x81000, 1, 0), P(UPLOAD), P(SYS_MEMORY_FORCE),
	/* Captured Yoga USB4 PHYC packages are empty. */
	P(TCSR_ROUTE), P(PIPE_HWCG_OFF), U(SET_81004, 0x81004, 1, 1),
	U(TIMER_HIGH, 0x80c8, 0xffff0000, 0x00040000),
	U(TIMER_LOW, 0x80c8, 0xffff, 0),
	U(TIMER_8148, 0x8148, 0xffffffff, 250000),
	U(TIMER_8144, 0x8144, 0xffffffff, 4000),
	/* Unlike the Surface order, X1 writes DROM after routing/timers. */
	P(DROM), P(PHY_COMPOSITE),
	U(ADAPTER_23, 0xd000, 1U << 23, 0),
	U(ADAPTER_22, 0xd000, 1U << 22, 0),
	U(ADAPTER_21, 0xd000, 1U << 21, 0),
	U(CLEAR_D064_6, 0xd064, 1U << 6, 0),
	P(MCU_START), P(PRESET),
};

const char *x1_step_name(enum x1_step step)
{
	unsigned int i;
	if (step == X1_NONE)
		return "none";
	if (step == X1_VALIDATE)
		return "validate";
	if (step == X1_OWNER)
		return "owner";
	for (i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i)
		if (operations[i].step == step)
			return operations[i].name;
	return "invalid";
}

static int validate(const struct x1_config *cfg, const struct x1_ops *ops)
{
	u8 drom[QCOM_USB4_DROM_SIZE];
	int ret;
	if (!cfg || !ops || !ops->owner || !ops->step || !ops->read ||
	    !ops->update || !ops->delay_us)
		return -EINVAL;
	/* This profile intentionally does not imply support for other ports. */
	if (!cfg->serial || cfg->router || cfg->phy_rx_eq != 0x6f || cfg->mcu_preset != 0x0c63 ||
	    cfg->reset_release_us != 100)
		return -EINVAL;
	ret = qcom_usb4_x1_drom_build(drom, sizeof(drom), cfg->serial, 0);
	if (ret)
		return ret;
	return qcom_usb4_fw_validate(cfg->firmware, cfg->firmware_size, 0xf000);
}

static int clear_1200c(const struct x1_ops *ops, void *ctx)
{
	u32 value;
	unsigned int attempts = 0;
	int ret;
	for (;;) {
		ret = ops->read(ctx, 0x1200c, &value);
		if (ret)
			return ret;
		if (!(value & (1U << 31)))
			break;
		if (attempts++ == 11)
			return -ETIMEDOUT;
		ret = ops->update(ctx, 0x1200c, 1U << 31, 0);
		if (ret)
			return ret;
		ops->delay_us(ctx, 1);
	}
	ops->delay_us(ctx, 10000);
	return 0;
}

static int execute(const struct operation *op, const struct x1_config *cfg,
		   const struct x1_ops *ops, void *ctx)
{
	switch (op->kind) {
	case PROVIDER:
		return ops->step(ctx, op->step, cfg);
	case UPDATE:
		return ops->update(ctx, op->offset, op->mask, op->value);
	case DELAY:
		ops->delay_us(ctx, op->value);
		return 0;
	case RELEASE_DELAY:
		ops->delay_us(ctx, cfg->reset_release_us);
		return 0;
	case CLEAR_1200C:
		return clear_1200c(ops, ctx);
	}
	return -EINVAL;
}

int x1_startup_run(struct x1_state *state, const struct x1_config *cfg,
		   const struct x1_ops *ops, void *ctx)
{
	unsigned int i;
	int ret;
	if (!state)
		return -EINVAL;
	if (state->phase != X1_NEW)
		return state->phase == X1_RUNNING ? -EBUSY : -EALREADY;
	state->phase = X1_RUNNING;
	state->active = X1_VALIDATE;
	state->completed = X1_NONE;
	state->error = 0;
	ret = validate(cfg, ops);
	if (ret)
		goto fail;
	state->completed = state->active;
	state->active = X1_OWNER;
	ret = ops->owner(ctx, cfg);
	if (ret)
		goto fail;
	state->completed = state->active;
	for (i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
		state->active = operations[i].step;
		ret = execute(&operations[i], cfg, ops, ctx);
		if (ret)
			goto fail;
		state->completed = state->active;
	}
	state->active = X1_NONE;
	state->phase = X1_COMPLETE;
	return 0;
fail:
	state->error = ret < 0 ? ret : -EPROTO;
	state->phase = X1_FAILED;
	return state->error;
}
