/* SPDX-License-Identifier: GPL-2.0-only */
/* Narrow root-lane hotplug demultiplexing, derived from Linux tb_msgs.h and
 * ctl.c:tb_cfg_ack_plug(). Original authorship remains in those sources.
 * EVENT=5 is an incoming notification; ERROR=3/error=7 is its outgoing ACK.
 * This does not configure the external router or create a tunnel.
 */
#define X1_EVENT_LIMIT 16
struct x1_plug_event {
	unsigned int port;
	bool unplug;
};
static int x1_event_decode(const u8 *wire, unsigned int size, unsigned int sof,
			   unsigned int eof, struct x1_plug_event *event)
{
	u32 data;
	if (!wire || !event) return -EINVAL;
	if (size != 16 || sof || eof != 5) return -EPROTO;
	if (get_unaligned_be32(wire + 12) != ~crc32c(~0U, wire, 12))
		return -EBADMSG;
	if (get_unaligned_be32(wire) != 0x80000000U ||
	    get_unaligned_be32(wire + 4)) return -EPROTO;
	data = get_unaligned_be32(wire + 8);
	if ((data & 0x7fffffc0U) || ((data & 0x3f) != 2 && (data & 0x3f) != 3))
		return -EPROTO;
	event->port = data & 0x3f;
	event->unplug = !!(data & 0x80000000U);
	return 0;
}
static int x1_event_ack(u8 *wire, const struct x1_plug_event *event)
{
	if (!wire || !event || (event->port != 2 && event->port != 3))
		return -EINVAL;
	put_unaligned_be32(0, wire);
	put_unaligned_be32(0, wire + 4); /* Only the observed root router. */
	put_unaligned_be32((event->unplug ? 0xc0000000U : 0x80000000U) |
			   (event->port << 8) | 7, wire + 8);
	put_unaligned_be32(~crc32c(~0U, wire, 12), wire + 12);
	return 0;
}
