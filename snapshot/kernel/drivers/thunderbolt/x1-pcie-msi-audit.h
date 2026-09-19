/* SPDX-License-Identifier: GPL-2.0-only */
/* Private v37, read-only PCI0 evidence. Call once under bridge_lock, after
 * the guarded external scan. These are standard DWC / Qualcomm PARF offsets
 * in mappings already owned by this bridge. No clear-on-read registers,
 * writes, retries, speculative ID mappings or new physical mappings.
 * See MSIX-V37.md for provenance and limits on interpreting the values.
 */
#ifndef X1_PCIE_MSI_AUDIT_H
#define X1_PCIE_MSI_AUDIT_H
#define X1_MSI_AUDIT_WORDS 9
static void x1_pci0_msi_audit(struct x1_pci0_io *io,
			      u32 values[X1_MSI_AUDIT_WORDS])
{
	static const u32 regs[X1_MSI_AUDIT_WORDS][2] = {
		{ X1_ROOT, 0x820 }, /* PCIE_MSI_ADDR_LO */
		{ X1_ROOT, 0x824 }, /* PCIE_MSI_ADDR_HI */
		{ X1_ROOT, 0x828 }, /* PCIE_MSI_INTR0_ENABLE */
		{ X1_ROOT, 0x82c }, /* PCIE_MSI_INTR0_MASK */
		{ X1_ROOT, 0x830 }, /* PCIE_MSI_INTR0_STATUS (write-one-to-clear) */
		{ X1_PARF, 0x1a8 }, /* PARF_AXI_MSTR_WR_ADDR_HALT_V2 */
		{ X1_PARF, 0x234 }, /* PARF_SID_OFFSET */
		{ X1_PARF, 0x24c }, /* PARF_BDF_TRANSLATE_CFG */
		{ X1_PARF, 0x2c00 }, /* PARF_BDF_TO_SID_CFG */
	};
	unsigned int i;

	for (i = 0; i < X1_MSI_AUDIT_WORDS; i++)
		values[i] = x1_pr(io, regs[i][0], regs[i][1]);
}
#endif
