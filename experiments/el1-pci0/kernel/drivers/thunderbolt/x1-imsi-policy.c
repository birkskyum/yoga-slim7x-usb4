// SPDX-License-Identifier: GPL-2.0-only
/*
 * PRIVATE. Source-derived, bounded receiver-test policy, not proof of
 * USB4 PCI0 MSI delivery. No registration, IRQ handler or active DT change.
 *
 * pcie-qcom.c enables PARF_INT_MSI_DEV_0_7 in its ordinary optional global
 * setup. Its firmware-managed ECAM branch initializes DWC MSI and returns
 * before that setup. DWC dispatches MSI through eight separate bank parents.
 * Thus the global handler is NOT a software dependency of MSI dispatch.
 * Whether this USB4 integration needs these PARF enables is a hardware
 * hypothesis. Enable only the named bank bits; leave global IRQ unrequested
 * and disabled, preserve every other bit, and never acknowledge status.
 * No hotplug/link-down service is implemented by this first-test policy.
 */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/resource_ext.h>
#include "x1-imsi-policy.h"

#define X1_POLICY_HOST "/soc@0/pcie-imsi@400000000"
#define X1_POLICY_COMPAT "birk,yoga-x1-pci0-imsi-local"
#define X1_POLICY_STATUS 0x224
#define X1_POLICY_MASK 0x22c
#define X1_POLICY_BANKS GENMASK(30, 23)
/* PARF_INT_ALL_LINK_UP in upstream ba4a2e2317b9. This event is expected
 * after training, including with its PARF source-enable inherited from
 * firmware. The separate global GIC IRQ must remain disabled/unrequested
 * at every boundary below. Preserve both source-enable and status; no ack.
 * All other pending events (including link-down/MSI/error) remain terminal.
 */
#define X1_POLICY_LINK_UP BIT(13)

static int x1_policy_live(struct x1_imsi_policy *p)
{
	int ret;

	ret = p->check_live(p->context);
	if (ret)
		return ret < 0 ? ret : -EIO;
	if (!pm_runtime_active(&p->pdev->dev) || !pm_runtime_active(p->owner))
		return -EHOSTDOWN;
	return 0;
}

static int x1_policy_admit(struct x1_imsi_policy *p)
{
	struct device_node *host, *owner;
	struct resource *res;
	int ret;

	if (!p || !p->pdev || !p->owner || !p->bridge || !p->parf || !p->check_live)
		return -EINVAL;
	if (!of_machine_is_compatible("lenovo,yoga-slim7x") ||
	    p->bridge->dev.parent != &p->pdev->dev || p->bridge->bus ||
	    !p->pdev->dev.driver || !p->pdev->dev.driver->suppress_bind_attrs ||
	    !p->owner->driver || !p->owner->driver->suppress_bind_attrs)
		return -ENODEV;
	host = of_find_node_by_path(X1_POLICY_HOST);
	ret = host && host == p->pdev->dev.of_node &&
		of_device_is_compatible(host, X1_POLICY_COMPAT) ? 0 : -ENODEV;
	of_node_put(host);
	if (ret)
		return ret;
	owner = of_find_node_by_path("/soc@0/usb4@15600000");
	ret = owner && owner == p->owner->of_node &&
		of_device_is_compatible(owner, "qcom,x1e80100-usb4") ? 0 : -ENODEV;
	of_node_put(owner);
	if (ret)
		return ret;
	res = platform_get_resource_byname(p->pdev, IORESOURCE_MEM, "parf");
	if (!res || res->start != 0x1c28000 || resource_size(res) != 0x8000)
		return -ENODEV;
	return x1_policy_live(p);
}

static int x1_policy_global_idle(struct x1_imsi_policy *p, int irq)
{
	struct irq_data *data;
	struct device_node *intc;
	int ret;

	ret = x1_policy_live(p);
	if (ret)
		return ret;
	if (irq <= 0 || platform_get_irq_byname(p->pdev, "global") != irq)
		return -EINVAL;
	data = irq_get_irq_data(irq);
	intc = of_find_node_by_path("/soc@0/interrupt-controller@17000000");
	ret = intc && data && data->domain &&
		irq_domain_get_of_node(data->domain) == intc &&
		irqd_to_hwirq(data) == 167 + 32 &&
		irqd_get_trigger_type(data) == IRQ_TYPE_LEVEL_HIGH &&
		irqd_irq_disabled(data) && !irq_has_action(irq) &&
		!irq_get_handler_data(irq) ? 0 : -EBUSY;
	of_node_put(intc);
	return ret;
}

int x1_imsi_policy_global(void *context, int global_irq)
{
	struct x1_imsi_policy *p = context;
	u32 desired;
	int ret;

	if (!p)
		return -EINVAL;
	if (p->global_attempted)
		return -EALREADY;
	p->global_attempted = true;
	ret = x1_policy_admit(p);
	if (ret)
		goto out;
	ret = x1_policy_global_idle(p, global_irq);
	if (ret)
		goto out;
	p->status_before = readl(p->parf + X1_POLICY_STATUS);
	if (p->status_before & ~X1_POLICY_LINK_UP) {
		ret = -EBUSY;
		goto out;
	}
	ret = x1_policy_global_idle(p, global_irq);
	if (ret)
		goto out;
	p->mask_before = readl(p->parf + X1_POLICY_MASK);
	if (p->mask_before == U32_MAX) {
		ret = -EIO;
		goto out;
	}
	desired = p->mask_before | X1_POLICY_BANKS;
	if (desired == U32_MAX) {
		ret = -EIO; /* Refuse an indistinguishable all-ones readback. */
		goto out;
	}
	ret = x1_policy_global_idle(p, global_irq);
	if (ret)
		goto out;
	if (desired != p->mask_before) {
		p->mask_write_issued = true;
		writel(desired, p->parf + X1_POLICY_MASK);
		p->mask_write_returned = true;
	}
	/* Same-address readback flushes the sole possible write. */
	p->mask_after = readl(p->parf + X1_POLICY_MASK);
	if (p->mask_after != desired) {
		ret = -EIO;
		goto out;
	}
	ret = x1_policy_global_idle(p, global_irq);
	if (ret)
		goto out;
	p->status_after = readl(p->parf + X1_POLICY_STATUS);
	if (p->status_after & ~X1_POLICY_LINK_UP) {
		ret = -EBUSY;
		goto out;
	}
	ret = x1_policy_global_idle(p, global_irq);
	if (!ret)
		p->global_ready = true;
out:
	p->global_error = ret;
	return ret;
}

int x1_imsi_policy_target(void *context, u64 target)
{
	struct x1_imsi_policy *p = context;
	struct resource_entry *win;
	u64 end, bus_start, bus_end;
	unsigned int entries = 0;
	int ret;

	if (!p)
		return -EINVAL;
	if (p->target_attempted)
		return -EALREADY;
	p->target_attempted = true;
	p->target = target;
	ret = x1_policy_admit(p);
	if (ret)
		goto out;
	/* DWC reserved sizeof(u64). Zero is a legitimate reserved DMA address.
	 * Reject wrap/alignment, not an unsubstantiated nonzero requirement.
	 */
	if ((target & 7) || target > U64_MAX - 7) {
		ret = -EINVAL;
		goto out;
	}
	end = target + 7;
	resource_list_for_each_entry(win, &p->bridge->windows) {
		if (++entries > 16 || !win->res) {
			ret = -EINVAL;
			goto out;
		}
		if (resource_type(win->res) == IORESOURCE_BUS)
			continue;
		/* This bounded host admits no I/O BAR window or negative translation.
		 * Do not silently omit an unknown resource type/address transform.
		 */
		if (resource_type(win->res) != IORESOURCE_MEM ||
		    win->res->end < win->res->start || win->offset > win->res->start) {
			ret = -EINVAL;
			goto out;
		}
		bus_start = win->res->start - win->offset;
		bus_end = win->res->end - win->offset;
		p->memory_windows++;
		/* Exclude both address spaces conservatively. The MSI target is a
		 * DMA API/bus address; a CPU-range exclusion alone is insufficient.
		 */
		if ((target <= bus_end && end >= bus_start) ||
		    (target <= win->res->end && end >= win->res->start)) {
			ret = -ERANGE;
			goto out;
		}
	}
	if (!p->memory_windows) {
		ret = -ENODEV;
		goto out;
	}
	ret = x1_policy_live(p);
	if (!ret)
		p->target_ready = true;
out:
	p->target_error = ret;
	return ret;
}
