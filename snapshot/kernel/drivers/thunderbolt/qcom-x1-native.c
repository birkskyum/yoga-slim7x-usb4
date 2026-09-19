// SPDX-License-Identifier: GPL-2.0-only
/*
 * Private Yoga/X1 native-CM integration and read-only storage candidate.
 * Uses Linux's connection manager and Jim Martin's Qualcomm NHI IRQ adapter.
 * The existing Yoga frontend exclusively owns power, X1 startup and firmware.
 * No diagnostic rings are ever allocated in this mode. No Surface startup.
 * One attempt, retained owner/device/domain until cold shutdown; no PM/retry.
 * PCIe approval requires the private PCI0 bridge and exact live LaCie identity.
 * v24 emergency-severity progress stays visible at console_loglevel=3.
 * These private markers do not indicate a panic or prove disk access.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "tb.h"
#include "nhi.h"
#include "nhi_regs.h"
#include "qcom-usb4-nhi.h"
#include "x1-native.h"
#include "x1-native-policy.h"
#include "x1-pcie.h"

struct x1_native {
	struct qcom_usb4_nhi qnhi;
	struct tb *tb;
	int (*connect)(void *, u32);
	void *connect_ctx;
	bool live, attempted, stopped, allow_pcie;
	/* Updated and observed only while holding tb->lock. */
	unsigned int scan_results;
	int scan_error;
	bool scan_preconfig;
	const char *scan_stage;
};
static DEFINE_MUTEX(native_lock);
static struct x1_native *owner;
static void native_expire(struct work_struct *work);
static DECLARE_DELAYED_WORK(native_watchdog, native_expire);

bool x1_native_test(const struct tb *tb)
{
	return tb && tb->nhi && tb->nhi->dev &&
		of_device_is_compatible(tb->nhi->dev->of_node, "birk,yoga-x1-usb4-mcu-test");
}

bool x1_native_pcie_authorizing(const struct tb *tb)
{
	struct x1_native *n = READ_ONCE(owner);
	return n && n->tb == tb && READ_ONCE(n->live) &&
		!READ_ONCE(n->stopped) && READ_ONCE(n->allow_pcie);
}

static int no_suspend(struct tb_nhi *nhi) { return -EBUSY; }
static const struct tb_nhi_ops native_ops = {
	.init_interrupts = qcom_usb4_nhi_init_interrupts,
	.request_ring_irq = qcom_usb4_nhi_request_ring_irq,
	.release_ring_irq = qcom_usb4_nhi_release_ring_irq,
	.ring_interrupt_mask = qcom_usb4_nhi_ring_interrupt_mask,
	.disable_interrupts = qcom_usb4_nhi_disable_interrupts,
	.runtime_suspend = no_suspend,
};

int x1_native_guard_router(struct tb *tb, u64 route, const u32 *header)
{
	u32 uid[2];
	int ret;
	if (!x1_native_test(tb))
		return 0;
	if (!header) return -EPERM;
	ret = x1_native_header_allowed(route, header);
	dev_emerg(tb->nhi->dev, "NATIVE GUARD header route=%llx words=%08x %08x %08x %08x %08x errno=%d\n",
		 route, header[0], header[1], header[2], header[3], header[4], ret);
	if (ret || !route)
		return ret;
	dev_emerg(tb->nhi->dev, "NATIVE GUARD uid-read BEGIN route=%llx offset=7 words=2\n", route);
	ret = tb_cfg_read(tb->ctl, uid, route, 0, TB_CFG_SWITCH, 7, 2);
	if (ret) {
		dev_emerg(tb->nhi->dev, "NATIVE GUARD uid-read END route=%llx errno=%d\n", route, ret);
		return ret;
	}
	ret = x1_native_uid_allowed(route, (u64)uid[1] << 32 | uid[0]);
	dev_emerg(tb->nhi->dev, "NATIVE GUARD uid-read END route=%llx raw=%08x %08x native=%016llx expected=%016llx errno=%d\n",
		 route, uid[0], uid[1], (u64)uid[1] << 32 | uid[0], X1_NATIVE_LACIE_UID, ret);
	return ret;
}

/* No native_lock here: the connect caller holds it while topology work runs.
 * tb_scan_port() and our polling/rescan path both hold the domain mutex.
 */
void x1_native_scan_result(struct tb *tb, u64 route, unsigned int port,
			   const char *stage, int error, bool preconfig)
{
	struct x1_native *n = READ_ONCE(owner);
	if (!x1_native_test(tb)) return;
	dev_emerg(tb->nhi->dev, "NATIVE ENUM route=%llx port=%u stage=%s errno=%d preconfig=%u\n",
		 route, port, stage, error, preconfig);
	if (!n || n->tb != tb || !READ_ONCE(n->live) || !READ_ONCE(n->attempted) || READ_ONCE(n->stopped) ||
	    route != 2 || port != 2) return;
	/* A later duplicate hotplug event must not erase a terminal refusal. */
	if (n->scan_results && n->scan_error && !n->scan_preconfig) return;
	n->scan_results++;
	n->scan_error = error;
	n->scan_preconfig = preconfig;
	n->scan_stage = stage;
}

/* isolation(), exact IRQ and board checks are performed by the caller first. */
int x1_native_start(struct device *dev, void __iomem *router, int irq)
{
	struct x1_native *n;
	struct tb_nhi *nhi;
	unsigned int i;
	int ret;
	if (!dev || !router || irq <= 0 ||
	    !of_property_read_bool(of_root, "birk,usb4-native-cm-test"))
		return -EPERM;
	mutex_lock(&native_lock);
	if (owner) { ret = -EALREADY; goto out; }
	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n) { ret = -ENOMEM; goto out; }
	owner = n; /* One owner on every exit, including partial failure. */
	nhi = &n->qnhi.nhi;
	nhi->dev = get_device(dev);
	nhi->dma_dev = dev;
	nhi->iobase = router + 0x3f000;
	nhi->ops = &native_ops;
	nhi->hop_count = 3;
	spin_lock_init(&nhi->lock);
	init_completion(&nhi->domain_released);
	if (readl(nhi->iobase + REG_CAPS) != 3) { ret = -ENODEV; goto out; }
	for (i = 0; i < 3; i++) {
		if ((readl(nhi->iobase + REG_TX_OPTIONS_BASE + i * 32) |
		     readl(nhi->iobase + REG_RX_OPTIONS_BASE + i * 32)) & RING_FLAG_ENABLE) {
			ret = -EBUSY;
			goto out;
		}
	}
	nhi->tx_rings = kcalloc(3, sizeof(*nhi->tx_rings), GFP_KERNEL);
	nhi->rx_rings = kcalloc(3, sizeof(*nhi->rx_rings), GFP_KERNEL);
	if (!nhi->tx_rings || !nhi->rx_rings) { ret = -ENOMEM; goto out; }
	ret = qcom_usb4_nhi_prepare(&n->qnhi, 0x40000, 3, QCOM_USB4_IRQ_32, irq);
	if (ret) goto out;
	ret = qcom_usb4_nhi_init_interrupts(nhi);
	if (ret) goto mask;
	/* Do not call nhi_probe(): it resets interfaces and replaces dev drvdata.
	 * The existing frontend must retain its result, connect and power owner.
	 */
	n->tb = tb_probe(nhi);
	if (!n->tb) { ret = -ENOMEM; goto mask; }
	ret = tb_domain_add(n->tb, false);
	if (ret) goto mask;
	pm_runtime_forbid(&n->tb->dev);
	pm_runtime_forbid(dev);
	n->live = true;
	schedule_delayed_work(&native_watchdog, msecs_to_jiffies(600000));
	dev_emerg(dev, "NATIVE CM READY: single owner, root configured; no PCIe tunnel yet. Not a disk-read PASS.\n");
	goto out;
mask:
	qcom_usb4_nhi_disable_interrupts(nhi);
	/* Keep domain, rings and DMA allocations; no uncertain teardown/retry. */
out:
	mutex_unlock(&native_lock);
	return ret;
}

int x1_native_register_connect(int (*connect)(void *, u32), void *ctx)
{
	int ret = 0;
	mutex_lock(&native_lock);
	if (!owner || !owner->live || owner->stopped || owner->connect || !connect || !ctx)
		ret = -EPERM;
	else { owner->connect = connect; owner->connect_ctx = ctx; }
	mutex_unlock(&native_lock);
	return ret;
}

/* Called once with native_lock held, after control enumeration has completed. */
static int x1_native_prepare_storage(struct x1_native *n, bool (*valid)(void *), void *ctx)
{
	struct tb_switch *sw;
	int ret;
	if (!valid(ctx)) return -ENOTCONN;
	dev_emerg(n->qnhi.nhi.dev, "NATIVE PCI0 START BEGIN\n");
	ret = x1_pcie_start(n->qnhi.nhi.iobase - 0x3f000);
	dev_emerg(n->qnhi.nhi.dev, "NATIVE PCI0 START END errno=%d\n", ret);
	if (ret) return ret;
	mutex_lock(&n->tb->lock);
	sw = tb_switch_find_by_route(n->tb, 2);
	if (!sw) { ret = -ENODEV; goto unlock; }
	ret = x1_native_uid_allowed(2, sw->uid);
	if (!ret && (!valid(ctx) || sw->is_unplugged || !sw->link_usb4 || sw->authorized ||
		    !tb_switch_find_port(sw, TB_TYPE_PCIE_UP))) ret = -ENODEV;
	if (!ret) {
		WRITE_ONCE(n->allow_pcie, true);
		dev_emerg(n->qnhi.nhi.dev, "NATIVE PCIE TUNNEL BEGIN\n");
		ret = tb_domain_approve_switch(n->tb, sw);
		WRITE_ONCE(n->allow_pcie, false);
		if (!ret) sw->authorized = 1;
		dev_emerg(n->qnhi.nhi.dev, "NATIVE PCIE TUNNEL ret=%d; storage driver still absent\n", ret);
	}
	tb_switch_put(sw);
unlock:
	mutex_unlock(&n->tb->lock);
	if (ret) return ret;
	if (!valid(ctx)) return -ENOTCONN;
	dev_emerg(n->qnhi.nhi.dev, "NATIVE PCI0 TRAIN BEGIN\n");
	ret = x1_pcie_train();
	dev_emerg(n->qnhi.nhi.dev, "NATIVE PCI0 TRAIN END errno=%d\n", ret);
	if (ret) return ret;
	if (!valid(ctx)) return -ENOTCONN;
	dev_emerg(n->qnhi.nhi.dev, "NATIVE PCI0 SCAN BEGIN\n");
	ret = x1_pcie_scan();
	dev_emerg(n->qnhi.nhi.dev, "NATIVE PCI0 SCAN END errno=%d\n", ret);
	if (!ret && !valid(ctx)) ret = -ENOTCONN;
	return ret;
}

int x1_native_connect(u32 word, bool (*valid)(void *), void *ctx)
{
	struct x1_native *n;
	struct tb_switch *sw;
	unsigned long deadline;
	bool rescanned = false;
	unsigned int lock_busy = 0;
	int ret = x1_native_connect_word_allowed(word);
	if (ret || !valid) return ret ?: -EINVAL;
	mutex_lock(&native_lock);
	n = owner;
	if (!n || !n->live || n->stopped || n->attempted || !n->connect) {
		ret = -EPERM;
		goto out;
	}
	n->attempted = true;
	if (!valid(ctx)) { ret = -ENOTCONN; goto out; }
	dev_emerg(n->qnhi.nhi.dev, "NATIVE CONNECT BEGIN word=%08x; one MCU command, at most one preconfiguration rescan\n", word);
	ret = n->connect(n->connect_ctx, word);
	dev_emerg(n->qnhi.nhi.dev, "NATIVE CONNECT END errno=%d; command ACK is not disk data\n", ret);
	if (ret) goto out;
	deadline = jiffies + msecs_to_jiffies(20000);
	ret = -ETIMEDOUT;
	do {
		if (!valid(ctx)) { ret = -ENOTCONN; break; }
		/* Never wait unbounded behind an in-progress topology operation. */
		if (mutex_trylock(&n->tb->lock)) {
			if (n->scan_results && n->scan_error && !n->scan_preconfig) {
				ret = n->scan_error;
				mutex_unlock(&n->tb->lock);
				break;
			}
			sw = tb_switch_find_by_route(n->tb, 2);
			if (sw) {
				ret = x1_native_uid_allowed(2, sw->uid);
				if (!ret && (sw->is_unplugged || !sw->link_usb4 ||
				    !tb_switch_find_port(sw, TB_TYPE_PCIE_UP))) ret = -ENODEV;
				if (!ret)
					dev_emerg(n->qnhi.nhi.dev, "NATIVE EXTERNAL ENUMERATED route=2 uid=%016llx speed=%u width=%u authorized=%u; not disk data\n",
						 sw->uid, sw->link_speed, sw->link_width, sw->authorized);
				tb_switch_put(sw);
				mutex_unlock(&n->tb->lock);
				break;
			}
			if (n->scan_results && n->scan_error && !n->scan_preconfig) {
				ret = n->scan_error;
				mutex_unlock(&n->tb->lock);
				break;
			}
			if (x1_native_rescan_allowed(rescanned, n->scan_preconfig, n->scan_error) &&
			    time_before(jiffies + msecs_to_jiffies(8000), deadline)) {
				/* Same live PAN, same transport, no second MCU command or reset.
				 * Never recover an identity or post-configuration failure.
				 */
				rescanned = true;
				if (!valid(ctx)) {
					ret = -ENOTCONN;
					mutex_unlock(&n->tb->lock);
					break;
				}
				dev_emerg(n->qnhi.nhi.dev, "NATIVE RESCAN BEGIN previous-stage=%s errno=%d\n",
					 n->scan_stage, n->scan_error);
				ret = tb_x1_native_rescan(n->tb);
				dev_emerg(n->qnhi.nhi.dev, "NATIVE RESCAN END errno=%d; enumeration result remains required\n", ret);
				if (ret) { mutex_unlock(&n->tb->lock); break; }
				ret = -ETIMEDOUT;
			}
			mutex_unlock(&n->tb->lock);
		} else lock_busy++;
		msleep(20);
	} while (time_before(jiffies, deadline));
	if (!ret) ret = x1_native_prepare_storage(n, valid, ctx);
out:
	if (n)
		dev_emerg(n->qnhi.nhi.dev, "NATIVE RESULT errno=%d rescan=%u lock-busy=%u\n",
			 ret, rescanned, lock_busy);
	mutex_unlock(&native_lock);
	return ret;
}

void x1_native_stop(void)
{
	struct x1_native *n;
	mutex_lock(&native_lock);
	n = owner;
	if (!n || n->stopped) goto out;
	n->stopped = true;
	n->live = false;
	if (n->tb) {
		/* Stop the channel without freeing its domain or DMA buffers. Queued
		 * workers see stopped transport; no second NHI owner may start.
		 */
		mutex_lock(&n->tb->lock);
		tb_ctl_stop(n->tb->ctl);
		mutex_unlock(&n->tb->lock);
	}
	if (n->qnhi.prepared) qcom_usb4_nhi_disable_interrupts(&n->qnhi.nhi);
	dev_emerg(n->qnhi.nhi.dev, "NATIVE CONTROL STOPPED; retained until cold shutdown; disk result is separate\n");
out:
	mutex_unlock(&native_lock);
}

static void native_expire(struct work_struct *work) { x1_native_stop(); }
