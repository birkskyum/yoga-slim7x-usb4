// SPDX-License-Identifier: GPL-2.0-only
/* Reference excerpt from built-in pcie-qcom.c, not a standalone module. */
#include <linux/of.h>
#include <linux/pci.h>

/*
 * Private Yoga PCI0 host only.  This must be built in: the PCI core's early
 * fixup table does not include declarations in the loadable USB4 frontend.
 * Correct Linux's class, not the hardware class register.  The root port
 * must not allocate from the endpoint's internal MSI receiver.  Its driver
 * remains held by the frontend's scan-phase per-device override; no_msi is
 * not permission to start unvalidated root-port INTx service interrupts.
 */
static void qcom_yoga_x1_pci0_fixup(struct pci_dev *dev)
{
	struct pci_host_bridge *bridge;
	struct device_node *expected;
	bool owned;

	if (!of_machine_is_compatible("lenovo,yoga-slim7x") ||
	    !dev->bus || !pci_is_root_bus(dev->bus) ||
	    pci_domain_nr(dev->bus) || dev->bus->number || dev->devfn ||
	    dev->vendor != PCI_VENDOR_ID_QCOM || dev->device != 0x0111 ||
	    dev->hdr_type != PCI_HEADER_TYPE_BRIDGE || dev->multifunction ||
	    !pci_is_pcie(dev) || pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT ||
	    (dev->class != 0xff0000 &&
	     dev->class != PCI_CLASS_BRIDGE_PCI_NORMAL))
		return;

	bridge = pci_find_host_bridge(dev->bus);
	if (!bridge || bridge->bus != dev->bus || !bridge->dev.parent)
		return;
	expected = of_find_node_by_path("/soc@0/pcie-imsi@400000000");
	owned = expected && bridge->dev.parent->of_node == expected &&
		of_device_is_available(expected) &&
		of_device_is_compatible(expected, "birk,yoga-x1-pci0-imsi-local");
	of_node_put(expected);
	if (!owned)
		return;

	dev->class = PCI_CLASS_BRIDGE_PCI_NORMAL;
	dev->no_msi = 1;
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0111,
		      qcom_yoga_x1_pci0_fixup);
