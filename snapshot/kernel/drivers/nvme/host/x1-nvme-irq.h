/* SPDX-License-Identifier: GPL-2.0 */
/* Private v33: setup/timeout MSI-X evidence, no synthetic interrupts.
 * Only the admitted endpoint and its exact root. No routing/table/mask writes.
 */
#ifndef X1_NVME_IRQ_H
#define X1_NVME_IRQ_H
/* Match the exact OF node, not full_name: unflattened nodes store a local
 * name there; %pOF reconstructs the absolute path from its parents.
 */
static bool x1_nvme_irq_controller(struct device_node *np)
{
	struct device_node *expected;
	bool match;

	if (!np)
		return false;
	expected = of_find_node_by_path(
		"/soc@0/interrupt-controller@17000000/msi-controller@17040000");
	match = expected && np == expected;
	of_node_put(expected);
	return match;
}

static bool x1_nvme_irq_test(struct pci_dev *pdev)
{
	return IS_ENABLED(CONFIG_USB4_X1_NATIVE) && x1_native_nvme_allowed(pdev);
}

static unsigned int x1_nvme_irq_flags(struct pci_dev *pdev, unsigned int normal)
{
	return x1_nvme_irq_test(pdev) ? PCI_IRQ_MSIX | (normal & PCI_IRQ_AFFINITY) : normal;
}

static unsigned int x1_nvme_irq_count(struct pci_dev *pdev, unsigned int normal)
{
	return x1_nvme_irq_test(pdev) ? min(normal, 2U) : normal;
}

static atomic_t x1_irq_entries[2] = { ATOMIC_INIT(0), ATOMIC_INIT(0) };

/* Count hard-IRQ entry, including IRQ_NONE; never called by timeout polling.
 * Threaded mode counts the primary handler only, not its subsequent thread.
 */
static void x1_nvme_irq_enter(struct pci_dev *pdev, unsigned int qid)
{
	if (x1_nvme_irq_test(pdev) && qid < 2)
		atomic_inc(&x1_irq_entries[qid]);
}

static void x1_nvme_irq_snapshot(struct pci_dev *pdev, unsigned int qid,
			       unsigned int vector, int result)
{
	struct msi_msg msg = {};
	struct device_node *np = NULL;
	struct irq_data *d;
	struct msi_desc *desc;
	void __iomem *entry;
	u32 translated, lo, hi, data, mask, table = 0, pba = 0;
	u16 ctrl = 0;
	bool node_ok, address_ok, route_ok, its_chip = false;
	int irq, ret, pos, depth;

	if (!x1_nvme_irq_test(pdev))
		return;
	dev_emerg(&pdev->dev, "V33 IRQ REQUEST qid=%u vector=%u result=%d msi=%u msix=%u\n",
		  qid, vector, result, pdev->msi_enabled, pdev->msix_enabled);
	if (result || pdev->msi_enabled || !pdev->msix_enabled || vector > 1 || qid > 1)
		return;
	irq = pci_irq_vector(pdev, vector);
	if (irq <= 0)
		return;
	d = irq_get_irq_data(irq);
	if (!d)
		return;
	desc = irq_data_get_msi_desc(d);
	/* get_cached_msi_msg() dereferences the descriptor unconditionally. */
	if (!desc || desc->dev != &pdev->dev || !desc->pci.msi_attrib.is_msix ||
	    desc->pci.msi_attrib.is_virtual)
		return;
	get_cached_msi_msg(irq, &msg);
	translated = of_msi_xlate(&pdev->dev, &np, pci_dev_id(pdev));
	node_ok = x1_nvme_irq_controller(np);
	address_ok = x1_native_msi_address(pdev, msg.address_lo, msg.address_hi);
	route_ok = node_ok &&
		translated == 0x80100 && address_ok && msg.data == vector;
	dev_emerg(&pdev->dev, "V33 IRQ ROUTE irq=%d rid=%04x dt_id=%08x controller=%pOF cached=%08x:%08x data=%08x\n",
		  irq, pci_dev_id(pdev), translated, np,
		  msg.address_hi, msg.address_lo, msg.data);
	of_node_put(np);
	for (depth = 0; d && depth < 4; depth++, d = d->parent_data) {
		struct irq_chip *chip = irq_data_get_irq_chip(d);

		if (chip && chip->name && !strcmp(chip->name, "ITS") &&
		    chip->irq_set_irqchip_state && d->hwirq >= 8192)
			its_chip = true;
		dev_emerg(&pdev->dev, "V33 IRQ DOMAIN depth=%d hwirq=%lu name=%s chip=%s set_pending=%u\n",
			  depth, (unsigned long)d->hwirq,
			  d->domain && d->domain->name ? d->domain->name : "none",
			  chip && chip->name ? chip->name : "none",
			  !!(chip && chip->irq_set_irqchip_state));
	}
	pos = pdev->msix_cap;
	if (!pos)
		return;
	dev_emerg(&pdev->dev, "V33 IRQ CONFIG BEGIN cap=%02x\n", pos);
	ret = pci_read_config_word(pdev, pos + PCI_MSIX_FLAGS, &ctrl);
	if (!ret) ret = pci_read_config_dword(pdev, pos + PCI_MSIX_TABLE, &table);
	if (!ret) ret = pci_read_config_dword(pdev, pos + PCI_MSIX_PBA, &pba);
	dev_emerg(&pdev->dev, "V33 IRQ CONFIG END status=%d ctrl=%04x table=%08x pba=%08x\n",
		  ret, ctrl, table, pba);
	if (ret || !(ctrl & PCI_MSIX_FLAGS_ENABLE) || !desc->pci.mask_base ||
	    desc->msi_index > (ctrl & PCI_MSIX_FLAGS_QSIZE))
		return;
	/* Same address formula as PCI core pci_msix_desc_addr(). */
	entry = desc->pci.mask_base + desc->msi_index * PCI_MSIX_ENTRY_SIZE;
	dev_emerg(&pdev->dev, "V33 IRQ TABLE BEGIN index=%u\n", desc->msi_index);
	lo = readl(entry + PCI_MSIX_ENTRY_LOWER_ADDR);
	hi = readl(entry + PCI_MSIX_ENTRY_UPPER_ADDR);
	data = readl(entry + PCI_MSIX_ENTRY_DATA);
	mask = readl(entry + PCI_MSIX_ENTRY_VECTOR_CTRL);
	dev_emerg(&pdev->dev, "V33 IRQ TABLE END addr=%08x:%08x data=%08x ctrl=%08x cache_match=%u\n",
		  hi, lo, data, mask,
		  hi == msg.address_hi && lo == msg.address_lo && data == msg.data);
	dev_emerg(&pdev->dev, "V33 IRQ GUARD qid=%u node=%u devid=%u address=%u event=%u its=%u queue=%u index=%u function_unmasked=%u vector_unmasked=%u cache=%u\n",
		  qid, node_ok, translated == 0x80100,
		  address_ok,
		  msg.data == vector, its_chip, qid == vector, desc->msi_index == vector,
		  !(ctrl & PCI_MSIX_FLAGS_MASKALL), !(mask & PCI_MSIX_ENTRY_CTRL_MASKBIT),
		  hi == msg.address_hi && lo == msg.address_lo && data == msg.data);
	/* Do not infer device-originated delivery from a software injection.
	 * v32 already demonstrated that separate host-only path. */
	dev_emerg(&pdev->dev, "V33 IRQ SETUP qid=%u route=%u its=%u entries=%d synthetic=0\n",
		  qid, route_ok, its_chip, atomic_read(&x1_irq_entries[qid]));
}

#include "x1-nvme-irq-evidence.h"

static void x1_nvme_irq_setup(struct pci_dev *pdev, unsigned int qid,
			      unsigned int vector, int result)
{
	x1_nvme_irq_snapshot(pdev, qid, vector, result);
	if (!result)
		x1_nvme_irq_state(pdev, qid, vector, "setup");
}

/* The original IRQ request and its return value are preserved exactly. */
#define X1_NVME_IRQ_REQUEST(pdev, qid, vector, operation) ({ \
	int __x1_irq_ret = (operation); \
	x1_nvme_irq_setup((pdev), (qid), (vector), __x1_irq_ret); \
	__x1_irq_ret; \
})
#endif
