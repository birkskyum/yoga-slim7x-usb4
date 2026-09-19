// SPDX-License-Identifier: GPL-2.0-only
/* Private v13 UCSI 1.2 query/notification diagnostic. Transport layout derived from
 * ucsi_glink.c, copyright Linux Foundation / Linaro. No normal UCSI consumer.
 * Commands are restricted by yoga_ucsi_query.h. No user-supplied commands. */
#include <linux/auxiliary_bus.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <linux/soc/qcom/pdr.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/soc/qcom/x1-usb4-diag.h>
#include "yoga_ucsi_packet.h"
#include "yoga_ucsi_query.h"

struct query {
	struct device *dev;
	struct pmic_glink_client *client;
	struct mutex control;
	spinlock_t lock;
	struct completion done;
	struct yoga_ucsi_sample reply;
	unsigned int attempts, commands, notifications, opcode;
	unsigned long deadline, group_deadline;
	int error, reply_error;
	bool service_up, awaiting, initialized;
	bool policy_attempted, policy_active;
	unsigned int polls, events;
	unsigned long policy_deadline;
	struct delayed_work policy_work;
	u8 connectors, cap[16], caps[3][4], status[3][16], cable[3][16], cam[3];
	u8 status_len[3], cable_len[3];
	u16 features;
};
static atomic_t policy_once = ATOMIC_INIT(0);
static void query_callback(const void *data, size_t len, void *priv)
{
	struct query *s = priv;
	const u8 *p = data;
	unsigned long flags;
	u32 opcode;
	if (len < 12 || get_unaligned_le32(p) != 32779) return;
	opcode = get_unaligned_le32(p + 8);
	spin_lock_irqsave(&s->lock, flags);
	if (opcode == 0x13) {
		if (len == 24 && get_unaligned_le32(p + 4) == 2) s->notifications++;
	} else if (opcode == s->opcode && s->awaiting) {
		if (opcode == 0x11) {
			s->reply_error = len == 64 ? yoga_ucsi_decode(p, len, &s->reply) : -EMSGSIZE;
		} else {
			s->reply_error = len != 16 ? -EMSGSIZE :
				get_unaligned_le32(p + 4) != 1 ? -EBADMSG :
				get_unaligned_le32(p + 12) ? -EREMOTEIO : 0;
		}
		s->awaiting = false;
		complete(&s->done);
	}
	spin_unlock_irqrestore(&s->lock, flags);
}
static void query_pdr(void *priv, int state)
{
	struct query *s = priv;
	unsigned long flags;
	spin_lock_irqsave(&s->lock, flags);
	s->service_up = state == SERVREG_SERVICE_STATE_UP;
	if (!s->service_up && s->attempts) {
		if (!s->error) s->error = -ENOTCONN;
		s->awaiting = false;
		complete(&s->done);
	}
	spin_unlock_irqrestore(&s->lock, flags);
}
static unsigned long query_left(struct query *s)
{
	unsigned long now = jiffies;
	if (time_after_eq(now, s->deadline) || time_after_eq(now, s->group_deadline)) return 0;
	return min(s->deadline - now, s->group_deadline - now);
}
static int query_exchange(struct query *s, u32 opcode, u64 command,
			  struct yoga_ucsi_sample *sample)
{
	u8 request[64] = {};
	unsigned long flags, left;
	int ret = 0;
	if (opcode != 0x11 && opcode != 0x12) return -EPERM;
	if (opcode == 0x12 && !uq_allowed(command)) return -EPERM;
	put_unaligned_le32(32779, request);
	put_unaligned_le32(1, request + 4);
	put_unaligned_le32(opcode, request + 8);
	if (opcode == 0x12) put_unaligned_le64(command, request + 20);
	spin_lock_irqsave(&s->lock, flags);
	if (s->error) ret = s->error;
	else if (!s->service_up) ret = -ENOTCONN;
	else if (!query_left(s)) ret = -ETIMEDOUT;
	if (ret) {
		s->error = ret;
		spin_unlock_irqrestore(&s->lock, flags);
		return ret;
	}
	reinit_completion(&s->done);
	s->reply_error = -ETIMEDOUT;
	s->opcode = opcode;
	s->awaiting = true;
	spin_unlock_irqrestore(&s->lock, flags);
	ret = pmic_glink_send(s->client, request, opcode == 0x11 ? 12 : 64);
	left = query_left(s);
	if (!ret && (!left || !wait_for_completion_timeout(&s->done, left))) ret = -ETIMEDOUT;
	spin_lock_irqsave(&s->lock, flags);
	s->awaiting = false;
	if (s->error) ret = s->error;
	if (!ret) ret = s->reply_error;
	if (ret) s->error = ret; /* No reuse after timeout: wire replies have no sequence ID. */
	else if (sample) *sample = s->reply;
	spin_unlock_irqrestore(&s->lock, flags);
	return ret;
}
static int query_write(void *ctx, u64 cmd)
{
	return query_exchange(ctx, 0x12, cmd, NULL);
}
static int query_read(void *ctx, struct yoga_ucsi_sample *sample)
{
	return query_exchange(ctx, 0x11, 0, sample);
}
static void query_pause(void *ctx) { msleep(100); }
static void query_deadline(void *ctx)
{
	struct query *s = ctx;
	s->deadline = jiffies + 5 * HZ;
}
static int run_query(struct query *s, u64 cmd, u8 *data, int minimum, int maximum,
		     bool optional)
{
	struct uq_io io = { s, query_write, query_read, query_pause, query_deadline };
	struct yoga_ucsi_sample reply = {};
	int ret = uq_command(&io, cmd, &reply);
	s->commands++;
	dev_info(s->dev, "UCSI QUERY round=%u cmd=%016llx CCI=%08x result=%d raw=%*ph\n",
		 s->attempts, cmd, reply.cci, ret, 16, reply.prefix + 16);
	if (ret == -EOPNOTSUPP && optional) return 0;
	if (ret < 0) return ret;
	if (ret < minimum || ret > maximum) return -EPROTO;
	if (data) memcpy(data, reply.prefix + 16, ret);
	return ret;
}
static int query_collect(struct query *s)
{
	unsigned int i, recipient, partner;
	u32 status;
	int ret;
	if (!s->initialized) {
		ret = run_query(s, UQ_NOTIFY, NULL, 0, 0, false);
		if (ret < 0) return ret;
		ret = run_query(s, 6, s->cap, 16, 16, false);
		if (ret < 0) return ret;
		s->connectors = s->cap[4];
		if (!s->connectors || s->connectors > 3) return -ERANGE;
		s->features = get_unaligned_le16(s->cap + 5);
		for (i = 0; i < s->connectors; i++) {
			ret = run_query(s, (u64)(i + 1) << 16 | 7, s->caps[i], 2, 4, false);
			if (ret < 0) return ret;
		}
		s->initialized = true;
	}
	memset(s->status, 0, sizeof(s->status));
	memset(s->cable, 0, sizeof(s->cable));
	memset(s->status_len, 0, sizeof(s->status_len));
	memset(s->cable_len, 0, sizeof(s->cable_len));
	memset(s->cam, 0xff, sizeof(s->cam));
	for (i = 0; i < s->connectors; i++) {
		ret = run_query(s, (u64)(i + 1) << 16 | 0x12, s->status[i], 9, 16, false);
		if (ret < 0) return ret;
		s->status_len[i] = ret;
		status = get_unaligned_le32(s->status[i]);
		partner = (status >> 29) & 7;
		dev_info(s->dev, "UCSI STATUS connector=%u connected=%u partner=%u power_mode=%u flags=%02x\n",
			 i + 1, !!(status & BIT(19)), partner, (status >> 16) & 7, (status >> 21) & 255);
		/* Query accessory details only for a connected UFP / cable+UFP.
		 * No assumption that PAN port 0 is UCSI connector 1. */
		if (!(status & BIT(19)) || (partner != 2 && partner != 4)) continue;
		if (s->features & BIT(5)) {
			ret = run_query(s, (u64)(i + 1) << 16 | 0x11, s->cable[i], 5, 8, true);
			if (ret < 0) return ret;
			s->cable_len[i] = ret;
		}
		if (!(s->features & BIT(2))) continue;
		ret = run_query(s, (u64)(i + 1) << 16 | 0x0e, &s->cam[i], 1, 1, true);
		if (ret < 0) return ret;
		/* One first mode per SOP / SOP-prime recipient; no mode entry. */
		for (recipient = 1; recipient <= 2; recipient++) {
			ret = run_query(s, (u64)(i + 1) << 24 | (u64)recipient << 16 | 0x0c,
					NULL, 0, 6, true);
			if (ret < 0) return ret;
		}
	}
	return 0;
}
/* Caller holds control. Notifications may race any command/ACK; polling CCI
 * after commands, rather than clearing an IRQ flag, preserves pending work.
 * All three advertised connectors are serviced without changing their roles. */
static int query_drain(struct query *s)
{
	struct uq_io io = { s, query_write, query_read, query_pause, query_deadline };
	struct yoga_ucsi_sample reply;
	unsigned int i;
	int connector, ret;
	for (i = 0; i < 8; i++) {
		connector = uq_pending(&io, s->connectors);
		if (connector <= 0) return connector;
		if (s->events >= 64) return -ENOSPC;
		memset(&reply, 0, sizeof(reply));
		ret = uq_command_kind(&io, (u64)connector << 16 | 0x12, &reply, connector);
		s->commands++;
		dev_info(s->dev, "UCSI EVENT connector=%d CCI=%08x status_len=%d raw=%*ph combined_ack=%u\n",
			 connector, reply.cci, ret, 16, reply.prefix + 16, ret >= 0);
		if (ret < 0) return ret;
		s->events++;
	}
	/* Yield after eight events. The next bounded poll reads CCI again. */
	return 0;
}
static void query_fail(struct query *s, int ret)
{
	unsigned long flags;
	spin_lock_irqsave(&s->lock, flags);
	if (ret && !s->error) s->error = ret;
	s->policy_active = false;
	spin_unlock_irqrestore(&s->lock, flags);
}
static void policy_worker(struct work_struct *work)
{
	struct query *s = container_of(to_delayed_work(work), struct query, policy_work);
	int ret = 0;
	mutex_lock(&s->control);
	if (!s->policy_active) goto out;
	if (READ_ONCE(s->error)) ret = READ_ONCE(s->error);
	else if (time_after_eq(jiffies, s->policy_deadline) || s->polls >= 300)
		ret = -ETIMEDOUT;
	if (!ret) {
		s->polls++;
		s->group_deadline = jiffies + 20 * HZ;
		if (time_after(s->group_deadline, s->policy_deadline))
			s->group_deadline = s->policy_deadline;
		ret = query_drain(s);
	}
	if (ret) {
		query_fail(s, ret);
		dev_err(s->dev, "UCSI POLICY stopped error=%d; no retry\n", ret);
	} else {
		schedule_delayed_work(&s->policy_work, HZ);
	}
out:
	mutex_unlock(&s->control);
}
static void policy_stop(struct query *s)
{
	mutex_lock(&s->control);
	query_fail(s, 0);
	mutex_unlock(&s->control);
	cancel_delayed_work_sync(&s->policy_work);
}
static ssize_t policy_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct query *s = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);
	if (ret) return ret;
	if (!enable) { policy_stop(s); return count; }
	if (!x1_diag_nhi_complete()) return -EPERM;
	mutex_lock(&s->control);
	ret = READ_ONCE(s->error);
	if (ret) goto out;
	if (!READ_ONCE(s->service_up)) { ret = -ENOTCONN; goto out; }
	/* Deliberately limited to the exact observed 1.2 capability combination.
	 * DB05 is Linux's supported notification mask for features=0004. */
	if (!s->initialized || s->connectors != 3 || s->features != 0x0004) {
		ret = -EPROTONOSUPPORT; goto out;
	}
	if (atomic_cmpxchg(&policy_once, 0, 1)) { ret = -EPERM; goto out; }
	s->policy_attempted = true;
	s->policy_deadline = jiffies + 300 * HZ;
	s->group_deadline = jiffies + 20 * HZ;
	ret = run_query(s, UQ_NOTIFY_READY, NULL, 0, 0, false);
	dev_info(s->dev, "UCSI POLICY notify_mask=db05 enable_result=%d; no PPM reset or mode/role command\n", ret);
	if (!ret) ret = query_drain(s);
	if (ret) query_fail(s, ret);
	else {
		s->policy_active = true;
		schedule_delayed_work(&s->policy_work, HZ);
	}
out:
	mutex_unlock(&s->control);
	if (ret) return ret;
	return count;
}
static DEVICE_ATTR_WO(policy);
static ssize_t query_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct query *s = dev_get_drvdata(dev);
	unsigned long flags;
	bool enable;
	int ret = kstrtobool(buf, &enable);
	if (ret || !enable) return -EINVAL;
	if (!x1_diag_nhi_complete()) return -EPERM;
	mutex_lock(&s->control);
	spin_lock_irqsave(&s->lock, flags);
	if (s->error) ret = s->error;
	else if (!s->service_up) ret = -ENOTCONN;
	else if (s->attempts >= 3) ret = -ENOSPC;
	if (!ret) s->attempts++;
	spin_unlock_irqrestore(&s->lock, flags);
	if (ret) goto out;
	s->group_deadline = jiffies + 60 * HZ;
	if (s->policy_attempted) {
		if (!s->policy_active) { ret = -ESHUTDOWN; goto out; }
		if (time_after(s->group_deadline, s->policy_deadline))
			s->group_deadline = s->policy_deadline;
		ret = query_drain(s);
		if (ret) goto failed;
	}
	ret = query_collect(s);
	if (!ret && s->policy_active) ret = query_drain(s);
failed:
	spin_lock_irqsave(&s->lock, flags);
	if (s->error) ret = s->error;
	if (ret) s->error = ret;
	spin_unlock_irqrestore(&s->lock, flags);
out:
	mutex_unlock(&s->control);
	if (ret) return ret;
	return count;
}
static DEVICE_ATTR_WO(query);
static ssize_t result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct query *s = dev_get_drvdata(dev);
	unsigned long flags;
	unsigned int i;
	int n;
	mutex_lock(&s->control);
	spin_lock_irqsave(&s->lock, flags);
	n = sysfs_emit(buf, "UCSI_QUERY service_up=%u rounds=%u commands=%u initialized=%u connectors=%u features=%04x notifications=%u errno=%d\n",
		s->service_up, s->attempts, s->commands, s->initialized, s->connectors, s->features,
		s->notifications, s->error);
	spin_unlock_irqrestore(&s->lock, flags);
	n += sysfs_emit_at(buf, n, "UCSI_POLICY attempted=%u active=%u polls=%u events=%u requested_mask=%04x\n",
		 s->policy_attempted, s->policy_active, s->polls, s->events,
		 s->policy_attempted ? 0xdb05 : 1);
	n += sysfs_emit_at(buf, n, "UCSI CAP version=0120 raw=%*ph\n", 16, s->cap);
	for (i = 0; i < min_t(unsigned int, s->connectors, 3); i++) {
		n += sysfs_emit_at(buf, n, "UCSI connector=%u cap=%*ph status_len=%u status=%*ph\n",
			 i + 1, 4, s->caps[i], s->status_len[i], 16, s->status[i]);
		n += sysfs_emit_at(buf, n, "UCSI connector=%u cable_len=%u cable=%*ph current_cam=%02x\n",
			 i + 1, s->cable_len[i], 8, s->cable[i], s->cam[i]);
	}
	mutex_unlock(&s->control);
	return n;
}
static DEVICE_ATTR_RO(result);
static struct attribute *query_attrs[] = { &dev_attr_query.attr, &dev_attr_result.attr,
	&dev_attr_policy.attr, NULL };
static const struct attribute_group query_group = { .attrs = query_attrs };
static int query_probe(struct auxiliary_device *adev, const struct auxiliary_device_id *id)
{
	struct device *dev = &adev->dev;
	struct query *s;
	int ret;
	if (!x1_diag_board_allowed() || !of_property_read_bool(of_root, "birk,usb4-retimer-test"))
		return -EPERM;
	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s) return -ENOMEM;
	s->dev = dev;
	mutex_init(&s->control); spin_lock_init(&s->lock); init_completion(&s->done);
	INIT_DELAYED_WORK(&s->policy_work, policy_worker);
	dev_set_drvdata(dev, s);
	s->client = devm_pmic_glink_client_alloc(dev, 32779, query_callback, query_pdr, s);
	if (IS_ERR(s->client)) return PTR_ERR(s->client);
	ret = device_add_group(dev, &query_group);
	if (ret) return ret;
	pmic_glink_client_register(s->client);
	return 0;
}
static void query_remove(struct auxiliary_device *adev)
{
	device_remove_group(&adev->dev, &query_group);
	policy_stop(dev_get_drvdata(&adev->dev));
}
static const struct auxiliary_device_id query_ids[] = { { .name = "pmic_glink.ucsi" }, {} };
static struct auxiliary_driver query_driver = {
	.name = "yoga_ucsi_query", .probe = query_probe, .remove = query_remove, .id_table = query_ids,
	.driver = { .suppress_bind_attrs = true },
};
module_auxiliary_driver(query_driver);
MODULE_LICENSE("GPL");
