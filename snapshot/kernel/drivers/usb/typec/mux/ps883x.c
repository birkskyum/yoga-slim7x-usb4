// SPDX-License-Identifier: GPL-2.0+
/*
 * Parade ps883x usb retimer driver
 *
 * Copyright (C) 2024 Linaro Ltd.
 */

#include <drm/bridge/aux-bridge.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/pd.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_retimer.h>
#include <linux/usb/typec_tbt.h>
#include <linux/soc/qcom/x1-usb4-diag.h>

#define REG_USB_PORT_CONN_STATUS_0		0x00

#define CONN_STATUS_0_CONNECTION_PRESENT	BIT(0)
#define CONN_STATUS_0_ORIENTATION_REVERSED	BIT(1)
#define CONN_STATUS_0_ACTIVE_CABLE		BIT(2)
#define CONN_STATUS_0_USB_3_1_CONNECTED		BIT(5)

#define REG_USB_PORT_CONN_STATUS_1		0x01

#define CONN_STATUS_1_DP_CONNECTED		BIT(0)
#define CONN_STATUS_1_DP_SINK_REQUESTED		BIT(1)
#define CONN_STATUS_1_DP_PIN_ASSIGNMENT_C_D	BIT(2)
#define CONN_STATUS_1_DP_HPD_LEVEL		BIT(7)

#define REG_USB_PORT_CONN_STATUS_2		0x02

#define CONN_STATUS_2_TBT_CONNECTED		BIT(0)
#define CONN_STATUS_2_TBT_UNIDIR_LSRX_ACT_LT	BIT(4)
#define CONN_STATUS_2_USB4_CONNECTED		BIT(7)

struct ps883x_retimer {
	struct i2c_client *client;
	struct gpio_desc *reset_gpio;
	struct regmap *regmap;
	struct typec_switch_dev *sw;
	struct typec_retimer *retimer;
	struct clk *xo_clk;
	struct regulator *vdd_supply;
	struct regulator *vdd33_supply;
	struct regulator *vdd33_cap_supply;
	struct regulator *vddat_supply;
	struct regulator *vddar_supply;
	struct regulator *vddio_supply;

	struct typec_switch *typec_switch;
	struct typec_mux *typec_mux;

	struct mutex lock; /* protect non-concurrent retimer & switch */

	enum typec_orientation orientation;
	bool in_reset;
	u32 diag_applied;
};

/* Private v9 derivative: observe the owning driver's ordinary I2C writes,
 * never a competing raw-I2C accessor. Only bits owned by this driver are
 * compared; other readback bits are retained in the log. */
static int ps883x_diag_verify(struct ps883x_retimer *rt, int cfg0, int cfg1, int cfg2)
{
	u8 got[3];
	const u8 want[3] = { cfg0, cfg1, cfg2 };
	const u8 masks[3] = { 0x27, 0x87, 0x91 };
	int ret, i;
	if (!IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG))
		return 0;
	ret = regmap_bulk_read(rt->regmap, 0, got, sizeof(got));
	if (ret)
		return ret;
	for (i = 0; i < 3; i++)
		if ((got[i] & masks[i]) != (want[i] & masks[i]))
			ret = -EIO;
	dev_info(&rt->client->dev, "RETIMER readback want=%*ph got=%*ph masks=%*ph ret=%d\n",
		 3, want, 3, got, 3, masks, ret);
	if (!ret)
		rt->diag_applied++;
	return ret;
}

static ssize_t diag_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ps883x_retimer *rt = dev_get_drvdata(dev);
	guard(mutex)(&rt->lock);
	return sysfs_emit(buf, "retimer=left-rear in_reset=%u orientation=%u applied=%u\n",
		 rt->in_reset, rt->orientation, rt->diag_applied);
}
static DEVICE_ATTR_RO(diag_status);
static struct attribute *diag_attrs[] = { &dev_attr_diag_status.attr, NULL };
static const struct attribute_group diag_group = { .attrs = diag_attrs };

static int ps883x_enable_vregs(struct ps883x_retimer *retimer)
{
	struct device *dev = &retimer->client->dev;
	int ret;

	ret = regulator_enable(retimer->vdd33_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD 3.3V regulator: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(retimer->vdd33_cap_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD 3.3V CAP regulator: %d\n", ret);
		goto err_vdd33_disable;
	}

	usleep_range(4000, 10000);

	ret = regulator_enable(retimer->vdd_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD regulator: %d\n", ret);
		goto err_vdd33_cap_disable;
	}

	ret = regulator_enable(retimer->vddar_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD AR regulator: %d\n", ret);
		goto err_vdd_disable;
	}

	ret = regulator_enable(retimer->vddat_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD AT regulator: %d\n", ret);
		goto err_vddar_disable;
	}

	ret = regulator_enable(retimer->vddio_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD IO regulator: %d\n", ret);
		goto err_vddat_disable;
	}

	return 0;

err_vddat_disable:
	regulator_disable(retimer->vddat_supply);
err_vddar_disable:
	regulator_disable(retimer->vddar_supply);
err_vdd_disable:
	regulator_disable(retimer->vdd_supply);
err_vdd33_cap_disable:
	regulator_disable(retimer->vdd33_cap_supply);
err_vdd33_disable:
	regulator_disable(retimer->vdd33_supply);

	return ret;
}

static void ps883x_disable_vregs(struct ps883x_retimer *retimer)
{
	regulator_disable(retimer->vddio_supply);
	regulator_disable(retimer->vddat_supply);
	regulator_disable(retimer->vddar_supply);
	regulator_disable(retimer->vdd_supply);
	regulator_disable(retimer->vdd33_cap_supply);
	regulator_disable(retimer->vdd33_supply);
}

static void ps883x_reset(struct ps883x_retimer *retimer)
{
	if (retimer->in_reset)
		return;

	gpiod_set_value(retimer->reset_gpio, 1);
	ps883x_disable_vregs(retimer);
	retimer->in_reset = true;
}

static int ps883x_configure(struct ps883x_retimer *retimer, int cfg0,
			    int cfg1, int cfg2, bool reset)
{
	struct device *dev = &retimer->client->dev;
	int ret;

	if (reset) {
		ps883x_reset(retimer);
		if (IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG))
			dev_info(dev, "RETIMER safe/reset applied; no I2C read while powered off\n");

		return 0;
	} else if (retimer->in_reset) {
		ret = ps883x_enable_vregs(retimer);
		if (ret)
			return ret;

		gpiod_set_value(retimer->reset_gpio, 0);

		/* firmware initialization delay */
		msleep(60);

		retimer->in_reset = false;
	}

	ret = regmap_write(retimer->regmap, REG_USB_PORT_CONN_STATUS_0, cfg0);
	if (ret) {
		dev_err(dev, "failed to write conn_status_0: %d\n", ret);
		return ret;
	}

	ret = regmap_write(retimer->regmap, REG_USB_PORT_CONN_STATUS_1, cfg1);
	if (ret) {
		dev_err(dev, "failed to write conn_status_1: %d\n", ret);
		return ret;
	}

	ret = regmap_write(retimer->regmap, REG_USB_PORT_CONN_STATUS_2, cfg2);
	if (ret) {
		dev_err(dev, "failed to write conn_status_2: %d\n", ret);
		return ret;
	}

	return ps883x_diag_verify(retimer, cfg0, cfg1, cfg2);
}

static int ps883x_set(struct ps883x_retimer *retimer, struct typec_retimer_state *state)
{
	struct typec_thunderbolt_data *tb_data;
	const struct enter_usb_data *eudo_data;
	int cfg0 = CONN_STATUS_0_CONNECTION_PRESENT;
	int cfg1 = 0x00;
	int cfg2 = 0x00;
	bool reset = false;

	if (retimer->orientation == TYPEC_ORIENTATION_REVERSE)
		cfg0 |= CONN_STATUS_0_ORIENTATION_REVERSED;

	if (state->alt) {
		switch (state->alt->svid) {
		case USB_TYPEC_DP_SID:
			cfg1 |= CONN_STATUS_1_DP_CONNECTED |
				CONN_STATUS_1_DP_HPD_LEVEL;

			switch (state->mode)  {
			case TYPEC_DP_STATE_D:
				cfg0 |= CONN_STATUS_0_USB_3_1_CONNECTED;
				fallthrough;
			case TYPEC_DP_STATE_C:
				cfg1 |= CONN_STATUS_1_DP_SINK_REQUESTED |
					CONN_STATUS_1_DP_PIN_ASSIGNMENT_C_D;
				break;
			default: /* MODE_E */
				break;
			}
			break;
		case USB_TYPEC_TBT_SID:
			tb_data = state->data;

			/* Unconditional */
			cfg2 |= CONN_STATUS_2_TBT_CONNECTED;

			if (tb_data->cable_mode & TBT_CABLE_ACTIVE_PASSIVE)
				cfg0 |= CONN_STATUS_0_ACTIVE_CABLE;

			if (tb_data->enter_vdo & TBT_ENTER_MODE_UNI_DIR_LSRX)
				cfg2 |= CONN_STATUS_2_TBT_UNIDIR_LSRX_ACT_LT;
			break;
		default:
			dev_err(&retimer->client->dev, "Got unsupported SID: 0x%x\n",
				state->alt->svid);
			return -EOPNOTSUPP;
		}
	} else {
		switch (state->mode) {
		/* SAFE can be transient or point to an actual disconnect */
		case TYPEC_STATE_SAFE:
			reset = retimer->orientation == TYPEC_ORIENTATION_NONE;
			break;
		/* USB2 pins don't even go through this chip */
		case TYPEC_MODE_USB2:
			reset = true;
			break;
		case TYPEC_STATE_USB:
		case TYPEC_MODE_USB3:
			cfg0 |= CONN_STATUS_0_USB_3_1_CONNECTED;
			break;
		case TYPEC_MODE_USB4:
			eudo_data = state->data;

			cfg2 |= CONN_STATUS_2_USB4_CONNECTED;

			if (FIELD_GET(EUDO_CABLE_TYPE_MASK, eudo_data->eudo) != EUDO_CABLE_TYPE_PASSIVE)
				cfg0 |= CONN_STATUS_0_ACTIVE_CABLE;
			break;
		default:
			dev_err(&retimer->client->dev, "Got unsupported mode: %lu\n",
				state->mode);
			return -EOPNOTSUPP;
		}
	}

	return ps883x_configure(retimer, cfg0, cfg1, cfg2, reset);
}

static int ps883x_sw_set(struct typec_switch_dev *sw,
			 enum typec_orientation orientation)
{
	struct ps883x_retimer *retimer = typec_switch_get_drvdata(sw);
	int ret = 0;

	ret = typec_switch_set(retimer->typec_switch, orientation);
	if (ret)
		return ret;

	guard(mutex)(&retimer->lock);

	if (retimer->orientation != orientation) {
		retimer->orientation = orientation;

		/*
		 * Orientation notifications usually come prior to mode switch
		 * events. If the retimer is already in reset, we still want to
		 * cache the new orientation value for the subsequent ps883x_set().
		 */
		if (retimer->in_reset)
			return 0;

		ret = regmap_assign_bits(retimer->regmap, REG_USB_PORT_CONN_STATUS_0,
					 CONN_STATUS_0_ORIENTATION_REVERSED,
					 orientation == TYPEC_ORIENTATION_REVERSE);
		if (ret)
			dev_err(&retimer->client->dev, "failed to set orientation: %d\n", ret);
	}

	return ret;
}

static int ps883x_retimer_set(struct typec_retimer *rtmr,
			      struct typec_retimer_state *state)
{
	struct ps883x_retimer *retimer = typec_retimer_get_drvdata(rtmr);
	struct typec_mux_state mux_state;
	int ret = 0;

	mutex_lock(&retimer->lock);
	ret = ps883x_set(retimer, state);
	mutex_unlock(&retimer->lock);

	if (ret)
		return ret;

	mux_state.alt = state->alt;
	mux_state.data = state->data;
	mux_state.mode = state->mode;

	return typec_mux_set(retimer->typec_mux, &mux_state);
}

static int ps883x_get_vregs(struct ps883x_retimer *retimer)
{
	struct device *dev = &retimer->client->dev;

	retimer->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(retimer->vdd_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vdd_supply),
				     "failed to get VDD\n");

	retimer->vdd33_supply = devm_regulator_get(dev, "vdd33");
	if (IS_ERR(retimer->vdd33_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vdd33_supply),
				     "failed to get VDD 3.3V\n");

	retimer->vdd33_cap_supply = devm_regulator_get(dev, "vdd33-cap");
	if (IS_ERR(retimer->vdd33_cap_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vdd33_cap_supply),
				     "failed to get VDD CAP 3.3V\n");

	retimer->vddat_supply = devm_regulator_get(dev, "vddat");
	if (IS_ERR(retimer->vddat_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vddat_supply),
				     "failed to get VDD AT\n");

	retimer->vddar_supply = devm_regulator_get(dev, "vddar");
	if (IS_ERR(retimer->vddar_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vddar_supply),
				     "failed to get VDD AR\n");

	retimer->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(retimer->vddio_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vddio_supply),
				     "failed to get VDD IO\n");

	return 0;
}

static const struct regmap_config ps883x_retimer_regmap = {
	.max_register = 0x1f,
	.reg_bits = 8,
	.val_bits = 8,
};

/* No hardware access: resolve OF identity, never compare full_name to a path. */
static int ps883x_diag_guard(struct device *dev)
{
	struct device_node *expected;
	bool board, nhi, marker, node, enabled;

	board = x1_diag_board_allowed();
	nhi = x1_diag_nhi_complete();
	marker = of_property_read_bool(of_root, "birk,usb4-retimer-test");
	expected = of_find_node_by_path("/soc@0/geniqup@bc0000/i2c@b8c000/typec-mux@8");
	node = expected && dev->of_node == expected;
	enabled = node && of_device_is_available(expected) &&
		of_device_is_compatible(expected, "parade,ps8830");
	of_node_put(expected);
	if (!board || !nhi || !marker || !node || !enabled)
		return dev_err_probe(dev, -EPERM,
			"RETIMER REFUSED: board=%d nhi=%d marker=%d node=%d enabled=%d path=%pOF\n",
			board, nhi, marker, node, enabled, dev->of_node);
	dev_info(dev, "RETIMER GUARD PASS: exact left-rear node=%pOF, NHI complete\n",
		 dev->of_node);
	return 0;
}

static int ps883x_retimer_probe(struct i2c_client *client)
{
	static bool diag_attempted;
	struct device *dev = &client->dev;
	struct typec_switch_desc sw_desc = { };
	struct typec_retimer_desc rtmr_desc = { };
	struct ps883x_retimer *retimer;
	unsigned int val;
	int ret;
	if (IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG)) {
		ret = ps883x_diag_guard(dev);
		if (ret)
			return ret;
	}

	retimer = devm_kzalloc(dev, sizeof(*retimer), GFP_KERNEL);
	if (!retimer)
		return -ENOMEM;

	retimer->client = client;

	mutex_init(&retimer->lock);

	retimer->regmap = devm_regmap_init_i2c(client, &ps883x_retimer_regmap);
	if (IS_ERR(retimer->regmap))
		return dev_err_probe(dev, PTR_ERR(retimer->regmap),
				     "failed to allocate register map\n");

	ret = ps883x_get_vregs(retimer);
	if (ret)
		return ret;

	retimer->xo_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(retimer->xo_clk))
		return dev_err_probe(dev, PTR_ERR(retimer->xo_clk),
				     "failed to get xo clock\n");

	retimer->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(retimer->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(retimer->reset_gpio),
				     "failed to get reset gpio\n");

	retimer->typec_switch = typec_switch_get(dev);
	if (IS_ERR(retimer->typec_switch))
		return dev_err_probe(dev, PTR_ERR(retimer->typec_switch),
				     "failed to acquire orientation-switch\n");

	retimer->typec_mux = typec_mux_get(dev);
	if (IS_ERR(retimer->typec_mux)) {
		ret = dev_err_probe(dev, PTR_ERR(retimer->typec_mux),
				    "failed to acquire mode-mux\n");
		goto err_switch_put;
	}
	if (IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG) &&
	    (!retimer->typec_switch || !retimer->typec_mux)) {
		ret = dev_err_probe(dev, -ENODEV,
			"RETIMER PROBE: downstream QMP switch=%d mux=%d required\n",
			!!retimer->typec_switch, !!retimer->typec_mux);
		goto err_mux_put;
	}

	ret = drm_aux_bridge_register(dev);
	if (ret)
		goto err_mux_put;
	if (IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG)) {
		dev_info(dev, "RETIMER PROBE: resources, downstream QMP and auxiliary registration ready\n");
		if (diag_attempted) {
			ret = dev_err_probe(dev, -EPERM, "RETIMER REFUSED: hardware already attempted; cold shutdown required\n");
			goto err_mux_put;
		}
		diag_attempted = true;
		__module_get(THIS_MODULE); /* Cold shutdown only, including partial failure. */
	}

	ret = ps883x_enable_vregs(retimer);
	if (ret)
		goto err_mux_put;
	if (IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG))
		dev_info(dev, "RETIMER PROBE: regulators enabled\n");

	ret = clk_prepare_enable(retimer->xo_clk);
	if (ret) {
		dev_err(dev, "failed to enable XO: %d\n", ret);
		goto err_vregs_disable;
	}
	if (IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG))
		dev_info(dev, "RETIMER PROBE: XO enabled; checking existing connection/reset state\n");

	/* skip resetting if already configured */
	if (regmap_test_bits(retimer->regmap, REG_USB_PORT_CONN_STATUS_0,
			     CONN_STATUS_0_CONNECTION_PRESENT) == 1) {
		gpiod_direction_output(retimer->reset_gpio, 0);
	} else {
		gpiod_direction_output(retimer->reset_gpio, 1);

		/* VDD IO supply enable to reset release delay */
		usleep_range(4000, 14000);

		gpiod_set_value(retimer->reset_gpio, 0);

		/* firmware initialization delay */
		msleep(60);

		/* make sure device is accessible */
		ret = regmap_read(retimer->regmap, REG_USB_PORT_CONN_STATUS_0,
				  &val);
		if (ret) {
			dev_err(dev, "failed to read conn_status_0: %d\n", ret);
			if (ret == -ENXIO)
				ret = -EIO;
			goto err_clk_disable;
		}
	}

	if (IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG))
		dev_info(dev, "RETIMER PROBE: I2C access passed; returning to reset before PAN\n");
	/* Keep the retimer in reset until a Type-C notification comes */
	ps883x_reset(retimer);

	sw_desc.drvdata = retimer;
	sw_desc.fwnode = dev_fwnode(dev);
	sw_desc.set = ps883x_sw_set;

	retimer->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(retimer->sw)) {
		ret = PTR_ERR(retimer->sw);
		dev_err(dev, "failed to register typec switch: %d\n", ret);
		goto err_clk_disable;
	}

	rtmr_desc.drvdata = retimer;
	rtmr_desc.fwnode = dev_fwnode(dev);
	rtmr_desc.set = ps883x_retimer_set;

	retimer->retimer = typec_retimer_register(dev, &rtmr_desc);
	if (IS_ERR(retimer->retimer)) {
		ret = PTR_ERR(retimer->retimer);
		dev_err(dev, "failed to register typec retimer: %d\n", ret);
		goto err_switch_unregister;
	}

	i2c_set_clientdata(client, retimer);
	if (IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG)) {
		ret = devm_device_add_group(dev, &diag_group);
		if (ret)
			dev_warn(dev, "RETIMER diagnostic status unavailable: %d\n", ret);
		dev_info(dev, "RETIMER left-rear provider ready, held in reset until PAN\n");
	}
	return 0;

err_switch_unregister:
	typec_switch_unregister(retimer->sw);
err_clk_disable:
	clk_disable_unprepare(retimer->xo_clk);
err_vregs_disable:
	gpiod_set_value(retimer->reset_gpio, 1);
	ps883x_disable_vregs(retimer);
err_mux_put:
	typec_mux_put(retimer->typec_mux);
err_switch_put:
	typec_switch_put(retimer->typec_switch);

	return ret;
}

static void ps883x_retimer_remove(struct i2c_client *client)
{
	struct ps883x_retimer *retimer = i2c_get_clientdata(client);

	typec_retimer_unregister(retimer->retimer);
	typec_switch_unregister(retimer->sw);

	gpiod_set_value(retimer->reset_gpio, 1);

	clk_disable_unprepare(retimer->xo_clk);

	ps883x_disable_vregs(retimer);

	typec_mux_put(retimer->typec_mux);
	typec_switch_put(retimer->typec_switch);
}

static const struct of_device_id ps883x_retimer_of_table[] = {
	{ .compatible = "parade,ps8830" },
	{ }
};
MODULE_DEVICE_TABLE(of, ps883x_retimer_of_table);

static struct i2c_driver ps883x_retimer_driver = {
	.driver = {
		.name = "ps883x_retimer",
		.of_match_table = ps883x_retimer_of_table,
		.suppress_bind_attrs = IS_ENABLED(CONFIG_USB4_X1_TYPEC_DIAG),
	},
	.probe		= ps883x_retimer_probe,
	.remove		= ps883x_retimer_remove,
};

module_i2c_driver(ps883x_retimer_driver);

MODULE_DESCRIPTION("Parade ps883x Type-C Retimer driver");
MODULE_LICENSE("GPL");
