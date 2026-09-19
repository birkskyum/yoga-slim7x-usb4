/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef YOGA_PAN_PACKET_H
#define YOGA_PAN_PACKET_H
/* Same 32-byte PAN wire layout consumed by pmic_glink_altmode.c. No I/O.
 * Preserve raw fields, including reserved/version bits, instead of turning
 * unknown data into a USB4 connect command. The header SVID is authoritative
 * in the existing producer; retain the payload SVID independently for audit.
 */
struct yoga_pan {
	u8 port, orientation_raw, mux_raw, ext[8];
	u16 vid, svid_header, svid_payload;
};
static inline int yoga_pan_decode(const u8 *p, size_t len, struct yoga_pan *out)
{
	struct yoga_pan v;
	unsigned int i;
	if (!p || !out || len != 32)
		return -EINVAL;
	if (get_unaligned_le32(p) != 32780 || get_unaligned_le32(p + 4) != 2 ||
	    (get_unaligned_le32(p + 8) & 0xffff) != 0x16 || p[12] >= 3)
		return -EBADMSG;
	v.port = p[12]; v.orientation_raw = p[13]; v.mux_raw = p[14];
	v.vid = get_unaligned_le16(p + 16);
	v.svid_payload = get_unaligned_le16(p + 18);
	v.svid_header = get_unaligned_le32(p + 8) >> 16;
	for (i = 0; i < 8; i++) v.ext[i] = p[20 + i];
	*out = v;
	return 0;
}
#endif
