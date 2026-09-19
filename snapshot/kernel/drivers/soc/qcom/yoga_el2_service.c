// SPDX-License-Identifier: GPL-2.0-only
/* Passive PMIC service checkpoint. No UCSI commands, Type-C policy or MMIO. */
#include <asm/virt.h>
#include <linux/auxiliary_bus.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/qcom/pdr.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/soc/qcom/x1-adsp-handoff-policy.h>

bool x1_adsp_el2_available(void)
{
	return is_hyp_mode_available();
}
EXPORT_SYMBOL_GPL(x1_adsp_el2_available);

struct yoga_el2_service {
	struct device *dev;
	bool up;
};

static void service_rx(const void *data, size_t len, void *priv) { }

static void service_state(void *priv, int state)
{
	struct yoga_el2_service *s = priv;

	WRITE_ONCE(s->up, state == SERVREG_SERVICE_STATE_UP);
	dev_info(s->dev, "EL2 PMIC service_up=%u (passive transport observation only)\n",
		 READ_ONCE(s->up));
}

static ssize_t result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct yoga_el2_service *s = dev_get_drvdata(dev);

	return sysfs_emit(buf, "EL2_PMICSERVICE service_up=%u passive=1 no_usb4_probe=1\n",
			  READ_ONCE(s->up));
}
static DEVICE_ATTR_RO(result);
static struct attribute *service_attrs[] = { &dev_attr_result.attr, NULL };
ATTRIBUTE_GROUPS(service);

static int service_probe(struct auxiliary_device *adev, const struct auxiliary_device_id *id)
{
	struct device *dev = &adev->dev;
	struct pmic_glink_client *client;
	struct yoga_el2_service *s;
	int ret;

	if (!is_hyp_mode_available() || !of_machine_is_compatible("lenovo,yoga-slim7x") ||
	    !of_property_read_bool(of_root, "birk,adsp-el2-service-test"))
		return -EPERM;
	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;
	dev_set_drvdata(dev, s);
	client = devm_pmic_glink_client_alloc(dev, id->driver_data, service_rx, service_state, s);
	if (IS_ERR(client))
		return PTR_ERR(client);
	ret = devm_device_add_group(dev, service_groups[0]);
	if (ret)
		return ret;
	pmic_glink_client_register(client);
	return 0;
}
static const struct auxiliary_device_id service_ids[] = {
	{ .name = "pmic_glink.ucsi", .driver_data = 32779 },
	{ .name = "pmic_glink.altmode", .driver_data = 32780 },
	{}
};
static struct auxiliary_driver service_driver = {
	.name = "yoga_el2_service", .probe = service_probe, .id_table = service_ids,
	.driver = { .suppress_bind_attrs = true },
};
module_auxiliary_driver(service_driver);
MODULE_LICENSE("GPL");
