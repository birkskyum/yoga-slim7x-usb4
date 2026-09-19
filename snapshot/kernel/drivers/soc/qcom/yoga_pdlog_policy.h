/* SPDX-License-Identifier: GPL-2.0-only */
/* Private bounded GET-only policy; wire layout from pmic_pdcharger_ulog.c. */
#ifndef YOGA_PDLOG_POLICY_H
#define YOGA_PDLOG_POLICY_H
#define PDLOG_SIZE 8192
#define PDLOG_MAX_REQUESTS 128
struct pdlog_state {
	bool attempted, active, awaiting, limited;
	unsigned int requests, replies;
	int error;
};
static int pdlog_start(struct pdlog_state *s, bool allowed)
{
	if (!allowed || s->attempted) return -EPERM;
	s->attempted = s->active = true;
	return 0;
}
static void pdlog_stop(struct pdlog_state *s, int error)
{
	s->active = s->awaiting = false;
	if (error && !s->error) s->error = error;
}
static bool pdlog_next(struct pdlog_state *s, bool expired)
{
	if (!s->active || s->awaiting || s->error) return false;
	if (expired || s->requests >= PDLOG_MAX_REQUESTS) {
		s->limited = true;
		pdlog_stop(s, 0);
		return false;
	}
	s->requests++;
	s->awaiting = true;
	return true;
}
static void pdlog_request(u8 *request)
{
	memset(request, 0, 16);
	put_unaligned_le32(32778, request);
	put_unaligned_le32(1, request + 4);
	put_unaligned_le32(0x18, request + 8);
	put_unaligned_le32(PDLOG_SIZE, request + 12);
}
static int pdlog_decode(const u8 *p, size_t len, char *text)
{
	size_t i;
	if (len != PDLOG_SIZE + 12) return -EMSGSIZE;
	if (get_unaligned_le32(p) != 32778 || get_unaligned_le32(p + 4) != 1 ||
	    get_unaligned_le32(p + 8) != 0x18) return -EBADMSG;
	/* Preserve printable text, LF and TAB, never emit terminal escapes. The
	 * original driver treats this as a NUL-terminated text log, not binary. */
	for (i = 0; i < PDLOG_SIZE && p[12 + i]; i++) {
		u8 c = p[12 + i];
		text[i] = (c >= 32 && c <= 126) || c == '\n' || c == '\t' ? c : '.';
	}
	text[i] = 0;
	return 0;
}
static bool pdlog_reply(struct pdlog_state *s, int error)
{
	if (!s->active || !s->awaiting || s->error) return false;
	s->awaiting = false;
	if (error) pdlog_stop(s, error);
	else s->replies++;
	return true;
}
#endif
