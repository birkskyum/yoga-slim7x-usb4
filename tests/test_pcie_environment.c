/* Exact production policy/address code, no hardware or mapping writes. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
struct device { int unused; };
struct pci_dev { struct device dev; };
static struct pci_dev endpoint;
#include "x1_address_mock.h"

int main(void)
{
    for (unsigned int f=0; f<128; f++) {
        bool accepted=x1_pcie_boot_policy(f&1,f&2,f&4,f&8,f&16,f&32,f&64);
        assert(accepted == (f==119 || (f&31)==8));
    }
    address_reset();
    assert(x1_native_msi_address(&endpoint,0x17050040,0));
    assert(!x1_native_msi_address(&endpoint,0x17050040,1));
    assert(!x1_native_msi_address(&endpoint,0x17050044,0));
    assert(!mock_translations);
    mock_el2=true;
    assert(!x1_native_msi_address(&endpoint,0x17050040,0));
    mock_domain_present=true;
    for (unsigned int type=0; type<8; type++) {
        mock_domain.type=type;
        assert(!!x1_pcie_dma_domain(&endpoint.dev) ==
               (type==IOMMU_DOMAIN_DMA || type==IOMMU_DOMAIN_DMA_FQ));
        assert(x1_native_msi_address(&endpoint,(u32)mock_iova,0) ==
               (type==IOMMU_DOMAIN_DMA || type==IOMMU_DOMAIN_DMA_FQ));
    }
    mock_domain.type=IOMMU_DOMAIN_DMA;
    mock_iova=0x100000040ULL;
    assert(x1_native_msi_address(&endpoint,0x40,1));
    assert(!x1_native_msi_address(&endpoint,0x40,0));
    mock_tail_missing=true;
    assert(!x1_native_msi_address(&endpoint,0x40,1));
    mock_tail_missing=false;mock_physical=0;
    assert(!x1_native_msi_address(&endpoint,0x40,1));
    mock_physical=0x17050044;
    assert(!x1_native_msi_address(&endpoint,0x40,1));
    mock_physical=0x17050040;mock_iova=0x17050040;
    assert(x1_native_msi_address(&endpoint,0x17050040,0));
    mock_translations=0;
    assert(!x1_native_msi_address(&endpoint,0x17050041,0));
    assert(!x1_native_msi_address(&endpoint,0xffffffff,0xffffffff));
    assert(!mock_translations);
    puts("PASS EL2 source policy: 128 boot combinations; absent/identity/blocked/unmanaged domains refused; 32/64-bit IOVA, 1:1, unmapped, wrong/torn/unaligned MSI target; no mapping writes.");
}
