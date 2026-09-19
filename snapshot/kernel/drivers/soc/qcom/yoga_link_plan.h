/* SPDX-License-Identifier: GPL-2.0-only */
/* Strictly the actual v13 LaCie/passive Gen3 PAN, not a generic Type-C API. */
static int yoga_link_word(const struct yoga_pan *events, unsigned int count,
			  unsigned int processed, u32 *word)
{
	struct yoga_port_plan p;
	const struct yoga_pan *e = NULL;
	unsigned int i;
	if (!events || !word || !count || count > 32 || count != processed) return -EAGAIN;
	for (i = 0; i < count; i++) if (!events[i].port) e = &events[i];
	if (!e || e->vid != 0x059f || e->ext[0] != 1 || yoga_plan(e, &p) || p.mode != YOGA_USB4 ||
	    (p.connect_word != 0x2501 && p.connect_word != 0x2701)) return -ENOTCONN;
	*word = p.connect_word;
	return 0;
}
