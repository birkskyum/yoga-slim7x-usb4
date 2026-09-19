/* SPDX-License-Identifier: GPL-2.0-only */
/* Private v38. Included after its_irq_chip, using the owning ITS driver's
 * mapping and authoritative irq_data. Caller holds IRQ allocation lifetime
 * (setup or NVMe shutdown_lock). Never called from hard IRQ or after free.
 * Arm IHI0069G 12.19.11/13/14: STATUSR 0x40, UMSIR 0x48, TYPER.UMSI bit44.
 * STATUSR is sticky W1C: read only, never acknowledge or enable reporting IRQs.
 */
#if IS_ENABLED(CONFIG_USB4_X1_NATIVE)
static unsigned long x1_its_audit_seen;

void x1_its_msi_audit(struct pci_dev *pdev, unsigned int irq, bool timeout)
{
	struct irq_data *d;
	struct msi_desc *desc;
	struct its_device *idev;
	struct its_node *its;
	struct device_node *np;
	struct msi_msg msg = {};
	unsigned int event, depth;
	u64 typer, unmapped = 0;
	u32 before, after;
	bool node_ok, valid;
	const char *stage = timeout ? "timeout" : "setup";

	if (!pdev || !x1_native_nvme_allowed(pdev) ||
	    pdev->msi_enabled || !pdev->msix_enabled)
		return;
	d = irq_get_irq_data(irq);
	if (!d)
		return;
	desc = irq_data_get_msi_desc(d);
	if (!desc || desc->dev != &pdev->dev ||
	    !desc->pci.msi_attrib.is_msix || desc->pci.msi_attrib.is_virtual ||
	    desc->msi_index > 1)
		return;
	for (depth = 0; d && depth < 4; depth++, d = d->parent_data)
		if (irq_data_get_irq_chip(d) == &its_irq_chip)
			break;
	if (!d || depth == 4 || irqd_is_forwarded_to_vcpu(d))
		return;
	idev = irq_data_get_irq_chip_data(d);
	if (!idev || !(its = idev->its) || !its->base ||
	    its->phys_base != 0x17040000 || idev->device_id != 0x80100)
		return;
	event = its_get_event_id(d);
	if (event > 1 || event != desc->msi_index || event >= idev->nr_ites)
		return;
	np = of_find_node_by_path(
		"/soc@0/interrupt-controller@17000000/msi-controller@17040000");
	node_ok = np && its->fwnode_handle == of_fwnode_handle(np);
	of_node_put(np);
	if (!node_ok)
		return;
	get_cached_msi_msg(irq, &msg);
	if (msg.data != event ||
	    !x1_native_msi_address(pdev, msg.address_lo, msg.address_hi))
		return;
	/* At most setup + first timeout for each of the two events per boot.
	 * Do not repeat q0 setup when the existing NVMe flow reallocates IRQs. */
	if (test_and_set_bit(event + (timeout ? 2 : 0), &x1_its_audit_seen))
		return;
	typer = its->typer; /* Already read by ITS probe; no speculative mapping. */
	dev_emerg(&pdev->dev, "V38 ITS AUDIT stage=%s event=%u irq=%u devid=%08x lpi=%lu typer=%016llx umsi_cap=%u\n",
		  stage, event, irq, idev->device_id, (unsigned long)d->hwirq,
		  (unsigned long long)typer, !!(typer & BIT_ULL(44)));
	if (!(typer & BIT_ULL(44))) {
		dev_emerg(&pdev->dev, "V38 ITS SKIP stage=%s event=%u reason=unmapped-report-unsupported; no diagnostic MMIO, not evidence of delivery\n",
			  stage, event);
		return;
	}
	dev_emerg(&pdev->dev, "V38 ITS STATUS BEGIN stage=%s event=%u off=0040; read only\n", stage, event);
	before = readl_relaxed(its->base + 0x40);
	dev_emerg(&pdev->dev, "V38 ITS STATUS END stage=%s event=%u value=%08x\n", stage, event, before);
	if (before == ~0U) {
		dev_emerg(&pdev->dev, "V38 ITS SKIP stage=%s event=%u reason=invalid-status\n", stage, event);
		return;
	}
	if (!(before & BIT(4))) {
		dev_emerg(&pdev->dev, "V38 ITS REPORT stage=%s event=%u status=%08x latched=0; absence is not delivery proof\n",
			  stage, event, before);
		return;
	}
	dev_emerg(&pdev->dev, "V38 ITS UMSIR BEGIN stage=%s event=%u off=0048; sticky global evidence\n", stage, event);
	unmapped = readq_relaxed(its->base + 0x48);
	dev_emerg(&pdev->dev, "V38 ITS UMSIR END stage=%s event=%u raw=%016llx\n", stage, event, (unsigned long long)unmapped);
	dev_emerg(&pdev->dev, "V38 ITS RECHECK BEGIN stage=%s event=%u off=0040\n", stage, event);
	after = readl_relaxed(its->base + 0x40);
	/* No retries or clear writes. A racing/previous global report is not
	 * attributed to this queue merely because we observed it here. */
	valid = after != ~0U && (after & BIT(4)) && before == after;
	dev_emerg(&pdev->dev, "V38 ITS REPORT stage=%s event=%u before=%08x after=%08x stable=%u latched=1 overflow=%u syndrome=%u observed_devid=%08x observed_event=%08x; global sticky report, not IRQ success\n",
		  stage, event, before, after, valid, !!(after & BIT(5)),
		  (after >> 6) & 0xf, (u32)(unmapped >> 32), (u32)unmapped);
}
EXPORT_SYMBOL_GPL(x1_its_msi_audit);
#endif
