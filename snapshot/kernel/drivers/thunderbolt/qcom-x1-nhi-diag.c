// SPDX-License-Identifier: GPL-2.0-only
/* Private RAM-only finite enumeration checkpoint. No domain or tunnels.
 * Uses the existing Linux ring API and Jim Martin's Qualcomm IRQ adapter.
 * All DMA allocations are deliberately retained until cold shutdown, including
 * canceled frames. Nothing is recycled after uncertain hardware quiescence.
 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/crc32.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/irq.h>
#include <linux/kernel_stat.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/qcom/x1-usb4-diag.h>
#include <linux/unaligned.h>
#include "qcom-usb4-nhi.h"
#include "nhi_regs.h"
#include "x1-diag-packet.h"
#include "x1-link-packet.h"
#include "x1-enum-packet.h"
#include "x1-event-packet.h"
#include "x1-topology.h"
#include "x1-native.h"

#define X1_RX_COUNT 384
#define X1_TX_COUNT 256
struct x1_packet {
	struct ring_frame frame;
	struct completion done;
	u8 *data;
	bool canceled;
};
struct x1_control {
	struct qcom_usb4_nhi qnhi;
	struct tb_ring *tx, *rx;
	struct x1_packet send[X1_TX_COUNT], receive[X1_RX_COUNT];
	bool rings_started, stopped, link_attempted;
	unsigned int next_tx, next_rx;
	int (*connect)(void *, u32);
	void *connect_ctx;
};
static bool attempted;
static DEFINE_MUTEX(link_lock);
static struct x1_control *link_ctl;
static void link_expire(struct work_struct *work);
static DECLARE_DELAYED_WORK(link_watchdog, link_expire);

static int only_device(struct device *dev, void *data)
{
	return dev == data ? 0 : -EBUSY;
}

static int isolation(struct device *dev)
{
	struct of_phandle_args args;
	struct resource res;
	struct iommu_domain *domain;
	struct iommu_group *group;
	bool valid;
	int ret;
	if (!x1_diag_board_allowed() ||
	    !of_property_read_bool(of_root, "birk,usb4-enumeration-test") ||
	    !of_property_read_bool(of_root, "birk,usb4-link-test") ||
	    !of_property_read_bool(of_root, "birk,usb4-nhi-ram-probe") ||
	    !of_device_is_compatible(dev->of_node, "birk,yoga-x1-usb4-mcu-test") ||
	    of_address_to_resource(dev->of_node, 0, &res) ||
	    res.start != 0x15600000 || resource_size(&res) != 0x100000)
		return -EPERM;
	ret = of_parse_phandle_with_args(dev->of_node, "iommus", "#iommu-cells", 0, &args);
	if (ret)
		return ret;
	valid = args.args_count == 2 && args.args[0] == 0x1440 && args.args[1] == 0 &&
		of_device_is_compatible(args.np, "qcom,x1e80100-smmu-500");
	of_node_put(args.np);
	if (!valid || of_count_phandle_with_args(dev->of_node, "iommus", "#iommu-cells") != 1)
		return -EINVAL;
	domain = iommu_get_domain_for_dev(dev);
	if (!domain || (domain->type != IOMMU_DOMAIN_DMA && domain->type != IOMMU_DOMAIN_DMA_FQ))
		return -EPERM;
	group = iommu_group_get(dev);
	if (!group)
		return -ENODEV;
	ret = iommu_group_for_each_dev(group, dev, only_device);
	iommu_group_put(group);
	if (ret)
		return ret;
	/* Conservative low IOVA subset, not a claim about maximum X1 DMA width. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (!ret)
		dev_info(dev, "NHI isolation PASS: translated domain type=%u, sole device, 32-bit IOVA\n", domain->type);
	return ret;
}

static void packet_done(struct tb_ring *ring, struct ring_frame *frame, bool canceled)
{
	struct x1_packet *packet = container_of(frame, struct x1_packet, frame);
	packet->canceled = canceled;
	complete(&packet->done);
}

static int packet_alloc(struct device *dev, struct x1_packet *packet)
{
	init_completion(&packet->done);
	packet->frame.callback = packet_done;
	packet->data = dma_alloc_coherent(dev, TB_FRAME_SIZE, &packet->frame.buffer_phy,
					 GFP_KERNEL | __GFP_ZERO);
	if (!packet->data)
		return -ENOMEM;
	if (packet->frame.buffer_phy > U32_MAX - (TB_FRAME_SIZE - 1) ||
	    !iommu_iova_to_phys(iommu_get_domain_for_dev(dev), packet->frame.buffer_phy) ||
	    !iommu_iova_to_phys(iommu_get_domain_for_dev(dev), packet->frame.buffer_phy + TB_FRAME_SIZE - 1))
		return -EFAULT;
	return 0;
}

static bool ring_mapping_valid(struct device *dev, struct tb_ring *ring)
{
	unsigned int bytes = ring->size * sizeof(struct ring_desc);
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	return ring->descriptors_dma <= U32_MAX - (bytes - 1) &&
		iommu_iova_to_phys(domain, ring->descriptors_dma) &&
		iommu_iova_to_phys(domain, ring->descriptors_dma + bytes - 1);
}

static const struct tb_nhi_ops diag_ops = {
	.request_ring_irq = qcom_usb4_nhi_request_ring_irq,
	.release_ring_irq = qcom_usb4_nhi_release_ring_irq,
	.ring_interrupt_mask = qcom_usb4_nhi_ring_interrupt_mask,
};

static void snapshot(struct x1_control *ctl, const char *label)
{
	static const u32 offsets[] = { REG_CAPS, REG_DMA_MISC, REG_TX_OPTIONS_BASE,
		REG_RX_OPTIONS_BASE, REG_TX_RING_BASE + 8, REG_RX_RING_BASE + 8,
		0x37800, 0x38208, 0x38210 }; /* Qualcomm 32-bit pending/mask/enable. */
	struct tb_nhi *nhi = &ctl->qnhi.nhi;
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		dev_info(nhi->dev, "NHI %s read BEGIN off=%05x\n", label, offsets[i]);
		dev_info(nhi->dev, "NHI %s read END off=%05x value=%08x\n", label, offsets[i],
			 readl(nhi->iobase + offsets[i]));
	}
}

static bool baseline_complete;
bool x1_diag_nhi_complete(void)
{
	return READ_ONCE(baseline_complete);
}
EXPORT_SYMBOL_GPL(x1_diag_nhi_complete);

int x1_diag_nhi_run(struct device *dev, void __iomem *router)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct of_phandle_args interrupt;
	struct x1_control *ctl;
	struct tb_nhi *nhi;
	u32 reply[2][X1_READ_WORDS] = { 0 };
	unsigned int seq, next_rx = 0, i, irq_before;
	unsigned long deadline, left;
	int ret, irq;
	if (!router || attempted)
		return -EPERM;
	ret = isolation(dev);
	if (ret)
		return dev_err_probe(dev, ret, "NHI REFUSED: board or translated DMA isolation\n");
	if (of_irq_parse_one(dev->of_node, 0, &interrupt))
		return -EINVAL;
	ret = of_device_is_compatible(interrupt.np, "arm,gic-v3") &&
		interrupt.args_count == 3 && interrupt.args[0] == 0 &&
		interrupt.args[1] == 472 && interrupt.args[2] == IRQ_TYPE_LEVEL_HIGH ? 0 : -EINVAL;
	of_node_put(interrupt.np);
	if (ret)
		return ret;
	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return irq ?: -EINVAL;
	attempted = true;
	if (IS_ENABLED(CONFIG_USB4_X1_NATIVE)) {
		ret = x1_native_start(dev, router, irq);
		if (!ret) WRITE_ONCE(baseline_complete, true);
		return ret;
	}
	ctl = kzalloc(sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return -ENOMEM;
	/* Retain ctl and device for the rest of this boot, on every path. */
	nhi = &ctl->qnhi.nhi;
	nhi->dev = get_device(dev);
	nhi->dma_dev = dev;
	nhi->iobase = router + 0x3f000;
	nhi->ops = &diag_ops;
	nhi->hop_count = 3;
	spin_lock_init(&nhi->lock);
	nhi->tx_rings = kcalloc(3, sizeof(*nhi->tx_rings), GFP_KERNEL);
	nhi->rx_rings = kcalloc(3, sizeof(*nhi->rx_rings), GFP_KERNEL);
	if (!nhi->tx_rings || !nhi->rx_rings)
		return -ENOMEM;
	dev_info(dev, "NHI CHECKPOINT ownership/capabilities\n");
	if (readl(nhi->iobase + REG_CAPS) != 3)
		return -EINVAL;
	/* Refuse inherited live rings; do not reset a potentially active owner. */
	for (i = 0; i < 3; i++)
		if ((readl(nhi->iobase + REG_TX_OPTIONS_BASE + i * 32) |
		     readl(nhi->iobase + REG_RX_OPTIONS_BASE + i * 32)) & RING_FLAG_ENABLE)
			return -EBUSY;
	ret = qcom_usb4_nhi_prepare(&ctl->qnhi, 0x40000, 3, QCOM_USB4_IRQ_32, irq);
	if (ret)
		return ret;
	irq_before = kstat_irqs_usr(irq);
	dev_info(dev, "NHI CHECKPOINT IRQ init linux_irq=%d SPI=472\n", irq);
	ret = qcom_usb4_nhi_init_interrupts(nhi);
	if (ret)
		goto stop;
	ctl->tx = tb_ring_alloc_tx(nhi, 0, 10, RING_FLAG_NO_SUSPEND);
	ctl->rx = tb_ring_alloc_rx(nhi, 0, 10, RING_FLAG_NO_SUSPEND, 0, 0xffff, 0xffff, NULL, NULL);
	if (!ctl->tx || !ctl->rx) {
		ret = -ENOMEM;
		goto stop;
	}
	if (!ring_mapping_valid(dev, ctl->tx) || !ring_mapping_valid(dev, ctl->rx)) {
		ret = -EFAULT;
		goto stop;
	}
	for (i = 0; i < X1_TX_COUNT; i++) {
		ret = packet_alloc(dev, &ctl->send[i]);
		if (ret)
			goto stop;
		if (i < 2) x1_packet_request(ctl->send[i].data, i);
		ctl->send[i].frame.size = 16;
		ctl->send[i].frame.sof = 1;
		ctl->send[i].frame.eof = 1;
	}
	for (i = 0; i < X1_RX_COUNT; i++) {
		ret = packet_alloc(dev, &ctl->receive[i]);
		if (ret)
			goto stop;
	}
	dev_info(dev, "NHI CHECKPOINT start control TX/RX rings only\n");
	tb_ring_start(ctl->tx);
	tb_ring_start(ctl->rx);
	ctl->rings_started = true;
	for (i = 0; i < X1_RX_COUNT; i++) {
		ret = tb_ring_rx(ctl->rx, &ctl->receive[i].frame);
		if (ret)
			goto stop;
	}
	for (seq = 0; seq < 2; seq++) {
		dev_info(dev, "NHI CHECKPOINT READ route=0 switch offset=0 words=5 seq=%u\n", seq);
		ret = tb_ring_tx(ctl->tx, &ctl->send[seq].frame);
		if (ret)
			goto stop;
		deadline = jiffies + msecs_to_jiffies(3000);
		ret = -ETIMEDOUT;
		while (next_rx < X1_RX_COUNT && time_before(jiffies, deadline)) {
			struct x1_packet *packet = &ctl->receive[next_rx];
			left = jiffies;
			if (time_after_eq(left, deadline))
				break;
			left = deadline - left;
			if (!wait_for_completion_timeout(&packet->done, left))
				break;
			next_rx++;
			dev_info(dev, "NHI RX size=%u sof=%u eof=%u flags=%x canceled=%d\n",
				 packet->frame.size, packet->frame.sof, packet->frame.eof,
				 packet->frame.flags, packet->canceled);
			if (!packet->canceled && packet->frame.size >= 16 && packet->frame.size <= TB_FRAME_SIZE)
				dev_info(dev, "NHI RX prefix=%08x %08x %08x %08x\n",
					 get_unaligned_be32(packet->data), get_unaligned_be32(packet->data + 4),
					 get_unaligned_be32(packet->data + 8), get_unaligned_be32(packet->data + 12));
			if (packet->canceled) {
				ret = -ESHUTDOWN;
				break;
			}
			if (packet->frame.eof != 1 && packet->frame.eof != 3)
				continue; /* Finite unsolicited packet budget; no ACK/write. */
			ret = x1_packet_reply(packet->data, packet->frame.size, packet->frame.sof,
					      packet->frame.eof, seq, reply[seq]);
			break;
		}
		if (ret)
			goto stop;
		if (!wait_for_completion_timeout(&ctl->send[seq].done, msecs_to_jiffies(1000)) ||
		    ctl->send[seq].canceled) {
			ret = -ETIMEDOUT;
			goto stop;
		}
		dev_info(dev, "NHI READ PASS seq=%u words=%08x %08x %08x %08x %08x\n", seq,
			 reply[seq][0], reply[seq][1], reply[seq][2], reply[seq][3], reply[seq][4]);
	}
	/* v16 retains v14's SAME owner/rings. Completed frames
	 * are never recycled, and no second owner or ring restart is created.
	 */
	mutex_lock(&link_lock);
	ctl->next_rx = next_rx;
	ctl->next_tx = 2;
	link_ctl = ctl;
	WRITE_ONCE(baseline_complete, true);
	schedule_delayed_work(&link_watchdog, 600 * HZ);
	mutex_unlock(&link_lock);
	dev_info(dev, "NHI CONTROL COMPLETE errno=0; v16 control rings retained LIVE for one manual enumeration, maximum 600 seconds. No tunnel/storage.\n");
	return 0;
stop:
	dev_info(dev, "NHI CHECKPOINT stop ret=%d irq_delta=%u rx_frames=%u\n", ret,
		 kstat_irqs_usr(irq) - irq_before, next_rx);
	qcom_usb4_nhi_disable_interrupts(nhi);
	if (ctl->qnhi.irq_requested)
		synchronize_irq(irq);
	snapshot(ctl, "before-stop");
	if (ctl->rings_started) {
		tb_ring_stop(ctl->rx);
		tb_ring_stop(ctl->tx);
	}
	snapshot(ctl, "after-stop");
	if (ctl->rings_started &&
	    ((readl(nhi->iobase + REG_RX_OPTIONS_BASE) |
	      readl(nhi->iobase + REG_TX_OPTIONS_BASE)) & RING_FLAG_ENABLE))
		ret = ret ?: -EBUSY;
	dev_info(dev, "NHI CONTROL %s errno=%d. DMA buffers retained until cold shutdown. No link/storage test.\n",
		 ret ? "FAILED" : "COMPLETE", ret);
	if (!ret)
		WRITE_ONCE(baseline_complete, true);
	return ret;
}
EXPORT_SYMBOL_GPL(x1_diag_nhi_run);

/* Caller holds link_lock. Never release or re-use any descriptor/data DMA. */
static void link_stop_locked(struct x1_control *ctl)
{
	struct tb_nhi *nhi = &ctl->qnhi.nhi;
	if (ctl->stopped) return;
	ctl->stopped = true;
	qcom_usb4_nhi_disable_interrupts(nhi);
	if (ctl->qnhi.irq_requested) synchronize_irq(ctl->qnhi.irq);
	if (ctl->rings_started) {
		tb_ring_stop(ctl->rx);
		tb_ring_stop(ctl->tx);
	}
	dev_info(nhi->dev, "LINK CONTROL STOPPED tx=%u rx=%u options=%08x/%08x; all DMA retained; MCU/PHY powered until cold shutdown\n",
		 ctl->next_tx, ctl->next_rx, readl(nhi->iobase + REG_TX_OPTIONS_BASE),
		 readl(nhi->iobase + REG_RX_OPTIONS_BASE));
}
void x1_diag_link_stop(void)
{
	if (IS_ENABLED(CONFIG_USB4_X1_NATIVE)) {
		x1_native_stop();
		return;
	}
	mutex_lock(&link_lock);
	if (link_ctl) link_stop_locked(link_ctl);
	mutex_unlock(&link_lock);
}
EXPORT_SYMBOL_GPL(x1_diag_link_stop);
static void link_expire(struct work_struct *work)
{
	x1_diag_link_stop();
}
int x1_diag_link_register(int (*connect)(void *, u32), void *ctx)
{
	int ret = 0;
	if (IS_ENABLED(CONFIG_USB4_X1_NATIVE))
		return x1_native_register_connect(connect, ctx);
	mutex_lock(&link_lock);
	if (!connect || !ctx || !link_ctl || link_ctl->stopped || link_ctl->connect)
		ret = -EPERM;
	else { link_ctl->connect = connect; link_ctl->connect_ctx = ctx; }
	mutex_unlock(&link_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(x1_diag_link_register);

struct x1_link_session {
	struct x1_control *ctl;
	bool (*valid)(void *);
	void *ctx;
	unsigned long deadline;
	unsigned int events;
};
static int link_event(struct x1_link_session *s, struct x1_packet *rx,
		      unsigned long deadline)
{
	struct x1_control *ctl = s->ctl;
	struct x1_plug_event event;
	struct x1_packet *tx;
	unsigned long now;
	int ret = x1_event_decode(rx->data, rx->frame.size, rx->frame.sof,
				 rx->frame.eof, &event);
	if (ret) {
		dev_err(ctl->qnhi.nhi.dev, "LINK EVENT rejected errno=%d size=%u sof=%u eof=%u\n",
			ret, rx->frame.size, rx->frame.sof, rx->frame.eof);
		return ret;
	}
	if (s->events >= X1_EVENT_LIMIT || ctl->next_tx >= X1_TX_COUNT)
		return -ENOSPC;
	if (!s->valid(s->ctx)) return -ENOTCONN;
	if (time_after_eq(jiffies, deadline)) return -ETIMEDOUT;
	s->events++;
	tx = &ctl->send[ctl->next_tx++]; /* Never reuse a live/completed DMA buffer. */
	ret = x1_event_ack(tx->data, &event);
	if (ret) return ret;
	tx->frame.size = 16;
	tx->frame.sof = tx->frame.eof = 3;
	dev_info(ctl->qnhi.nhi.dev, "LINK EVENT root port=%u unplug=%u count=%u ACK BEGIN word=%08x\n",
		event.port, event.unplug, s->events, get_unaligned_be32(tx->data + 8));
	ret = tb_ring_tx(ctl->tx, &tx->frame);
	if (ret) return ret;
	/* ACK has no reply. Keep the original READ/WRITE sequence and deadline. */
	now = jiffies;
	if (time_after_eq(now, deadline) ||
	    !wait_for_completion_timeout(&tx->done, deadline - now)) return -ETIMEDOUT;
	if (tx->canceled) return -ESHUTDOWN;
	dev_info(ctl->qnhi.nhi.dev, "LINK EVENT root port=%u unplug=%u ACK TX COMPLETE\n",
		event.port, event.unplug);
	if (event.unplug || !s->valid(s->ctx)) return -ENOTCONN;
	return 0;
}
static int link_transfer(struct x1_link_session *s, struct x1_read_spec *read,
			 struct x1_write_spec *write, u32 *out)
{
	struct x1_control *ctl = s->ctl;
	struct x1_packet *tx;
	unsigned long deadline, now;
	int ret;
	if ((!read == !write) || (read && !out)) return -EINVAL;
	if (ctl->next_tx == X1_TX_COUNT) return -ENOSPC;
	if (!s->valid(s->ctx)) return -ENOTCONN;
	if (time_after_eq(jiffies, s->deadline)) return -ETIME;
	tx = &ctl->send[ctl->next_tx++];
	if (write) {
		write->seq = (ctl->next_tx - 1) & 3;
		ret = x1_write_request(tx->data, write);
		tx->frame.size = 16 + 4 * write->words;
		tx->frame.sof = tx->frame.eof = 2;
	} else {
		read->seq = (ctl->next_tx - 1) & 3;
		ret = x1_read_request(tx->data, read);
		tx->frame.size = 16;
		tx->frame.sof = tx->frame.eof = 1;
	}
	if (ret) return ret;
	dev_info(ctl->qnhi.nhi.dev, "LINK %s route=%x port=%u space=%u offset=%x words=%u seq=%u\n",
		write ? "WRITE" : "READ", write ? write->route : read->route,
		write ? write->port : read->port, write ? write->space : read->space,
		write ? write->offset : read->offset, write ? write->words : read->words,
		write ? write->seq : read->seq);
	ret = tb_ring_tx(ctl->tx, &tx->frame);
	if (ret) return ret;
	deadline = min(s->deadline, jiffies + msecs_to_jiffies(1000));
	ret = -ETIMEDOUT;
	while (ctl->next_rx < X1_RX_COUNT) {
		struct x1_packet *rx = &ctl->receive[ctl->next_rx];
		now = jiffies;
		if (time_after_eq(now, deadline) ||
		    !wait_for_completion_timeout(&rx->done, deadline - now)) break;
		ctl->next_rx++;
		if (rx->canceled) return -ESHUTDOWN;
		if (rx->frame.flags & (RING_DESC_CRC_ERROR | RING_DESC_BUFFER_OVERRUN)) return -EBADMSG;
		if (rx->frame.eof == 5) {
			ret = link_event(s, rx, deadline);
			if (ret) return ret;
			ret = -ETIMEDOUT;
			continue;
		}
		ret = write ? x1_write_reply(rx->data, rx->frame.size, rx->frame.sof,
					    rx->frame.eof, write) :
			x1_read_reply(rx->data, rx->frame.size, rx->frame.sof,
				      rx->frame.eof, read, out);
		if (ret) dev_err(ctl->qnhi.nhi.dev, "LINK reply error=%d size=%u sof=%u eof=%u prefix=%08x %08x %08x\n",
			ret, rx->frame.size, rx->frame.sof, rx->frame.eof,
			get_unaligned_be32(rx->data), get_unaligned_be32(rx->data+4), get_unaligned_be32(rx->data+8));
		break;
	}
	if (ret) return ret;
	now = jiffies;
	if (time_after_eq(now, deadline) || !wait_for_completion_timeout(&tx->done, deadline - now) || tx->canceled)
		return -ETIMEDOUT;
	return s->valid(s->ctx) ? 0 : -ENOTCONN;
}
static int link_read(struct x1_link_session *s, u32 route, unsigned int port,
		     unsigned int space, unsigned int offset, unsigned int words, u32 *out)
{
	struct x1_read_spec spec = { route, port, space, offset, words, 0 };
	int ret = link_transfer(s, &spec, NULL, out);
	if (ret) dev_err(s->ctl->qnhi.nhi.dev,
		"LINK FAILED READ route=%u port=%u space=%u offset=%x words=%u errno=%d tx=%u rx=%u\n",
		route, port, space, offset, words, ret, s->ctl->next_tx, s->ctl->next_rx);
	return ret;
}
static int link_write(struct x1_link_session *s, unsigned int port, unsigned int space,
		      unsigned int offset, unsigned int words, const u32 *before, const u32 *after)
{
	struct x1_write_spec spec = { port, space, offset, words, 0, before, after, 0 };
	return link_transfer(s, NULL, &spec, NULL);
}
static int link_external_write(struct x1_link_session *s, unsigned int offset,
			       unsigned int words, const u32 *before, const u32 *after)
{
	struct x1_write_spec spec = { 0, 2, offset, words, 0, before, after, 2 };
	int ret = link_transfer(s, NULL, &spec, NULL);
	if (ret) dev_err(s->ctl->qnhi.nhi.dev,
		"LINK FAILED WRITE route=2 port=0 space=2 offset=%x words=%u errno=%d\n",
		offset, words, ret);
	return ret;
}

static int link_external_init(struct x1_link_session *s, const u32 *header,
			      u32 *configured, const char **stage)
{
	u32 plan[4], state[4], before, after, got;
	unsigned long deadline;
	unsigned int i;
	int ret;
	*stage = "external-identity";
	ret = x1_external_plan(header, plan);
	if (ret) return ret;
	/* usb4_switch_read_uid(): CS7 low word, CS8 high word, native endian.
	 * Require the exact LaCie UID from the saved Mac reference, not VID alone.
	 */
	ret = link_read(s, 2, 0, 2, 5, 4, state);
	if (ret) return ret;
	dev_info(s->ctl->qnhi.nhi.dev, "ENUM EXTERNAL BASELINE cs5=%08x cs6=%08x uid=%08x%08x\n",
		 state[0], state[1], state[3], state[2]);
	/* Public snapshot: legacy external-configuration path disabled. */
	if (true || state[2] != 0 || state[3] != 0 ||
	    (state[1] & 0x02000001U)) return -EPERM; /* No CR/sleep. */
	ret = x1_external_setup_plan(state[0], &after);
	if (ret) return ret;
	*stage = "external-config-write";
	ret = link_external_write(s, 1, 4, header + 1, plan);
	if (ret) return ret;
	*stage = "external-config-verify";
	ret = link_read(s, 2, 0, 2, 0, 5, configured);
	if (ret) return ret;
	dev_info(s->ctl->qnhi.nhi.dev, "ENUM EXTERNAL HEADER %08x %08x %08x %08x %08x\n",
		 configured[0], configured[1], configured[2], configured[3], configured[4]);
	if (configured[0] != header[0] || memcmp(configured + 1, plan, sizeof(plan)))
		return -EIO;
	*stage = "external-setup";
	ret = link_read(s, 2, 0, 2, 5, 1, &before);
	if (ret) return ret;
	ret = x1_external_setup_plan(before, &after);
	if (ret) return ret;
	/* Native setup with tunnel policy disabled. Only CNS may change; no
	 * PTO/UTO/HCO/CV enables, protocol adapter writes or path programming.
	 */
	ret = link_external_write(s, 5, 1, &before, &after);
	if (ret) return ret;
	ret = link_read(s, 2, 0, 2, 5, 1, &got);
	if (ret) return ret;
	dev_info(s->ctl->qnhi.nhi.dev, "ENUM EXTERNAL SETUP before=%08x expected=%08x got=%08x; tunnels off\n",
		 before, after, got);
	if (got != after) return -EIO;
	*stage = "external-router-ready";
	deadline = s->deadline;
	s->deadline = min(deadline, jiffies + msecs_to_jiffies(500));
	ret = -ETIMEDOUT;
	for (i = 0; i < 11; i++) {
		if (time_after_eq(jiffies, s->deadline)) { ret = -ETIMEDOUT; break; }
		ret = link_read(s, 2, 0, 2, 6, 1, &got);
		if (ret) break;
		dev_info(s->ctl->qnhi.nhi.dev, "ENUM EXTERNAL READY poll=%u cs6=%08x\n", i, got);
		if (got & 0x02000001U) { ret = -EPERM; break; }
		if (got & 0x01000000U) break;
		ret = -ETIMEDOUT;
		msleep(50);
	}
	s->deadline = deadline;
	if (!ret) dev_info(s->ctl->qnhi.nhi.dev,
		"ENUM EXTERNAL READY PASS route=%u; adapter inventory only, no tunnels\n", 2);
	return ret;
}

static int topology_read(void *ctx, u32 route, unsigned int port, unsigned int space,
			 unsigned int offset, unsigned int words, u32 *out)
{
	return link_read(ctx, route, port, space, offset, words, out);
}
static void topology_report(void *ctx, const char *kind, u32 route, unsigned int port,
			    unsigned int offset, const u32 *d, unsigned int words)
{
	struct x1_link_session *s = ctx;
	struct device *dev = s->ctl->qnhi.nhi.dev;
	dev_info(dev, "TOPO RAW %s r=%u p=%u off=%02x %*ph\n", kind, route, port,
		 offset, (int)(words * 4), d);
	if (!strcmp(kind, "ADAPTER"))
		dev_info(dev, "TOPO ADAPTER r=%u p=%u type=%06x cap=%02x hops=%u/%u buffers=%u lock=%u\n",
			route, port, d[2] & 0xffffff, d[1] & 0xff, d[5] & 0x7ff,
			(d[5] >> 11) & 0x7ff, (d[4] >> 20) & 0x3ff, !!(d[4] & BIT(31)));
	else if (!strcmp(kind, "PHY"))
		dev_info(dev, "TOPO PHY r=%u p=%u state=%u speed_code=%x width_code=%x disabled=%u bonded=%u raw=%08x/%08x\n",
			route, port, (d[1] >> 26) & 0xf, (d[1] >> 16) & 0xf,
			(d[1] >> 20) & 0x3f, !!(d[1] & BIT(14)), !!(d[1] & BIT(15)), d[0], d[1]);
	else if (!strcmp(kind, "PROTOCOL"))
		dev_info(dev, "TOPO PROTOCOL r=%u p=%u cap=%02x enabled=%u raw=%08x/%08x\n",
			route, port, offset, !!(d[0] & BIT(31)), d[0], d[1]);
	else if (!strcmp(kind, "ROUTER"))
		dev_info(dev, "TOPO ROUTER r=%u cs5=%08x cs6=%08x cs7=%08x cs8=%08x\n",
			route, d[0], d[1], d[2], d[3]);
}

int x1_diag_link_run(u32 word, bool (*valid)(void *), void *ctx)
{
	struct x1_control *ctl;
	struct x1_link_session s;
	struct x1_topology topo = { .read = topology_read, .report = topology_report };
	u32 host[5], data[8], root_config[4], external[5], configured[5], before, after;
	u8 caps[8] = { 0 }, seen[256];
	unsigned int port, max_port, i, cap, state, lanes = 0, up = 0;
	const char *stage = "guard";
	int ret = -EPERM;
	if (IS_ENABLED(CONFIG_USB4_X1_NATIVE))
		return x1_native_connect(word, valid, ctx);
	mutex_lock(&link_lock);
	ctl = link_ctl;
	if (!ctl || ctl->stopped || ctl->link_attempted || !ctl->connect || !valid ||
	    !x1_diag_board_allowed() || !of_property_read_bool(of_root, "birk,usb4-link-test") ||
	    !of_property_read_bool(of_root, "birk,usb4-enumeration-test") ||
	    !of_property_read_bool(of_root, "birk,usb4-external-enumeration-test")) goto out;
	ctl->link_attempted = true;
	/* This derivative only supports the observed passive Gen3 LaCie PAN. */
	if ((word != 0x2501 && word != 0x2701) || !valid(ctx)) goto stop_link;
	s = (struct x1_link_session){ .ctl = ctl, .valid = valid, .ctx = ctx,
		.deadline = jiffies + 20 * HZ };
	stage = "root-read";
	ret = link_read(&s, 0, 0, 2, 0, 5, host);
	if (ret) goto stop_link;
	dev_info(ctl->qnhi.nhi.dev, "LINK ROOT BEFORE %08x %08x %08x %08x %08x\n",
		host[0], host[1], host[2], host[3], host[4]);
	max_port = (host[1] >> 14) & 0x3f;
	ret = x1_root_plan(host, root_config);
	if (ret) goto stop_link;
	stage = "root-write";
	ret = link_write(&s, 0, 2, 1, 4, host + 1, root_config);
	if (ret) goto stop_link;
	stage = "root-verify";
	ret = link_read(&s, 0, 0, 2, 0, 5, data);
	if (ret) goto stop_link;
	dev_info(ctl->qnhi.nhi.dev, "LINK ROOT AFTER %08x %08x %08x %08x %08x\n",
		data[0], data[1], data[2], data[3], data[4]);
	if (data[0] != host[0] || memcmp(data + 1, root_config, sizeof(root_config))) {
		ret = -EIO; goto stop_link;
	}
	stage = "host-discovery";
	for (port = 1; port <= max_port; port++) {
		ret = link_read(&s, 0, port, 1, 0, 8, data);
		if (ret) goto stop_link;
		dev_info(ctl->qnhi.nhi.dev, "LINK HOST ADAPTER port=%u type=%06x cap=%02x header=%08x %08x %08x %08x %08x %08x %08x %08x\n",
			port, data[2] & 0xffffff, data[1] & 0xff, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
		if ((data[2] & 0xffffff) != 1) continue; /* Lane adapters only. */
		cap = data[1] & 0xff;
		memset(seen, 0, sizeof(seen));
		for (i = 0; cap && i < 16; i++) {
			if (cap < 8 || cap > 254 || seen[cap]) { ret = -EPROTO; goto stop_link; }
			seen[cap] = 1;
			ret = link_read(&s, 0, port, 1, cap, 2, data);
			if (ret) goto stop_link;
			dev_info(ctl->qnhi.nhi.dev, "LINK HOST CAP port=%u offset=%02x words=%08x %08x\n", port, cap, data[0], data[1]);
			if (((data[0] >> 8) & 0xff) == 1) { caps[port] = cap; lanes++; break; }
			cap = data[0] & 0xff;
		}
		if (!caps[port]) { ret = -EOPNOTSUPP; goto stop_link; }
	}
	if (!lanes) { ret = -ENODEV; goto stop_link; }
	/* This derivative is solely for the observed router-0 primary pair 2/3. */
	if (lanes != 2 || !caps[2] || !caps[3]) { ret = -ENODEV; goto stop_link; }
	if (!valid(ctx)) { ret = -ENOTCONN; goto stop_link; }
	stage = "connect";
	dev_info(ctl->qnhi.nhi.dev, "LINK MCU CONNECT BEGIN word=%08x; no tunnel commands\n", word);
	ret = ctl->connect(ctl->connect_ctx, word);
	dev_info(ctl->qnhi.nhi.dev, "LINK MCU CONNECT END errno=%d; ACK is NOT link proof\n", ret);
	if (ret) goto stop_link;
	/* Read both lane states; never force enable or bond. */
	stage = "lane-poll";
	for (i = 0; i < 20; i++) {
		up = 0;
		for (port = 1; port <= max_port; port++) {
			if (!caps[port]) continue;
			ret = link_read(&s, 0, port, 1, caps[port], 2, data);
			if (ret) goto stop_link;
			state = (data[1] >> 26) & 0xf;
			dev_info(ctl->qnhi.nhi.dev, "LINK LANE port=%u poll=%u state=%u disabled=%u raw=%08x %08x\n",
				port, i, state, !!(data[1] & BIT(14)), data[0], data[1]);
			if (state >= 2 && state <= 6 && !(data[1] & BIT(14))) up |= BIT(port);
		}
		if (up & BIT(2)) break; /* Do not stop early if only the secondary is up. */
		msleep(100);
	}
	if (!up) { ret = -ENOLINK; goto stop_link; }
	/* Native CM scans the primary port, not the secondary bonded-lane member. */
	if (!(up & BIT(2))) { ret = -ENOLINK; goto stop_link; }
	stage = "port-lock-read";
	ret = link_read(&s, 0, 2, 1, 4, 1, &before);
	if (ret) goto stop_link;
	after = before & ~BIT(31);
	dev_info(ctl->qnhi.nhi.dev, "LINK PRIMARY LOCK before=%08x locked=%u\n", before, !!(before & BIT(31)));
	if (before != after) {
		stage = "port-unlock";
		ret = link_write(&s, 2, 1, 4, 1, &before, &after);
		if (ret) goto stop_link;
		stage = "port-unlock-verify";
		ret = link_read(&s, 0, 2, 1, 4, 1, data);
		if (ret) goto stop_link;
		dev_info(ctl->qnhi.nhi.dev, "LINK PRIMARY LOCK after=%08x expected=%08x\n", data[0], after);
		if (data[0] != after) { ret = -EIO; goto stop_link; }
	}
	stage = "external-header";
	ret = link_read(&s, 2, 0, 2, 0, 5, data);
	if (ret) goto stop_link;
	dev_info(ctl->qnhi.nhi.dev, "LINK EXTERNAL HEADER READ PASS route=2 header=%08x %08x %08x %08x %08x; no tunnels/storage\n",
		data[0], data[1], data[2], data[3], data[4]);
	stage = "topology-read";
	topo.ctx = &s;
	memcpy(external, data, sizeof(external));
	/* Preserve v17's known external header checkpoint, then finish all host
	 * inventory before exposing the first external adapter transaction.
	 */
	stage = "topology-host";
	host[3] = root_config[2]; host[4] = root_config[3];
	ret = x1_topology_router(&topo, 0, host);
	if (ret) goto stop_link;
	ret = link_external_init(&s, external, configured, &stage);
	if (ret) goto stop_link;
	stage = "topology-external";
	ret = x1_topology_router(&topo, 2, configured);
stop_link:
	dev_info(ctl->qnhi.nhi.dev, "TOPO SUMMARY errno=%d reads=%u adapters=%u caps=%u phys=%u host_pcie_down=%u external_pcie_up=%u skipped=%u; inventory only, no tunnel or endpoint DMA\n",
		ret, topo.reads, topo.adapters, topo.caps, topo.lanes, topo.pcie_down, topo.pcie_up, topo.skipped);
	dev_info(ctl->qnhi.nhi.dev, "LINK CHECKPOINT %s stage=%s errno=%d up_lanes=%02x; NOT a disk-read or throughput test\n",
		 ret ? "INCOMPLETE" : "TOPOLOGY_READ_COMPLETE", stage, ret, up);
	link_stop_locked(ctl);
out:
	mutex_unlock(&link_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(x1_diag_link_run);
