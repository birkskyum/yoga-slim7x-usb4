// SPDX-License-Identifier: GPL-2.0-only
/* Private Yoga diagnostic. Based on the protocol in pmic_glink_altmode.c
 * (Linux Foundation / Linaro). v9 additionally configures the left-rear
 * PS8830 through Type-C APIs BEFORE ACK. v14 adds a guarded manual request
 * to the existing MCU/NHI owner after an ACKed USB4 PAN; no storage/tunnels.
 */
#include <linux/auxiliary_bus.h>
#include <linux/completion.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <linux/soc/qcom/pdr.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/soc/qcom/x1-usb4-diag.h>
#include <linux/usb/pd.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_retimer.h>
#include <linux/usb/typec_tbt.h>
#include "yoga_pan_packet.h"
#include "yoga_pan_plan.h"
#include "yoga_link_plan.h"

#define PAN_LIMIT 32
struct observer {
	struct device *dev;
	struct pmic_glink_client *client;
	struct completion ack;
	struct mutex request_lock, control_lock;
	spinlock_t lock;
	struct work_struct work;
	struct yoga_pan events[PAN_LIMIT];
	unsigned int count, processed, ack_len;
	int error;
	bool service_up, active, attempted, awaiting;
	struct typec_switch *sw;
	struct typec_retimer *retimer;
	struct typec_altmode tbt_alt;
	unsigned int configured, tunneling;
	bool link_attempted;
	int link_result;
	unsigned int link_count;
};

static int configure_port(struct observer *o, const struct yoga_pan *e)
{
	struct yoga_port_plan plan;
	struct typec_retimer_state state = { 0 };
	struct enter_usb_data usb4 = { 0 };
	struct typec_thunderbolt_data tbt = { 0 };
	enum typec_orientation orientation;
	unsigned long flags;
	u32 speed, active;
	int ret;
	if (e->port != 0) {
		dev_info(o->dev, "PAN port=%u receipt-only ACK; retimer outside test remains untouched\n", e->port);
		return 0;
	}
	ret = yoga_plan(e, &plan);
	if (ret) return ret;
	if (!x1_diag_nhi_complete() || !o->sw || !o->retimer) return -EPERM;
	orientation = plan.orientation == 2 ? TYPEC_ORIENTATION_NONE :
		plan.orientation == 1 ? TYPEC_ORIENTATION_REVERSE : TYPEC_ORIENTATION_NORMAL;
	ret = typec_switch_set(o->sw, orientation);
	if (ret) return ret;
	switch (plan.mode) {
	case YOGA_SAFE: state.mode = TYPEC_STATE_SAFE; break;
	case YOGA_USB3: state.mode = TYPEC_STATE_USB; break;
	case YOGA_USB4:
		state.mode = TYPEC_MODE_USB4;
		usb4.eudo = FIELD_PREP(EUDO_USB_MODE_MASK, EUDO_USB_MODE_USB4) |
			FIELD_PREP(EUDO_CABLE_SPEED_MASK, plan.speed ? EUDO_CABLE_SPEED_USB4_GEN3 : EUDO_CABLE_SPEED_USB4_GEN2) |
			FIELD_PREP(EUDO_CABLE_TYPE_MASK, plan.cable_type);
		state.data = &usb4;
		break;
	case YOGA_TBT:
		state.mode = TYPEC_TBT_MODE;
		state.alt = &o->tbt_alt;
		state.data = &tbt;
		speed = plan.speed ? TBT_CABLE_10_AND_20GBPS : TBT_CABLE_USB3_PASSIVE;
		active = plan.cable_type ? TBT_CABLE_RETIMER | TBT_SET_CABLE_ROUNDED(plan.rounded) : 0;
		tbt.device_mode = TBT_MODE | TBT_SET_ADAPTER(TBT_ADAPTER_TBT3);
		tbt.cable_mode = TBT_MODE | TBT_SET_CABLE_SPEED(speed) | active |
			(plan.cable_type ? TBT_CABLE_ACTIVE_PASSIVE : 0);
		tbt.enter_vdo = TBT_MODE | TBT_ENTER_MODE_CABLE_SPEED(speed) | active |
			(plan.cable_type ? TBT_ENTER_MODE_ACTIVE_CABLE : 0);
		break;
	default: return -EINVAL;
	}
	ret = typec_retimer_set(o->retimer, &state);
	dev_info(o->dev, "PAN APPLY port=0 mode=%u orientation=%u ret=%d candidate_connect=%08x NOT SENT\n",
		 plan.mode, plan.orientation, ret, plan.connect_word);
	if (!ret) {
		spin_lock_irqsave(&o->lock, flags);
		o->configured++;
		if (plan.connect_word) o->tunneling++;
		spin_unlock_irqrestore(&o->lock, flags);
	}
	return ret;
}

static int request(struct observer *o, u32 cmd, u32 arg)
{
	struct { struct pmic_glink_hdr hdr; __le32 cmd, arg, reserved; } req = {
		.hdr = { cpu_to_le32(32780), cpu_to_le32(1), cpu_to_le32(0x15) },
		.cmd = cpu_to_le32(cmd), .arg = cpu_to_le32(arg),
	};
	unsigned long flags;
	int ret;
	mutex_lock(&o->request_lock);
	spin_lock_irqsave(&o->lock, flags);
	if (!o->active || !o->service_up || o->error) {
		ret = o->error ?: -ENOTCONN;
		spin_unlock_irqrestore(&o->lock, flags);
		goto out;
	}
	reinit_completion(&o->ack);
	o->awaiting = true;
	spin_unlock_irqrestore(&o->lock, flags);
	ret = pmic_glink_send(o->client, &req, sizeof(req));
	if (!ret && !wait_for_completion_timeout(&o->ack, 5 * HZ))
		ret = -ETIMEDOUT;
	spin_lock_irqsave(&o->lock, flags);
	o->awaiting = false;
	if (!o->service_up) ret = -ENOTCONN;
	if (o->error) ret = o->error;
	if (ret) { o->error = ret; o->active = false; }
	spin_unlock_irqrestore(&o->lock, flags);
	dev_info(o->dev, "PAN command=%02x arg=%u transport_ack_len=%u ret=%d\n",
		 cmd, arg, READ_ONCE(o->ack_len), ret);
out:
	mutex_unlock(&o->request_lock);
	return ret;
}

static void worker(struct work_struct *work)
{
	struct observer *o = container_of(work, struct observer, work);
	struct yoga_pan e;
	unsigned long flags;
	unsigned int index;
	int ret;
	for (;;) {
		spin_lock_irqsave(&o->lock, flags);
		if (!o->active || o->error || o->processed == o->count) {
			spin_unlock_irqrestore(&o->lock, flags);
			return;
		}
		index = o->processed;
		e = o->events[index];
		spin_unlock_irqrestore(&o->lock, flags);
		dev_info(o->dev, "PAN #%u port=%u orientation_raw=%02x mux_raw=%02x vid=%04x svid_header=%04x svid_payload=%04x ext=%*ph\n",
			 index, e.port, e.orientation_raw, e.mux_raw, e.vid,
			 e.svid_header, e.svid_payload, 8, e.ext);
		ret = configure_port(o, &e);
		if (ret) {
			spin_lock_irqsave(&o->lock, flags);
			o->error = ret; o->active = false;
			spin_unlock_irqrestore(&o->lock, flags);
			dev_err(o->dev, "PAN APPLY FAILED port=%u ret=%d; no ACK, no retry\n", e.port, ret);
			return;
		}
		if (request(o, 0x11, e.port)) return;
		spin_lock_irqsave(&o->lock, flags);
		o->processed++;
		spin_unlock_irqrestore(&o->lock, flags);
	}
}

static void callback(const void *data, size_t len, void *priv)
{
	struct observer *o = priv;
	const u8 *p = data;
	struct yoga_pan e;
	unsigned long flags;
	u32 opcode;
	int ret;
	if (len < 12) return;
	if (get_unaligned_le32(p) != 32780) return;
	opcode = get_unaligned_le32(p + 8);
	spin_lock_irqsave(&o->lock, flags);
	if (opcode == 0x15 && get_unaligned_le32(p + 4) == 1) {
		if (o->awaiting && len <= 64) {
			o->ack_len = len;
			complete(&o->ack);
		}
		spin_unlock_irqrestore(&o->lock, flags);
		return;
	}
	if ((opcode & 0xff) != 0x16 || !o->active) {
		spin_unlock_irqrestore(&o->lock, flags);
		return;
	}
	ret = yoga_pan_decode(p, len, &e);
	if (ret || o->count == PAN_LIMIT) {
		dev_warn(o->dev, "PAN capture stopped: len=%zu opcode=%08x prefix=%*ph error=%d\n",
			 len, opcode, (int)min_t(size_t, len, 32), p, ret ?: -ENOSPC);
		o->error = ret ?: -ENOSPC;
		o->active = false;
	} else {
		o->events[o->count++] = e;
		schedule_work(&o->work);
	}
	spin_unlock_irqrestore(&o->lock, flags);
}

static void pdr_notify(void *priv, int state)
{
	struct observer *o = priv;
	unsigned long flags;
	spin_lock_irqsave(&o->lock, flags);
	o->service_up = state == SERVREG_SERVICE_STATE_UP;
	if (!o->service_up && o->active) {
		o->error = -ENOTCONN; o->active = false;
		if (o->awaiting) complete(&o->ack);
	}
	spin_unlock_irqrestore(&o->lock, flags);
}

static ssize_t result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct observer *o = dev_get_drvdata(dev);
	unsigned long flags;
	unsigned int i;
	int n;
	spin_lock_irqsave(&o->lock, flags);
	n = sysfs_emit(buf, "service_up=%u active=%u attempted=%u events=%u processed=%u configured=%u negotiated_events=%u errno=%d\n",
		 o->service_up, o->active, o->attempted, o->count, o->processed, o->configured, o->tunneling, o->error);
	for (i = 0; i < o->count; i++) {
		struct yoga_pan *e = &o->events[i];
		n += sysfs_emit_at(buf, n, "#%u port=%u orient=%02x mux=%02x vid=%04x svid=%04x/%04x ext=%*ph\n",
			 i, e->port, e->orientation_raw, e->mux_raw, e->vid,
			 e->svid_header, e->svid_payload, 8, e->ext);
	}
	spin_unlock_irqrestore(&o->lock, flags);
	return n;
}
static DEVICE_ATTR_RO(result);

static ssize_t observe_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct observer *o = dev_get_drvdata(dev);
	unsigned long flags;
	bool enable;
	int ret = kstrtobool(buf, &enable);
	if (ret) return ret;
	mutex_lock(&o->control_lock);
	spin_lock_irqsave(&o->lock, flags);
	if (enable && !x1_diag_nhi_complete()) {
		ret = -EPERM;
	} else if (enable && (o->attempted || !o->service_up)) {
		ret = o->attempted ? -EPERM : -ENOTCONN;
	} else if (enable) {
		o->attempted = o->active = true;
	} else {
		o->active = false;
	}
	spin_unlock_irqrestore(&o->lock, flags);
	if (!ret && enable) ret = request(o, 0x10, 0); /* X1 legacy PAN, not Surface V2 */
	if (!enable || ret) cancel_work_sync(&o->work);
	mutex_unlock(&o->control_lock);
	if (ret) return ret;
	return count;
}
static DEVICE_ATTR_WO(observe);

struct link_token { struct observer *o; unsigned int count; };
static bool link_valid(void *ctx)
{
	struct link_token *t = ctx;
	struct observer *o = t->o;
	unsigned long flags;
	bool valid;
	spin_lock_irqsave(&o->lock, flags);
	valid = o->active && o->service_up && !o->error &&
		o->count == t->count && o->count == o->processed;
	spin_unlock_irqrestore(&o->lock, flags);
	return valid;
}
static ssize_t link_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct observer *o = dev_get_drvdata(dev);
	struct link_token token = { .o = o };
	unsigned long flags;
	u32 word = 0;
	int ret;
	if (!sysfs_streq(buf, "1")) return -EINVAL;
	dev_emerg(dev, "V33 LINK STORE BEGIN\n");
	mutex_lock(&o->control_lock);
	spin_lock_irqsave(&o->lock, flags);
	if (o->link_attempted || !o->active || !o->service_up || o->error) {
		ret = -EPERM;
	} else {
		o->link_attempted = true;
		token.count = o->count;
		o->link_count = o->count;
		ret = yoga_link_word(o->events, o->count, o->processed, &word);
	}
	spin_unlock_irqrestore(&o->lock, flags);
	if (!ret) {
		dev_emerg(dev, "V33 LINK DISPATCH BEGIN word=%08x\n", word);
		ret = x1_diag_link_run(word, link_valid, &token);
		dev_emerg(dev, "V33 LINK DISPATCH END errno=%d\n", ret);
	}
	/* Native CM stays live for the gated storage check; stop on any failure. */
	if (ret || !IS_ENABLED(CONFIG_USB4_X1_NATIVE))
		x1_diag_link_stop();
	WRITE_ONCE(o->link_result, ret);
	mutex_unlock(&o->control_lock);
	dev_emerg(dev, "V33 LINK STORE END errno=%d\n", ret);
	return ret ?: count;
}
static DEVICE_ATTR_WO(link);
static ssize_t link_result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct observer *o = dev_get_drvdata(dev);
	return sysfs_emit(buf, "LINK attempted=%u errno=%d; %s\n",
		READ_ONCE(o->link_attempted), READ_ONCE(o->link_result),
		IS_ENABLED(CONFIG_USB4_X1_NATIVE) ? "native PCIe stage; disk result separate" : "no storage or tunnels");
}
static DEVICE_ATTR_RO(link_result);
static ssize_t link_ready_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct observer *o = dev_get_drvdata(dev);
	unsigned long flags;
	u32 word = 0;
	int ret;
	spin_lock_irqsave(&o->lock, flags);
	ret = o->active && o->service_up && !o->error && !o->link_attempted ?
		yoga_link_word(o->events, o->count, o->processed, &word) : -EPERM;
	spin_unlock_irqrestore(&o->lock, flags);
	return sysfs_emit(buf, "LINK_READY ready=%u word=%08x errno=%d\n", !ret, word, ret);
}
static DEVICE_ATTR_RO(link_ready);
static ssize_t connection_valid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct observer *o = dev_get_drvdata(dev);
	unsigned long flags;
	u32 word = 0;
	bool valid;
	spin_lock_irqsave(&o->lock, flags);
	valid = o->active && o->service_up && !o->error && o->link_attempted &&
		!o->link_result && o->count == o->link_count &&
		!yoga_link_word(o->events, o->count, o->processed, &word);
	spin_unlock_irqrestore(&o->lock, flags);
	return sysfs_emit(buf, "CONNECTION_VALID valid=%u\n", valid);
}
static DEVICE_ATTR_RO(connection_valid);
static struct attribute *observer_attrs[] = { &dev_attr_result.attr, &dev_attr_observe.attr,
	&dev_attr_link.attr, &dev_attr_link_result.attr, &dev_attr_link_ready.attr,
	&dev_attr_connection_valid.attr, NULL };
static const struct attribute_group observer_group = { .attrs = observer_attrs };

static void put_providers(void *data)
{
	struct observer *o = data;
	typec_retimer_put(o->retimer);
	typec_switch_put(o->sw);
}

static int observer_probe(struct auxiliary_device *adev, const struct auxiliary_device_id *id)
{
	struct device *dev = &adev->dev;
	struct observer *o;
	struct device_node *connector;
	int ret;
	if (!x1_diag_board_allowed() || !of_property_read_bool(of_root, "birk,usb4-typec-observer"))
		return -EPERM;
	o = devm_kzalloc(dev, sizeof(*o), GFP_KERNEL);
	if (!o) return -ENOMEM;
	o->dev = dev;
	spin_lock_init(&o->lock);
	mutex_init(&o->request_lock); mutex_init(&o->control_lock);
	init_completion(&o->ack); INIT_WORK(&o->work, worker);
	dev_set_drvdata(dev, o);
	connector = of_find_node_by_path("/pmic-glink/connector@0");
	if (!connector) return -ENODEV;
	o->sw = fwnode_typec_switch_get(of_fwnode_handle(connector));
	o->retimer = fwnode_typec_retimer_get(of_fwnode_handle(connector));
	of_node_put(connector);
	if (IS_ERR_OR_NULL(o->sw) || IS_ERR_OR_NULL(o->retimer)) {
		ret = IS_ERR(o->sw) ? PTR_ERR(o->sw) : IS_ERR(o->retimer) ? PTR_ERR(o->retimer) : -ENODEV;
		put_providers(o);
		return dev_err_probe(dev, ret, "left-rear Type-C providers unavailable\n");
	}
	ret = devm_add_action_or_reset(dev, put_providers, o);
	if (ret) return ret;
	o->tbt_alt.svid = USB_TYPEC_TBT_SID;
	o->tbt_alt.mode = TYPEC_TBT_MODE;
	o->tbt_alt.active = 1;
	o->client = devm_pmic_glink_client_alloc(dev, 32780, callback, pdr_notify, o);
	if (IS_ERR(o->client)) return PTR_ERR(o->client);
	ret = devm_device_add_group(dev, &observer_group);
	if (ret) return ret;
	pmic_glink_client_register(o->client);
	dev_info(dev, "Yoga v9 PAN/retimer ready, inactive until manual request. No MCU link commands.\n");
	return 0;
}
static const struct auxiliary_device_id observer_ids[] = { { .name = "pmic_glink.altmode" }, {} };
static struct auxiliary_driver observer_driver = {
	.name = "yoga_typec_observer", .probe = observer_probe, .id_table = observer_ids,
	.driver = { .suppress_bind_attrs = true },
};
module_auxiliary_driver(observer_driver);
MODULE_LICENSE("GPL");
