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
#include <linux/workqueue.h>

#include <drm/drm_panel.h>

#include "novatek-nt36xxx.h"

/* Bounded retries of a failed controller restart. */
#define NVT36XXX_START_RETRIES		5
#define NVT36XXX_START_RETRY_MS		200

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
	/*
	 * The following are protected by input->mutex.  @running is true while
	 * the controller has been restarted and its IRQ is enabled.
	 */
	bool running;
	bool identified;
	bool suspended;
	bool is_panel_follower;
	bool panel_prepared;
	unsigned int start_retries;
	struct delayed_work start_work;
	struct drm_panel_follower panel_follower;
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

static int nvt36xxx_wait_reset(struct nvt36xxx_data *data, u8 min_state,
			       u8 *last_state)
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
		if (last_state)
			*last_state = state[0];
		if (state[0] >= min_state && state[0] != 0xff)
			return 0;

		usleep_range(10000, 11000);
	}

	return -ETIMEDOUT;
}

/*
 * On this TDDI part the panel driver's RESX pulse also resets the touch MCU,
 * and the vendor resumes touch only after the display reset and DSI
 * initialisation, with a bootloader reset followed by a wait for
 * RESET_STATE_REK (aeon_nt36xxx/nt36xxx.c nvt_ts_resume()).  Repeat that
 * here whenever reporting is (re)started.  The caller holds data->lock.
 */
static int nvt36xxx_restart(struct nvt36xxx_data *data)
{
	struct device *dev = &data->client->dev;
	u8 state = 0;
	int ret;

	ret = nvt36xxx_bootloader_reset(data);
	if (ret)
		return ret;

	ret = nvt36xxx_wait_reset(data, NVT36XXX_RESET_STATE_REK, &state);
	if (ret == -ETIMEDOUT) {
		/* Do not make an unsettled baseline fatal; report it. */
		dev_warn(dev, "controller reset state %#04x did not reach REK\n",
			 state);
		return 0;
	}
	if (!ret)
		dev_dbg(dev, "controller reset state %#04x\n", state);

	return ret;
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
	/*
	 * Linux's novatek-nvt-ts reads the same 0x78 parameter block and takes
	 * byte 11 as the firmware's INT mode (0 rising, 1 falling, 2 low,
	 * 3 high).  Report it so the DT trigger can be checked against it.
	 */
	dev_info(&data->client->dev, "firmware %#04x, INT mode byte %#04x\n",
		 info[0], info[NVT36XXX_FW_INFO_IRQ_TYPE]);

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

/*
 * Identify the controller: match its trim, take it out of reset and read the
 * firmware's coordinate range.  The caller holds data->lock or has exclusive
 * access during probe.
 */
static int nvt36xxx_identify_controller(struct nvt36xxx_data *data)
{
	struct device *dev = &data->client->dev;
	int ret;

	ret = nvt36xxx_identify(data);
	if (ret == -ENODEV)
		return dev_err_probe(dev, ret, "unsupported NT36xxx trim %*ph\n",
				     NVT36XXX_TRIM_LEN, data->trim);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read NT36xxx trim\n");

	ret = nvt36xxx_bootloader_reset(data);
	if (ret)
		return dev_err_probe(dev, ret, "bootloader reset failed\n");

	ret = nvt36xxx_wait_reset(data, NVT36XXX_RESET_STATE_INIT, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "controller did not leave reset\n");

	ret = nvt36xxx_read_fw_info(data);
	if (ret)
		return dev_err_probe(dev, ret, "invalid firmware information\n");

	return 0;
}

/*
 * A panel follower cannot talk to the controller before the panel has been
 * prepared, so it identifies the controller on its first start instead of at
 * probe, as the vendor initialises touch only after the LCM is up.  Its input
 * device was registered with the DT touchscreen size, which the firmware must
 * confirm.  The caller holds data->lock.
 */
static int nvt36xxx_identify_follower(struct nvt36xxx_data *data)
{
	struct device *dev = &data->client->dev;
	u32 x_max = data->raw_x_max, y_max = data->raw_y_max;
	int ret;

	ret = nvt36xxx_identify_controller(data);
	if (!ret && (data->raw_x_max != x_max || data->raw_y_max != y_max)) {
		dev_err(dev, "firmware range %ux%u does not match DT size %ux%u\n",
			data->raw_x_max, data->raw_y_max, x_max, y_max);
		ret = -EINVAL;
	}
	/* Event filtering keeps using the registered range. */
	data->raw_x_max = x_max;
	data->raw_y_max = y_max;
	if (ret)
		return ret;

	data->identified = true;
	dev_info(dev, "%s trim %*ph detected (%ux%u)\n", data->chip->name,
		 NVT36XXX_TRIM_LEN, data->trim, x_max, y_max);

	return 0;
}

/* The caller holds input->mutex. */
static int nvt36xxx_start(struct nvt36xxx_data *data)
{
	int ret = 0;

	if (data->running)
		return 0;

	mutex_lock(&data->lock);
	if (!data->identified)
		ret = nvt36xxx_identify_follower(data);
	if (!ret)
		ret = nvt36xxx_restart(data);
	mutex_unlock(&data->lock);

	/*
	 * Do not expose post-reset I2C traffic to the event handler until the
	 * controller has passed the bounded reset-state gate.
	 */
	if (ret)
		return ret;

	enable_irq(data->client->irq);
	data->running = true;

	return 0;
}

/* The caller holds input->mutex. */
static void nvt36xxx_stop(struct nvt36xxx_data *data)
{
	if (!data->running)
		return;

	disable_irq(data->client->irq);
	data->running = false;

	/* Release any contacts that were down when reporting stopped. */
	input_mt_sync_frame(data->input);
	input_report_key(data->input, BTN_TOUCH, 0);
	input_sync(data->input);
}

/*
 * When the touchscreen follows its panel, the panel's power sequence resets
 * the controller, so it can only be started while the panel is prepared.
 */
static bool nvt36xxx_want_running(struct nvt36xxx_data *data)
{
	if (data->suspended || !input_device_enabled(data->input))
		return false;

	return !data->is_panel_follower || data->panel_prepared;
}

/*
 * Start reporting if it is wanted.  A failed restart is reported and retried
 * a bounded number of times, so that a controller which was still coming out
 * of reset does not leave the touchscreen dead.  The caller holds
 * input->mutex.
 */
static void nvt36xxx_update(struct nvt36xxx_data *data)
{
	struct device *dev = &data->client->dev;
	int ret;

	if (!nvt36xxx_want_running(data) || data->running) {
		data->start_retries = 0;
		return;
	}

	ret = nvt36xxx_start(data);
	if (!ret) {
		data->start_retries = 0;
		return;
	}

	if (data->start_retries >= NVT36XXX_START_RETRIES) {
		dev_err(dev, "controller restart failed: %d; giving up\n", ret);
		data->start_retries = 0;
		return;
	}

	data->start_retries++;
	dev_warn(dev, "controller restart failed: %d; retry %u of %u\n", ret,
		 data->start_retries, NVT36XXX_START_RETRIES);
	mod_delayed_work(system_wq, &data->start_work,
			 msecs_to_jiffies(NVT36XXX_START_RETRY_MS));
}

static void nvt36xxx_start_work(struct work_struct *work)
{
	struct nvt36xxx_data *data =
		container_of(to_delayed_work(work), struct nvt36xxx_data,
			     start_work);

	guard(mutex)(&data->input->mutex);
	nvt36xxx_update(data);
}

static void nvt36xxx_cancel_start_work(void *data)
{
	struct nvt36xxx_data *nvt = data;

	cancel_delayed_work_sync(&nvt->start_work);
}

static int nvt36xxx_open(struct input_dev *input)
{
	nvt36xxx_update(input_get_drvdata(input));

	return 0;
}

static void nvt36xxx_close(struct input_dev *input)
{
	nvt36xxx_stop(input_get_drvdata(input));
}

static int nvt36xxx_panel_prepared(struct drm_panel_follower *follower)
{
	struct nvt36xxx_data *data =
		container_of(follower, struct nvt36xxx_data, panel_follower);

	guard(mutex)(&data->input->mutex);
	data->panel_prepared = true;
	/* The panel power-on has just reset the controller: restart it. */
	nvt36xxx_stop(data);
	data->start_retries = 0;
	nvt36xxx_update(data);

	return 0;
}

static int nvt36xxx_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct nvt36xxx_data *data =
		container_of(follower, struct nvt36xxx_data, panel_follower);

	cancel_delayed_work_sync(&data->start_work);

	guard(mutex)(&data->input->mutex);
	nvt36xxx_stop(data);
	data->panel_prepared = false;

	return 0;
}

static const struct drm_panel_follower_funcs nvt36xxx_panel_follower_funcs = {
	.panel_prepared = nvt36xxx_panel_prepared,
	.panel_unpreparing = nvt36xxx_panel_unpreparing,
};

static int nvt36xxx_suspend(struct device *dev)
{
	struct nvt36xxx_data *data = i2c_get_clientdata(to_i2c_client(dev));

	/* A panel follower is stopped and started with its panel. */
	if (data->is_panel_follower)
		return 0;

	cancel_delayed_work_sync(&data->start_work);

	guard(mutex)(&data->input->mutex);
	data->suspended = true;
	nvt36xxx_stop(data);

	return 0;
}

static int nvt36xxx_resume(struct device *dev)
{
	struct nvt36xxx_data *data = i2c_get_clientdata(to_i2c_client(dev));

	if (data->is_panel_follower)
		return 0;

	guard(mutex)(&data->input->mutex);
	data->suspended = false;
	data->start_retries = 0;
	nvt36xxx_update(data);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(nvt36xxx_pm_ops, nvt36xxx_suspend,
				nvt36xxx_resume);

/* Identify the controller at probe time, when it is not a panel follower. */
static int nvt36xxx_probe_hw(struct nvt36xxx_data *data)
{
	struct device *dev = &data->client->dev;
	int ret;

	/*
	 * This is an explicit runtime identity gate for recovery images.  The trim
	 * protocol is stateful, so restore the controller through the same bounded
	 * bootloader-reset/reset-state path before declining to bind.  Returning
	 * -ENODEV leaves no input device or IRQ registered; a later full activation
	 * must use a DT without novatek,identity-only.
	 */
	if (device_property_read_bool(dev, "novatek,identity-only")) {
		ret = nvt36xxx_identify(data);
		if (ret == -ENODEV)
			return dev_err_probe(dev, ret, "unsupported NT36xxx trim %*ph\n",
					     NVT36XXX_TRIM_LEN, data->trim);
		if (ret)
			return dev_err_probe(dev, ret, "failed to read NT36xxx trim\n");

		ret = nvt36xxx_bootloader_reset(data);
		if (!ret)
			ret = nvt36xxx_wait_reset(data,
						  NVT36XXX_RESET_STATE_INIT,
						  NULL);
		if (ret)
			return dev_err_probe(dev, ret,
					     "identity trim read could not restore controller\n");

		dev_info(dev, "identity-only: %s trim %*ph detected; keeping input and IRQ quiescent\n",
			 data->chip->name, NVT36XXX_TRIM_LEN, data->trim);
		return -ENODEV;
	}

	ret = nvt36xxx_identify_controller(data);
	if (ret)
		return ret;

	data->identified = true;
	dev_info(dev, "%s trim %*ph detected (%ux%u)\n", data->chip->name,
		 NVT36XXX_TRIM_LEN, data->trim, data->raw_x_max, data->raw_y_max);

	return 0;
}

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

	/*
	 * With a panel reference, the panel's reset sequence also resets the
	 * controller, and the panel holds it in reset until it is prepared,
	 * which may be long after probe.  Do not touch the hardware here: take
	 * the coordinate range from DT and identify the controller when the
	 * panel is first prepared.
	 */
	data->is_panel_follower = drm_is_panel_follower(dev);
	if (data->is_panel_follower) {
		if (device_property_read_u32(dev, "touchscreen-size-x",
					     &data->raw_x_max) ||
		    device_property_read_u32(dev, "touchscreen-size-y",
					     &data->raw_y_max) ||
		    !data->raw_x_max || !data->raw_y_max ||
		    data->raw_x_max > NVT36XXX_MAX_COORD ||
		    data->raw_y_max > NVT36XXX_MAX_COORD)
			return dev_err_probe(dev, -EINVAL,
					     "panel follower needs a valid touchscreen-size-x/y\n");
	} else {
		ret = nvt36xxx_probe_hw(data);
		if (ret)
			return ret;
	}

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = "Novatek NT36xxx touchscreen";
	input->id.bustype = BUS_I2C;
	input->open = nvt36xxx_open;
	input->close = nvt36xxx_close;
	input_set_drvdata(input, data);
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
	INIT_DELAYED_WORK(&data->start_work, nvt36xxx_start_work);
	ret = devm_add_action_or_reset(dev, nvt36xxx_cancel_start_work, data);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, client->irq, NULL, nvt36xxx_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	/*
	 * Reporting is started and stopped with the panel.  Follow it before
	 * the input device can be opened.
	 */
	if (data->is_panel_follower) {
		data->panel_follower.funcs = &nvt36xxx_panel_follower_funcs;
		ret = devm_drm_panel_add_follower(dev, &data->panel_follower);
		if (ret)
			return dev_err_probe(dev, ret, "failed to follow panel\n");
	}

	/* The IRQ stays disabled until the first open restarts the controller. */
	ret = input_register_device(input);
	if (ret)
		return ret;

	if (data->is_panel_follower)
		dev_info(dev, "following panel; controller is identified when the panel is prepared (%ux%u from DT)\n",
			 data->raw_x_max, data->raw_y_max);

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
