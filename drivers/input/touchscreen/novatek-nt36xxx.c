// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mainline-oriented driver for the Novatek NT36772 protocol.
 *
 * The Gemini PDA uses 0x62 for hardware reset commands.  Once the
 * controller enters its normal state, the event/xdata target is 0x01.
 * This is a logical target selected by the controller, not a second
 * device-tree I2C client.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <linux/unaligned.h>

#define NVT36XXX_HW_ADDR		0x62
#define NVT36XXX_FW_ADDR		0x01
#define NVT36XXX_XDATA_CMD		0xff
#define NVT36XXX_TRIM_CMD		0x4e
#define NVT36XXX_TRIM_LEN		6
#define NVT36XXX_EVENT_MAP		0x11e00
#define NVT36XXX_EVENT_DATA_LEN	65
#define NVT36XXX_FW_INFO_CMD		0x78
#define NVT36XXX_FW_INFO_LEN		16
#define NVT36XXX_RESET_STATE_CMD	0x60
#define NVT36XXX_RESET_STATE_LEN	5
#define NVT36XXX_MAX_TOUCHES		10
#define NVT36XXX_MAX_COORD		4096
#define NVT36XXX_MAX_PRESSURE		1000
#define NVT36XXX_MAX_WIDTH		255
#define NVT36XXX_RESET_RETRIES		5
#define NVT36XXX_RESET_STATE_INIT	0xa0

struct nvt36xxx_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[2];
	struct touchscreen_properties prop;
	u8 event[NVT36XXX_EVENT_DATA_LEN];
	u32 raw_x_max;
	u32 raw_y_max;
	u8 fw_version;
	u16 pid;
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

static int nvt36xxx_reset(struct nvt36xxx_data *data)
{
	static const u8 commands[] = { 0xa5, 0x35 };
	static const unsigned int delays[] = { 15, 10 };
	int ret;
	int i;

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
	int attempt;

	for (attempt = 0; attempt < NVT36XXX_RESET_RETRIES; attempt++) {
		ret = nvt36xxx_reset(data);
		if (ret)
			continue;

		ret = nvt36xxx_select_xdata(data, 0x1f600);
		if (ret)
			continue;

		ret = nvt36xxx_read(data, NVT36XXX_FW_ADDR,
				    NVT36XXX_TRIM_CMD, trim, sizeof(trim));
		if (ret)
			continue;

		/* Source/ELF trim entry 8: FF FF FF 72 66 03. */
		if (trim[3] == 0x72 && trim[4] == 0x66 && trim[5] == 0x03)
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
	int i;

	for (i = 0; i < 100; i++) {
		ret = nvt36xxx_select_xdata(data, NVT36XXX_EVENT_MAP);
		if (ret)
			return ret;

		ret = nvt36xxx_read(data, NVT36XXX_FW_ADDR,
				    NVT36XXX_RESET_STATE_CMD, state, sizeof(state));
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

	ret = nvt36xxx_select_xdata(data, NVT36XXX_EVENT_MAP);
	if (ret)
		return ret;

	ret = nvt36xxx_read(data, NVT36XXX_FW_ADDR,
			    NVT36XXX_FW_INFO_CMD, info, sizeof(info));
	if (ret)
		return ret;

	if ((u8)(info[0] + info[1]) != 0xff)
		return -EIO;

	data->fw_version = info[0];
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
	unsigned int i;
	unsigned int active = 0;
	int ret;

	ret = nvt36xxx_select_xdata(data, NVT36XXX_EVENT_MAP);
	if (ret)
		return IRQ_HANDLED;

	ret = nvt36xxx_read(data, NVT36XXX_FW_ADDR, 0x00,
			    data->event, sizeof(data->event));
	if (ret)
		return IRQ_HANDLED;

	for (i = 0; i < NVT36XXX_MAX_TOUCHES; i++) {
		u8 *touch = &data->event[i * 6];
		unsigned int slot = touch[0] >> 3;
		unsigned int status = touch[0] & 0x07;
		unsigned int x, y, width, pressure;

		if (!slot || slot > NVT36XXX_MAX_TOUCHES ||
		    (status != 0x01 && status != 0x02))
			continue;

		x = (touch[1] << 4) | (touch[3] >> 4);
		y = (touch[2] << 4) | (touch[3] & 0x0f);
		if (x >= data->raw_x_max || y >= data->raw_y_max)
			continue;

		width = touch[4] ? touch[4] : 1;
		width = min_t(unsigned int, width, NVT36XXX_MAX_WIDTH);
		if (i < 2)
			pressure = touch[5] | (data->event[62 + i] << 8);
		else
			pressure = touch[5];
		pressure = clamp_val(pressure, 1U, NVT36XXX_MAX_PRESSURE);

		/* Match the board's vendor orientation: swap X/Y, then reverse Y. */
		input_mt_slot(data->input, slot - 1);
		input_mt_report_slot_state(data->input, MT_TOOL_FINGER, true);
		input_report_abs(data->input, ABS_MT_POSITION_X, y);
		input_report_abs(data->input, ABS_MT_POSITION_Y,
				   data->raw_x_max - 1 - x);
		input_report_abs(data->input, ABS_MT_TOUCH_MAJOR, width);
		input_report_abs(data->input, ABS_MT_PRESSURE, pressure);
		active++;
	}

	input_mt_sync_frame(data->input);
	input_report_key(data->input, BTN_TOUCH, active != 0);
	input_sync(data->input);

	return IRQ_HANDLED;
}

static void nvt36xxx_disable_regulators(void *arg)
{
	struct nvt36xxx_data *data = arg;

	regulator_bulk_disable(ARRAY_SIZE(data->supplies), data->supplies);
}

static int nvt36xxx_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct nvt36xxx_data *data;
	struct input_dev *input;
	int ret;

	if (client->addr != NVT36XXX_HW_ADDR || !client->irq)
		return -EINVAL;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);
	data->supplies[0].supply = "vcc";
	data->supplies[1].supply = "iovcc";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(data->supplies),
				      data->supplies);
	if (ret)
		return ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(data->supplies), data->supplies);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, nvt36xxx_disable_regulators, data);
	if (ret)
		return ret;

	data->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						    GPIOD_OUT_LOW);
	if (IS_ERR(data->reset_gpio))
		return PTR_ERR(data->reset_gpio);

	ret = nvt36xxx_identify(data);
	if (ret)
		return dev_err_probe(dev, ret, "NT36772 trim identity failed\n");

	/* The vendor probe re-enters bootloader reset before FW-info reads. */
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

	input->name = "Novatek NT36772 touchscreen";
	input->id.bustype = BUS_I2C;
	touchscreen_parse_properties(input, true, &data->prop);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0,
				      data->raw_y_max - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0,
				      data->raw_x_max - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0,
				      NVT36XXX_MAX_WIDTH, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0,
				      NVT36XXX_MAX_PRESSURE, 0, 0);
	ret = input_mt_init_slots(input, NVT36XXX_MAX_TOUCHES,
					   INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	data->input = input;
	input_set_drvdata(input, data);
	ret = devm_request_threaded_irq(dev, client->irq, NULL, nvt36xxx_irq,
					IRQF_ONESHOT, dev_name(dev), data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	return input_register_device(input);
}

static const struct of_device_id nvt36xxx_of_match[] = {
	{ .compatible = "novatek,nt36772-ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, nvt36xxx_of_match);

static const struct i2c_device_id nvt36xxx_i2c_id[] = {
	{ "nt36772-ts" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nvt36xxx_i2c_id);

static struct i2c_driver nvt36xxx_driver = {
	.driver = {
		.name = "novatek-nt36xxx",
		.of_match_table = nvt36xxx_of_match,
	},
	.probe = nvt36xxx_probe,
	.id_table = nvt36xxx_i2c_id,
};
module_i2c_driver(nvt36xxx_driver);

MODULE_DESCRIPTION("Novatek NT36772 touchscreen driver");
MODULE_AUTHOR("Julien Etienne <julien.etienne@gmail.com>");
MODULE_LICENSE("GPL");
