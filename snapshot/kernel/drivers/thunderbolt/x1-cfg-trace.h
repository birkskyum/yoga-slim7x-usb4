/* SPDX-License-Identifier: GPL-2.0 */
/* Included after struct tb_ctl; only observes a completed request result. */
#ifndef X1_CFG_TRACE_H
#define X1_CFG_TRACE_H

static inline void x1_cfg_trace(struct tb_ctl *ctl, const char *direction,
			       u64 route, u32 port, enum tb_cfg_space space,
			       u32 offset, u32 length,
			       const struct tb_cfg_result *res)
{
	if (IS_ENABLED(CONFIG_USB4_X1_NATIVE) && res->err)
		dev_emerg(ctl->nhi->dev,
			  "V33 CFG FAIL dir=%s route=%llx port=%u space=%u offset_dwords=%#x words=%u raw_err=%d tb_error=%d response_port=%u timeout_ms=%d\n",
			  direction, (unsigned long long)route, port, space, offset, length,
			  res->err, res->err == 1 ? (int)res->tb_error : -1,
			  res->response_port, ctl->timeout_msec);
}

#endif
