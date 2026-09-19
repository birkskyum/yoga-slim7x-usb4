/* SPDX-License-Identifier: GPL-2.0-only */
/* Private RAM-only diagnostic interface, not a supported Linux driver ABI. */
#ifndef X1_USB4_DIAG_H
#define X1_USB4_DIAG_H
#include <linux/of.h>
#include <linux/phy/phy.h>

enum x1_diag_gcc_op {
	X1_GCC_RX_REFERENCE, X1_GCC_RX_PHY,
	X1_GCC_MISC_ASSERT, X1_GCC_MISC_CLEAR,
	X1_GCC_EXTRA_ASSERT, X1_GCC_EXTRA_CLEAR,
	X1_GCC_PIPE_ASSERT, X1_GCC_PIPE_CLEAR,
	X1_GCC_SYS_READY, X1_GCC_MEMORY_FORCE, X1_GCC_HWCG_OFF,
};
enum x1_diag_phy_op { X1_DIAG_PHY_PREPARE, X1_DIAG_PHY_DP_ASSERT, X1_DIAG_PHY_DP_CLEAR };

static inline bool x1_diag_disabled(const char *path)
{
	struct device_node *np = of_find_node_by_path(path);
	bool disabled = np && !of_device_is_available(np);
	of_node_put(np);
	return disabled;
}

static inline bool x1_diag_board_allowed(void)
{
	if (IS_ENABLED(CONFIG_USB4_X1_NATIVE) &&
	    !of_property_read_bool(of_root, "birk,usb4-native-cm-test"))
		return false;
	if (IS_ENABLED(CONFIG_USB_DWC3) || IS_ENABLED(CONFIG_DRM_MSM) ||
	    (IS_ENABLED(CONFIG_QCOM_PMIC_GLINK) &&
	     (!IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG) ||
	      !of_property_read_bool(of_root, "birk,usb4-typec-observer"))) ||
	    (IS_ENABLED(CONFIG_TYPEC_MUX_PS883X) &&
	     (!IS_MODULE(CONFIG_TYPEC_MUX_PS883X) ||
	      !IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG) ||
	      !of_property_read_bool(of_root, "birk,usb4-retimer-test"))) ||
	    IS_ENABLED(CONFIG_TYPEC_UCSI) ||
	    IS_ENABLED(CONFIG_USB_XHCI_HCD) ||
	    (IS_ENABLED(CONFIG_USB4) && !IS_ENABLED(CONFIG_USB4_X1_DIAG)) ||
	    IS_ENABLED(CONFIG_USB4_QCOM) ||
	    (IS_ENABLED(CONFIG_BLK_DEV_NVME) &&
	     (!IS_ENABLED(CONFIG_USB4_X1_NATIVE) || !IS_MODULE(CONFIG_BLK_DEV_NVME) ||
	      !of_property_read_bool(of_root, "birk,usb4-pci0-test") ||
	      !of_property_read_bool(of_root, "birk,usb4-pci0-init-v23") ||
	      !x1_diag_disabled("/soc@0/pcie@1bf8000") ||
	      !x1_diag_disabled("/soc@0/pcie@1c08000"))) ||
	    IS_ENABLED(CONFIG_SCSI) || IS_ENABLED(CONFIG_ATA) ||
	    IS_ENABLED(CONFIG_MMC) || IS_ENABLED(CONFIG_MTD) ||
	    IS_ENABLED(CONFIG_USB_STORAGE) || IS_ENABLED(CONFIG_USB_UAS))
		return false;
	return of_machine_is_compatible("lenovo,yoga-slim7x") &&
		of_property_read_bool(of_root, "birk,usb4-mcu-ram-probe") &&
		x1_diag_disabled("/soc@0/usb@a600000") &&
		x1_diag_disabled("/soc@0/display-subsystem@ae00000");
}

int x1_diag_gcc_claim(struct device *owner);
int x1_diag_gcc_apply(struct device *owner, enum x1_diag_gcc_op op);
int x1_diag_phy_apply(struct phy *phy, enum x1_diag_phy_op op);
int x1_diag_nhi_run(struct device *dev, void __iomem *router);
int x1_diag_link_register(int (*connect)(void *, u32), void *ctx);
int x1_diag_link_run(u32 word, bool (*valid)(void *), void *ctx);
void x1_diag_link_stop(void);
bool x1_diag_nhi_complete(void);
struct pci_dev;
#if IS_ENABLED(CONFIG_USB4_X1_NATIVE)
bool x1_native_nvme_allowed(struct pci_dev *pdev);
bool x1_native_msi_address(struct pci_dev *pdev, u32 lo, u32 hi);
#else
static inline bool x1_native_nvme_allowed(struct pci_dev *pdev) { return false; }
static inline bool x1_native_msi_address(struct pci_dev *pdev, u32 lo, u32 hi) { return false; }
#endif
#endif
