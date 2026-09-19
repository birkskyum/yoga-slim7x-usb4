/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef YOGA_PAN_PLAN_H
#define YOGA_PAN_PLAN_H
/* Pure, conservative X1 legacy-PAN planner. No I/O; output unchanged on error.
 * MCU word is logged ONLY, never submitted by this retimer diagnostic. */
enum yoga_port_mode { YOGA_SAFE, YOGA_USB3, YOGA_USB4, YOGA_TBT };
struct yoga_port_plan {
	u8 orientation, mode, cable_type, speed, rounded;
	u32 connect_word;
};
static inline int yoga_plan(const struct yoga_pan *e, struct yoga_port_plan *out)
{
	struct yoga_port_plan p = { 0 };
	u32 cable_class;
	unsigned int i;
	if (!e || !out || e->port != 0 || e->orientation_raw > 2)
		return -EINVAL;
	p.orientation = e->orientation_raw;
	if (e->mux_raw == 0) {
		p.orientation = 2;
		p.mode = YOGA_SAFE;
	} else if (e->mux_raw == 1) {
		if (p.orientation == 2) return -EINVAL;
		if (e->svid_header != 0xff00 || e->svid_payload != 0xff00)
			return -EOPNOTSUPP;
		p.mode = YOGA_USB3;
	} else if (e->mux_raw == 4) {
		if (p.orientation == 2 || e->svid_header != e->svid_payload)
			return -EINVAL;
		if (e->svid_header == 0xff00) p.mode = YOGA_USB4;
		else if (e->svid_header == 0x8087) p.mode = YOGA_TBT;
		else return -EOPNOTSUPP;
		if (e->ext[0] & 0x80) return -EOPNOTSUPP;
		for (i = 1; i < 8; i++)
			if (e->ext[i]) return -EOPNOTSUPP;
		p.speed = e->ext[0] & 7;
		p.cable_type = (e->ext[0] >> 3) & 7;
		p.rounded = (e->ext[0] >> 6) & 1;
		if (p.speed > 1 || p.cable_type > 1) return -EOPNOTSUPP;
		cable_class = p.cable_type ?
			(p.mode == YOGA_TBT && !p.rounded ? 3 : 1) : 0;
		p.connect_word = 1 | (p.mode == YOGA_USB4 ? 1U << 8 : 0) |
			(u32)p.orientation << 9 | 1U << 10 | cable_class << 11 |
			(u32)p.speed << 13;
	} else {
		return -EOPNOTSUPP; /* No DP, reset request, V2 bits or guessed fallback. */
	}
	*out = p;
	return 0;
}
#endif
