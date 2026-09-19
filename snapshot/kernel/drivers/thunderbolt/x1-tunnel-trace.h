/* SPDX-License-Identifier: GPL-2.0 */
/* Private diagnostics only. No hardware access, retry or policy change. */
#ifndef X1_TUNNEL_TRACE_H
#define X1_TUNNEL_TRACE_H

static inline bool x1_tunnel_begin(const struct tb *tb, const char *fn,
				  const char *op)
{
	if (!x1_native_pcie_authorizing(tb))
		return false;
	pr_emerg("V33 TUNNEL BEGIN %s: %s\n", fn, op);
	return true;
}

static inline bool x1_tunnel_port_begin(const struct tb_port *port,
				       const char *fn, const char *op)
{
	if (!x1_native_pcie_authorizing(port->sw->tb))
		return false;
	pr_emerg("V33 TUNNEL PORT route=%llx port=%u type=%#x cap=%#x\n",
		 (unsigned long long)tb_route(port->sw), port->port,
		 port->config.type, port->cap_adap);
	return x1_tunnel_begin(port->sw->tb, fn, op);
}

static inline void x1_tunnel_selected(const struct tb_port *down,
				     const struct tb_port *up)
{
	if (x1_native_pcie_authorizing(down->sw->tb))
		pr_emerg("V33 TUNNEL SELECT down=%llx:%u cap=%#x up=%llx:%u cap=%#x\n",
			 (unsigned long long)tb_route(down->sw), down->port, down->cap_adap,
			 (unsigned long long)tb_route(up->sw), up->port, up->cap_adap);
}

static inline void x1_tunnel_path(const struct tb *tb, int index)
{
	if (x1_native_pcie_authorizing(tb))
		pr_emerg("V33 TUNNEL PATH index=%d\n", index);
}

/* int-returning operations only; typeof and stringification do not run op. */
#define X1_TUNNEL_STEP(tb, op) ({ \
	bool __x1_trace = x1_tunnel_begin((tb), __func__, #op); \
	int __x1_result = (op); \
	if (__x1_trace) \
		pr_emerg("V33 TUNNEL END %s: %s ret=%d\n", __func__, #op, __x1_result); \
	__x1_result; \
})

#define X1_TUNNEL_PORT(port, op) ({ \
	bool __x1_trace = x1_tunnel_port_begin((port), __func__, #op); \
	int __x1_result = (op); \
	if (__x1_trace) \
		pr_emerg("V33 TUNNEL END %s: %s ret=%d\n", __func__, #op, __x1_result); \
	__x1_result; \
})

#define X1_TUNNEL_VOID(tb, op) do { \
	bool __x1_trace = x1_tunnel_begin((tb), __func__, #op); \
	(op); \
	if (__x1_trace) \
		pr_emerg("V33 TUNNEL END %s: %s\n", __func__, #op); \
} while (0)

#endif
