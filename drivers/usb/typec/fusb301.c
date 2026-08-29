// SPDX-License-Identifier: GPL-2.0-only
//
// onsemi FUSB301/FUSB301A autonomous USB Type-C controller

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/typec.h>

#define FUSB301_REG_DEVICE_ID	0x01
#define FUSB301_REG_MODE	0x02
#define FUSB301_REG_CONTROL	0x03
#define FUSB301_REG_MASK	0x10
#define FUSB301_REG_STATUS	0x11
#define FUSB301_REG_TYPE	0x12
#define FUSB301_REG_INTERRUPT	0x13

#define FUSB301_DEVICE_ID	0x12

#define FUSB301_MODE_SOURCE	BIT(0)
#define FUSB301_MODE_SOURCE_ACC	BIT(1)
#define FUSB301_MODE_SINK	BIT(2)
#define FUSB301_MODE_SINK_ACC	BIT(3)
#define FUSB301_MODE_DRP	BIT(4)
#define FUSB301_MODE_DRP_ACC	BIT(5)
#define FUSB301_MODE_MASK	GENMASK(5, 0)

#define FUSB301_CONTROL_HOST_CUR	GENMASK(2, 1)
#define FUSB301_CONTROL_INT_MASK	BIT(0)
#define FUSB301_HOST_CUR_DEFAULT	BIT(1)
#define FUSB301_HOST_CUR_1_5A	BIT(2)
#define FUSB301_HOST_CUR_3A	GENMASK(2, 1)

#define FUSB301_STATUS_ORIENT	GENMASK(5, 4)
#define FUSB301_STATUS_VBUSOK	BIT(3)
#define FUSB301_STATUS_BC_LVL	GENMASK(2, 1)
#define FUSB301_STATUS_ATTACH	BIT(0)
#define FUSB301_ORIENT_CC1	BIT(4)
#define FUSB301_ORIENT_CC2	BIT(5)
#define FUSB301_BC_DEFAULT	BIT(1)
#define FUSB301_BC_1_5A	BIT(2)
#define FUSB301_BC_3A	GENMASK(2, 1)

#define FUSB301_TYPE_SINK	BIT(4)
#define FUSB301_TYPE_SOURCE	BIT(3)
#define FUSB301_TYPE_DEBUG_ACC	BIT(1)
#define FUSB301_TYPE_AUDIO_ACC	BIT(0)

struct fusb301 {
	struct device *dev;
	struct regmap *regmap;
	struct typec_capability cap;
	struct typec_port *port;
	struct typec_partner *partner;
	struct fwnode_handle *connector;
	enum typec_pwr_opmode pwr_opmode;
	unsigned int partner_type;
	struct regulator *vbus;
	bool vbus_on;
};

static int fusb301_set_vbus(struct fusb301 *fusb, bool on)
{
	int ret;

	if (!fusb->vbus || fusb->vbus_on == on)
		return 0;

	if (on)
		ret = regulator_enable(fusb->vbus);
	else
		ret = regulator_disable(fusb->vbus);
	if (ret)
		return ret;

	fusb->vbus_on = on;
	dev_info(fusb->dev, "connector VBUS %s\n", on ? "enabled" : "disabled");
	return 0;
}

static unsigned int fusb301_mode_for_port_type(enum typec_port_type type)
{
	switch (type) {
	case TYPEC_PORT_SRC:
		return FUSB301_MODE_SOURCE;
	case TYPEC_PORT_SNK:
		return FUSB301_MODE_SINK;
	case TYPEC_PORT_DRP:
	default:
		return FUSB301_MODE_DRP;
	}
}

static enum typec_role fusb301_default_role(struct fusb301 *fusb)
{
	switch (fusb->cap.type) {
	case TYPEC_PORT_SRC:
		return TYPEC_SOURCE;
	case TYPEC_PORT_SNK:
		return TYPEC_SINK;
	case TYPEC_PORT_DRP:
	default:
		return fusb->cap.prefer_role == TYPEC_SOURCE ?
			TYPEC_SOURCE : TYPEC_SINK;
	}
}

static unsigned int fusb301_host_current(enum typec_pwr_opmode mode)
{
	switch (mode) {
	case TYPEC_PWR_MODE_1_5A:
		return FUSB301_HOST_CUR_1_5A;
	case TYPEC_PWR_MODE_3_0A:
		return FUSB301_HOST_CUR_3A;
	case TYPEC_PWR_MODE_USB:
	default:
		return FUSB301_HOST_CUR_DEFAULT;
	}
}

static enum typec_orientation fusb301_orientation(unsigned int status)
{
	switch (status & FUSB301_STATUS_ORIENT) {
	case FUSB301_ORIENT_CC1:
		return TYPEC_ORIENTATION_NORMAL;
	case FUSB301_ORIENT_CC2:
		return TYPEC_ORIENTATION_REVERSE;
	default:
		return TYPEC_ORIENTATION_NONE;
	}
}

static enum typec_pwr_opmode fusb301_sink_opmode(unsigned int status)
{
	switch (status & FUSB301_STATUS_BC_LVL) {
	case FUSB301_BC_1_5A:
		return TYPEC_PWR_MODE_1_5A;
	case FUSB301_BC_3A:
		return TYPEC_PWR_MODE_3_0A;
	case FUSB301_BC_DEFAULT:
	default:
		return TYPEC_PWR_MODE_USB;
	}
}

static int fusb301_set_port_type(struct typec_port *port,
				  enum typec_port_type type)
{
	struct fusb301 *fusb = typec_get_drvdata(port);

	return regmap_update_bits(fusb->regmap, FUSB301_REG_MODE,
				  FUSB301_MODE_MASK,
				  fusb301_mode_for_port_type(type));
}

static const struct typec_operations fusb301_typec_ops = {
	.port_type_set = fusb301_set_port_type,
};

static void fusb301_unregister_partner(struct fusb301 *fusb)
{
	if (!fusb->partner)
		return;

	typec_unregister_partner(fusb->partner);
	fusb->partner = NULL;
}

static int fusb301_update_status(struct fusb301 *fusb)
{
	struct typec_partner_desc desc = { };
	unsigned int status, type, partner_type;
	enum typec_role pwr_role;
	enum typec_data_role data_role;
	int ret;

	ret = regmap_read(fusb->regmap, FUSB301_REG_STATUS, &status);
	if (ret)
		return ret;

	ret = regmap_read(fusb->regmap, FUSB301_REG_TYPE, &type);
	if (ret)
		return ret;

	if (!(status & FUSB301_STATUS_ATTACH)) {
		ret = fusb301_set_vbus(fusb, false);
		if (ret)
			return ret;

		fusb301_unregister_partner(fusb);
		fusb->partner_type = 0;
		pwr_role = fusb301_default_role(fusb);
		data_role = pwr_role == TYPEC_SOURCE ? TYPEC_HOST : TYPEC_DEVICE;
		typec_set_data_role(fusb->port, data_role);
		typec_set_pwr_role(fusb->port, pwr_role);
		typec_set_vconn_role(fusb->port, pwr_role);
		typec_set_pwr_opmode(fusb->port, fusb->pwr_opmode);
		typec_set_orientation(fusb->port, TYPEC_ORIENTATION_NONE);
		return 0;
	}

	partner_type = type & (FUSB301_TYPE_SINK | FUSB301_TYPE_SOURCE |
				       FUSB301_TYPE_DEBUG_ACC | FUSB301_TYPE_AUDIO_ACC);
	if (!partner_type)
		return -EIO;

	if (partner_type & FUSB301_TYPE_SINK)
		pwr_role = TYPEC_SOURCE;
	else if (partner_type & FUSB301_TYPE_SOURCE)
		pwr_role = TYPEC_SINK;
	else
		pwr_role = fusb301_default_role(fusb);

	/* A source may apply VBUS only after CC has identified a sink. */
	ret = fusb301_set_vbus(fusb,
				pwr_role == TYPEC_SOURCE &&
				(partner_type & FUSB301_TYPE_SINK));
	if (ret)
		return ret;

	if (partner_type & FUSB301_TYPE_AUDIO_ACC)
		desc.accessory = TYPEC_ACCESSORY_AUDIO;
	else if (partner_type & FUSB301_TYPE_DEBUG_ACC)
		desc.accessory = TYPEC_ACCESSORY_DEBUG;

	if (partner_type != fusb->partner_type) {
		fusb301_unregister_partner(fusb);
		fusb->partner = typec_register_partner(fusb->port, &desc);
		if (IS_ERR(fusb->partner)) {
			ret = PTR_ERR(fusb->partner);
			fusb->partner = NULL;
			return ret;
		}

		data_role = pwr_role == TYPEC_SOURCE ? TYPEC_HOST : TYPEC_DEVICE;
		typec_set_pwr_role(fusb->port, pwr_role);
		typec_set_vconn_role(fusb->port, pwr_role);
		typec_set_data_role(fusb->port, data_role);
	}

	if (pwr_role == TYPEC_SINK)
		typec_set_pwr_opmode(fusb->port, fusb301_sink_opmode(status));
	else
		typec_set_pwr_opmode(fusb->port, fusb->pwr_opmode);

	typec_set_orientation(fusb->port, fusb301_orientation(status));
	fusb->partner_type = partner_type;

	return 0;
}

static irqreturn_t fusb301_irq(int irq, void *data)
{
	struct fusb301 *fusb = data;
	unsigned int interrupt;
	int ret;

	ret = regmap_read(fusb->regmap, FUSB301_REG_INTERRUPT, &interrupt);
	if (ret)
		return IRQ_NONE;

	ret = fusb301_update_status(fusb);
	if (ret)
		dev_warn(fusb->dev, "failed to update Type-C status: %d\n", ret);

	return IRQ_HANDLED;
}

static const struct regmap_config fusb301_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = FUSB301_REG_INTERRUPT,
};

static int fusb301_hw_init(struct fusb301 *fusb)
{
	int ret;

	ret = regmap_write(fusb->regmap, FUSB301_REG_MODE,
			   fusb301_mode_for_port_type(fusb->cap.type));
	if (ret)
		return ret;

	ret = regmap_update_bits(fusb->regmap, FUSB301_REG_CONTROL,
				 FUSB301_CONTROL_HOST_CUR |
				 FUSB301_CONTROL_INT_MASK,
				 fusb301_host_current(fusb->pwr_opmode));
	if (ret)
		return ret;

	return regmap_write(fusb->regmap, FUSB301_REG_MASK, 0);
}

static int fusb301_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fusb301 *fusb;
	const char *opmode;
	unsigned int id;
	int ret;

	fusb = devm_kzalloc(dev, sizeof(*fusb), GFP_KERNEL);
	if (!fusb)
		return -ENOMEM;

	fusb->vbus = devm_regulator_get_optional(dev, "vbus");
	if (IS_ERR(fusb->vbus)) {
		ret = PTR_ERR(fusb->vbus);
		if (ret == -ENODEV)
			fusb->vbus = NULL;
		else
			return dev_err_probe(dev, ret,
					     "failed to get VBUS supply\n");
	}

	fusb->dev = dev;
	fusb->regmap = devm_regmap_init_i2c(client, &fusb301_regmap_config);
	if (IS_ERR(fusb->regmap))
		return dev_err_probe(dev, PTR_ERR(fusb->regmap),
				     "failed to initialize regmap\n");

	ret = regmap_read(fusb->regmap, FUSB301_REG_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device ID\n");
	if (id != FUSB301_DEVICE_ID)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected device ID 0x%02x\n", id);

	fusb->connector = device_get_named_child_node(dev, "connector");
	if (!fusb->connector)
		return dev_err_probe(dev, -ENODEV, "missing connector node\n");

	ret = typec_get_fw_cap(&fusb->cap, fusb->connector);
	if (ret)
		goto err_put_connector;

	ret = fwnode_property_read_string(fusb->connector,
					  "typec-power-opmode", &opmode);
	if (ret)
		goto err_put_connector;

	ret = typec_find_pwr_opmode(opmode);
	if (ret < 0 || ret == TYPEC_PWR_MODE_PD) {
		ret = -EINVAL;
		goto err_put_connector;
	}
	fusb->pwr_opmode = ret;
	if (client->irq <= 0) {
		ret = -EINVAL;
		dev_err_probe(dev, ret, "missing active-low interrupt\n");
		goto err_put_connector;
	}

	fusb->cap.revision = USB_TYPEC_REV_1_1;
	fusb->cap.accessory[0] = TYPEC_ACCESSORY_AUDIO;
	fusb->cap.accessory[1] = TYPEC_ACCESSORY_DEBUG;
	fusb->cap.orientation_aware = true;
	fusb->cap.driver_data = fusb;
	fusb->cap.ops = &fusb301_typec_ops;

	ret = fusb301_hw_init(fusb);
	if (ret)
		goto err_put_connector;

	fusb->port = typec_register_port(dev, &fusb->cap);
	if (IS_ERR(fusb->port)) {
		ret = PTR_ERR(fusb->port);
		goto err_put_connector;
	}

	ret = fusb301_update_status(fusb);
	if (ret)
		goto err_unregister_port;

	ret = devm_request_threaded_irq(dev, client->irq, NULL, fusb301_irq,
					IRQF_ONESHOT, dev_name(dev), fusb);
	if (ret)
		goto err_unregister_port;

	i2c_set_clientdata(client, fusb);
	fwnode_handle_put(fusb->connector);
	return 0;

err_unregister_port:
	fusb301_set_vbus(fusb, false);
	fusb301_unregister_partner(fusb);
	typec_unregister_port(fusb->port);
err_put_connector:
	fwnode_handle_put(fusb->connector);
	return ret;
}

static void fusb301_remove(struct i2c_client *client)
{
	struct fusb301 *fusb = i2c_get_clientdata(client);

	fusb301_set_vbus(fusb, false);
	fusb301_unregister_partner(fusb);
	typec_unregister_port(fusb->port);
}

static const struct of_device_id fusb301_of_match[] = {
	{ .compatible = "onsemi,fusb301" },
	{ .compatible = "onsemi,fusb301a" },
	{ }
};
MODULE_DEVICE_TABLE(of, fusb301_of_match);

static const struct i2c_device_id fusb301_i2c_id[] = {
	{ "fusb301" },
	{ "fusb301a" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fusb301_i2c_id);

static struct i2c_driver fusb301_driver = {
	.probe = fusb301_probe,
	.remove = fusb301_remove,
	.driver = {
		.name = "fusb301",
		.of_match_table = fusb301_of_match,
	},
	.id_table = fusb301_i2c_id,
};
module_i2c_driver(fusb301_driver);

MODULE_DESCRIPTION("onsemi FUSB301 autonomous USB Type-C controller");
MODULE_LICENSE("GPL");
