/* SPDX-License-Identifier: GPL-2.0 */
/* Included after nvme_queue/nvme_dev definitions. Before the normal timeout
 * poll; one snapshot per queue, no retry if teardown is in progress. */
static atomic_t x1_irq_timeouts[2] = { ATOMIC_INIT(0), ATOMIC_INIT(0) };

static void x1_nvme_irq_timeout(struct nvme_queue *q, u32 csts)
{
	struct nvme_dev *dev = q->dev;
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	u16 head, status;
	u8 phase;

	if (!x1_nvme_irq_test(pdev) || q->qid > 1 ||
	    atomic_cmpxchg(&x1_irq_timeouts[q->qid], 0, 1))
		return;
	if (!mutex_trylock(&dev->shutdown_lock)) {
		dev_emerg(&pdev->dev, "V33 IRQ TIMEOUT SKIP qid=%u reason=shutdown-lock\n", q->qid);
		return;
	}
	if (!test_bit(NVMEQ_ENABLED, &q->flags) || test_bit(NVMEQ_POLLED, &q->flags) ||
	    !q->cqes || q->cq_vector != q->qid || !dev->bar_mapped_size) {
		dev_emerg(&pdev->dev, "V33 IRQ TIMEOUT SKIP qid=%u reason=queue-lifetime\n", q->qid);
		goto out;
	}
	/* Observe, never consume a CQE. These are deliberately not an atomic
	 * multi-field snapshot; a real IRQ can race, so phase is only a hint. */
	head = READ_ONCE(q->cq_head);
	phase = READ_ONCE(q->cq_phase);
	if (head >= q->q_depth) {
		dev_emerg(&pdev->dev, "V33 IRQ TIMEOUT SKIP qid=%u reason=head-bounds\n", q->qid);
		goto out;
	}
	status = le16_to_cpu(READ_ONCE(q->cqes[head].status));
	dev_emerg(&pdev->dev, "V33 IRQ TIMEOUT qid=%u csts=%08x head=%u phase=%u cq_status=%04x pending_hint=%u entries=%d synthetic=0\n",
		  q->qid, csts, head, phase, status, (status & 1) == phase,
		  atomic_read(&x1_irq_entries[q->qid]));
	x1_nvme_irq_state(pdev, q->qid, q->cq_vector, "timeout");
out:
	mutex_unlock(&dev->shutdown_lock);
}
