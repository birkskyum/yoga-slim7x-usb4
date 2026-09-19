/* SPDX-License-Identifier: GPL-2.0-only */
/* Source-only EL2 candidate. Providers are supplied by the owning driver.
 * No MMIO, mapping changes, IRQ injection or discovery of alternate IDs.
 */
#ifndef X1_PCIE_ENVIRONMENT_H
#define X1_PCIE_ENVIRONMENT_H

static inline bool x1_pcie_boot_policy(bool el2_build, bool hyp, bool el2_marker,
			       bool reserved, bool available, bool maps,
			       bool other_hosts_disabled)
{
	if (el2_build)
		return hyp && el2_marker && !reserved && available && maps &&
			other_hosts_disabled;
	return !hyp && !el2_marker && reserved && !available;
}

/* Called only for the admitted PCI function, from its bound NVMe driver
 * (including probe and hard IRQ). Never query an unbound rescan candidate.
 * This is a Linux domain check, not proof of the hardware's actual SID.
 */
static inline struct iommu_domain *x1_pcie_dma_domain(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);

	if (!domain || (domain->type != IOMMU_DOMAIN_DMA &&
			domain->type != IOMMU_DOMAIN_DMA_FQ))
		return NULL;
	return domain;
}

static inline bool x1_pcie_msi_target(struct device *dev, bool el2, u64 address,
			       u64 *physical)
{
	struct iommu_domain *domain;

	*physical = 0;
	/* MSI is an aligned four-byte write. Reject wraparound before lookup. */
	if (address & 3)
		return false;
	if (!el2) {
		*physical = address;
		return address == 0x17050040;
	}
	domain = x1_pcie_dma_domain(dev);
	if (!domain)
		return false;
	*physical = iommu_iova_to_phys(domain, address);
	return *physical == 0x17050040 &&
		iommu_iova_to_phys(domain, address + 3) == 0x17050043;
}
#endif
