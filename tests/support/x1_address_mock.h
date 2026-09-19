/* Offline providers for the exact production domain/address predicates. */
enum { IOMMU_DOMAIN_IDENTITY = 1, IOMMU_DOMAIN_BLOCKED,
       IOMMU_DOMAIN_DMA, IOMMU_DOMAIN_DMA_FQ, IOMMU_DOMAIN_UNMANAGED };
struct iommu_domain { unsigned int type; };
static struct iommu_domain mock_domain;
static bool mock_el2, mock_domain_present, mock_tail_missing;
static u64 mock_iova, mock_physical;
static unsigned int mock_translations;
static struct iommu_domain *iommu_get_domain_for_dev(struct device *dev)
{
    assert(dev == &endpoint.dev);
    return mock_domain_present ? &mock_domain : NULL;
}
static u64 iommu_iova_to_phys(struct iommu_domain *domain, u64 address)
{
    assert(domain == &mock_domain);
    mock_translations++;
    if (address == mock_iova) return mock_physical;
    if (address == mock_iova + 3 && !mock_tail_missing) return mock_physical + 3;
    return 0;
}
#include "kernel/drivers/thunderbolt/x1-pcie-environment.h"
static bool x1_native_msi_address(struct pci_dev *pdev, u32 lo, u32 hi)
{
    u64 physical;
    assert(pdev == &endpoint);
    return x1_pcie_msi_target(&pdev->dev, mock_el2, ((u64)hi << 32) | lo, &physical);
}
static void address_reset(void)
{
    mock_el2 = mock_domain_present = mock_tail_missing = false;
    mock_domain.type = IOMMU_DOMAIN_DMA;
    mock_iova = 0x12345040;
    mock_physical = 0x17050040;
    mock_translations = 0;
}
