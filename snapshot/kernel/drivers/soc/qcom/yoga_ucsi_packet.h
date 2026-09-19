/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef YOGA_UCSI_PACKET_H
#define YOGA_UCSI_PACKET_H
/* ucsi_glink.c lengths: hdr + 48/528-byte buffer + status.
 * Decoding alone does not establish message-in freshness. */
struct yoga_ucsi_sample { u16 version; u32 cci; u8 prefix[32]; };
static inline int yoga_ucsi_decode(const u8 *p, size_t len, struct yoga_ucsi_sample *out)
{
	struct yoga_ucsi_sample s;
	unsigned int i;
	if (!p || !out || (len != 64 && len != 544)) return -EMSGSIZE;
	if (get_unaligned_le32(p) != 32779 || get_unaligned_le32(p + 4) != 1 ||
	    get_unaligned_le32(p + 8) != 0x11) return -EBADMSG;
	if (get_unaligned_le32(p + len - 4)) return -EREMOTEIO;
	s.version = get_unaligned_le16(p + 12);
	if ((len == 64 && (s.version < 0x100 || s.version >= 0x200)) ||
	    (len == 544 && (s.version < 0x200 || s.version > 0x300)))
		return -EPROTONOSUPPORT;
	s.cci = get_unaligned_le32(p + 16);
	for (i = 0; i < 32; i++) s.prefix[i] = p[12 + i];
	*out = s;
	return 0;
}
#endif
