/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded route-0 READ encoding derived from ctl.c / tb_msgs.h.
 * Common protocol work: Andreas Noever and Intel Corporation; see those files.
 * Types, errno, endian and crc32c helpers are supplied by the includer.
 */
#define X1_READ_WORDS 5
#define X1_REPLY_BYTES (16 + 4 * X1_READ_WORDS)

static void x1_packet_request(u8 *wire, unsigned int seq)
{
	put_unaligned_be32(0, wire);
	put_unaligned_be32(0, wire + 4);
	/* offset=0, length=5, port=0, SWITCH=2, sequence=0 or 1. */
	put_unaligned_be32((X1_READ_WORDS << 13) | (2U << 25) | (seq << 27), wire + 8);
	put_unaligned_be32(~crc32c(~0U, wire, 12), wire + 12);
}

static int x1_packet_reply(const u8 *wire, unsigned int size,
			   unsigned int sof, unsigned int eof,
			   unsigned int seq, u32 *words)
{
	u32 addr;
	unsigned int i;
	if (size != X1_REPLY_BYTES || sof || eof != 1 || seq > 1)
		return -EPROTO;
	if (get_unaligned_be32(wire + size - 4) != ~crc32c(~0U, wire, size - 4))
		return -EBADMSG;
	if (get_unaligned_be32(wire) != 0x80000000U || get_unaligned_be32(wire + 4))
		return -EPROTO;
	addr = get_unaligned_be32(wire + 8);
	/* Port is the replying router's upstream port, not the request port. */
	if ((addr & ~(0x3fU << 19)) !=
	    ((X1_READ_WORDS << 13) | (2U << 25) | (seq << 27)))
		return -EPROTO;
	for (i = 0; i < X1_READ_WORDS; i++)
		words[i] = get_unaligned_be32(wire + 12 + 4 * i);
	return 0;
}
