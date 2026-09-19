/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded READ-only protocol, derived from Linux ctl.c/tb_msgs.h.
 * Andreas Noever / Intel attribution is retained in those files.
 * No config WRITE, route assignment, event ACK, path or tunnel setup.
 */
struct x1_read_spec {
	u32 route;
	unsigned int port, space, offset, words, seq;
};
static int x1_read_address(const struct x1_read_spec *s, u32 *address)
{
	if (!s || !address || (s->route != 0 && s->route != 2) || s->port > 7 || s->seq > 3 ||
	    !s->words || s->words > 8 || s->offset > 255 || s->offset + s->words > 256 ||
	    (s->space != 1 && s->space != 2) ||
	    (s->space == 2 && (s->port ||
	      !((s->offset == 0 && s->words == 5) ||
	        (s->offset == 5 && s->words == 4) ||
	        (s->route == 2 && (s->offset == 5 || s->offset == 6) && s->words == 1) ||
	        (s->offset >= 9 && s->words <= 2)))) ||
	    (s->space == 1 && (!s->port || (s->route && s->port > 5)))) return -EINVAL;
	*address = s->offset | (s->words << 13) | (s->port << 19) |
		(s->space << 25) | (s->seq << 27);
	return 0;
}
static int x1_read_request(u8 *wire, const struct x1_read_spec *s)
{
	u32 address;
	int ret = x1_read_address(s, &address);
	if (ret || !wire) return ret ?: -EINVAL;
	put_unaligned_be32(0, wire);
	put_unaligned_be32(s->route, wire + 4);
	put_unaligned_be32(address, wire + 8);
	put_unaligned_be32(~crc32c(~0U, wire, 12), wire + 12);
	return 0;
}
static int x1_read_reply(const u8 *wire, unsigned int size, unsigned int sof,
			unsigned int eof, const struct x1_read_spec *s, u32 *words)
{
	u32 address, got;
	unsigned int i;
	int ret = x1_read_address(s, &address);
	if (ret || !wire || !words) return ret ?: -EINVAL;
	if (size < 16 || size > 48 || sof || (size & 3)) return -EPROTO;
	if (get_unaligned_be32(wire + size - 4) != ~crc32c(~0U, wire, size - 4))
		return -EBADMSG;
	/* Incoming EVENT=5 is handled separately; ERROR=3 is never a plug event. */
	if (eof == 3) {
		if (size != 16) return -EPROTO;
		return -ENOLINK;
	}
	if (eof != 1 || size != 16 + 4 * s->words ||
	    get_unaligned_be32(wire) != 0x80000000U ||
	    get_unaligned_be32(wire + 4) != s->route) return -EPROTO;
	got = get_unaligned_be32(wire + 8);
	/* Reply port is metadata, not an echoed request port (Linux ctl.c). */
	got &= ~(0x3fU << 19); address &= ~(0x3fU << 19);
	if (got != address) return -EPROTO;
	for (i = 0; i < s->words; i++) words[i] = get_unaligned_be32(wire + 12 + i * 4);
	return 0;
}
