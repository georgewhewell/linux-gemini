// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the legacy Novatek NT36xxx I2C protocol used by Gemini's
 * vendor kernel. The controller is addressed at 0x62 for hardware commands
 * and at its documented alternate target 0x01 for firmware/xdata accesses.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "novatek-nt36xxx.h"

struct nvt36xxx_data {
	struct i2c_client *client;
	struct input_dev *input;
	/* Serializes controller xdata selection and event reads. */
	struct mutex lock;
	struct touchscreen_properties prop;
	const struct nvt36xxx_chip *chip;
	u8 trim[NVT36XXX_TRIM_LEN];
	u8 event[NVT36XXX_EVENT_DATA_LEN];
	u32 raw_x_max;
	u32 raw_y_max;
};

static int nvt36xxx_write(struct nvt36xxx_data *data, u16 target,
			  const u8 *buf, size_t len)
{
	struct i2c_msg msg = {
		.addr = target,
		.len = len,
		.buf = (u8 *)buf,
	};
	int ret;

	ret = i2c_transfer(data->client->adapter, &msg, 1);
	if (ret == 1)
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int nvt36xxx_read(struct nvt36xxx_data *data, u16 target, u8 command,
			 u8 *buf, size_t len)
{
	u8 reg = command;
	struct i2c_msg msg[2] = {
		{
			.addr = target,
			.len = 1,
			.buf = &reg,
		}, {
			.addr = target,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};
	int ret;

	ret = i2c_transfer(data->client->adapter, msg, ARRAY_SIZE(msg));
	if (ret == ARRAY_SIZE(msg))
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int nvt36xxx_select_xdata(struct nvt36xxx_data *data, u32 address)
{
	u8 buf[3] = {
		NVT36XXX_XDATA_CMD,
		(address >> 16) & 0xff,
		(address >> 8) & 0xff,
	};

	return nvt36xxx_write(data, NVT36XXX_FW_ADDR, buf, sizeof(buf));
}

static int nvt36xxx_bootloader_reset(struct nvt36xxx_data *data)
{
	u8 buf[2] = { 0x00, 0x69 };
	int ret;

	ret = nvt36xxx_write(data, NVT36XXX_HW_ADDR, buf, sizeof(buf));
	if (ret)
		return ret;

	msleep(35);

	return 0;
}

static int nvt36xxx_reset_for_trim_read(struct nvt36xxx_data *data)
{
	static const u8 commands[] = { 0xa5, 0x35 };
	static const unsigned int delays[] = { 15, 10 };
	int ret;
	unsigned int i;

	ret = nvt36xxx_bootloader_reset(data);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		u8 buf[2] = { 0x00, commands[i] };

		ret = nvt36xxx_write(data, NVT36XXX_HW_ADDR, buf, sizeof(buf));
		if (ret)
			return ret;
		msleep(delays[i]);
	}

	return 0;
}

static int nvt36xxx_identify(struct nvt36xxx_data *data)
{
	u8 trim[NVT36XXX_TRIM_LEN];
	int ret = -ENODEV;
	unsigned int attempt;

	for (attempt = 0; attempt < NVT36XXX_RESET_RETRIES; attempt++) {
		ret = nvt36xxx_reset_for_trim_read(data);
		if (ret)
			continue;

		ret = nvt36xxx_select_xdata(data, 0x1f600);
		if (ret)
			continue;

		ret = nvt36xxx_read(data, NVT36XXX_FW_ADDR, NVT36XXX_TRIM_CMD,
				    trim, sizeof(trim));
		if (ret)
			continue;

		memcpy(data->trim, trim, sizeof(data->trim));
		data->chip = nvt36xxx_find_chip(trim);
		if (data->chip)
			return 0;

		ret = -ENODEV;
		usleep_range(10000, 11000);
	}

	return ret;
}

static int nvt36xxx_wait_reset(struct nvt36xxx_data *data)
{
	u8 state[NVT36XXX_RESET_STATE_LEN];
	int ret;
	unsigned int i;

	for (i = 0; i < 100; i++) {
		ret = nvt36xxx_select_xdata(data,
					    data->chip->mem_map->event_buf_addr);
		if (ret)
			return ret;

		ret = nvt36xxx_read(data, NVT36XXX_FW_ADDR,
				    NVT36XXX_RESET_STATE_CMD, state,
				    sizeof(state));
		if (ret)
			return ret;
		if (state[0] >= NVT36XXX_RESET_STATE_INIT && state[0] != 0xff)
			return 0;

		usleep_range(10000, 11000);
	}

	return -ETIMEDOUT;
}

static int nvt36xxx_read_fw_info(struct nvt36xxx_data *data)
{
	u8 info[NVT36XXX_FW_INFO_LEN];
	int ret;

	ret = nvt36xxx_select_xdata(data, data->chip->mem_map->event_buf_addr);
	if (ret)
		return ret;

	ret = nvt36xxx_read(data, NVT36XXX_FW_ADDR, NVT36XXX_FW_INFO_CMD,
			    info, sizeof(info));
	if (ret)
		return ret;

	if ((u8)(info[0] + info[1]) != 0xff)
		return -EIO;

	data->raw_x_max = get_unaligned_be16(&info[4]);
	data->raw_y_max = get_unaligned_be16(&info[6]);
	if (!data->raw_x_max || !data->raw_y_max ||
	    data->raw_x_max > NVT36XXX_MAX_COORD ||
	    data->raw_y_max > NVT36XXX_MAX_COORD)
		return -EINVAL;

	return 0;
}

static irqreturn_t nvt36xxx_irq(int irq, void *arg)
{
	struct nvt36xxx_data *data = arg;
	unsigned int active = 0;
	unsigned int i;
	int ret;

	mutex_lock(&data->lock);

	ret = nvt36xxx_select_xdata(data, data->chip->mem_map->event_buf_addr);
	if (ret)
		goto out;

	ret = nvt36xxx_read(data, NVT36XXX_FW_ADDR, 0x00,
			    data->event, sizeof(data->event));
	if (ret)
		goto out;

	for (i = 0; i < NVT36XXX_MAX_TOUCHES; i++) {
		struct nvt36xxx_contact contact;

		if (!nvt36xxx_decode_contact(data->event, i, &contact))
			continue;
		if (contact.x >= data->raw_x_max || contact.y >= data->raw_y_max)
			continue;

		input_mt_slot(data->input, contact.slot - 1);
		input_mt_report_slot_state(data->input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(data->input, &data->prop, contact.x,
				       contact.y, true);
		input_report_abs(data->input, ABS_MT_TOUCH_MAJOR,
				 min_t(unsigned int, contact.width,
				       NVT36XXX_MAX_WIDTH));
		input_report_abs(data->input, ABS_MT_PRESSURE,
				 clamp_val(contact.pressure, 1U,
					   NVT36XXX_MAX_PRESSURE));
		active++;
	}

	input_mt_sync_frame(data->input);
	input_report_key(data->input, BTN_TOUCH, active != 0);
	input_sync(data->input);

out:
	mutex_unlock(&data->lock);

	return IRQ_HANDLED;
}

static int nvt36xxx_suspend(struct device *dev)
{
	struct nvt36xxx_data *data = i2c_get_clientdata(to_i2c_client(dev));

	disable_irq(data->client->irq);

	return 0;
}

static int nvt36xxx_resume(struct device *dev)
{
	struct nvt36xxx_data *data = i2c_get_clientdata(to_i2c_client(dev));
	int ret;

	mutex_lock(&data->lock);
	ret = nvt36xxx_bootloader_reset(data);
	if (!ret)
		ret = nvt36xxx_wait_reset(data);
	mutex_unlock(&data->lock);

	enable_irq(data->client->irq);

	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(nvt36xxx_pm_ops, nvt36xxx_suspend,
				nvt36xxx_resume);

static int nvt36xxx_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct nvt36xxx_data *data;
	struct input_dev *input;
	int ret;

	if (client->addr != NVT36XXX_HW_ADDR || !client->irq)
		return -EINVAL;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->lock);
	i2c_set_clientdata(client, data);

	ret = nvt36xxx_identify(data);
	if (ret == -ENODEV)
		return dev_err_probe(dev, ret, "unsupported NT36xxx trim %*ph\n",
				     NVT36XXX_TRIM_LEN, data->trim);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read NT36xxx trim\n");

	/*
	 * This is an explicit runtime identity gate for recovery images.  The trim
	 * protocol is stateful, so restore the controller through the same bounded
	 * bootloader-reset/reset-state path before declining to bind.  Returning
	 * -ENODEV leaves no input device or IRQ registered; a later full activation
	 * must use a DT without novatek,identity-only.
	 */
	if (device_property_read_bool(dev, "novatek,identity-only")) {
		ret = nvt36xxx_bootloader_reset(data);
		if (!ret)
			ret = nvt36xxx_wait_reset(data);
		if (ret)
			return dev_err_probe(dev, ret,
					     "identity trim read could not restore controller\n");

		dev_info(dev, "identity-only: %s trim %*ph detected; keeping input and IRQ quiescent\n",
			 data->chip->name, NVT36XXX_TRIM_LEN, data->trim);
		return -ENODEV;
	}

	ret = nvt36xxx_bootloader_reset(data);
	if (ret)
		return dev_err_probe(dev, ret, "bootloader reset failed\n");

	ret = nvt36xxx_wait_reset(data);
	if (ret)
		return dev_err_probe(dev, ret, "controller did not leave reset\n");

	ret = nvt36xxx_read_fw_info(data);
	if (ret)
		return dev_err_probe(dev, ret, "invalid firmware information\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = "Novatek NT36xxx touchscreen";
	input->id.bustype = BUS_I2C;
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, data->raw_x_max - 1,
			     0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, data->raw_y_max - 1,
			     0, 0);
	touchscreen_parse_properties(input, true, &data->prop);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, NVT36XXX_MAX_WIDTH,
			     0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, NVT36XXX_MAX_PRESSURE,
			     0, 0);
	ret = input_mt_init_slots(input, NVT36XXX_MAX_TOUCHES,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	data->input = input;
	ret = devm_request_threaded_irq(dev, client->irq, NULL, nvt36xxx_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	ret = input_register_device(input);
	if (ret)
		return ret;

	enable_irq(client->irq);

	dev_info(dev, "%s trim %*ph detected (%ux%u)\n", data->chip->name,
		 NVT36XXX_TRIM_LEN, data->trim, data->raw_x_max, data->raw_y_max);

	return 0;
}

static const struct of_device_id nvt36xxx_of_match[] = {
	{ .compatible = "novatek,nt36xxx" },
	{ }
};
MODULE_DEVICE_TABLE(of, nvt36xxx_of_match);

static const struct i2c_device_id nvt36xxx_i2c_id[] = {
	{ "nt36xxx" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nvt36xxx_i2c_id);

static struct i2c_driver nvt36xxx_driver = {
	.driver = {
		.name = "novatek-nt36xxx",
		.pm = pm_sleep_ptr(&nvt36xxx_pm_ops),
		.of_match_table = nvt36xxx_of_match,
	},
	.probe = nvt36xxx_probe,
	.id_table = nvt36xxx_i2c_id,
};
module_i2c_driver(nvt36xxx_driver);

MODULE_DESCRIPTION("Novatek legacy NT36xxx touchscreen driver");
MODULE_LICENSE("GPL");
