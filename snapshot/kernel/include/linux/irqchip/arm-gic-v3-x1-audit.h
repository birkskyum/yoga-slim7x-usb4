/* SPDX-License-Identifier: GPL-2.0-only */
/* Private RAM-only Yoga diagnostic; not a driver ABI. */
#ifndef ARM_GIC_V3_X1_AUDIT_H
#define ARM_GIC_V3_X1_AUDIT_H
struct pci_dev;
#if IS_ENABLED(CONFIG_USB4_X1_NATIVE) && IS_ENABLED(CONFIG_ARM_GIC_V3_ITS)
void x1_its_msi_audit(struct pci_dev *pdev, unsigned int irq, bool timeout);
#else
static inline void x1_its_msi_audit(struct pci_dev *pdev, unsigned int irq,
				    bool timeout) { }
#endif
#endif
