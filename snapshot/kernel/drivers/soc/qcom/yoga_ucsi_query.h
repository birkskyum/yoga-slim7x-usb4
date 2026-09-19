/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef YOGA_UCSI_QUERY_H
#define YOGA_UCSI_QUERY_H
/* UCSI 1.2 only. No reset, role change or Enter_USB.
 * v13 adds a fixed notification mask and a validated status/event ACK path.
 * Transport/command failures are terminal; there is no CANCEL or retry path. */
#define UQ_NOTIFY 0x10005ULL
#define UQ_NOTIFY_READY 0xdb050005ULL /* Linux supported mask for features=0004 */
#define UQ_ACK 0x20004ULL
#define UQ_ACK_EVENT 0x30004ULL
#define UQ_COMPLETE 0x80000000U
#define UQ_ERROR 0x40000000U
#define UQ_ACKED 0x20000000U
#define UQ_BUSY 0x10000000U
#define UQ_UNSUPPORTED 0x02000000U
struct uq_io {
	void *ctx;
	int (*write)(void *ctx, u64 command);
	int (*read)(void *ctx, struct yoga_ucsi_sample *sample);
	void (*pause)(void *ctx);
	void (*deadline)(void *ctx);
};
static bool uq_allowed(u64 cmd)
{
	unsigned int port, recipient;
	if (cmd == UQ_NOTIFY || cmd == UQ_NOTIFY_READY || cmd == UQ_ACK ||
	    cmd == UQ_ACK_EVENT || cmd == 6) return true;
	for (port = 1; port <= 3; port++) {
		if (cmd == ((u64)port << 16 | 7) ||
		    cmd == ((u64)port << 16 | 0x11) ||
		    cmd == ((u64)port << 16 | 0x12) ||
		    cmd == ((u64)port << 16 | 0x0e)) return true;
		for (recipient = 1; recipient <= 2; recipient++)
			if (cmd == ((u64)port << 24 | (u64)recipient << 16 | 0x0c)) return true;
	}
	return false;
}
static int uq_read(struct uq_io *io, struct yoga_ucsi_sample *sample)
{
	int ret = io->read(io->ctx, sample);
	if (ret) return ret;
	return sample->version == 0x0120 ? 0 : -EPROTONOSUPPORT;
}
static int uq_poll(struct uq_io *io, struct yoga_ucsi_sample *sample, bool ack)
{
	unsigned int i;
	int ret;
	for (i = 0; i < 50; i++) {
		ret = uq_read(io, sample);
		if (ret) return ret;
		/* All other CCI fields are invalid while BUSY is set. */
		if (!(sample->cci & UQ_BUSY)) {
			if (sample->cci & ~(UQ_COMPLETE | UQ_ERROR | UQ_ACKED |
					    UQ_UNSUPPORTED | 0xfffeU)) return -EPROTO;
			if (ack && (sample->cci & UQ_ACKED)) {
				if (sample->cci & ~(UQ_ACKED | 0xfeU)) return -EPROTO;
				return 0;
			}
			if (!ack && (sample->cci & UQ_COMPLETE)) {
				if ((sample->cci & UQ_ACKED) || ((sample->cci >> 8) & 0xff) > 16)
					return -EPROTO;
				return 0;
			}
		}
		io->pause(io->ctx);
	}
	return -ETIMEDOUT;
}
static int uq_command_kind(struct uq_io *io, u64 cmd,
			   struct yoga_ucsi_sample *reply, unsigned int event_connector)
{
	struct yoga_ucsi_sample sample;
	int ret, result;
	if (!uq_allowed(cmd) || cmd == UQ_ACK || cmd == UQ_ACK_EVENT) return -EPERM;
	if (event_connector && (event_connector > 3 ||
	    cmd != ((u64)event_connector << 16 | 0x12))) return -EPERM;
	io->deadline(io->ctx);
	ret = uq_read(io, &sample);
	if (ret) return ret;
	/* Never clear a command left by another owner or an uncertain earlier run. */
	if (sample.cci & ~(UQ_ACKED | 0xfeU)) return -EBUSY;
	if (event_connector && ((sample.cci >> 1) & 0x7f) != event_connector)
		return -EPROTO;
	ret = io->write(io->ctx, cmd);
	if (ret) return ret;
	ret = uq_poll(io, reply, false);
	if (ret) return ret;
	result = (reply->cci >> 8) & 0xff;
	if (reply->cci & UQ_UNSUPPORTED) result = -EOPNOTSUPP;
	if (reply->cci & UQ_ERROR) result = -EREMOTEIO;
	/* Only consume a connector event with a successful, correctly sized status
	 * for the pending connector. Malformed/error replies get command ACK only. */
	if (event_connector && result >= 0 && (result < 9 || result > 16 ||
	    ((reply->cci >> 1) & 0x7f) != event_connector)) result = -EPROTO;
	io->deadline(io->ctx);
	ret = io->write(io->ctx, event_connector && result >= 0 ? UQ_ACK_EVENT : UQ_ACK);
	if (ret) return ret;
	ret = uq_poll(io, &sample, true);
	return ret ? ret : result;
}
static int uq_command(struct uq_io *io, u64 cmd, struct yoga_ucsi_sample *reply)
{
	return uq_command_kind(io, cmd, reply, 0);
}
static int uq_pending(struct uq_io *io, unsigned int connectors)
{
	struct yoga_ucsi_sample sample;
	unsigned int connector;
	int ret;
	if (!connectors || connectors > 3) return -ERANGE;
	io->deadline(io->ctx);
	ret = uq_read(io, &sample);
	if (ret) return ret;
	if (sample.cci & ~(UQ_ACKED | 0xfeU)) return -EBUSY;
	connector = (sample.cci >> 1) & 0x7f;
	return connector > connectors ? -EPROTO : (int)connector;
}
#endif
