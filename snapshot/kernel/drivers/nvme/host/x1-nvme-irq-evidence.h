/* SPDX-License-Identifier: GPL-2.0 */
/* Private v33. Read-only evidence, not an MSI routing fix. Caller serializes
 * IRQ/queue teardown with shutdown_lock for timeout snapshots. Setup calls
 * are within the existing IRQ-allocation lifetime. No status is cleared.
 */
#ifndef X1_NVME_IRQ_EVIDENCE_H
#define X1_NVME_IRQ_EVIDENCE_H
static void x1_nvme_irq_errors(struct pci_dev *pdev, const char *role,
			       const char *stage, unsigned int qid)
{
	u16 command = 0, status = 0, devsta = 0;
	u32 unc = 0, cor = 0, umask = 0, cmask = 0, cap = 0, header[4] = {};
	unsigned int i;
	int ret, pos = pdev->aer_cap;
	bool aer_valid = pos >= 0x100 && pos <= 0xfc0 && !(pos & 3);

	dev_emerg(&pdev->dev, "V33 IRQ ERROR BEGIN stage=%s qid=%u role=%s aer=%03x\n",
		  stage, qid, role, pos);
	ret = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (!ret) ret = pci_read_config_word(pdev, PCI_STATUS, &status);
	if (!ret) ret = pcie_capability_read_word(pdev, PCI_EXP_DEVSTA, &devsta);
	/* Only a PCI-core-discovered, aligned extended capability. No search
	 * through arbitrary registers, no AER mask changes or W1C writes. */
	if (!ret && aer_valid) {
		ret = pci_read_config_dword(pdev, pos + PCI_ERR_UNCOR_STATUS, &unc);
		if (!ret) ret = pci_read_config_dword(pdev, pos + PCI_ERR_UNCOR_MASK, &umask);
		if (!ret) ret = pci_read_config_dword(pdev, pos + PCI_ERR_COR_STATUS, &cor);
		if (!ret) ret = pci_read_config_dword(pdev, pos + PCI_ERR_COR_MASK, &cmask);
		if (!ret) ret = pci_read_config_dword(pdev, pos + PCI_ERR_CAP, &cap);
		if (!ret && unc && unc != 0xffffffff) {
			for (i = 0; i < ARRAY_SIZE(header) && !ret; i++)
				ret = pci_read_config_dword(pdev, pos + PCI_ERR_HEADER_LOG + 4*i, &header[i]);
		}
	}
	dev_emerg(&pdev->dev, "V33 IRQ ERROR END stage=%s qid=%u role=%s ret=%d cmd=%04x status=%04x devsta=%04x aer=%03x aer_valid=%u unc=%08x umask=%08x cor=%08x cmask=%08x cap=%08x\n",
		  stage, qid, role, ret, command, status, devsta, pos, aer_valid, unc, umask, cor, cmask, cap);
	if (unc && unc != 0xffffffff)
		dev_emerg(&pdev->dev, "V33 IRQ HEADER stage=%s qid=%u role=%s ret=%d words=%08x %08x %08x %08x; sticky evidence, not necessarily an MSI TLP\n",
			  stage, qid, role, ret, header[0], header[1], header[2], header[3]);
}

static void x1_nvme_irq_state(struct pci_dev *pdev, unsigned int qid,
			     unsigned int vector, const char *stage)
{
	struct pci_dev *root;
	struct irq_data *d;
	struct msi_desc *desc;
	struct msi_msg msg = {};
	void __iomem *entry, *pending;
	u32 table = 0, pba = 0, lo, hi, data, mask, bits;
	u16 ctrl = 0;
	int irq, ret;

	if (!x1_nvme_irq_test(pdev) || qid > 1 || qid != vector ||
	    pdev->msi_enabled || !pdev->msix_enabled || !pdev->msix_cap)
		return;
	irq = pci_irq_vector(pdev, vector);
	if (irq <= 0 || !(d = irq_get_irq_data(irq)))
		return;
	desc = irq_data_get_msi_desc(d);
	if (!desc || desc->dev != &pdev->dev || !desc->pci.msi_attrib.is_msix ||
	    desc->pci.msi_attrib.is_virtual || desc->msi_index != vector ||
	    !desc->pci.mask_base)
		return;
	get_cached_msi_msg(irq, &msg);
	x1_its_msi_audit(pdev, irq, !strcmp(stage, "timeout"));
	dev_emerg(&pdev->dev, "V33 IRQ STATE BEGIN stage=%s qid=%u entries=%d synthetic=0\n",
		  stage, qid, atomic_read(&x1_irq_entries[qid]));
	ret = pci_read_config_word(pdev, pdev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	if (!ret) ret = pci_read_config_dword(pdev, pdev->msix_cap + PCI_MSIX_TABLE, &table);
	if (!ret) ret = pci_read_config_dword(pdev, pdev->msix_cap + PCI_MSIX_PBA, &pba);
	dev_emerg(&pdev->dev, "V33 IRQ FLAGS stage=%s qid=%u ret=%d ctrl=%04x table=%08x pba=%08x\n",
		  stage, qid, ret, ctrl, table, pba);
	if (ret || !(ctrl & PCI_MSIX_FLAGS_ENABLE) ||
	    desc->msi_index > (ctrl & PCI_MSIX_FLAGS_QSIZE))
		return;
	entry = desc->pci.mask_base + desc->msi_index * PCI_MSIX_ENTRY_SIZE;
	dev_emerg(&pdev->dev, "V33 IRQ ENTRY BEGIN stage=%s qid=%u\n", stage, qid);
	lo = readl(entry + PCI_MSIX_ENTRY_LOWER_ADDR);
	hi = readl(entry + PCI_MSIX_ENTRY_UPPER_ADDR);
	data = readl(entry + PCI_MSIX_ENTRY_DATA);
	mask = readl(entry + PCI_MSIX_ENTRY_VECTOR_CTRL);
	dev_emerg(&pdev->dev, "V33 IRQ ENTRY END stage=%s qid=%u addr=%08x:%08x data=%08x mask=%08x cache_match=%u\n",
		  stage, qid, hi, lo, data, mask,
		  lo == msg.address_lo && hi == msg.address_hi && data == msg.data);
	/* Only the PBA layout actually observed on this admitted LaCie. A dword
	 * read covers vectors 0/1; it neither acknowledges nor clears pending.
	 * Map precisely four bytes inside the existing owned BAR0 resource.
	 * Read even if masked: a pending masked vector is useful evidence. */
	if (table == 0x2000 && pba == 0x3000 &&
	    (pci_resource_flags(pdev, 0) & IORESOURCE_MEM) &&
	    pci_resource_start(pdev, 0) && pci_resource_len(pdev, 0) >= 0x3004) {
		dev_emerg(&pdev->dev, "V33 IRQ PBA BEGIN stage=%s qid=%u\n", stage, qid);
		pending = pci_iomap_range(pdev, 0, 0x3000, sizeof(u32));
		if (pending) {
			bits = readl(pending);
			pci_iounmap(pdev, pending);
			dev_emerg(&pdev->dev, "V33 IRQ PBA END stage=%s qid=%u bits=%08x pending=%u valid=%u\n",
				  stage, qid, bits, !!(bits & (1U << vector)), bits != 0xffffffff);
		} else {
			dev_emerg(&pdev->dev, "V33 IRQ PBA SKIP stage=%s qid=%u reason=map-failed\n", stage, qid);
		}
	} else {
		dev_emerg(&pdev->dev, "V33 IRQ PBA SKIP stage=%s qid=%u reason=layout-or-resource\n", stage, qid);
	}
	x1_nvme_irq_errors(pdev, "endpoint", stage, qid);
	root = pci_upstream_bridge(pdev);
	if (root && pci_domain_nr(root->bus) == 0 && pci_dev_id(root) == 0 &&
	    root->vendor == 0x17cb && root->device == 0x0111 &&
	    pci_pcie_type(root) == PCI_EXP_TYPE_ROOT_PORT)
		x1_nvme_irq_errors(root, "root", stage, qid);
	else
		dev_emerg(&pdev->dev, "V33 IRQ ERROR SKIP stage=%s qid=%u role=root reason=identity\n", stage, qid);
}
#endif
