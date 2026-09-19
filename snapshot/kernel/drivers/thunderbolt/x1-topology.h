/* SPDX-License-Identifier: GPL-2.0-only */
/* Private read-only inventory derived from Linux cap.c, switch.c, tb_regs.h.
 * Original Andreas Noever / Intel attribution remains in those source files.
 * No writes or paths. Caller retains the v16 owner, PAN checks and deadlines.
 */
#define X1_TOPO_READ_LIMIT 128
#define X1_TOPO_CAP_LIMIT 16
struct x1_topology {
	void *ctx;
	int (*read)(void *, u32, unsigned int, unsigned int, unsigned int,
		    unsigned int, u32 *);
	void (*report)(void *, const char *, u32, unsigned int, unsigned int,
		       const u32 *, unsigned int);
	unsigned int reads, adapters, caps, lanes, pcie_up, pcie_down, skipped;
};
static int x1_topo_read(struct x1_topology *t, u32 route, unsigned int port,
			unsigned int space, unsigned int offset, unsigned int words, u32 *out)
{
	struct x1_read_spec spec = { route, port, space, offset, words, 0 };
	u32 address;
	int ret = x1_read_address(&spec, &address);
	if (ret) return ret;
	if (t->reads >= X1_TOPO_READ_LIMIT) return -ENOSPC;
	t->reads++;
	return t->read(t->ctx, route, port, space, offset, words, out);
}
static int x1_topo_caps(struct x1_topology *t, u32 route, unsigned int port,
			unsigned int cap, u32 type)
{
	u8 seen[256] = { 0 };
	u32 data[2];
	unsigned int n, id, next, words, space = port ? 1 : 2;
	int ret;
	for (n = 0; cap && n < X1_TOPO_CAP_LIMIT; n++) {
		if (cap > 254) {
			t->skipped++;
			data[0] = cap;
			t->report(t->ctx, "CAP_OUTSIDE_WINDOW", route, port, cap, data, 1);
			return 0; /* Known read-window limit, not a transport failure. */
		}
		if (cap < (port ? 8 : 9) || seen[cap]) return -EPROTO;
		seen[cap] = 1;
		/* Native switch walker reads two words; port walker reads one. */
		words = port ? 1 : 2;
		ret = x1_topo_read(t, route, port, space, cap, words, data);
		if (ret) return ret;
		id = (data[0] >> 8) & 0xff;
		next = data[0] & 0xff;
		if (!port) {
			if (id == 5 && !(data[0] >> 24)) next = data[1] & 0xffff;
			else if (id != 3 && id != 5) {
				t->report(t->ctx, "UNKNOWN_ROUTER_CAP", route, port, cap, data, words);
				t->skipped++;
				return 0; /* Preserve header without interpreting unknown payload. */
			}
		}
		t->caps++;
		t->report(t->ctx, "CAP", route, port, cap, data, words);
		if (port && ((id == 1 && type == 1) ||
			    (id == 4 && (type == 0x100101 || type == 0x100102 ||
				       type == 0x200101 || type == 0x200102)))) {
			/* PHY CS1 / protocol CS1, no sideband op, reset or enable. */
			ret = x1_topo_read(t, route, port, 1, cap + 1, 1, data + 1);
			if (ret) return ret;
			t->report(t->ctx, id == 1 ? "PHY" : "PROTOCOL", route, port, cap, data, 2);
			if (id == 1) t->lanes++;
		}
		cap = next;
	}
	return cap ? -ELOOP : 0;
}
static int x1_topology_router(struct x1_topology *t, u32 route, const u32 *header)
{
	u32 data[8], types[8] = { 0 };
	u8 cap_heads[8] = { 0 };
	unsigned int port, max;
	int ret;
	if (!t || !t->read || !t->report || !header) return -EINVAL;
	/* Inventory only the exact configured root/LaCie classes. */
	if (route == 0) {
		if (header[0] != 0x093805c6 || header[1] != 0x0101c11c ||
		    header[2] || header[3] != 0x80000000U || header[4] != 0x200010ff)
			return -EPERM;
	} else if (route == 2) {
		if (header[0] != 0x113b059f || header[1] != 0x00114120 ||
		    header[2] != 2 || header[3] != 0x80000000U || header[4] != 0x200010ff)
			return -EPERM;
	} else return -EPERM;
	max = route ? 5 : 7;
	/* Complete host inventory before the first external write/adapter read.
	 * Within each router, collect all headers before interpreting capabilities. */
	for (port = 1; port <= max; port++) {
		ret = x1_topo_read(t, route, port, 1, 0, 8, data);
		if (ret) return ret;
		t->adapters++;
		t->report(t->ctx, "ADAPTER", route, port, 0, data, 8);
		if ((data[2] & 0xffffff) == 0x100102 && route) t->pcie_up++;
		if ((data[2] & 0xffffff) == 0x100101 && !route) t->pcie_down++;
		types[port] = data[2] & 0xffffff;
		cap_heads[port] = data[1] & 0xff;
	}
	for (port = 1; port <= max; port++) {
		if (!route && port == 1) continue; /* NHI capabilities not needed. */
		if (!types[port]) continue;
		ret = x1_topo_caps(t, route, port, cap_heads[port], types[port]);
		if (ret) return ret;
	}
	ret = x1_topo_read(t, route, 0, 2, 5, 4, data);
	if (ret) return ret;
	t->report(t->ctx, "ROUTER", route, 0, 5, data, 4);
	return x1_topo_caps(t, route, 0, header[1] & 0xff, 0);
}
