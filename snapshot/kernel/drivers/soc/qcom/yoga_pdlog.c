// SPDX-License-Identifier: GPL-2.0-only
/* Private v12 Yoga diagnostic. GET wire layout/channel from
 * pmic_pdcharger_ulog.c, copyright Linux Foundation (2019-2022), Linaro (2023).
 * No SET properties, persistent changes, auto-start, or unbounded polling. */
#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <linux/soc/qcom/x1-usb4-diag.h>
#include "yoga_pdlog_policy.h"

static atomic_t attempted_this_boot = ATOMIC_INIT(0);

struct pdlog {
	struct rpmsg_device *rpdev;
	struct mutex control;
	spinlock_t lock;
	struct completion done;
	struct delayed_work work;
	struct pdlog_state state;
	unsigned long deadline;
	unsigned int nonempty;
	char text[PDLOG_SIZE + 1];
};
static int pdlog_callback(struct rpmsg_device *rpdev, void *data, int len,
			 void *priv, u32 addr)
{
	struct pdlog *p = dev_get_drvdata(&rpdev->dev);
	unsigned long flags;
	int ret;
	if (!p) return 0;
	spin_lock_irqsave(&p->lock, flags);
	if (p->state.active && p->state.awaiting && !p->state.error) {
		ret = len < 0 ? -EMSGSIZE : pdlog_decode(data, len, p->text);
		if (pdlog_reply(&p->state, ret)) complete(&p->done);
	}
	spin_unlock_irqrestore(&p->lock, flags);
	return 0;
}
static void pdlog_work(struct work_struct *work)
{
	struct pdlog *p = container_of(to_delayed_work(work), struct pdlog, work);
	unsigned long flags, left, now;
	u8 request[16];
	unsigned int seq;
	bool again, valid;
	char *line, *cursor;
	int ret;
	spin_lock_irqsave(&p->lock, flags);
	if (!pdlog_next(&p->state, time_after_eq(jiffies, p->deadline))) {
		spin_unlock_irqrestore(&p->lock, flags);
		return;
	}
	reinit_completion(&p->done);
	seq = p->state.requests;
	spin_unlock_irqrestore(&p->lock, flags);
	pdlog_request(request);
	/* Nonblocking send: no transport's implicit 15-second wait/retry. */
	ret = rpmsg_trysend(p->rpdev->ept, request, sizeof(request));
	now = jiffies;
	left = time_before(now, p->deadline) ?
		min_t(unsigned long, 5 * HZ, p->deadline - now) : 0;
	if (!ret && (!left || !wait_for_completion_timeout(&p->done, left)))
		ret = -ETIMEDOUT;
	spin_lock_irqsave(&p->lock, flags);
	if (ret) pdlog_stop(&p->state, ret);
	valid = !p->state.error && p->state.replies == seq;
	if (valid && p->text[0]) p->nonempty++;
	ret = p->state.error;
	spin_unlock_irqrestore(&p->lock, flags);
	/* Only this work item sends requests. Callback ignores extra packets;
	 * text cannot change until we schedule the next request below. */
	if (valid) {
		dev_info(&p->rpdev->dev, "PDLOG sample=%u text_bytes=%zu\n", seq, strlen(p->text));
		cursor = p->text;
		while ((line = strsep(&cursor, "\n"))) {
			size_t length = strlen(line), off;
			for (off = 0; off < length; off += 160)
				dev_info(&p->rpdev->dev, "PDLOG[%u] %.*s\n", seq,
					 (int)min_t(size_t, 160, length - off), line + off);
		}
	} else if (ret) {
		dev_warn(&p->rpdev->dev, "PDLOG stopped errno=%d; no retry this boot\n", ret);
	}
	spin_lock_irqsave(&p->lock, flags);
	again = p->state.active && !p->state.error;
	/* Schedule while locked: a concurrent stop clears active before its
	 * cancel_delayed_work_sync(), which then catches any scheduled work. */
	if (again) schedule_delayed_work(&p->work, 2 * HZ);
	spin_unlock_irqrestore(&p->lock, flags);
}
static void pdlog_cancel(struct pdlog *p, int error)
{
	unsigned long flags;
	spin_lock_irqsave(&p->lock, flags);
	pdlog_stop(&p->state, error);
	complete(&p->done);
	spin_unlock_irqrestore(&p->lock, flags);
	cancel_delayed_work_sync(&p->work);
}
static ssize_t capture_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct pdlog *p = dev_get_drvdata(dev);
	unsigned long flags;
	int ret = 0;
	if (!sysfs_streq(buf, "0") && !sysfs_streq(buf, "1")) return -EINVAL;
	mutex_lock(&p->control);
	if (sysfs_streq(buf, "0")) {
		pdlog_cancel(p, 0);
	} else {
		spin_lock_irqsave(&p->lock, flags);
		ret = pdlog_start(&p->state, x1_diag_nhi_complete() &&
				 atomic_cmpxchg(&attempted_this_boot, 0, 1) == 0);
		if (!ret) {
			p->deadline = jiffies + 300 * HZ;
			schedule_delayed_work(&p->work, 0);
		}
		spin_unlock_irqrestore(&p->lock, flags);
	}
	mutex_unlock(&p->control);
	return ret ? ret : count;
}
static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pdlog *p = dev_get_drvdata(dev);
	unsigned long flags;
	int n;
	spin_lock_irqsave(&p->lock, flags);
	n = sysfs_emit(buf, "PDLOG attempted=%u active=%u requests=%u replies=%u nonempty=%u limited=%u errno=%d\n",
		p->state.attempted, p->state.active, p->state.requests, p->state.replies,
		p->nonempty, p->state.limited, p->state.error);
	spin_unlock_irqrestore(&p->lock, flags);
	return n;
}
static DEVICE_ATTR_WO(capture);
static DEVICE_ATTR_RO(status);
static struct attribute *pdlog_attrs[] = { &dev_attr_capture.attr, &dev_attr_status.attr, NULL };
static const struct attribute_group pdlog_group = { .attrs = pdlog_attrs };
static int pdlog_probe(struct rpmsg_device *rpdev)
{
	struct pdlog *p;
	if (!x1_diag_board_allowed() || !of_property_read_bool(of_root, "birk,usb4-retimer-test"))
		return -EPERM;
	p = devm_kzalloc(&rpdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p) return -ENOMEM;
	p->rpdev = rpdev;
	mutex_init(&p->control);
	spin_lock_init(&p->lock);
	init_completion(&p->done);
	INIT_DELAYED_WORK(&p->work, pdlog_work);
	dev_set_drvdata(&rpdev->dev, p);
	return sysfs_create_group(&rpdev->dev.kobj, &pdlog_group);
}
static void pdlog_remove(struct rpmsg_device *rpdev)
{
	struct pdlog *p = dev_get_drvdata(&rpdev->dev);
	sysfs_remove_group(&rpdev->dev.kobj, &pdlog_group);
	/* Draining sysfs first prevents a late start after cancellation. */
	pdlog_cancel(p, -ENOTCONN);
}
static const struct rpmsg_device_id pdlog_ids[] = { { "PMIC_LOGS_ADSP_APPS" }, {} };
static struct rpmsg_driver pdlog_driver = {
	.probe = pdlog_probe, .remove = pdlog_remove, .callback = pdlog_callback,
	.id_table = pdlog_ids,
	.drv = { .name = "yoga_pdlog", .suppress_bind_attrs = true },
};
module_rpmsg_driver(pdlog_driver);
MODULE_LICENSE("GPL");
