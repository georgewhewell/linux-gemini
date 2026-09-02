// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 post-patch WMT and VCN33 calibration sequence.
 *
 * The caller supplies an active full-STP transaction capability. It completes
 * the vendor post-patch transactions without taking ownership of CONSYS,
 * BTIF, firmware, or a radio client; those lifetimes remain with the caller.
 *
 * MT6797 uses fixed 3.3 V VCN33 PALDOs.  The old efuse opcode 0x12 sequence
 * was MT6580-only and must not be sent here.
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mt6797-consys-wmt-postpatch.h>
#include <linux/soc/mediatek/mtk-wmt.h>

#define MT6797_POSTPATCH_TIMEOUT		msecs_to_jiffies(3000)
#define MT6797_POSTPATCH_VCN33_UV		3300000
#define MT6797_POSTPATCH_COEX_DELAY_MS	5

enum mt6797_postpatch_stage {
	POSTPATCH_SNAPSHOT, POSTPATCH_SET_BT, POSTPATCH_SET_WIFI, POSTPATCH_ENABLE_BT, POSTPATCH_ENABLE_WIFI,
	POSTPATCH_LTE_EXT, POSTPATCH_LTE_FILTER, POSTPATCH_LTE_FREQ, POSTPATCH_LTE_UNSAFE, POSTPATCH_LTE_L,
	POSTPATCH_CALIBRATE, POSTPATCH_DISABLE_BT, POSTPATCH_DISABLE_WIFI, POSTPATCH_VERIFY_OFF,
	POSTPATCH_RESTORE_SELECTOR, POSTPATCH_COEX, POSTPATCH_FM_STRAP,
};

struct mt6797_postpatch_ops {
	int (*get_voltage)(void *context, bool wifi);
	int (*is_enabled)(void *context, bool wifi);
	int (*set_voltage)(void *context, bool wifi, int uv);
	int (*enable)(void *context, bool wifi);
	int (*disable)(void *context, bool wifi);
	int (*wmt)(void *context, u8 opcode, const u8 *data, size_t data_len,
		   const struct mtk_wmt_match *expected);
	int (*wmt_capture)(void *context, u8 opcode, const u8 *data,
			   size_t data_len, u8 expected_opcode,
			   size_t min_data_len, size_t max_data_len,
			   u8 *reply, size_t reply_len,
			   size_t *actual_data_len);
	void (*delay_ms)(void *context, unsigned int milliseconds);
	void *context;
};

struct mt6797_postpatch_state {
	int original_uv;
	bool selector_dirty;
	bool bt_enabled;
	bool wifi_enabled;
	bool calibration_reply_valid;
	u8 calibration_reply[2];
	size_t calibration_event_len;
	int cleanup_error;
	enum mt6797_postpatch_stage stage;
};

/* Exact raw SDUs from wmt_ic_soc.c's MT6797 post-patch tables. */
static const u8 postpatch_lte_ext[] = { 0x0d, 0x00, 0x00 };
static const u8 postpatch_lte_filter[] = {
	0x11, 0x00, 0x00, 0x01, 0x00, 0x11, 0x11, 0x16,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x63, 0x63,
	0x00, 0x39, 0x43, 0x63, 0x63, 0x02, 0x02, 0x03,
	0x00, 0x01, 0x01, 0x01, 0x01, 0x0e, 0x0e, 0x0e,
	0x00, 0x0a, 0x0c, 0x0e, 0x0e, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
};
static const u8 postpatch_lte_freq[] = {
	0x12, 0xfc, 0x08, 0x15, 0x09, 0x2e, 0x09, 0x47,
	0x09, 0xc4, 0x09, 0xd4, 0x09, 0xe3, 0x09, 0x5a,
	0x0a, 0x14, 0x09, 0x2d, 0x09, 0x46, 0x09, 0x60,
	0x09, 0xd3, 0x09, 0xe2, 0x09, 0x59, 0x0a, 0x8b,
	0x0a,
};
static const u8 postpatch_lte_unsafe[] = { 0x13, 0x00 };
static const u8 postpatch_lte_l[] = { 0x21, 0x01 };
static const u8 postpatch_calibrate[] = { 0x01 };
static const u8 postpatch_coex[] = { 0x01, 0x01 };
static const u8 postpatch_fm_strap[] = { 0x02, 0x02 };
static const u8 postpatch_ok[] = { 0x00 };
static const u8 postpatch_fm_strap_reply[] = { 0x00, 0x02 };

static const struct mtk_wmt_match postpatch_ok_10 = {
	.opcode = 0x10, .data = postpatch_ok, .data_len = sizeof(postpatch_ok),
};
static const struct mtk_wmt_match postpatch_fm_strap_match = {
	.opcode = 0x05, .data = postpatch_fm_strap_reply,
	.data_len = sizeof(postpatch_fm_strap_reply),
};

static const char * const mt6797_postpatch_stage_name[] = {
	[POSTPATCH_SNAPSHOT] = "snapshot", [POSTPATCH_SET_BT] = "set-vcn33-bt",
	[POSTPATCH_SET_WIFI] = "set-vcn33-wifi", [POSTPATCH_ENABLE_BT] = "enable-vcn33-bt",
	[POSTPATCH_ENABLE_WIFI] = "enable-vcn33-wifi", [POSTPATCH_LTE_EXT] = "lte-ext",
	[POSTPATCH_LTE_FILTER] = "lte-filter", [POSTPATCH_LTE_FREQ] = "lte-frequency",
	[POSTPATCH_LTE_UNSAFE] = "lte-unsafe", [POSTPATCH_LTE_L] = "lte-l",
	[POSTPATCH_CALIBRATE] = "rf-calibration", [POSTPATCH_DISABLE_BT] = "disable-vcn33-bt",
	[POSTPATCH_DISABLE_WIFI] = "disable-vcn33-wifi", [POSTPATCH_VERIFY_OFF] = "verify-vcn33-off",
	[POSTPATCH_RESTORE_SELECTOR] = "restore-vcn33-selector", [POSTPATCH_COEX] = "coex-antenna",
	[POSTPATCH_FM_STRAP] = "fm-strap",
};

static int mt6797_postpatch_wmt(struct mt6797_postpatch_ops *ops,
		struct mt6797_postpatch_state *state, enum mt6797_postpatch_stage stage,
		u8 opcode, const u8 *data, size_t data_len,
		const struct mtk_wmt_match *expected)
{
	state->stage = stage;
	return ops->wmt(ops->context, opcode, data, data_len, expected);
}

static int mt6797_postpatch_calibrate(struct mt6797_postpatch_ops *ops,
		struct mt6797_postpatch_state *state)
{
	int ret;

	state->stage = POSTPATCH_CALIBRATE;
	ret = ops->wmt_capture(ops->context, 0x14, postpatch_calibrate,
			sizeof(postpatch_calibrate), 0x14,
			sizeof(state->calibration_reply), MTK_WMT_MAX_DATA,
			state->calibration_reply, sizeof(state->calibration_reply),
			&state->calibration_event_len);
	if (!ret)
		state->calibration_reply_valid = true;
	return ret;
}

static int mt6797_postpatch_snapshot(struct mt6797_postpatch_ops *ops,
		struct mt6797_postpatch_state *state)
{
	int bt_uv, wifi_uv, ret;

	state->stage = POSTPATCH_SNAPSHOT;
	bt_uv = ops->get_voltage(ops->context, false);
	if (bt_uv <= 0)
		return bt_uv ?: -EINVAL;
	wifi_uv = ops->get_voltage(ops->context, true);
	if (wifi_uv <= 0)
		return wifi_uv ?: -EINVAL;
	if (bt_uv != wifi_uv)
		return -EUCLEAN;
	ret = ops->is_enabled(ops->context, false);
	if (ret)
		return ret < 0 ? ret : -EBUSY;
	ret = ops->is_enabled(ops->context, true);
	if (ret)
		return ret < 0 ? ret : -EBUSY;
	state->original_uv = bt_uv;
	return 0;
}

/*
 * MT6351's VCN33 handles have distinct enable bits but a shared selector.
 * Disable every consumer in vendor BT-then-Wi-Fi order, prove both off, and
 * only then restore the one shared selector.  A failed cleanup never writes
 * the selector while either consumer might still be powered.
 */
static int mt6797_postpatch_cleanup(struct mt6797_postpatch_ops *ops,
		struct mt6797_postpatch_state *state)
{
	int ret = 0, tmp, bt_on, wifi_on;

	if (state->bt_enabled) {
		state->stage = POSTPATCH_DISABLE_BT;
		tmp = ops->disable(ops->context, false);
		if (tmp)
			ret = tmp;
		else
			state->bt_enabled = false;
	}
	if (state->wifi_enabled) {
		state->stage = POSTPATCH_DISABLE_WIFI;
		tmp = ops->disable(ops->context, true);
		if (!ret && tmp)
			ret = tmp;
		else if (!tmp)
			state->wifi_enabled = false;
	}
	if (!state->selector_dirty)
		return ret;

	state->stage = POSTPATCH_VERIFY_OFF;
	bt_on = ops->is_enabled(ops->context, false);
	wifi_on = ops->is_enabled(ops->context, true);
	if (ret || bt_on || wifi_on)
		return ret ?: (bt_on < 0 ? bt_on : (wifi_on < 0 ? wifi_on : -EBUSY));

	state->stage = POSTPATCH_RESTORE_SELECTOR;
	tmp = ops->set_voltage(ops->context, false, state->original_uv);
	if (tmp)
		return tmp;
	state->selector_dirty = false;
	return 0;
}

static int mt6797_postpatch_enable(struct mt6797_postpatch_ops *ops,
		struct mt6797_postpatch_state *state, bool wifi)
{
	bool *enabled = wifi ? &state->wifi_enabled : &state->bt_enabled;
	int ret, on;

	ret = ops->enable(ops->context, wifi);
	if (!ret) {
		*enabled = true;
		return 0;
	}
	/* A regulator enable may have taken effect before reporting an error. */
	on = ops->is_enabled(ops->context, wifi);
	if (on > 0)
		*enabled = true;
	return ret;
}

static int mt6797_postpatch_postpatch_run(struct mt6797_postpatch_ops *ops,
		struct mt6797_postpatch_state *state)
{
	enum mt6797_postpatch_stage stage_before_cleanup;
	int ret, cleanup_ret;

	if (!ops || !state || !ops->get_voltage || !ops->is_enabled ||
	    !ops->set_voltage || !ops->enable || !ops->disable || !ops->wmt ||
	    !ops->wmt_capture ||
	    !ops->delay_ms)
		return -EINVAL;
	ret = mt6797_postpatch_snapshot(ops, state);
	if (ret)
		return ret;
	/* Vendor wmt_stp_wifi_lte_coex() waits after vendor clock-down. */
	ops->delay_ms(ops->context, MT6797_POSTPATCH_COEX_DELAY_MS);
	ret = mt6797_postpatch_wmt(ops, state, POSTPATCH_LTE_EXT, 0x10,
		postpatch_lte_ext, sizeof(postpatch_lte_ext), &postpatch_ok_10);
	if (ret)
		goto out_cleanup;
	ret = mt6797_postpatch_wmt(ops, state, POSTPATCH_LTE_FILTER, 0x10,
		postpatch_lte_filter, sizeof(postpatch_lte_filter), &postpatch_ok_10);
	if (ret)
		goto out_cleanup;
	ret = mt6797_postpatch_wmt(ops, state, POSTPATCH_LTE_FREQ, 0x10,
		postpatch_lte_freq, sizeof(postpatch_lte_freq), &postpatch_ok_10);
	if (ret)
		goto out_cleanup;
	ret = mt6797_postpatch_wmt(ops, state, POSTPATCH_LTE_UNSAFE, 0x10,
		postpatch_lte_unsafe, sizeof(postpatch_lte_unsafe), &postpatch_ok_10);
	if (ret)
		goto out_cleanup;
	ret = mt6797_postpatch_wmt(ops, state, POSTPATCH_LTE_L, 0x10,
		postpatch_lte_l, sizeof(postpatch_lte_l), &postpatch_ok_10);
	if (ret)
		goto out_cleanup;
	state->stage = POSTPATCH_SET_BT;
	/* set_voltage() can change the shared selector before reporting failure. */
	state->selector_dirty = true;
	ret = ops->set_voltage(ops->context, false, MT6797_POSTPATCH_VCN33_UV);
	if (ret)
		goto out_cleanup;
	state->stage = POSTPATCH_SET_WIFI;
	ret = ops->set_voltage(ops->context, true, MT6797_POSTPATCH_VCN33_UV);
	if (ret)
		goto out_cleanup;
	state->stage = POSTPATCH_ENABLE_BT;
	ret = mt6797_postpatch_enable(ops, state, false);
	if (ret)
		goto out_cleanup;
	state->stage = POSTPATCH_ENABLE_WIFI;
	ret = mt6797_postpatch_enable(ops, state, true);
	if (ret)
		goto out_cleanup;

	ret = mt6797_postpatch_calibrate(ops, state);
out_cleanup:
	stage_before_cleanup = state->stage;
	cleanup_ret = mt6797_postpatch_cleanup(ops, state);
	state->cleanup_error = cleanup_ret;
	/* Preserve the causal stage in logs when cleanup itself succeeds. */
	if (ret)
		state->stage = stage_before_cleanup;
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		return ret;
	ret = mt6797_postpatch_wmt(ops, state, POSTPATCH_COEX, 0x10,
		postpatch_coex, sizeof(postpatch_coex), &postpatch_ok_10);
	if (ret)
		return ret;
	return mt6797_postpatch_wmt(ops, state, POSTPATCH_FM_STRAP, 0x05,
		postpatch_fm_strap, sizeof(postpatch_fm_strap), &postpatch_fm_strap_match);
}

struct mt6797_postpatch_owner {
	struct device *dev;
	struct regulator *bt;
	struct regulator *wifi;
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease;
};

static struct regulator *mt6797_postpatch_regulator(struct mt6797_postpatch_owner *owner,
		bool wifi)
{
	return wifi ? owner->wifi : owner->bt;
}

static int mt6797_postpatch_live_get_voltage(void *context, bool wifi)
{
	return regulator_get_voltage(mt6797_postpatch_regulator(context, wifi));
}

static int mt6797_postpatch_live_is_enabled(void *context, bool wifi)
{
	return regulator_is_enabled(mt6797_postpatch_regulator(context, wifi));
}

static int mt6797_postpatch_live_set_voltage(void *context, bool wifi, int uv)
{
	return regulator_set_voltage(mt6797_postpatch_regulator(context, wifi), uv, uv);
}

static int mt6797_postpatch_live_enable(void *context, bool wifi)
{
	return regulator_enable(mt6797_postpatch_regulator(context, wifi));
}

static int mt6797_postpatch_live_disable(void *context, bool wifi)
{
	return regulator_disable(mt6797_postpatch_regulator(context, wifi));
}

static int mt6797_postpatch_live_wmt(void *context, u8 opcode, const u8 *data,
		size_t data_len, const struct mtk_wmt_match *expected)
{
	struct mt6797_postpatch_owner *owner = context;

	return mt6797_btif_stp_bootstrap_full_wmt_transaction(owner->lease, opcode,
		data, data_len, expected, MT6797_POSTPATCH_TIMEOUT);
}

static int mt6797_postpatch_live_wmt_capture(void *context, u8 opcode,
		const u8 *data, size_t data_len, u8 expected_opcode,
		size_t min_data_len, size_t max_data_len, u8 *reply,
		size_t reply_len, size_t *actual_data_len)
{
	struct mt6797_postpatch_owner *owner = context;

	return mt6797_btif_stp_bootstrap_full_wmt_transaction_capture_range(
		owner->lease, opcode, data, data_len, expected_opcode,
		min_data_len, max_data_len, reply, reply_len, actual_data_len,
		MT6797_POSTPATCH_TIMEOUT);
}

static void mt6797_postpatch_live_delay_ms(void *context, unsigned int milliseconds)
{
	msleep(milliseconds);
}

static int mt6797_postpatch_execute(void *context,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	struct mt6797_postpatch_owner *owner = context;
	struct mt6797_postpatch_ops ops = {
		.get_voltage = mt6797_postpatch_live_get_voltage,
		.is_enabled = mt6797_postpatch_live_is_enabled,
		.set_voltage = mt6797_postpatch_live_set_voltage,
		.enable = mt6797_postpatch_live_enable,
		.disable = mt6797_postpatch_live_disable,
		.wmt = mt6797_postpatch_live_wmt,
		.wmt_capture = mt6797_postpatch_live_wmt_capture,
		.delay_ms = mt6797_postpatch_live_delay_ms,
		.context = owner,
	};
	struct mt6797_postpatch_state state = { };
	int ret;

	owner->lease = lease;
	dev_info(owner->dev, "post-patch start: fixed 3.3V, no function control\n");
	ret = mt6797_postpatch_postpatch_run(&ops, &state);
	owner->lease = NULL;
	if (ret)
		dev_err(owner->dev,
			"post-patch stage %s failed: %d cleanup=%d; CONSYS reset required\n",
			mt6797_postpatch_stage_name[state.stage], ret, state.cleanup_error);
	else
		dev_info(owner->dev, "post-patch passed; calibration complete\n");
	if (state.calibration_reply_valid)
		dev_info(owner->dev, "post-patch RF calibration event: len=%zu prefix=%*ph\n",
			 state.calibration_event_len,
			 (int)sizeof(state.calibration_reply), state.calibration_reply);
	return ret;
}

int mt6797_consys_wmt_postpatch_run(struct device *dev,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		struct regulator *bt_paldo, struct regulator *wifi_paldo)
{
	struct mt6797_postpatch_owner owner = {
		.dev = dev,
		.bt = bt_paldo,
		.wifi = wifi_paldo,
	};

	if (!dev || !lease || !bt_paldo || !wifi_paldo)
		return -EINVAL;
	if (!mt6797_btif_stp_bootstrap_full_wmt_lease_active(lease))
		return -ESHUTDOWN;
	if (regulator_is_equal(bt_paldo, wifi_paldo))
		return -EINVAL;

	return mt6797_postpatch_execute(&owner, lease);
}
EXPORT_SYMBOL_GPL(mt6797_consys_wmt_postpatch_run);

MODULE_DESCRIPTION("MediaTek MT6797 post-patch WMT calibration");

MODULE_LICENSE("GPL");

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_WMT_POSTPATCH_KUNIT_TEST)
#include <kunit/test.h>

enum mt6797_postpatch_test_event {
	POSTPATCH_TEST_DELAY, POSTPATCH_TEST_LTE_EXT, POSTPATCH_TEST_LTE_FILTER,
	POSTPATCH_TEST_LTE_FREQ, POSTPATCH_TEST_LTE_UNSAFE, POSTPATCH_TEST_LTE_L,
	POSTPATCH_TEST_SET_BT, POSTPATCH_TEST_SET_WIFI, POSTPATCH_TEST_ENABLE_BT,
	POSTPATCH_TEST_ENABLE_WIFI, POSTPATCH_TEST_CALIBRATE, POSTPATCH_TEST_DISABLE_BT,
	POSTPATCH_TEST_DISABLE_WIFI, POSTPATCH_TEST_RESTORE, POSTPATCH_TEST_COEX,
	POSTPATCH_TEST_FM_STRAP,
};

struct mt6797_postpatch_test_command {
	u8 opcode;
	const u8 *data;
	size_t data_len;
	const struct mtk_wmt_match *expected;
};

struct mt6797_postpatch_test {
	int voltage[2];
	bool enabled[2];
	int fail_set;
	int fail_enable;
	int fail_disable;
	int fail_wmt;
	int fail_enabled_call;
	unsigned int enabled_calls;
	unsigned int wmt_calls;
	unsigned int delay_calls;
	unsigned int delay_ms;
	bool wmt_before_delay;
	unsigned int restore_calls;
	bool restore_while_enabled;
	bool set_before_fail;
	bool enable_before_fail;
	u8 calibration_reply[2];
	size_t calibration_event_len;
	enum mt6797_postpatch_test_event event[24];
	unsigned int events;
	struct mt6797_postpatch_test_command command[8];
};

static void mt6797_postpatch_test_event(struct mt6797_postpatch_test *test,
		enum mt6797_postpatch_test_event event)
{
	if (test->events < ARRAY_SIZE(test->event))
		test->event[test->events++] = event;
}

static int mt6797_postpatch_test_get_voltage(void *context, bool wifi)
{
	struct mt6797_postpatch_test *test = context;

	return test->voltage[wifi];
}

static int mt6797_postpatch_test_is_enabled(void *context, bool wifi)
{
	struct mt6797_postpatch_test *test = context;

	if (test->fail_enabled_call >= 0 &&
	    test->enabled_calls++ == test->fail_enabled_call)
		return -ETIMEDOUT;
	return test->enabled[wifi];
}

static int mt6797_postpatch_test_set_voltage(void *context, bool wifi, int uv)
{
	struct mt6797_postpatch_test *test = context;

	if (uv == MT6797_POSTPATCH_VCN33_UV) {
		mt6797_postpatch_test_event(test, wifi ? POSTPATCH_TEST_SET_WIFI : POSTPATCH_TEST_SET_BT);
		if (test->fail_set == wifi) {
			if (test->set_before_fail) {
				test->voltage[false] = uv;
				test->voltage[true] = uv;
			}
			return -ETIMEDOUT;
		}
	} else {
		mt6797_postpatch_test_event(test, POSTPATCH_TEST_RESTORE);
		test->restore_calls++;
		test->restore_while_enabled = test->enabled[false] || test->enabled[true];
	}
	test->voltage[false] = uv;
	test->voltage[true] = uv;
	return 0;
}

static int mt6797_postpatch_test_enable(void *context, bool wifi)
{
	struct mt6797_postpatch_test *test = context;

	mt6797_postpatch_test_event(test, wifi ? POSTPATCH_TEST_ENABLE_WIFI : POSTPATCH_TEST_ENABLE_BT);
	if (test->fail_enable == wifi) {
		if (test->enable_before_fail)
			test->enabled[wifi] = true;
		return -ETIMEDOUT;
	}
	test->enabled[wifi] = true;
	return 0;
}

static int mt6797_postpatch_test_disable(void *context, bool wifi)
{
	struct mt6797_postpatch_test *test = context;

	mt6797_postpatch_test_event(test, wifi ? POSTPATCH_TEST_DISABLE_WIFI : POSTPATCH_TEST_DISABLE_BT);
	if (test->fail_disable == wifi)
		return -ETIMEDOUT;
	test->enabled[wifi] = false;
	return 0;
}

static int mt6797_postpatch_test_wmt(void *context, u8 opcode, const u8 *data,
		size_t data_len, const struct mtk_wmt_match *expected)
{
	struct mt6797_postpatch_test *test = context;
	unsigned int call = test->wmt_calls++;

	if (!test->delay_calls)
		test->wmt_before_delay = true;
	if (data == postpatch_lte_ext)
		mt6797_postpatch_test_event(test, POSTPATCH_TEST_LTE_EXT);
	else if (data == postpatch_lte_filter)
		mt6797_postpatch_test_event(test, POSTPATCH_TEST_LTE_FILTER);
	else if (data == postpatch_lte_freq)
		mt6797_postpatch_test_event(test, POSTPATCH_TEST_LTE_FREQ);
	else if (data == postpatch_lte_unsafe)
		mt6797_postpatch_test_event(test, POSTPATCH_TEST_LTE_UNSAFE);
	else if (data == postpatch_lte_l)
		mt6797_postpatch_test_event(test, POSTPATCH_TEST_LTE_L);
	else if (data == postpatch_calibrate)
		mt6797_postpatch_test_event(test, POSTPATCH_TEST_CALIBRATE);
	else if (data == postpatch_coex)
		mt6797_postpatch_test_event(test, POSTPATCH_TEST_COEX);
	else if (data == postpatch_fm_strap)
		mt6797_postpatch_test_event(test, POSTPATCH_TEST_FM_STRAP);
	if (call < ARRAY_SIZE(test->command))
		test->command[call] = (struct mt6797_postpatch_test_command) {
			.opcode = opcode, .data = data, .data_len = data_len,
			.expected = expected,
		};
	if (test->fail_wmt >= 0 && call == test->fail_wmt)
		return -ETIMEDOUT;
	return 0;
}

static int mt6797_postpatch_test_wmt_capture(void *context, u8 opcode,
		const u8 *data, size_t data_len, u8 expected_opcode,
		size_t min_data_len, size_t max_data_len, u8 *reply,
		size_t reply_len, size_t *actual_data_len)
{
	struct mt6797_postpatch_test *test = context;
	unsigned int call = test->wmt_calls++;
	size_t event_len = test->calibration_event_len ?:
			   sizeof(test->calibration_reply);

	if (opcode != 0x14 || data != postpatch_calibrate ||
	    data_len != sizeof(postpatch_calibrate) || expected_opcode != 0x14 ||
	    min_data_len != sizeof(test->calibration_reply) ||
	    max_data_len != MTK_WMT_MAX_DATA ||
	    reply_len != sizeof(test->calibration_reply) || !actual_data_len ||
	    event_len < min_data_len || event_len > max_data_len)
		return -EPROTO;
	mt6797_postpatch_test_event(test, POSTPATCH_TEST_CALIBRATE);
	if (call < ARRAY_SIZE(test->command))
		test->command[call] = (struct mt6797_postpatch_test_command) {
			.opcode = opcode, .data = data, .data_len = data_len,
			.expected = NULL,
		};
	if (test->fail_wmt >= 0 && call == test->fail_wmt)
		return -ETIMEDOUT;
	memcpy(reply, test->calibration_reply, reply_len);
	*actual_data_len = event_len;
	return 0;
}

static void mt6797_postpatch_test_delay(void *context, unsigned int milliseconds)
{
	struct mt6797_postpatch_test *test = context;

	test->delay_calls++;
	test->delay_ms = milliseconds;
	mt6797_postpatch_test_event(test, POSTPATCH_TEST_DELAY);
}

static struct mt6797_postpatch_ops mt6797_postpatch_test_ops(struct mt6797_postpatch_test *test)
{
	return (struct mt6797_postpatch_ops) {
		.get_voltage = mt6797_postpatch_test_get_voltage,
		.is_enabled = mt6797_postpatch_test_is_enabled,
		.set_voltage = mt6797_postpatch_test_set_voltage,
		.enable = mt6797_postpatch_test_enable,
		.disable = mt6797_postpatch_test_disable,
		.wmt = mt6797_postpatch_test_wmt,
		.wmt_capture = mt6797_postpatch_test_wmt_capture,
		.delay_ms = mt6797_postpatch_test_delay,
		.context = test,
	};
}

static void mt6797_postpatch_test_expect_command(struct kunit *test,
		const struct mt6797_postpatch_test_command *command, u8 opcode,
		const u8 *data, size_t data_len, const struct mtk_wmt_match *expected)
{
	KUNIT_EXPECT_EQ(test, command->opcode, opcode);
	KUNIT_EXPECT_EQ(test, command->data_len, data_len);
	KUNIT_EXPECT_PTR_EQ(test, command->expected, expected);
	KUNIT_EXPECT_MEMEQ(test, command->data, data, data_len);
}

static void mt6797_postpatch_test_exact_order(struct kunit *test)
{
	struct mt6797_postpatch_test context = {
		.voltage = { 3500000, 3500000 },
		.fail_set = -1, .fail_enable = -1, .fail_disable = -1,
		.fail_wmt = -1, .fail_enabled_call = -1,
		.calibration_reply = { 0x7a, 0xc3 },
		.calibration_event_len = 378,
	};
	struct mt6797_postpatch_state state = { };
	struct mt6797_postpatch_ops ops = mt6797_postpatch_test_ops(&context);

	KUNIT_ASSERT_EQ(test, mt6797_postpatch_postpatch_run(&ops, &state), 0);
	KUNIT_EXPECT_EQ(test, context.delay_calls, 1U);
	KUNIT_EXPECT_EQ(test, context.delay_ms, MT6797_POSTPATCH_COEX_DELAY_MS);
	KUNIT_EXPECT_FALSE(test, context.wmt_before_delay);
	KUNIT_EXPECT_EQ(test, context.wmt_calls, 8U);
	mt6797_postpatch_test_expect_command(test, &context.command[0], 0x10,
		postpatch_lte_ext, sizeof(postpatch_lte_ext), &postpatch_ok_10);
	mt6797_postpatch_test_expect_command(test, &context.command[1], 0x10,
		postpatch_lte_filter, sizeof(postpatch_lte_filter), &postpatch_ok_10);
	mt6797_postpatch_test_expect_command(test, &context.command[2], 0x10,
		postpatch_lte_freq, sizeof(postpatch_lte_freq), &postpatch_ok_10);
	mt6797_postpatch_test_expect_command(test, &context.command[3], 0x10,
		postpatch_lte_unsafe, sizeof(postpatch_lte_unsafe), &postpatch_ok_10);
	mt6797_postpatch_test_expect_command(test, &context.command[4], 0x10,
		postpatch_lte_l, sizeof(postpatch_lte_l), &postpatch_ok_10);
	mt6797_postpatch_test_expect_command(test, &context.command[5], 0x14,
		postpatch_calibrate, sizeof(postpatch_calibrate), NULL);
	KUNIT_EXPECT_TRUE(test, state.calibration_reply_valid);
	KUNIT_EXPECT_EQ(test, state.calibration_event_len, (size_t)378);
	KUNIT_EXPECT_MEMEQ(test, state.calibration_reply, context.calibration_reply,
			   sizeof(state.calibration_reply));
	mt6797_postpatch_test_expect_command(test, &context.command[6], 0x10,
		postpatch_coex, sizeof(postpatch_coex), &postpatch_ok_10);
	mt6797_postpatch_test_expect_command(test, &context.command[7], 0x05,
		postpatch_fm_strap, sizeof(postpatch_fm_strap), &postpatch_fm_strap_match);
	KUNIT_EXPECT_EQ(test, context.events, 16U);
	KUNIT_EXPECT_EQ(test, context.event[0], POSTPATCH_TEST_DELAY);
	KUNIT_EXPECT_EQ(test, context.event[1], POSTPATCH_TEST_LTE_EXT);
	KUNIT_EXPECT_EQ(test, context.event[2], POSTPATCH_TEST_LTE_FILTER);
	KUNIT_EXPECT_EQ(test, context.event[3], POSTPATCH_TEST_LTE_FREQ);
	KUNIT_EXPECT_EQ(test, context.event[4], POSTPATCH_TEST_LTE_UNSAFE);
	KUNIT_EXPECT_EQ(test, context.event[5], POSTPATCH_TEST_LTE_L);
	KUNIT_EXPECT_EQ(test, context.event[6], POSTPATCH_TEST_SET_BT);
	KUNIT_EXPECT_EQ(test, context.event[7], POSTPATCH_TEST_SET_WIFI);
	KUNIT_EXPECT_EQ(test, context.event[8], POSTPATCH_TEST_ENABLE_BT);
	KUNIT_EXPECT_EQ(test, context.event[9], POSTPATCH_TEST_ENABLE_WIFI);
	KUNIT_EXPECT_EQ(test, context.event[10], POSTPATCH_TEST_CALIBRATE);
	KUNIT_EXPECT_EQ(test, context.event[11], POSTPATCH_TEST_DISABLE_BT);
	KUNIT_EXPECT_EQ(test, context.event[12], POSTPATCH_TEST_DISABLE_WIFI);
	KUNIT_EXPECT_EQ(test, context.event[13], POSTPATCH_TEST_RESTORE);
	KUNIT_EXPECT_EQ(test, context.event[14], POSTPATCH_TEST_COEX);
	KUNIT_EXPECT_EQ(test, context.event[15], POSTPATCH_TEST_FM_STRAP);
	KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
}

static void mt6797_postpatch_test_every_wmt_failure_unwinds(struct kunit *test)
{
	int fail;

	for (fail = 0; fail < 8; fail++) {
		struct mt6797_postpatch_test context = {
			.voltage = { 3500000, 3500000 }, .fail_set = -1,
			.fail_enable = -1, .fail_disable = -1, .fail_wmt = fail,
			.fail_enabled_call = -1,
		};
		struct mt6797_postpatch_state state = { };
		struct mt6797_postpatch_ops ops = mt6797_postpatch_test_ops(&context);

		KUNIT_EXPECT_EQ_MSG(test, mt6797_postpatch_postpatch_run(&ops, &state),
			-ETIMEDOUT, "WMT command %d", fail);
		KUNIT_EXPECT_FALSE(test, context.enabled[false]);
		KUNIT_EXPECT_FALSE(test, context.enabled[true]);
		KUNIT_EXPECT_EQ(test, context.restore_calls, fail < 5 ? 0U : 1U);
		KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
	}
}

static void mt6797_postpatch_test_setup_and_cleanup_failures(struct kunit *test)
{
	int fail_set, fail_enable, fail_disable;

	for (fail_set = 0; fail_set < 2; fail_set++) {
		struct mt6797_postpatch_test context = {
			.voltage = { 3500000, 3500000 }, .fail_set = fail_set,
			.fail_enable = -1, .fail_disable = -1, .fail_wmt = -1,
			.fail_enabled_call = -1,
		};
		struct mt6797_postpatch_state state = { };
		struct mt6797_postpatch_ops ops = mt6797_postpatch_test_ops(&context);

		KUNIT_EXPECT_EQ(test, mt6797_postpatch_postpatch_run(&ops, &state), -ETIMEDOUT);
		KUNIT_EXPECT_FALSE(test, context.enabled[false]);
		KUNIT_EXPECT_FALSE(test, context.enabled[true]);
		KUNIT_EXPECT_EQ(test, context.restore_calls, 1U);
	}
	for (fail_enable = 0; fail_enable < 2; fail_enable++) {
		struct mt6797_postpatch_test context = {
			.voltage = { 3500000, 3500000 }, .fail_set = -1,
			.fail_enable = fail_enable, .fail_disable = -1, .fail_wmt = -1,
			.fail_enabled_call = -1,
		};
		struct mt6797_postpatch_state state = { };
		struct mt6797_postpatch_ops ops = mt6797_postpatch_test_ops(&context);

		KUNIT_EXPECT_EQ(test, mt6797_postpatch_postpatch_run(&ops, &state), -ETIMEDOUT);
		KUNIT_EXPECT_FALSE(test, context.enabled[false]);
		KUNIT_EXPECT_FALSE(test, context.enabled[true]);
		KUNIT_EXPECT_EQ(test, context.restore_calls, 1U);
	}
	for (fail_disable = 0; fail_disable < 2; fail_disable++) {
		struct mt6797_postpatch_test context = {
			.voltage = { 3500000, 3500000 }, .fail_set = -1,
			.fail_enable = -1, .fail_disable = fail_disable, .fail_wmt = 5,
			.fail_enabled_call = -1,
		};
		struct mt6797_postpatch_state state = { };
		struct mt6797_postpatch_ops ops = mt6797_postpatch_test_ops(&context);

		KUNIT_EXPECT_EQ(test, mt6797_postpatch_postpatch_run(&ops, &state), -ETIMEDOUT);
		KUNIT_EXPECT_EQ(test, context.restore_calls, 0U);
		KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
		KUNIT_EXPECT_EQ(test, context.event[11], POSTPATCH_TEST_DISABLE_BT);
		KUNIT_EXPECT_EQ(test, context.event[12], POSTPATCH_TEST_DISABLE_WIFI);
	}
	for (fail_disable = 2; fail_disable < 4; fail_disable++) {
		struct mt6797_postpatch_test context = {
			.voltage = { 3500000, 3500000 }, .fail_set = -1,
			.fail_enable = -1, .fail_disable = -1, .fail_wmt = 5,
			.fail_enabled_call = fail_disable,
		};
		struct mt6797_postpatch_state state = { };
		struct mt6797_postpatch_ops ops = mt6797_postpatch_test_ops(&context);

		KUNIT_EXPECT_EQ(test, mt6797_postpatch_postpatch_run(&ops, &state), -ETIMEDOUT);
		KUNIT_EXPECT_EQ(test, context.restore_calls, 0U);
		KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
	}
}

static void mt6797_postpatch_test_partial_mutations_unwind(struct kunit *test)
{
	struct mt6797_postpatch_test context = {
		.voltage = { 3500000, 3500000 }, .fail_set = 0,
		.fail_enable = -1, .fail_disable = -1, .fail_wmt = -1,
		.fail_enabled_call = -1, .set_before_fail = true,
	};
	struct mt6797_postpatch_state state = { };
	struct mt6797_postpatch_ops ops = mt6797_postpatch_test_ops(&context);

	KUNIT_EXPECT_EQ(test, mt6797_postpatch_postpatch_run(&ops, &state), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, context.restore_calls, 1U);
	KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
	KUNIT_EXPECT_EQ(test, context.voltage[false], 3500000);

	context = (struct mt6797_postpatch_test) {
		.voltage = { 3500000, 3500000 }, .fail_set = -1,
		.fail_enable = 0, .fail_disable = -1, .fail_wmt = -1,
		.fail_enabled_call = -1, .enable_before_fail = true,
	};
	state = (struct mt6797_postpatch_state) { };
	ops = mt6797_postpatch_test_ops(&context);
	KUNIT_EXPECT_EQ(test, mt6797_postpatch_postpatch_run(&ops, &state), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, context.enabled[false]);
	KUNIT_EXPECT_FALSE(test, context.enabled[true]);
	KUNIT_EXPECT_EQ(test, context.restore_calls, 1U);
	KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
}

static void mt6797_postpatch_test_rejects_unsafe_entry(struct kunit *test)
{
	struct mt6797_postpatch_test context = {
		.voltage = { 3300000, 3400000 }, .fail_set = -1,
		.fail_enable = -1, .fail_disable = -1, .fail_wmt = -1,
		.fail_enabled_call = -1,
	};
	struct mt6797_postpatch_state state = { };
	struct mt6797_postpatch_ops ops = mt6797_postpatch_test_ops(&context);

	KUNIT_EXPECT_EQ(test, mt6797_postpatch_postpatch_run(&ops, &state), -EUCLEAN);
	KUNIT_EXPECT_EQ(test, context.events, 0U);
	context.voltage[true] = 3300000;
	context.enabled[true] = true;
	KUNIT_EXPECT_EQ(test, mt6797_postpatch_postpatch_run(&ops, &state), -EBUSY);
	KUNIT_EXPECT_EQ(test, context.events, 0U);
}

static struct kunit_case mt6797_postpatch_postpatch_cases[] = {
	KUNIT_CASE(mt6797_postpatch_test_exact_order),
	KUNIT_CASE(mt6797_postpatch_test_every_wmt_failure_unwinds),
	KUNIT_CASE(mt6797_postpatch_test_setup_and_cleanup_failures),
	KUNIT_CASE(mt6797_postpatch_test_partial_mutations_unwind),
	KUNIT_CASE(mt6797_postpatch_test_rejects_unsafe_entry),
	{}
};

static struct kunit_suite mt6797_postpatch_postpatch_suite = {
	.name = "mt6797-consys-postpatch-postpatch",
	.test_cases = mt6797_postpatch_postpatch_cases,
};
kunit_test_suite(mt6797_postpatch_postpatch_suite);
#endif
