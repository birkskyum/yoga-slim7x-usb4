/* SPDX-License-Identifier: GPL-2.0 */
/* Private development checkpoint, not a general NVMe or EL1 workaround. */
#ifndef X1_PRIVATE_NVME_READ_H
#define X1_PRIVATE_NVME_READ_H

#include <linux/of.h>
#include <linux/property.h>

/* Only probe-time topology inspection. No firmware calls or IRQ-time OF walk.
 * The private fence applies only to the harness DT, which carries the read-test
 * opt-in; the Omarchy flavor's DT omits it and gets the standard NVMe driver.
 */
static bool x1_nvme_private_host(struct pci_dev *pdev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(pdev->bus);
	struct device_node *expected;
	bool match;

	if (!host || !host->dev.parent ||
	    !of_machine_is_compatible("lenovo,yoga-slim7x"))
		return false;
	expected = of_find_node_by_path("/soc@0/pcie-imsi@400000000");
	match = expected && host->dev.parent->of_node == expected &&
		of_device_is_compatible(expected, "birk,yoga-x1-pci0-imsi-local") &&
		of_property_read_bool(expected, "qcom,x1-private-nvme-read-test");
	of_node_put(expected);
	return match;
}

static bool x1_nvme_read_admitted(struct pci_dev *pdev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(pdev->bus);
	struct pci_dev *root = pci_upstream_bridge(pdev);

	return x1_nvme_private_host(pdev) &&
		device_property_read_bool(host->dev.parent,
					  "qcom,x1-private-nvme-read-test") &&
		pci_domain_nr(pdev->bus) == 0 && pdev->bus->number == 1 &&
		pdev->devfn == 0 && pdev->vendor == 0x1c19 &&
		pdev->device == 0x102b && pdev->class == PCI_CLASS_STORAGE_EXPRESS &&
		root && pci_is_root_bus(root->bus) && root->bus->number == 0 &&
		root->devfn == 0 && root->vendor == 0x17cb &&
		root->device == 0x0111 &&
		pci_pcie_type(root) == PCI_EXP_TYPE_ROOT_PORT;
}

/* One kernel-owned ticket per actual PCI device, never a resettable probe
 * flag. A failed generation remains admitted/consumed until cold boot. */
struct x1_nvme_session {
	struct pci_dev *pdev;
	u64 id;
	bool consumed, resources_retired;
};
static DEFINE_MUTEX(x1_nvme_session_lock);
static struct x1_nvme_session *x1_nvme_session;
static u64 x1_nvme_session_sequence;

int nvme_x1_open_session(struct pci_dev *pdev, u64 *id)
{
	struct x1_nvme_session *session;
	int ret = -EPERM;

	if (!pdev || !id || !x1_nvme_read_admitted(pdev))
		return -EINVAL;
	mutex_lock(&x1_nvme_session_lock);
	if (x1_nvme_session || pdev->dev.driver || pci_get_drvdata(pdev) ||
	    !device_is_registered(&pdev->dev) || x1_nvme_session_sequence == U64_MAX)
		goto out;
	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session) {
		ret = -ENOMEM;
		goto out;
	}
	session->pdev = pci_dev_get(pdev);
	session->id = ++x1_nvme_session_sequence;
	x1_nvme_session = session;
	*id = session->id;
	ret = 0;
out:
	mutex_unlock(&x1_nvme_session_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_x1_open_session);

static bool x1_nvme_claim_session(struct pci_dev *pdev)
{
	bool admitted = false;

	mutex_lock(&x1_nvme_session_lock);
	if (x1_nvme_session && x1_nvme_session->pdev == pdev &&
	    !x1_nvme_session->consumed && !x1_nvme_session->resources_retired) {
		x1_nvme_session->consumed = true;
		admitted = true;
	}
	mutex_unlock(&x1_nvme_session_lock);
	return admitted;
}

/* Called only by the driver's own completed resource-retirement path. */
static int x1_nvme_session_resources_retired(struct pci_dev *pdev)
{
	int ret = -ESTALE;

	mutex_lock(&x1_nvme_session_lock);
	if (x1_nvme_session && x1_nvme_session->pdev == pdev &&
	    x1_nvme_session->consumed && !x1_nvme_session->resources_retired) {
		x1_nvme_session->resources_retired = true;
		ret = 0;
	}
	mutex_unlock(&x1_nvme_session_lock);
	return ret;
}

int nvme_x1_close_session(struct pci_dev *pdev, u64 id)
{
	struct x1_nvme_session *session;
	int ret = -EPERM;

	if (!pdev || !id)
		return -EINVAL;
	mutex_lock(&x1_nvme_session_lock);
	session = x1_nvme_session;
	/* No topology walks: the old PCI bus has already been unregistered. */
	if (!session || session->pdev != pdev || session->id != id ||
	    !session->consumed || !session->resources_retired ||
	    pdev->dev.driver || pci_get_drvdata(pdev) || pdev->dev.msi.data ||
	    device_is_registered(&pdev->dev))
		goto out;
	x1_nvme_session = NULL;
	pci_dev_put(session->pdev);
	kfree(session);
	ret = 0;
out:
	mutex_unlock(&x1_nvme_session_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_x1_close_session);

struct x1_nvme_read_evidence {
	/* Immutable probe-time software configuration, not DMA delivery proof. */
	u64 dma_mask;
	u64 coherent_dma_mask;
	atomic64_t hard_irq[2];
	atomic64_t irq_cqe[2];
	atomic64_t timeouts[2];
	atomic64_t poll_refused;
	atomic64_t read_submitted;
	atomic64_t read_completed;
	atomic64_t write_submitted;
	atomic64_t write_completed;
	atomic64_t flush_submitted;
	atomic64_t flush_completed;
	u16 last_cid[2];
	u16 last_status[2];
};

/* First error persists; a late genuine completion cannot restore success. */
static void x1_nvme_read_stop(struct nvme_ctrl *ctrl, int error)
{
	if (ctrl->x1_private_read_test)
		atomic_cmpxchg(&ctrl->x1_private_terminal, 0,
			      error < 0 ? error : -EIO);
}

#endif
