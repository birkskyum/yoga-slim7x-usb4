/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef X1_PCIE_H
#define X1_PCIE_H
int x1_pcie_start(void __iomem *router);
int x1_pcie_train(void);
int x1_pcie_scan(void);
#endif
