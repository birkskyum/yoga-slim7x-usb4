/* SPDX-License-Identifier: GPL-2.0-only */
/* Private, narrow derivative of Linux switch.c/usb4.c/ctl.c.
 * Andreas Noever / Intel attribution remains in those source files.
 * Root CM configuration/primary unlock and exact LaCie enumeration only.
 * No path space, NVM, bonding, protocol enables or tunnel operations.
 */
struct x1_write_spec {
	unsigned int port, space, offset, words, seq;
	const u32 *before, *after;
	u32 route;
};
static int x1_root_plan(const u32 *header, u32 *out)
{
	if (!header || !out || header[0] != 0x093805c6 ||
	    header[1] != 0x0101c11c || header[2] || header[3] ||
	    header[4] != 0x2000000a) return -EPERM;
	/* tb_switch_configure(): root route 0, enabled, USB4v1 CMUV, timeout. */
	out[0] = header[1];
	out[1] = header[2];
	out[2] = header[3] | 0x80000000U;
	out[3] = (header[4] & 0xffff0000U) | 0x10ff;
	return 0;
}
static int x1_external_plan(const u32 *header, u32 *out)
{
	if (!header || !out || header[0] != 0x113b059f ||
	    header[1] != 0x00014120 || header[2] || header[3] ||
	    header[4] != 0x2000000a) return -EPERM;
	/* switch.c: upstream 1, depth 1, route 2, enabled, USB4v1 CMUV. */
	out[0] = header[1] | (1U << 20);
	out[1] = 2;
	out[2] = 0x80000000U;
	out[3] = 0x200010ff;
	return 0;
}
static int x1_external_setup_plan(u32 before, u32 *after)
{
	/* Refuse existing sleep/wake, tunnel-on, internal xHCI or CV state.
	 * usb4_switch_setup() with both tunnel policies disabled: clear CNS
	 * only, preserving all other bits. No configuration-valid write.
	 */
	if (!after || (before & 0x8700000fU)) return -EPERM;
	*after = before & ~0x00800000U;
	return 0;
}
static int x1_write_address(const struct x1_write_spec *s, u32 *address)
{
	if (!s || !address || !s->before || !s->after || s->seq > 3)
		return -EINVAL;
	if (s->route == 2) {
		u32 expected;
		if (s->port || s->space != 2) return -EPERM;
		if (s->offset == 1 && s->words == 4) {
			if (s->before[0] != 0x00014120 || s->before[1] || s->before[2] ||
			    s->before[3] != 0x2000000a || s->after[0] != 0x00114120 ||
			    s->after[1] != 2 || s->after[2] != 0x80000000U ||
			    s->after[3] != 0x200010ff) return -EPERM;
		} else if (s->offset == 5 && s->words == 1) {
			if (x1_external_setup_plan(s->before[0], &expected) ||
			    s->after[0] != expected) return -EPERM;
		} else return -EPERM;
	} else if (s->route) return -EPERM;
	else if (!s->port && s->space == 2 && s->offset == 1 && s->words == 4) {
		if (s->before[0] != 0x0101c11c || s->before[1] || s->before[2] ||
		    s->before[3] != 0x2000000a || s->after[0] != s->before[0] ||
		    s->after[1] || s->after[2] != 0x80000000U ||
		    s->after[3] != 0x200010ff) return -EPERM;
	} else if (s->port == 2 && s->space == 1 && s->offset == 4 && s->words == 1) {
		if (!(s->before[0] & 0x80000000U) ||
		    s->after[0] != (s->before[0] & 0x7fffffffU)) return -EPERM;
	} else return -EPERM;
	*address = s->offset | (s->words << 13) | (s->port << 19) |
		(s->space << 25) | (s->seq << 27);
	return 0;
}
static int x1_write_request(u8 *wire, const struct x1_write_spec *s)
{
	u32 address;
	unsigned int i, size;
	int ret = x1_write_address(s, &address);
	if (ret || !wire) return ret ?: -EINVAL;
	put_unaligned_be32(0, wire);
	put_unaligned_be32(s->route, wire + 4);
	put_unaligned_be32(address, wire + 8);
	for (i = 0; i < s->words; i++)
		put_unaligned_be32(s->after[i], wire + 12 + 4 * i);
	size = 12 + 4 * s->words;
	put_unaligned_be32(~crc32c(~0U, wire, size), wire + size);
	return 0;
}
static int x1_write_reply(const u8 *wire, unsigned int size, unsigned int sof,
			 unsigned int eof, const struct x1_write_spec *s)
{
	u32 address, got;
	int ret = x1_write_address(s, &address);
	if (ret || !wire) return ret ?: -EINVAL;
	if (size != 16 || sof) return -EPROTO;
	if (get_unaligned_be32(wire + 12) != ~crc32c(~0U, wire, 12))
		return -EBADMSG;
	if (eof == 3)
		return -ENOLINK; /* Incoming EVENT=5 is demultiplexed by the transport. */
	if (eof != 2 || get_unaligned_be32(wire) != 0x80000000U ||
	    get_unaligned_be32(wire + 4) != s->route) return -EPROTO;
	got = get_unaligned_be32(wire + 8);
	/* Response port is metadata, as in Linux ctl.c. */
	return (got & ~(0x3fU << 19)) == (address & ~(0x3fU << 19)) ? 0 : -EPROTO;
}
