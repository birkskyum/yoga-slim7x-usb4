// SPDX-License-Identifier: GPL-2.0-only
/*
 * PRIVATE X1 PCI0 iMSI-RX adapter. No independent module/driver registration.
 * Dev19 stop/release changes are not hardware-tested.
 *
 * API sequence derives from pinned qcom_pcie_ecam_host_init(). All receiver
 * implementation remains in DesignWare's GPL helpers. The IRQ assignment is
 * private supplied platform information; do not publish this candidate.
 *
 * This is NOT endpoint DMA containment. Standard DWC reserves a coherent MSI
 * message target when cfg0_base is above 4 GiB. Matching MSI writes are expected
 * to terminate in iMSI-RX, but external DMA/SMMU behavior remains unproved.
 * No generic host_init/setup_rc/PHY/reset/ATU/scan helper is called here.
 */
#include <linux/dma-mapping.h>
#include <linux/bitmap.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>

/* Add pinned drivers/pci/controller/dwc to the standalone compile include path. */
#include "pcie-designware.h"
#include "x1-imsi.h"

#define X1_DBI 0x400000000ULL
#define X1_CFG0 0x400100000ULL
#define X1_PARF 0x1c28000ULL
/* Private development DT opt-in; not an upstream binding. */
#define X1_IMSI_HOST_NODE "/soc@0/pcie-imsi@400000000"
#define X1_IMSI_HOST_COMPAT "birk,yoga-x1-pci0-imsi-local"

struct x1_imsi {
	struct x1_imsi_request request;
	struct dw_pcie pci;
	struct x1_imsi_receipt receipt;
	int parents[8], global;
	void *target_group;
	bool target_group_closed;
};

static DEFINE_MUTEX(x1_imsi_lock);
static struct x1_imsi *x1_imsi_active;

static int x1_imsi_guard(struct x1_imsi *r)
{
	int ret = r->request.check_live(r->request.context);

	if (ret)
		return ret < 0 ? ret : -EIO;
	if (!pm_runtime_active(&r->request.pdev->dev) ||
	    !pm_runtime_active(r->request.owner))
		return -EHOSTDOWN;
	return 0;
}

static bool x1_imsi_resource(struct platform_device *pdev, const char *name,
			     u64 start, u64 size)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	return res && res->start == start && resource_size(res) == size;
}

static int x1_imsi_parent(struct platform_device *pdev, unsigned int index,
			  const char *name, u32 spi, struct device_node *intc)
{
	struct of_phandle_args spec;
	struct irq_data *data;
	int irq, ret;

	if (of_property_match_string(pdev->dev.of_node, "interrupt-names", name) != (int)index)
		return -EINVAL;
	ret = of_irq_parse_one(pdev->dev.of_node, index, &spec);
	if (ret)
		return ret;
	ret = spec.np == intc && spec.args_count == 3 && spec.args[0] == GIC_SPI &&
		spec.args[1] == spi && spec.args[2] == IRQ_TYPE_LEVEL_HIGH ? 0 : -EINVAL;
	of_node_put(spec.np);
	if (ret)
		return ret;
	irq = platform_get_irq_byname(pdev, name);
	if (irq <= 0)
		return irq ?: -EINVAL;
	data = irq_get_irq_data(irq);
	if (!data || !data->domain || irq_domain_get_of_node(data->domain) != intc ||
	    irqd_to_hwirq(data) != spi + 32 ||
	    irqd_get_trigger_type(data) != IRQ_TYPE_LEVEL_HIGH ||
	    irq_has_action(irq) || irq_get_handler_data(irq))
		return -EBUSY;
	return irq;
}

static int x1_imsi_admit(struct x1_imsi *r)
{
	static const u32 spis[] = { 655, 657, 664, 668, 348, 349, 351, 702, 167 };
	struct device *dev = &r->request.pdev->dev;
	struct device_node *intc, *owner, *smmu, *host;
	const char *status;
	unsigned int i, j;
	int irq, ret = -ENODEV;

	if (!of_machine_is_compatible("lenovo,yoga-slim7x") || !dev->of_node ||
	    !dev_fwnode(dev) || dev == r->request.owner ||
	    !dev->driver || !dev->driver->suppress_bind_attrs ||
	    !r->request.owner->driver || !r->request.owner->driver->suppress_bind_attrs ||
	    r->request.bridge->dev.parent != dev || r->request.bridge->bus ||
	    dev_get_msi_domain(&r->request.bridge->dev) || r->request.bridge->msi_domain ||
	    !of_node_is_type(dev->of_node, "pci") || !pci_msi_enabled())
		return -ENODEV;
	if (of_property_present(dev->of_node, "msi-map") ||
	    of_property_present(dev->of_node, "msi-map-mask") ||
	    of_property_present(dev->of_node, "msi-parent") ||
	    of_property_present(dev->of_node, "iommus") ||
	    of_property_present(dev->of_node, "iommu-map") ||
	    irq_find_matching_fwnode(dev_fwnode(dev), DOMAIN_BUS_PCI_MSI))
		return -EBUSY;
	host = of_find_node_by_path(X1_IMSI_HOST_NODE);
	ret = host && host == dev->of_node &&
		of_device_is_compatible(host, X1_IMSI_HOST_COMPAT) ? 0 : -ENODEV;
	of_node_put(host);
	if (ret)
		return ret;
	if (!x1_imsi_resource(r->request.pdev, "root", X1_DBI, 0x2000) ||
	    !x1_imsi_resource(r->request.pdev, "config", X1_CFG0, 0x1000) ||
	    !x1_imsi_resource(r->request.pdev, "parf", X1_PARF, 0x8000) ||
	    of_property_count_strings(dev->of_node, "interrupt-names") != 9 ||
	    of_irq_count(dev->of_node) != 9)
		return -EINVAL;
	owner = of_find_node_by_path("/soc@0/usb4@15600000");
	if (owner != r->request.owner->of_node ||
	    !of_device_is_compatible(owner, "qcom,x1e80100-usb4")) {
		of_node_put(owner);
		return -ENODEV;
	}
	of_node_put(owner);
	smmu = of_find_node_by_path("/soc@0/iommu@15400000");
	if (!smmu)
		return -ENODEV;
	ret = of_property_read_string(smmu, "status", &status);
	if (!ret && (strcmp(status, "reserved") ||
		     !of_device_is_compatible(smmu, "arm,smmu-v3")))
		ret = -ENODEV;
	of_node_put(smmu);
	if (ret)
		return ret;
	intc = of_find_node_by_path("/soc@0/interrupt-controller@17000000");
	if (!intc)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(spis); i++) {
		char name[] = "msi0";

		name[3] += i;
		irq = x1_imsi_parent(r->request.pdev, i, i == 8 ? "global" : name,
				     spis[i], intc);
		if (irq < 0) {
			ret = irq;
			goto out;
		}
		for (j = 0; j < i && j < 8; j++)
			if (r->parents[j] == irq) {
				ret = -EINVAL;
				goto out;
			}
		if (i == 8)
			r->global = irq;
		else
			r->parents[i] = irq;
	}
	ret = 0;
out:
	of_node_put(intc);
	return ret;
}

static int x1_imsi_write_checked(void __iomem *base, u32 offset, u32 value)
{
	writel(value, base + offset);
	return readl(base + offset) == value ? 0 : -EIO;
}

static int x1_imsi_setup(struct x1_imsi *r)
{
	struct dw_pcie_rp *pp = &r->pci.pp;
	unsigned int i;
	int ret;

	r->receipt.step = "initial-fence";
	ret = x1_imsi_guard(r);
	if (ret)
		return ret;
	/* A future caller must separately validate live root/topology/MSI-off. */
	if (readl(r->request.dbi) != 0x011117cb)
		return -EBUSY;
	r->receipt.step = "mask-eight-banks";
	for (i = 0; i < 8; i++) {
		u32 off = i * MSI_REG_CTRL_BLOCK_SIZE;

		ret = x1_imsi_guard(r);
		if (ret)
			return ret;
		ret = x1_imsi_write_checked(r->request.dbi, PCIE_MSI_INTR0_MASK + off, U32_MAX);
		if (ret)
			return ret;
		if (readl(r->request.dbi + PCIE_MSI_INTR0_STATUS + off))
			return -EBUSY;
	}
	r->pci.dev = &r->request.pdev->dev;
	r->pci.dbi_base = r->request.dbi;
	pp->cfg0_base = X1_CFG0; /* NOT zero: force standard reserved-target allocation. */
	pp->num_vectors = 256;
	pp->bridge = r->request.bridge;
	raw_spin_lock_init(&pp->lock);
	r->receipt.step = "dw-msi-host-init";
	ret = x1_imsi_guard(r);
	if (ret)
		return ret;
	/* Leave msi_irq[] zero so the standard helper parses all eight names. */
	/* DWC keeps the target's virtual address private to its helper. A closed
	 * devres group owns exactly that helper's managed allocations; do not
	 * guess a virtual address from msi_data or release platform-wide devres.
	 */
	r->target_group = devres_open_group(r->pci.dev, NULL, GFP_KERNEL);
	if (!r->target_group)
		return -ENOMEM;
	ret = dw_pcie_msi_host_init(pp);
	devres_close_group(r->pci.dev, r->target_group);
	r->target_group_closed = true;
	if (ret)
		return ret; /* The standard helper unwinds its own allocation failure. */
	r->receipt.domain_ready = true;
	r->receipt.target_reserved = true;
	r->receipt.target = pp->msi_data;
	r->receipt.banks = 8;
	r->receipt.vectors = pp->num_vectors;
	for (i = 0; i < 8; i++)
		if (pp->msi_irq[i] != r->parents[i])
			return -EINVAL;
	if (!pp->irq_domain || pp->num_vectors != 256 ||
	    (u64)pp->msi_data == U64_MAX || pp->msi_data & 7 ||
	    ((u64)pp->msi_data >= X1_DBI && (u64)pp->msi_data < X1_DBI + 0x10000000ULL))
		return -EINVAL;
	ret = r->request.target_check(r->request.context, pp->msi_data);
	if (ret)
		return ret < 0 ? ret : -EIO;
	r->receipt.step = "dw-msi-register-init";
	ret = x1_imsi_guard(r);
	if (ret)
		return ret;
	pp->use_imsi_rx = true;
	dw_pcie_msi_init(pp);
	if (readl(r->request.dbi + PCIE_MSI_ADDR_LO) != lower_32_bits(pp->msi_data) ||
	    readl(r->request.dbi + PCIE_MSI_ADDR_HI) != upper_32_bits(pp->msi_data))
		return -EIO;
	for (i = 0; i < 8; i++) {
		u32 off = i * MSI_REG_CTRL_BLOCK_SIZE;

		if (readl(r->request.dbi + PCIE_MSI_INTR0_MASK + off) != U32_MAX ||
		    readl(r->request.dbi + PCIE_MSI_INTR0_ENABLE + off) != U32_MAX ||
		    readl(r->request.dbi + PCIE_MSI_INTR0_STATUS + off))
			return -EIO;
	}
	r->receipt.step = "global-route-policy";
	ret = x1_imsi_guard(r);
	if (ret)
		return ret;
	ret = r->request.global_route(r->request.context, r->global);
	if (ret)
		return ret < 0 ? ret : -EIO;
	r->receipt.global_policy_complete = true;
	r->receipt.step = "bridge-domain";
	ret = x1_imsi_guard(r);
	if (ret)
		return ret;
	/* No PCI bus is published; caller must verify inheritance after publication. */
	dev_set_msi_domain(&r->request.bridge->dev, pp->irq_domain);
	r->request.bridge->msi_domain = true;
	r->receipt.associated = true;
	r->receipt.ready = true;
	r->receipt.step = "receiver-ready-no-endpoint-dma-proof";
	return 0;
}

int x1_imsi_prepare(const struct x1_imsi_request *request, struct x1_imsi **out)
{
	struct x1_imsi *r;
	int ret;

	if (!request || !out || !request->pdev || !request->owner || !request->bridge ||
	    !request->dbi || !request->parf || !request->check_live || !request->retain)
		return -EINVAL;
	*out = NULL;
	mutex_lock(&x1_imsi_lock);
	if (x1_imsi_active) {
		ret = -EALREADY;
		goto unlock;
	}
	r = devm_kzalloc(&request->pdev->dev, sizeof(*r), GFP_KERNEL);
	if (!r) {
		ret = -ENOMEM;
		goto unlock;
	}
	*out = r;
	x1_imsi_active = r;
	r->request = *request;
	r->receipt.attempted = true;
	r->receipt.step = "admission";
	ret = x1_imsi_admit(r);
	if (ret)
		goto terminal;
	if (!request->global_route || !request->target_check) {
		r->receipt.step = "platform-policy-unresolved";
		ret = -EOPNOTSUPP;
		goto terminal;
	}
	r->receipt.step = "retain-cold-boot-lifetime";
	ret = request->retain(request->context);
	if (ret) {
		ret = ret < 0 ? ret : -EIO;
		goto terminal;
	}
	r->receipt.retained = true;
	ret = x1_imsi_setup(r);
terminal:
	r->receipt.error = ret;
	/* No generic teardown or second attempt, even after partial MMIO/IRQ setup. */
unlock:
	mutex_unlock(&x1_imsi_lock);
	return ret;
}

void x1_imsi_cached(const struct x1_imsi *r, struct x1_imsi_receipt *out)
{
	if (!r || !out)
		return;
	mutex_lock(&x1_imsi_lock);
	*out = r->receipt;
	mutex_unlock(&x1_imsi_lock);
}

int x1_imsi_stop_retained(struct x1_imsi *r)
{
	struct dw_pcie_rp *pp;
	struct x1_imsi_stop *s;
	unsigned long flags;
	unsigned int i;
	int ret = -EPERM;

	if (!r)
		return -EINVAL;
	mutex_lock(&x1_imsi_lock);
	pp = &r->pci.pp;
	s = &r->receipt.stop;
	if (s->attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (!r->receipt.ready || r->receipt.error || !r->receipt.retained ||
	    !r->receipt.associated || !r->receipt.domain_ready ||
	    !r->receipt.target_reserved || !pp->irq_domain || !pp->use_imsi_rx ||
	    pp->num_vectors != 256 || r->receipt.banks != 8 ||
	    r->pci.dbi_base != r->request.dbi)
		goto out;
	for (i = 0; i < 8; i++)
		if (r->parents[i] <= 0 || pp->msi_irq[i] != r->parents[i] ||
		    irq_get_handler_data(r->parents[i]) != pp)
			goto out;
	raw_spin_lock_irqsave(&pp->lock, flags);
	/* PCI core must have removed every child mapping first. An idle or
	 * disabled endpoint IRQ alone is insufficient: CPU-affinity/mask work
	 * could still touch its MSI-X table or this receiver's DBI registers.
	 */
	if (!bitmap_empty(pp->msi_irq_in_use, pp->num_vectors)) {
		raw_spin_unlock_irqrestore(&pp->lock, flags);
		goto out;
	}
	s->attempted = true;
	ret = 0;
	for (i = 0; i < 8; i++) {
		u32 off = i * MSI_REG_CTRL_BLOCK_SIZE;

		pp->irq_mask[i] = U32_MAX;
		ret = x1_imsi_write_checked(r->request.dbi, PCIE_MSI_INTR0_MASK + off, U32_MAX);
		if (!ret)
			ret = x1_imsi_write_checked(r->request.dbi, PCIE_MSI_INTR0_ENABLE + off, 0);
		if (ret)
			break;
		s->banks_disabled++;
	}
	raw_spin_unlock_irqrestore(&pp->lock, flags);
	/* An ISR can acquire pp->lock. Never hold it while waiting for that ISR.
	 * Detach even after a bank readback error; on that error no power-off or
	 * DMA release is admitted, and all allocations remain retained.
	 */
	for (i = 0; i < 8; i++) {
		disable_irq(r->parents[i]);
		irq_set_chained_handler_and_data(r->parents[i], NULL, NULL);
		if (irq_get_handler_data(r->parents[i]))
			ret = -EIO;
		else
			s->parents_detached++;
	}
	s->error = ret;
	s->finished = !ret;
out:
	mutex_unlock(&x1_imsi_lock);
	return ret;
}

int x1_imsi_release_stopped(struct x1_imsi *r,
			  int (*check_stopped)(void *), void *context)
{
	struct dw_pcie_rp *pp;
	struct x1_imsi_receipt *s;
	unsigned int i;
	int ret = -EPERM;

	if (!r || !check_stopped)
		return -EINVAL;
	mutex_lock(&x1_imsi_lock);
	pp = &r->pci.pp;
	s = &r->receipt;
	if (s->release_attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (!s->ready || s->error || !s->retained || !s->associated ||
	    !s->domain_ready || !s->target_reserved || !s->stop.attempted ||
	    !s->stop.finished || s->stop.error || s->stop.banks_disabled != 8 ||
	    s->stop.parents_detached != 8 || !r->target_group || !r->target_group_closed ||
	    !pp->irq_domain || pp->num_vectors != 256 ||
	    !bitmap_empty(pp->msi_irq_in_use, pp->num_vectors) ||
	    !r->request.bridge || r->request.bridge->bus ||
	    device_is_registered(&r->request.bridge->dev) ||
	    dev_get_msi_domain(&r->request.bridge->dev) != pp->irq_domain ||
	    !r->request.bridge->msi_domain)
		goto out;
	for (i = 0; i < 8; i++)
		if (r->parents[i] <= 0 || pp->msi_irq[i] != r->parents[i] ||
		    irq_get_handler_data(r->parents[i]) || irq_has_action(r->parents[i]))
			goto out;
	ret = check_stopped(context);
	if (ret)
		goto out;
	s->release_attempted = true;
	dev_set_msi_domain(&r->request.bridge->dev, NULL);
	r->request.bridge->msi_domain = false;
	s->associated = false;
	/* All child domains are gone and parent handlers detached/synchronized.
	 * Standard DWC removal touches software IRQ objects only, not DBI.
	 */
	dw_pcie_free_msi(pp);
	pp->irq_domain = NULL;
	pp->use_imsi_rx = false;
	s->domain_ready = false;
	s->ready = false;
	ret = devres_release_group(r->pci.dev, r->target_group);
	/* devres returns the number of non-group resources released, not an
	 * errno-style zero on success. This group contains the DWC coherent MSI
	 * target allocation, so zero cannot prove release of our owned target.
	 */
	if (ret <= 0) {
		ret = ret < 0 ? ret : -ENOENT;
		s->release_error = ret;
		goto out;
	}
	r->target_group = NULL;
	pp->msi_data = 0;
	s->target_reserved = false;
	s->released = true;
	ret = 0;
out:
	mutex_unlock(&x1_imsi_lock);
	return ret;
}

/* Release the active owner, not an old consumed flag. Parent IRQ ownership
 * has ended; the platform's devres release below frees the shell. A new
 * receiver gets a new allocation and never reuses this receipt or domain. */
int x1_imsi_retire_context(struct x1_imsi *r,
			   int (*check_stopped)(void *), void *context)
{
	unsigned int i;
	int ret = -EPERM;

	if (!r || !check_stopped)
		return -EINVAL;
	mutex_lock(&x1_imsi_lock);
	if (x1_imsi_active != r || !r->receipt.released ||
	    r->receipt.release_error || !r->receipt.stop.finished ||
	    r->receipt.stop.error || r->receipt.ready || r->receipt.associated ||
	    r->receipt.domain_ready || r->receipt.target_reserved ||
	    r->pci.pp.irq_domain || r->target_group || r->request.bridge->bus ||
	    device_is_registered(&r->request.bridge->dev))
		goto out;
	for (i = 0; i < ARRAY_SIZE(r->parents); i++)
		if (r->parents[i] <= 0 || irq_get_handler_data(r->parents[i]) ||
		    irq_has_action(r->parents[i]))
			goto out;
	ret = check_stopped(context);
	if (!ret)
		x1_imsi_active = NULL;
out:
	mutex_unlock(&x1_imsi_lock);
	return ret;
}
