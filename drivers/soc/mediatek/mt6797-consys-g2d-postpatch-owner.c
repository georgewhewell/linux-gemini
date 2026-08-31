// SPDX-License-Identifier: GPL-2.0-only
/*
 * Gated MT6797 G2d post-patch initialisation and G2g GNSS owner.
 *
 * This stays inside the bootstrap's retained full-STP callback.  It completes
 * the vendor post-patch transactions, then leaves BTIF fenced and CONSYS
 * reset-held.  Its separately gated GNSS probe issues one bounded GPS
 * FUNC_CTRL on/off pair. A distinct G2g build, DT and command-line gate may
 * instead retain the promoted full-STP session and register one GNSS client.
 *
 * MT6797 uses fixed 3.3 V VCN33 PALDOs.  The old efuse opcode 0x12 sequence
 * was MT6580-only and must not be sent here.
 */

#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mt6797-consys-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-consys-g2d-postpatch-owner.h>
#include <linux/soc/mediatek/mt6797-consys-postid.h>
#include <linux/soc/mediatek/mt6797-consys-runtime-controller.h>
#include <linux/soc/mediatek/mt6797-consys-runtime.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb-w0-phase.h>
#include <linux/soc/mediatek/mtk-gnss.h>
#include <linux/soc/mediatek/mtk-wmt.h>

#define MT6797_G2D_TIMEOUT		msecs_to_jiffies(3000)
#define MT6797_G2D_VCN33_UV		3300000
#define MT6797_G2D_COEX_DELAY_MS	5

static bool enable_experimental_g2d_postpatch_owner;
module_param_named(enable_experimental_g2d_postpatch_owner,
			enable_experimental_g2d_postpatch_owner, bool, 0400);
MODULE_PARM_DESC(enable_experimental_g2d_postpatch_owner,
	"allow one MT6797 G2d fixed-3.3V post-patch experiment (default: false)");

static bool enable_experimental_g2d_gnss_probe;
module_param_named(enable_experimental_g2d_gnss_probe,
		enable_experimental_g2d_gnss_probe, bool, 0400);
MODULE_PARM_DESC(enable_experimental_g2d_gnss_probe,
	"allow one MT6797 G2d GPS FUNC_CTRL probe after post-patch (default: false)");

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_G2G_GNSS_ENDPOINT)
static bool enable_experimental_g2g_gnss_endpoint;
module_param_named(enable_experimental_g2g_gnss_endpoint,
		enable_experimental_g2g_gnss_endpoint, bool, 0400);
MODULE_PARM_DESC(enable_experimental_g2g_gnss_endpoint,
	"retain MT6797 full STP and register one GNSS endpoint (default: false)");
#endif

static bool enable_experimental_g2e_bt_function_owner;
module_param_named(enable_experimental_g2e_bt_function_owner,
		enable_experimental_g2e_bt_function_owner, bool, 0400);
MODULE_PARM_DESC(enable_experimental_g2e_bt_function_owner,
	"allow one MT6797 G2e Bluetooth PALDO/FUNC_CTRL proof (default: false)");

static bool enable_experimental_g2f_bt_hci_proof_owner;
module_param_named(enable_experimental_g2f_bt_hci_proof_owner,
		enable_experimental_g2f_bt_hci_proof_owner, bool, 0400);
MODULE_PARM_DESC(enable_experimental_g2f_bt_hci_proof_owner,
	"allow one MT6797 G2f private Bluetooth H4 proof (default: false)");

/* W4 is a client, not another G2d proof: leave READY for W4's own lifetime. */
static bool enable_experimental_wifi_w4_retained_owner;
module_param_named(enable_experimental_wifi_w4_retained_owner,
		enable_experimental_wifi_w4_retained_owner, bool, 0400);
MODULE_PARM_DESC(enable_experimental_wifi_w4_retained_owner,
	"allow MT6797 W4 to own the retained controller start/stop (default: false)");

enum mt6797_g2d_stage {
	G2D_SNAPSHOT, G2D_SET_BT, G2D_SET_WIFI, G2D_ENABLE_BT, G2D_ENABLE_WIFI,
	G2D_LTE_EXT, G2D_LTE_FILTER, G2D_LTE_FREQ, G2D_LTE_UNSAFE, G2D_LTE_L,
	G2D_CALIBRATE, G2D_DISABLE_BT, G2D_DISABLE_WIFI, G2D_VERIFY_OFF,
	G2D_RESTORE_SELECTOR, G2D_COEX, G2D_FM_STRAP,
	G2D_GNSS_LNA_INIT, G2D_GNSS_LNA_ON, G2D_GNSS_FUNC_ON,
	G2D_GNSS_LNA_OFF, G2D_GNSS_FUNC_OFF,
};

struct mt6797_g2d_ops {
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

struct mt6797_g2d_state {
	int original_uv;
	bool selector_dirty;
	bool bt_enabled;
	bool wifi_enabled;
	bool calibration_reply_valid;
	u8 calibration_reply[2];
	size_t calibration_event_len;
	int cleanup_error;
	enum mt6797_g2d_stage causal_stage;
	int causal_error;
	bool cleanup_attempted;
	enum mt6797_g2d_stage stage;
};

struct mt6797_g2_bt_proof_ops {
	int (*paldo)(void *context, bool enable);
	int (*wmt)(void *context, u8 opcode, const u8 *data, size_t data_len,
		   const struct mtk_wmt_match *expected);
	int (*hci_exchange)(void *context, const u8 *command, size_t command_len,
			    u16 expected_opcode, size_t expected_reply_len,
			    u8 *reply, size_t reply_size, size_t *reply_len);
	void *context;
};

enum mt6797_g2_bt_proof_stage {
	G2_BT_PALDO_ON,
	G2_BT_FUNC_ON,
	G2_BT_HCI_RESET,
	G2_BT_HCI_VERSION,
};

struct mt6797_g2_bt_proof_state {
	enum mt6797_g2_bt_proof_stage stage;
	enum mt6797_g2_bt_proof_stage causal_stage;
	int causal_error;
	int func_off_error;
	int paldo_off_error;
	bool cleanup_attempted;
};

static const char * const mt6797_g2_bt_stage_name[] = {
	[G2_BT_PALDO_ON] = "paldo-on",
	[G2_BT_FUNC_ON] = "func-on",
	[G2_BT_HCI_RESET] = "hci-reset",
	[G2_BT_HCI_VERSION] = "hci-version",
};

/* Exact raw SDUs from wmt_ic_soc.c's MT6797 post-patch tables. */
static const u8 g2d_lte_ext[] = { 0x0d, 0x00, 0x00 };
static const u8 g2d_lte_filter[] = {
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
static const u8 g2d_lte_freq[] = {
	0x12, 0xfc, 0x08, 0x15, 0x09, 0x2e, 0x09, 0x47,
	0x09, 0xc4, 0x09, 0xd4, 0x09, 0xe3, 0x09, 0x5a,
	0x0a, 0x14, 0x09, 0x2d, 0x09, 0x46, 0x09, 0x60,
	0x09, 0xd3, 0x09, 0xe2, 0x09, 0x59, 0x0a, 0x8b,
	0x0a,
};
static const u8 g2d_lte_unsafe[] = { 0x13, 0x00 };
static const u8 g2d_lte_l[] = { 0x21, 0x01 };
static const u8 g2d_calibrate[] = { 0x01 };
static const u8 g2d_coex[] = { 0x01, 0x01 };
static const u8 g2d_fm_strap[] = { 0x02, 0x02 };
static const u8 g2d_ok[] = { 0x00 };
static const u8 g2d_fm_strap_reply[] = { 0x00, 0x02 };

static const struct mtk_wmt_match g2d_ok_10 = {
	.opcode = 0x10, .data = g2d_ok, .data_len = sizeof(g2d_ok),
};
static const struct mtk_wmt_match g2d_fm_strap_match = {
	.opcode = 0x05, .data = g2d_fm_strap_reply,
	.data_len = sizeof(g2d_fm_strap_reply),
};
static const struct mtk_wmt_match g2_bt_func_match = {
	.opcode = 6, .data = g2d_ok,
	.data_len = sizeof(g2d_ok),
};

static const char * const mt6797_g2d_stage_name[] = {
	[G2D_SNAPSHOT] = "snapshot", [G2D_SET_BT] = "set-vcn33-bt",
	[G2D_SET_WIFI] = "set-vcn33-wifi", [G2D_ENABLE_BT] = "enable-vcn33-bt",
	[G2D_ENABLE_WIFI] = "enable-vcn33-wifi", [G2D_LTE_EXT] = "lte-ext",
	[G2D_LTE_FILTER] = "lte-filter", [G2D_LTE_FREQ] = "lte-frequency",
	[G2D_LTE_UNSAFE] = "lte-unsafe", [G2D_LTE_L] = "lte-l",
	[G2D_CALIBRATE] = "rf-calibration", [G2D_DISABLE_BT] = "disable-vcn33-bt",
	[G2D_DISABLE_WIFI] = "disable-vcn33-wifi", [G2D_VERIFY_OFF] = "verify-vcn33-off",
	[G2D_RESTORE_SELECTOR] = "restore-vcn33-selector", [G2D_COEX] = "coex-antenna",
	[G2D_FM_STRAP] = "fm-strap",
	[G2D_GNSS_LNA_INIT] = "gnss-lna-init", [G2D_GNSS_LNA_ON] = "gnss-lna-on",
	[G2D_GNSS_FUNC_ON] = "gnss-func-on", [G2D_GNSS_LNA_OFF] = "gnss-lna-off",
	[G2D_GNSS_FUNC_OFF] = "gnss-func-off",
};

static int mt6797_g2d_wmt(struct mt6797_g2d_ops *ops,
		struct mt6797_g2d_state *state, enum mt6797_g2d_stage stage,
		u8 opcode, const u8 *data, size_t data_len,
		const struct mtk_wmt_match *expected)
{
	state->stage = stage;
	return ops->wmt(ops->context, opcode, data, data_len, expected);
}

#define MT6797_G2D_WMT_FUNC_CTRL	6
#define MT6797_G2D_WMT_GNSS_TYPE	2
static const u8 g2d_gnss_on[] = { MT6797_G2D_WMT_GNSS_TYPE, 1 };
static const u8 g2d_gnss_off[] = { MT6797_G2D_WMT_GNSS_TYPE, 0 };
static const struct mtk_wmt_match g2d_gnss_func_match = {
	.opcode = MT6797_G2D_WMT_FUNC_CTRL, .data = g2d_ok,
	.data_len = sizeof(g2d_ok),
};
struct mt6797_g2d_gnss_ops {
	int (*lna_init)(void *context);
	int (*lna_set)(void *context, bool enable);
	int (*wmt)(void *context, u8 opcode, const u8 *data, size_t data_len,
		   const struct mtk_wmt_match *expected);
	void *context;
};
static int mt6797_g2d_gnss_fail_low(struct mt6797_g2d_gnss_ops *ops,
		struct mt6797_g2d_state *state, enum mt6797_g2d_stage stage, int error)
{
	state->causal_stage = stage;
	state->causal_error = error;
	state->cleanup_attempted = true;
	state->cleanup_error = ops->lna_set(ops->context, false);
	state->stage = stage;
	return error;
}
static int mt6797_g2d_gnss_probe_run(struct mt6797_g2d_gnss_ops *ops,
		struct mt6797_g2d_state *state)
{
	int ret, cleanup;

	if (!ops || !state || !ops->lna_init || !ops->lna_set || !ops->wmt)
		return -EINVAL;
	state->stage = G2D_GNSS_LNA_INIT;
	ret = ops->lna_init(ops->context);
	if (ret)
		return mt6797_g2d_gnss_fail_low(ops, state, state->stage, ret);
	state->stage = G2D_GNSS_LNA_ON;
	ret = ops->lna_set(ops->context, true);
	if (ret)
		return mt6797_g2d_gnss_fail_low(ops, state, state->stage, ret);
	state->stage = G2D_GNSS_FUNC_ON;
	ret = ops->wmt(ops->context, MT6797_G2D_WMT_FUNC_CTRL, g2d_gnss_on,
		       sizeof(g2d_gnss_on), &g2d_gnss_func_match);
	if (ret)
		return mt6797_g2d_gnss_fail_low(ops, state, state->stage, ret);
	state->stage = G2D_GNSS_LNA_OFF;
	ret = ops->lna_set(ops->context, false);
	if (ret)
		return mt6797_g2d_gnss_fail_low(ops, state, state->stage, ret);
	state->stage = G2D_GNSS_FUNC_OFF;
	ret = ops->wmt(ops->context, MT6797_G2D_WMT_FUNC_CTRL, g2d_gnss_off,
		       sizeof(g2d_gnss_off), &g2d_gnss_func_match);
	return ret ? mt6797_g2d_gnss_fail_low(ops, state, state->stage, ret) : 0;
}

/* Vendor wmt_func_bt_on/off: PALDO, FUNC_CTRL { BT, 1/0 }, PALDO. */
static int mt6797_g2_bt_proof_run(struct mt6797_g2_bt_proof_ops *ops,
				  bool hci_proof,
				  struct mt6797_g2_bt_proof_state *state)
{
	static const u8 func_on[] = { 0x00, 0x01 };
	static const u8 func_off[] = { 0x00, 0x00 };
	static const u8 hci_reset[] = { 0x01, 0x03, 0x0c, 0x00 };
	static const u8 hci_version[] = { 0x01, 0x01, 0x10, 0x00 };
	u8 reply[15];
	size_t reply_len;
	bool paldo_dirty = false, function_dirty = false;
	int ret;

	if (!ops || !state || !ops->paldo || !ops->wmt ||
	    (hci_proof && !ops->hci_exchange))
		return -EINVAL;
	memset(state, 0, sizeof(*state));
	/* A failed operation may still have changed the hardware. */
	state->stage = G2_BT_PALDO_ON;
	paldo_dirty = true;
	ret = ops->paldo(ops->context, true);
	if (ret)
		goto out;
	state->stage = G2_BT_FUNC_ON;
	function_dirty = true;
	ret = ops->wmt(ops->context, 6, func_on, sizeof(func_on),
		       &g2_bt_func_match);
	if (ret)
		goto out;
	if (hci_proof) {
		state->stage = G2_BT_HCI_RESET;
		ret = ops->hci_exchange(ops->context, hci_reset, sizeof(hci_reset),
					0x0c03, 7, reply, sizeof(reply), &reply_len);
	}
	if (!ret && hci_proof) {
		state->stage = G2_BT_HCI_VERSION;
		ret = ops->hci_exchange(ops->context, hci_version, sizeof(hci_version),
					0x1001, 15, reply, sizeof(reply), &reply_len);
	}
out:
	if (ret) {
		state->causal_stage = state->stage;
		state->causal_error = ret;
	}
	if (function_dirty) {
		state->cleanup_attempted = true;
		cleanup = ops->wmt(ops->context, 6, func_off, sizeof(func_off),
				   &g2_bt_func_match);
		if (cleanup)
			state->func_off_error = cleanup;
	}
	if (paldo_dirty) {
		state->cleanup_attempted = true;
		cleanup = ops->paldo(ops->context, false);
		if (cleanup)
			state->paldo_off_error = cleanup;
	}
	if (!ret)
		ret = state->func_off_error ?: state->paldo_off_error;
	return ret;
}

static int mt6797_g2d_calibrate(struct mt6797_g2d_ops *ops,
		struct mt6797_g2d_state *state)
{
	int ret;

	state->stage = G2D_CALIBRATE;
	ret = ops->wmt_capture(ops->context, 0x14, g2d_calibrate,
			sizeof(g2d_calibrate), 0x14,
			sizeof(state->calibration_reply), MTK_WMT_MAX_DATA,
			state->calibration_reply, sizeof(state->calibration_reply),
			&state->calibration_event_len);
	if (!ret)
		state->calibration_reply_valid = true;
	return ret;
}

static int mt6797_g2d_snapshot(struct mt6797_g2d_ops *ops,
		struct mt6797_g2d_state *state)
{
	int bt_uv, wifi_uv, ret;

	state->stage = G2D_SNAPSHOT;
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
static int mt6797_g2d_cleanup(struct mt6797_g2d_ops *ops,
		struct mt6797_g2d_state *state)
{
	int ret = 0, tmp, bt_on, wifi_on;

	if (state->bt_enabled) {
		state->stage = G2D_DISABLE_BT;
		tmp = ops->disable(ops->context, false);
		if (tmp)
			ret = tmp;
		else
			state->bt_enabled = false;
	}
	if (state->wifi_enabled) {
		state->stage = G2D_DISABLE_WIFI;
		tmp = ops->disable(ops->context, true);
		if (!ret && tmp)
			ret = tmp;
		else if (!tmp)
			state->wifi_enabled = false;
	}
	if (!state->selector_dirty)
		return ret;

	state->stage = G2D_VERIFY_OFF;
	bt_on = ops->is_enabled(ops->context, false);
	wifi_on = ops->is_enabled(ops->context, true);
	if (ret || bt_on || wifi_on)
		return ret ?: (bt_on < 0 ? bt_on : (wifi_on < 0 ? wifi_on : -EBUSY));

	state->stage = G2D_RESTORE_SELECTOR;
	tmp = ops->set_voltage(ops->context, false, state->original_uv);
	if (tmp)
		return tmp;
	state->selector_dirty = false;
	return 0;
}

static int mt6797_g2d_enable(struct mt6797_g2d_ops *ops,
		struct mt6797_g2d_state *state, bool wifi)
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

static int mt6797_g2d_postpatch_run(struct mt6797_g2d_ops *ops,
		struct mt6797_g2d_state *state)
{
	enum mt6797_g2d_stage stage_before_cleanup;
	int ret, cleanup_ret;

	if (!ops || !state || !ops->get_voltage || !ops->is_enabled ||
	    !ops->set_voltage || !ops->enable || !ops->disable || !ops->wmt ||
	    !ops->wmt_capture ||
	    !ops->delay_ms)
		return -EINVAL;
	ret = mt6797_g2d_snapshot(ops, state);
	if (ret)
		return ret;
	/* Vendor wmt_stp_wifi_lte_coex() waits after G2c's clock-down. */
	ops->delay_ms(ops->context, MT6797_G2D_COEX_DELAY_MS);
	ret = mt6797_g2d_wmt(ops, state, G2D_LTE_EXT, 0x10,
		g2d_lte_ext, sizeof(g2d_lte_ext), &g2d_ok_10);
	if (ret)
		goto out_cleanup;
	ret = mt6797_g2d_wmt(ops, state, G2D_LTE_FILTER, 0x10,
		g2d_lte_filter, sizeof(g2d_lte_filter), &g2d_ok_10);
	if (ret)
		goto out_cleanup;
	ret = mt6797_g2d_wmt(ops, state, G2D_LTE_FREQ, 0x10,
		g2d_lte_freq, sizeof(g2d_lte_freq), &g2d_ok_10);
	if (ret)
		goto out_cleanup;
	ret = mt6797_g2d_wmt(ops, state, G2D_LTE_UNSAFE, 0x10,
		g2d_lte_unsafe, sizeof(g2d_lte_unsafe), &g2d_ok_10);
	if (ret)
		goto out_cleanup;
	ret = mt6797_g2d_wmt(ops, state, G2D_LTE_L, 0x10,
		g2d_lte_l, sizeof(g2d_lte_l), &g2d_ok_10);
	if (ret)
		goto out_cleanup;
	state->stage = G2D_SET_BT;
	/* set_voltage() can change the shared selector before reporting failure. */
	state->selector_dirty = true;
	ret = ops->set_voltage(ops->context, false, MT6797_G2D_VCN33_UV);
	if (ret)
		goto out_cleanup;
	state->stage = G2D_SET_WIFI;
	ret = ops->set_voltage(ops->context, true, MT6797_G2D_VCN33_UV);
	if (ret)
		goto out_cleanup;
	state->stage = G2D_ENABLE_BT;
	ret = mt6797_g2d_enable(ops, state, false);
	if (ret)
		goto out_cleanup;
	state->stage = G2D_ENABLE_WIFI;
	ret = mt6797_g2d_enable(ops, state, true);
	if (ret)
		goto out_cleanup;

	ret = mt6797_g2d_calibrate(ops, state);
out_cleanup:
	stage_before_cleanup = state->stage;
	cleanup_ret = mt6797_g2d_cleanup(ops, state);
	state->cleanup_error = cleanup_ret;
	/* Preserve the causal stage in logs when cleanup itself succeeds. */
	if (ret)
		state->stage = stage_before_cleanup;
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		return ret;
	ret = mt6797_g2d_wmt(ops, state, G2D_COEX, 0x10,
		g2d_coex, sizeof(g2d_coex), &g2d_ok_10);
	if (ret)
		return ret;
	return mt6797_g2d_wmt(ops, state, G2D_FM_STRAP, 0x05,
		g2d_fm_strap, sizeof(g2d_fm_strap), &g2d_fm_strap_match);
}

struct mt6797_g2d_owner {
	struct device *dev;
	struct device *runtime;
	struct device *controller;
	struct regulator *bt;
	struct regulator *wifi;
	struct pinctrl *pinctrl;
	struct pinctrl_state *gps_lna_init;
	struct pinctrl_state *gps_lna_on;
	struct pinctrl_state *gps_lna_off;
	struct mt6797_consys_runtime_resource_lease resources;
	struct mt6797_consys_runtime_provider_lease identity;
	struct mt6797_consys_postid_owner postid;
	struct mt6797_consys_runtime_controller_gnss_lease gnss_lease;
	struct mtk_gnss gnss;
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease;
	struct mutex w4_lock;
	bool wifi_w0_dirty_fatal;
	bool gnss_probe;
	bool persistent_gnss;
	bool persistent_gnss_registered;
	bool persistent_gnss_faulted;
	atomic64_t persistent_gnss_power_ons;
	atomic64_t persistent_gnss_power_offs;
	atomic64_t persistent_gnss_failures;
	atomic_t persistent_gnss_last_error;
	bool bt_function_proof;
	bool bt_hci_proof;
	bool wifi_w4_retained_owner;
	bool w4_resources_held;
	bool w4_identity_held;
	bool w4_postid_active;
};

static int mt6797_g2d_w4_release_locked(struct mt6797_g2d_owner *owner)
{
	int ret;

	if (!owner->w4_resources_held && !owner->w4_identity_held &&
	    !owner->w4_postid_active)
		return 0;
	if (!owner->w4_resources_held || !owner->w4_identity_held ||
	    !owner->w4_postid_active)
		return -EIO;
	ret = mt6797_consys_postid_stop(&owner->postid);
	if (ret)
		return ret;
	owner->w4_postid_active = false;
	ret = mt6797_consys_runtime_platform_identity_release(&owner->identity);
	if (ret)
		return ret;
	owner->w4_identity_held = false;
	ret = mt6797_consys_runtime_resource_lease_release(&owner->resources);
	if (ret)
		return ret;
	owner->w4_resources_held = false;
	put_device(owner->controller);
	owner->controller = NULL;
	put_device(owner->runtime);
	owner->runtime = NULL;
	return 0;
}

int mt6797_consys_g2d_postpatch_owner_w4_validate(struct device *dev)
{
	struct mt6797_g2d_owner *owner;
	int ret = 0;

	if (!dev)
		return -EINVAL;
	owner = dev_get_drvdata(dev);
	if (!owner)
		return -EPROBE_DEFER;
	mutex_lock(&owner->w4_lock);
	if (!owner->wifi_w4_retained_owner || !owner->w4_resources_held ||
	    !owner->w4_identity_held || !owner->w4_postid_active)
		ret = -ESHUTDOWN;
	mutex_unlock(&owner->w4_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_g2d_postpatch_owner_w4_validate);

int mt6797_consys_g2d_postpatch_owner_w4_release(struct device *dev)
{
	struct mt6797_g2d_owner *owner;
	int ret;

	if (!dev)
		return -EINVAL;
	owner = dev_get_drvdata(dev);
	if (!owner)
		return -ENODEV;
	mutex_lock(&owner->w4_lock);
	ret = owner->wifi_w4_retained_owner ?
		mt6797_g2d_w4_release_locked(owner) : -EPERM;
	mutex_unlock(&owner->w4_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_g2d_postpatch_owner_w4_release);

static struct regulator *mt6797_g2d_regulator(struct mt6797_g2d_owner *owner,
		bool wifi)
{
	return wifi ? owner->wifi : owner->bt;
}

static int mt6797_g2d_live_get_voltage(void *context, bool wifi)
{
	return regulator_get_voltage(mt6797_g2d_regulator(context, wifi));
}

static int mt6797_g2d_live_is_enabled(void *context, bool wifi)
{
	return regulator_is_enabled(mt6797_g2d_regulator(context, wifi));
}

static int mt6797_g2d_live_set_voltage(void *context, bool wifi, int uv)
{
	return regulator_set_voltage(mt6797_g2d_regulator(context, wifi), uv, uv);
}

static int mt6797_g2d_live_enable(void *context, bool wifi)
{
	return regulator_enable(mt6797_g2d_regulator(context, wifi));
}

static int mt6797_g2d_live_disable(void *context, bool wifi)
{
	return regulator_disable(mt6797_g2d_regulator(context, wifi));
}

static int mt6797_g2d_live_wmt(void *context, u8 opcode, const u8 *data,
		size_t data_len, const struct mtk_wmt_match *expected)
{
	struct mt6797_g2d_owner *owner = context;

	return mt6797_btif_stp_bootstrap_full_wmt_transaction(owner->lease, opcode,
		data, data_len, expected, MT6797_G2D_TIMEOUT);
}

static int mt6797_g2d_live_wmt_capture(void *context, u8 opcode,
		const u8 *data, size_t data_len, u8 expected_opcode,
		size_t min_data_len, size_t max_data_len, u8 *reply,
		size_t reply_len, size_t *actual_data_len)
{
	struct mt6797_g2d_owner *owner = context;

	return mt6797_btif_stp_bootstrap_full_wmt_transaction_capture_range(
		owner->lease, opcode, data, data_len, expected_opcode,
		min_data_len, max_data_len, reply, reply_len, actual_data_len,
		MT6797_G2D_TIMEOUT);
}

static int mt6797_g2d_live_paldo(void *context, bool enable)
{
	struct mt6797_g2d_owner *owner = context;

	return enable ? regulator_enable(owner->bt) : regulator_disable(owner->bt);
}

static int mt6797_g2d_live_hci_exchange(void *context, const u8 *command,
		size_t command_len, u16 expected_opcode, size_t expected_reply_len,
		u8 *reply, size_t reply_size, size_t *reply_len)
{
	struct mt6797_g2d_owner *owner = context;

	return mt6797_btif_stp_bootstrap_full_bt_hci_exchange(owner->lease,
		command, command_len, expected_opcode, expected_reply_len, reply,
		reply_size, reply_len, MT6797_G2D_TIMEOUT);
}

static void mt6797_g2d_live_delay_ms(void *context, unsigned int milliseconds)
{
	msleep(milliseconds);
}

static int mt6797_g2d_live_lna_init(void *context)
{
	struct mt6797_g2d_owner *owner = context;

	return pinctrl_select_state(owner->pinctrl, owner->gps_lna_init);
}

static int mt6797_g2d_live_lna_set(void *context, bool enable)
{
	struct mt6797_g2d_owner *owner = context;

	return pinctrl_select_state(owner->pinctrl,
		enable ? owner->gps_lna_on : owner->gps_lna_off);
}

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_G2G_GNSS_ENDPOINT)
static void mt6797_g2g_gnss_record_power(struct mt6797_g2d_owner *owner,
		bool enable, int error)
{
	if (error)
		atomic64_inc(&owner->persistent_gnss_failures);
	else if (enable)
		atomic64_inc(&owner->persistent_gnss_power_ons);
	else
		atomic64_inc(&owner->persistent_gnss_power_offs);
	atomic_set(&owner->persistent_gnss_last_error, error);
}

static int mt6797_g2g_gnss_set_power(void *context, bool enable)
{
	struct mt6797_g2d_owner *owner = context;
	int cleanup;
	int ret;

	if (READ_ONCE(owner->persistent_gnss_faulted)) {
		mt6797_g2g_gnss_record_power(owner, enable, -EIO);
		return -EIO;
	}

	if (enable) {
		ret = mt6797_g2d_live_lna_set(owner, true);
		if (ret)
			goto fault_and_lower_lna;
		ret = mt6797_consys_runtime_controller_gnss_set_power(
			owner->controller, &owner->gnss_lease, true);
		if (!ret) {
			mt6797_g2g_gnss_record_power(owner, true, 0);
			return 0;
		}
fault_and_lower_lna:
		cleanup = mt6797_g2d_live_lna_set(owner, false);
		WRITE_ONCE(owner->persistent_gnss_faulted, true);
		dev_err(owner->dev,
			"persistent GNSS-on failed=%d LNA-low cleanup=%d; reboot required\n",
			ret, cleanup);
		ret = ret ?: cleanup ?: -EIO;
		mt6797_g2g_gnss_record_power(owner, true, ret);
		return ret;
	}

	/* The hardware-passed shutdown order lowers the external LNA first. */
	ret = mt6797_g2d_live_lna_set(owner, false);
	cleanup = mt6797_consys_runtime_controller_gnss_set_power(
		owner->controller, &owner->gnss_lease, false);
	if (ret || cleanup) {
		WRITE_ONCE(owner->persistent_gnss_faulted, true);
		dev_err(owner->dev,
			"persistent GNSS-off failed: LNA-low=%d FUNC_CTRL-off=%d; reboot required\n",
			ret, cleanup);
	}
	ret = ret ?: cleanup;
	mt6797_g2g_gnss_record_power(owner, false, ret);
	return ret;
}

static ssize_t persistent_gnss_power_ons_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mt6797_g2d_owner *owner = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lld\n",
		atomic64_read(&owner->persistent_gnss_power_ons));
}
static DEVICE_ATTR_RO(persistent_gnss_power_ons);

static ssize_t persistent_gnss_power_offs_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mt6797_g2d_owner *owner = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lld\n",
		atomic64_read(&owner->persistent_gnss_power_offs));
}
static DEVICE_ATTR_RO(persistent_gnss_power_offs);

static ssize_t persistent_gnss_failures_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mt6797_g2d_owner *owner = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lld\n",
		atomic64_read(&owner->persistent_gnss_failures));
}
static DEVICE_ATTR_RO(persistent_gnss_failures);

static ssize_t persistent_gnss_last_error_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mt6797_g2d_owner *owner = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n",
		atomic_read(&owner->persistent_gnss_last_error));
}
static DEVICE_ATTR_RO(persistent_gnss_last_error);

static struct attribute *mt6797_g2g_gnss_attrs[] = {
	&dev_attr_persistent_gnss_power_ons.attr,
	&dev_attr_persistent_gnss_power_offs.attr,
	&dev_attr_persistent_gnss_failures.attr,
	&dev_attr_persistent_gnss_last_error.attr,
	NULL,
};
static const struct attribute_group mt6797_g2g_gnss_group = {
	.attrs = mt6797_g2g_gnss_attrs,
};

int mt6797_consys_g2d_postpatch_owner_gnss_attach(struct device *dev)
{
	struct mt6797_g2d_owner *owner;
	const struct mt6797_consys_stp_session_lease *session_lease;
	struct mtk_gnss_transport transport = {
		.set_power = mt6797_g2g_gnss_set_power,
	};
	int ret;

	if (!dev)
		return -EINVAL;
	owner = dev_get_drvdata(dev);
	if (!owner)
		return -ENODEV;
	mutex_lock(&owner->w4_lock);
	if (!owner->persistent_gnss) {
		ret = 0;
		goto unlock;
	}
	if (!owner->wifi_w4_retained_owner || !owner->controller ||
	    !owner->w4_resources_held || !owner->w4_identity_held ||
	    !owner->w4_postid_active) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (owner->persistent_gnss_registered) {
		ret = 0;
		goto unlock;
	}
	ret = mt6797_consys_runtime_controller_gnss_session_get(owner->controller,
		&owner->gnss_lease, &session_lease);
	if (ret)
		goto unlock;
	ret = mt6797_g2d_live_lna_init(owner);
	if (ret)
		goto put_lease;
	transport.context = owner;
	ret = mtk_gnss_register(&owner->gnss, owner->dev, session_lease,
		&transport);
	if (ret)
		goto put_lease;
	owner->persistent_gnss_registered = true;
	dev_info(owner->dev, "G2g attached /dev/gnss0 to W4 full-STP session\n");
	goto unlock;

put_lease:
	mt6797_consys_runtime_controller_gnss_session_put(&owner->gnss_lease);
unlock:
	mutex_unlock(&owner->w4_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_g2d_postpatch_owner_gnss_attach);

void mt6797_consys_g2d_postpatch_owner_gnss_detach(struct device *dev)
{
	struct mt6797_g2d_owner *owner;

	if (!dev)
		return;
	owner = dev_get_drvdata(dev);
	if (!owner)
		return;
	mutex_lock(&owner->w4_lock);
	if (owner->persistent_gnss_registered) {
		mtk_gnss_unregister(&owner->gnss);
		mt6797_consys_runtime_controller_gnss_session_put(&owner->gnss_lease);
		owner->persistent_gnss_registered = false;
	}
	mutex_unlock(&owner->w4_lock);
}
EXPORT_SYMBOL_GPL(mt6797_consys_g2d_postpatch_owner_gnss_detach);
#else
int mt6797_consys_g2d_postpatch_owner_gnss_attach(struct device *dev)
{
	return dev ? 0 : -EINVAL;
}
EXPORT_SYMBOL_GPL(mt6797_consys_g2d_postpatch_owner_gnss_attach);

void mt6797_consys_g2d_postpatch_owner_gnss_detach(struct device *dev)
{
}
EXPORT_SYMBOL_GPL(mt6797_consys_g2d_postpatch_owner_gnss_detach);
#endif

#if IS_REACHABLE(CONFIG_MTK_MT6797_WIFI_AHB_WCIR_OBSERVE)
/* The W0 PALDO interval is independent of the completed G2d rail pair. */
static int mt6797_g2d_w0_get_voltage(void *context)
{
	return regulator_get_voltage(((struct mt6797_g2d_owner *)context)->wifi);
}

static int mt6797_g2d_w0_is_enabled(void *context, bool wifi)
{
	return regulator_is_enabled(mt6797_g2d_regulator(context, wifi));
}

static int mt6797_g2d_w0_set_voltage(void *context, int uv)
{
	return regulator_set_voltage(((struct mt6797_g2d_owner *)context)->wifi,
				     uv, uv);
}

static int mt6797_g2d_w0_enable(void *context)
{
	return regulator_enable(((struct mt6797_g2d_owner *)context)->wifi);
}

static int mt6797_g2d_w0_disable(void *context)
{
	return regulator_disable(((struct mt6797_g2d_owner *)context)->wifi);
}

static int mt6797_g2d_w0_observe(void *context)
{
	struct mt6797_g2d_owner *owner = context;
	struct mt6797_wifi_ahb_wcir wcir;

	return mt6797_wifi_ahb_wcir_observe(owner->dev, owner->lease,
					    owner->wifi, &wcir);
}

static void mt6797_g2d_w0_fatal(void *context, int causal, int cleanup)
{
	struct mt6797_g2d_owner *owner = context;

	owner->wifi_w0_dirty_fatal = true;
	dev_err(owner->dev, "Wi-Fi W0 state dirty (causal=%d cleanup=%d); reboot required\n",
		causal, cleanup);
}

static int mt6797_g2d_wifi_wcir_phase(struct mt6797_g2d_owner *owner,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	struct mt6797_wifi_ahb_w0_ops ops = {
		.get_voltage = mt6797_g2d_w0_get_voltage,
		.is_enabled = mt6797_g2d_w0_is_enabled,
		.set_voltage = mt6797_g2d_w0_set_voltage,
		.enable = mt6797_g2d_w0_enable,
		.disable = mt6797_g2d_w0_disable,
		.observe = mt6797_g2d_w0_observe,
		.fatal = mt6797_g2d_w0_fatal,
		.context = owner,
	};
	struct mt6797_wifi_ahb_w0_result result;

	if (lease != owner->lease)
		return -EPERM;
	return mt6797_wifi_ahb_w0_run(&ops, &result, MT6797_G2D_VCN33_UV);
}
#endif

static int mt6797_g2d_postpatch_stage(void *context,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	struct mt6797_g2d_owner *owner = context;
	struct mt6797_g2d_ops ops = {
		.get_voltage = mt6797_g2d_live_get_voltage,
		.is_enabled = mt6797_g2d_live_is_enabled,
		.set_voltage = mt6797_g2d_live_set_voltage,
		.enable = mt6797_g2d_live_enable,
		.disable = mt6797_g2d_live_disable,
		.wmt = mt6797_g2d_live_wmt,
		.wmt_capture = mt6797_g2d_live_wmt_capture,
		.delay_ms = mt6797_g2d_live_delay_ms,
		.context = owner,
	};
	struct mt6797_g2d_state state = { };
	struct mt6797_g2d_gnss_ops gnss_ops = {
		.lna_init = mt6797_g2d_live_lna_init,
		.lna_set = mt6797_g2d_live_lna_set,
		.wmt = mt6797_g2d_live_wmt,
		.context = owner,
	};
	struct mt6797_g2_bt_proof_ops bt_ops = {
		.paldo = mt6797_g2d_live_paldo,
		.wmt = mt6797_g2d_live_wmt,
		.hci_exchange = mt6797_g2d_live_hci_exchange,
		.context = owner,
	};
	bool wcir_attempted = false, w1_attempted = false;
	struct mt6797_g2_bt_proof_state bt_state = { };
	int ret;

	owner->lease = lease;
	dev_info(owner->dev, "G2d start: fixed 3.3V%s\n",
		owner->bt_hci_proof ? ", then G2f private H4 proof" :
		(owner->bt_function_proof ? ", then G2e function proof" :
		 (owner->gnss_probe ? ", then GNSS FUNC_CTRL probe" :
		  (owner->persistent_gnss ? ", then retain GNSS transport" :
		   ", no function control"))));
	ret = mt6797_g2d_postpatch_run(&ops, &state);
#if IS_REACHABLE(CONFIG_MTK_MT6797_WIFI_AHB_WCIR_OBSERVE)
	if (!ret && owner->persistent_gnss &&
	    mt6797_wifi_ahb_wcir_enabled(owner->dev)) {
		dev_err(owner->dev,
			"persistent GNSS conflicts with the one-shot Wi-Fi phase\n");
		ret = -EBUSY;
	} else if (!ret && mt6797_wifi_ahb_wcir_enabled(owner->dev)) {
		wcir_attempted = true;
		ret = mt6797_g2d_wifi_wcir_phase(owner, lease);
	}
#endif
#if IS_REACHABLE(CONFIG_MTK_MT6797_WIFI_AHB_W1_FIRMWARE_PREFLIGHT)
	if (!ret && mt6797_wifi_ahb_w1_firmware_enabled(owner->dev)) {
		w1_attempted = true;
		/* W1 is a successor to the corrected live W0 portal observation. */
		if (!mt6797_wifi_ahb_wcir_enabled(owner->dev))
			ret = -EPERM;
		else
			ret = mt6797_wifi_ahb_w1_firmware_preflight(owner->dev, lease,
				owner->bt, owner->wifi);
	}
#endif
	if (!ret && owner->gnss_probe)
		ret = mt6797_g2d_gnss_probe_run(&gnss_ops, &state);
	if (!ret && (owner->bt_function_proof || owner->bt_hci_proof))
		ret = mt6797_g2_bt_proof_run(&bt_ops, owner->bt_hci_proof,
					     &bt_state);
	owner->lease = NULL;
	if (ret && w1_attempted)
		dev_err(owner->dev,
			"Wi-Fi W1 firmware preflight failed: %d; bootstrap must fence/reset\n",
			ret);
	else if (ret && wcir_attempted)
		dev_err(owner->dev,
			"G2d Wi-Fi WCIR phase failed: %d; bootstrap must fence/reset\n", ret);
	else if (ret && owner->bt_hci_proof && bt_state.causal_error)
		dev_err(owner->dev,
			"G2f causal stage %s failed: %d; FUNC_CTRL-off cleanup: %d; PALDO-off cleanup: %d; bootstrap must fence/reset\n",
			mt6797_g2_bt_stage_name[bt_state.causal_stage],
			bt_state.causal_error, bt_state.func_off_error,
			bt_state.paldo_off_error);
	else if (ret && owner->bt_function_proof && bt_state.causal_error)
		dev_err(owner->dev,
			"G2e causal stage %s failed: %d; FUNC_CTRL-off cleanup: %d; PALDO-off cleanup: %d; bootstrap must fence/reset\n",
			mt6797_g2_bt_stage_name[bt_state.causal_stage],
			bt_state.causal_error, bt_state.func_off_error,
			bt_state.paldo_off_error);
	else if (ret && owner->bt_hci_proof)
		dev_err(owner->dev,
			"G2f cleanup failed: FUNC_CTRL-off=%d PALDO-off=%d; bootstrap must fence/reset\n",
			bt_state.func_off_error, bt_state.paldo_off_error);
	else if (ret && owner->bt_function_proof)
		dev_err(owner->dev,
			"G2e cleanup failed: FUNC_CTRL-off=%d PALDO-off=%d; bootstrap must fence/reset\n",
			bt_state.func_off_error, bt_state.paldo_off_error);
	else if (ret && state.causal_error && state.cleanup_attempted)
		dev_err(owner->dev,
			"G2d causal stage %s failed: %d; GNSS LNA-low cleanup: %d; bootstrap must fence/reset\n",
			mt6797_g2d_stage_name[state.causal_stage], state.causal_error,
			state.cleanup_error);
	else if (ret)
		dev_err(owner->dev,
			"G2d stage %s failed: %d cleanup=%d; bootstrap must fence/reset\n",
			mt6797_g2d_stage_name[state.stage], ret, state.cleanup_error);
	else if (owner->bt_hci_proof)
		dev_info(owner->dev,
			 "G2f private H4 proof passed; bootstrap will fence/reset\n");
	else if (owner->bt_function_proof)
		dev_info(owner->dev,
			 "G2e BT function proof passed; bootstrap will fence/reset\n");
	else if (w1_attempted)
		dev_info(owner->dev,
			 "Wi-Fi W1 HIF/EMI/start/READY passed; IRQ/netdev remain fenced; bootstrap will fence/reset\n");
	else if (owner->gnss_probe)
		dev_info(owner->dev,
			 "G2d GNSS probe passed: GPIO69 LNA init/on/off and GPS FUNC_CTRL on/off acknowledged; bootstrap will fence/reset\n");
	else if (owner->persistent_gnss)
		dev_info(owner->dev,
			 "G2g post-patch stage passed; requesting retained full-STP GNSS ownership\n");
	else
		dev_info(owner->dev, "G2d passed; bootstrap will fence/reset\n");
	if (state.calibration_reply_valid)
		dev_info(owner->dev, "G2d RF calibration event: len=%zu prefix=%*ph\n",
			 state.calibration_event_len,
			 (int)sizeof(state.calibration_reply), state.calibration_reply);
	return ret;
}

static int mt6797_g2d_supplier(struct device *dev, const char *prop,
		struct device **out)
{
	struct device_node *node = of_parse_phandle(dev->of_node, prop, 0);
	struct platform_device *pdev;

	if (!node)
		return -EINVAL;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return -EPROBE_DEFER;
	*out = &pdev->dev;
	return 0;
}

static int mt6797_g2d_probe_failure(struct device *dev, int err,
		const char *stage)
{
	return dev_err_probe(dev, err, "G2d probe %s failed\n", stage);
}

static int mt6797_g2d_postpatch_owner_probe(struct platform_device *pdev)
{
	struct mt6797_g2d_owner *owner;
	bool postid_init = false, controller_active = false, emi_claimed = false;
	int ret, cleanup;

	owner = devm_kzalloc(&pdev->dev, sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return mt6797_g2d_probe_failure(&pdev->dev, -ENOMEM, "owner allocation");
	owner->dev = &pdev->dev;
	mutex_init(&owner->w4_lock);
	atomic64_set(&owner->persistent_gnss_power_ons, 0);
	atomic64_set(&owner->persistent_gnss_power_offs, 0);
	atomic64_set(&owner->persistent_gnss_failures, 0);
	atomic_set(&owner->persistent_gnss_last_error, 0);
	platform_set_drvdata(pdev, owner);
	owner->bt_function_proof = device_property_read_bool(&pdev->dev,
		"mediatek,experimental-g2e-bt-function-owner") &&
		READ_ONCE(enable_experimental_g2e_bt_function_owner);
	owner->bt_hci_proof = device_property_read_bool(&pdev->dev,
		"mediatek,experimental-g2f-bt-hci-proof-owner") &&
		READ_ONCE(enable_experimental_g2f_bt_hci_proof_owner);
	owner->wifi_w4_retained_owner = device_property_read_bool(&pdev->dev,
		"mediatek,experimental-wifi-w4-retained-owner") &&
		READ_ONCE(enable_experimental_wifi_w4_retained_owner);
	if (owner->bt_function_proof && owner->bt_hci_proof)
		return mt6797_g2d_probe_failure(&pdev->dev, -EINVAL,
			"multiple Bluetooth proof gates");
	if (!owner->bt_function_proof && !owner->bt_hci_proof &&
	    !owner->wifi_w4_retained_owner &&
	    (!READ_ONCE(enable_experimental_g2d_postpatch_owner) ||
	     !device_property_read_bool(&pdev->dev,
		"mediatek,experimental-g2d-postpatch-owner")))
		return mt6797_g2d_probe_failure(&pdev->dev, -EPERM, "opt-in gate");
	if (device_property_read_bool(&pdev->dev,
		"mediatek,experimental-g2g-gnss-endpoint")) {
#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_G2G_GNSS_ENDPOINT)
		if (!READ_ONCE(enable_experimental_g2g_gnss_endpoint))
			return mt6797_g2d_probe_failure(&pdev->dev, -EPERM,
				"persistent GNSS opt-in gate");
		owner->persistent_gnss = true;
#else
		return mt6797_g2d_probe_failure(&pdev->dev, -EOPNOTSUPP,
			"persistent GNSS build gate");
#endif
	}
	owner->gnss_probe = device_property_read_bool(&pdev->dev,
		"mediatek,experimental-g2d-gnss-probe") &&
		READ_ONCE(enable_experimental_g2d_gnss_probe);
	if (owner->persistent_gnss &&
	    (owner->gnss_probe || owner->bt_function_proof || owner->bt_hci_proof))
		return mt6797_g2d_probe_failure(&pdev->dev, -EINVAL,
			"persistent GNSS mixed with one-shot proof");
	if (owner->persistent_gnss && !owner->wifi_w4_retained_owner)
		return mt6797_g2d_probe_failure(&pdev->dev, -EINVAL,
			"persistent GNSS requires W4 retained owner");
	if (owner->gnss_probe || owner->persistent_gnss) {
		owner->pinctrl = devm_pinctrl_get(&pdev->dev);
		if (IS_ERR(owner->pinctrl))
			return mt6797_g2d_probe_failure(&pdev->dev, PTR_ERR(owner->pinctrl),
				"GNSS pinctrl");
		owner->gps_lna_init = pinctrl_lookup_state(owner->pinctrl, "gps-lna-init");
		owner->gps_lna_on = pinctrl_lookup_state(owner->pinctrl, "gps-lna-on");
		owner->gps_lna_off = pinctrl_lookup_state(owner->pinctrl, "gps-lna-off");
		if (IS_ERR(owner->gps_lna_init) || IS_ERR(owner->gps_lna_on) ||
		    IS_ERR(owner->gps_lna_off))
			return mt6797_g2d_probe_failure(&pdev->dev, -EINVAL,
				"GNSS LNA pinctrl state");
		if (owner->persistent_gnss) {
			ret = devm_device_add_group(&pdev->dev,
				&mt6797_g2g_gnss_group);
			if (ret)
				return mt6797_g2d_probe_failure(&pdev->dev, ret,
					"persistent GNSS diagnostics");
		}
	}
	owner->bt = devm_regulator_get(&pdev->dev, "vcn33-bt");
	if (IS_ERR(owner->bt))
		return mt6797_g2d_probe_failure(&pdev->dev, PTR_ERR(owner->bt),
			"vcn33-bt supply");
	owner->wifi = devm_regulator_get(&pdev->dev, "vcn33-wifi");
	if (IS_ERR(owner->wifi))
		return mt6797_g2d_probe_failure(&pdev->dev, PTR_ERR(owner->wifi),
			"vcn33-wifi supply");
	/*
	 * This compares backing regulator devices, not consumer handles.  MT6351
	 * gives BT/Wi-Fi distinct enable registers despite their shared selector;
	 * accepting one rdev for both would make the separate PALDO unwind unsafe.
	 */
	if (regulator_is_equal(owner->bt, owner->wifi))
		return mt6797_g2d_probe_failure(&pdev->dev, -EINVAL,
			"distinct VCN33 regulator check");
	ret = mt6797_g2d_supplier(&pdev->dev, "mediatek,runtime-provider",
		&owner->runtime);
	if (ret)
		return mt6797_g2d_probe_failure(&pdev->dev, ret, "runtime provider");
	ret = mt6797_g2d_supplier(&pdev->dev, "mediatek,controller",
		&owner->controller);
	if (ret) {
		ret = mt6797_g2d_probe_failure(&pdev->dev, ret, "runtime controller");
		goto put_runtime;
	}
	ret = mt6797_consys_runtime_resource_lease_acquire(owner->runtime,
		&owner->resources);
	if (ret == -ENODEV)
		ret = -EPROBE_DEFER;
	if (ret) {
		ret = mt6797_g2d_probe_failure(&pdev->dev, ret, "runtime resource lease");
		goto put_controller;
	}
	ret = mt6797_consys_runtime_controller_preflight(owner->controller);
	if (ret) {
		ret = mt6797_g2d_probe_failure(&pdev->dev, ret, "controller preflight");
		goto resources;
	}
	ret = mt6797_consys_emi_mpu_claim(&pdev->dev);
	if (ret) {
		ret = mt6797_g2d_probe_failure(&pdev->dev, ret, "persistent EMI claim");
		goto resources;
	}
	emi_claimed = true;
	ret = mt6797_consys_runtime_platform_identity_acquire(&owner->resources,
		&owner->identity);
	if (ret) {
		ret = mt6797_g2d_probe_failure(&pdev->dev, ret, "platform identity");
		goto resources;
	}
	ret = mt6797_consys_runtime_postid_owner_init(owner->runtime,
		owner->identity.identity, &owner->postid);
	if (ret) {
		ret = mt6797_g2d_probe_failure(&pdev->dev, ret, "post-ID owner init");
		goto identity;
	}
	postid_init = true;
	ret = mt6797_consys_postid_start(&owner->postid,
		owner->identity.identity, true);
	if (ret) {
		ret = mt6797_g2d_probe_failure(&pdev->dev, ret, "post-ID start");
		goto postid;
	}
	if (owner->wifi_w4_retained_owner) {
		owner->w4_resources_held = true;
		owner->w4_identity_held = true;
		owner->w4_postid_active = true;
		dev_info(&pdev->dev,
			"G2d W4 retained handoff: post-ID identity held; controller READY for W4\n");
		return 0;
	}
	ret = mt6797_consys_runtime_controller_start_with_postpatch_stage(
		owner->controller,
		mt6797_g2d_postpatch_stage, owner);
	if (ret)
		ret = mt6797_g2d_probe_failure(&pdev->dev, ret,
			"controller start or post-patch stage");
	if (!ret)
		controller_active = true;
postid:
	if (owner->gnss_lease.controller)
		mt6797_consys_runtime_controller_gnss_session_put(&owner->gnss_lease);
	if (controller_active)
		mt6797_consys_runtime_controller_stop(owner->controller);
	if (postid_init && mt6797_consys_postid_state(&owner->postid) ==
	    MT6797_CONSYS_POSTID_ACTIVE) {
		cleanup = mt6797_consys_postid_stop(&owner->postid);
		if (!ret)
			ret = cleanup;
	}
	if (postid_init && mt6797_consys_postid_state(&owner->postid) !=
	    MT6797_CONSYS_POSTID_IDLE) {
		mt6797_consys_emi_mpu_latch_fatal();
		if (!ret)
			ret = -EIO;
	}
identity:
	cleanup = mt6797_consys_runtime_platform_identity_release(&owner->identity);
	if (!ret)
		ret = cleanup;
resources:
	cleanup = mt6797_consys_runtime_resource_lease_release(&owner->resources);
	if (!ret)
		ret = cleanup;
	if (emi_claimed && ret) {
		mt6797_consys_emi_mpu_latch_fatal();
		dev_err(&pdev->dev, "G2d failed after persistent EMI claim; reboot required\n");
	}
put_controller:
	put_device(owner->controller);
put_runtime:
	put_device(owner->runtime);
	return ret;
}

static void mt6797_g2d_postpatch_owner_remove(struct platform_device *pdev)
{
	struct mt6797_g2d_owner *owner = platform_get_drvdata(pdev);
	int ret;

	if (!owner)
		return;
	if (!owner->wifi_w4_retained_owner)
		return;
	mt6797_consys_g2d_postpatch_owner_gnss_detach(&pdev->dev);
	if (owner->controller)
		mt6797_consys_runtime_controller_stop(owner->controller);
	ret = mt6797_consys_g2d_postpatch_owner_w4_release(&pdev->dev);
	if (ret) {
		mt6797_consys_emi_mpu_latch_fatal();
		dev_err(&pdev->dev, "G2d W4 retained handoff teardown failed: %d\n", ret);
	}
}

static const struct of_device_id mt6797_g2d_postpatch_owner_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys-g2d-postpatch-owner" },
	{ .compatible = "mediatek,mt6797-consys-g2e-bt-function-owner" },
	{ .compatible = "mediatek,mt6797-consys-g2f-bt-hci-proof-owner" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_g2d_postpatch_owner_of_match);

static struct platform_driver mt6797_g2d_postpatch_owner_driver = {
	.probe = mt6797_g2d_postpatch_owner_probe,
	.remove = mt6797_g2d_postpatch_owner_remove,
	.driver = {
		.name = "mt6797-consys-g2d-postpatch-owner",
		.of_match_table = mt6797_g2d_postpatch_owner_of_match,
		/* EMI ownership is persistent: no runtime unbind/retry. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_g2d_postpatch_owner_driver);

MODULE_DESCRIPTION("Gated MT6797 CONSYS G2d post-patch and G2g GNSS owner");
MODULE_LICENSE("GPL");

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_WMT_POSTPATCH_KUNIT_TEST)
#include <kunit/test.h>

enum mt6797_g2d_test_event {
	G2D_TEST_DELAY, G2D_TEST_LTE_EXT, G2D_TEST_LTE_FILTER,
	G2D_TEST_LTE_FREQ, G2D_TEST_LTE_UNSAFE, G2D_TEST_LTE_L,
	G2D_TEST_SET_BT, G2D_TEST_SET_WIFI, G2D_TEST_ENABLE_BT,
	G2D_TEST_ENABLE_WIFI, G2D_TEST_CALIBRATE, G2D_TEST_DISABLE_BT,
	G2D_TEST_DISABLE_WIFI, G2D_TEST_RESTORE, G2D_TEST_COEX,
	G2D_TEST_FM_STRAP,
};

struct mt6797_g2d_test_command {
	u8 opcode;
	const u8 *data;
	size_t data_len;
	const struct mtk_wmt_match *expected;
};

struct mt6797_g2d_test {
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
	enum mt6797_g2d_test_event event[24];
	unsigned int events;
	struct mt6797_g2d_test_command command[8];
};

struct mt6797_g2_bt_test {
	bool paldo_on_before_fail;
	bool func_on_before_fail;
	bool paldo_enabled;
	bool function_enabled;
	int paldo_on_error;
	int paldo_off_error;
	int func_on_error;
	int func_off_error;
	int hci_error;
	unsigned int paldo_on_calls;
	unsigned int paldo_off_calls;
	unsigned int func_on_calls;
	unsigned int func_off_calls;
};

static int mt6797_g2_bt_test_paldo(void *context, bool enable)
{
	struct mt6797_g2_bt_test *test = context;

	if (enable) {
		test->paldo_on_calls++;
		if (test->paldo_on_before_fail)
			test->paldo_enabled = true;
		if (test->paldo_on_error)
			return test->paldo_on_error;
		test->paldo_enabled = true;
		return 0;
	}
	test->paldo_off_calls++;
	test->paldo_enabled = false;
	return test->paldo_off_error;
}

static int mt6797_g2_bt_test_wmt(void *context, u8 opcode, const u8 *data,
				  size_t data_len,
				  const struct mtk_wmt_match *expected)
{
	struct mt6797_g2_bt_test *test = context;

	if (opcode != MT6797_G2D_WMT_FUNC_CTRL || data_len != 2 ||
	    data[0] != 0 || !expected)
		return -EPROTO;
	if (data[1]) {
		test->func_on_calls++;
		if (test->func_on_before_fail)
			test->function_enabled = true;
		if (test->func_on_error)
			return test->func_on_error;
		test->function_enabled = true;
		return 0;
	}
	test->func_off_calls++;
	test->function_enabled = false;
	return test->func_off_error;
}

static int mt6797_g2_bt_test_hci_exchange(void *context, const u8 *command,
					    size_t command_len, u16 expected_opcode,
					    size_t expected_reply_len, u8 *reply,
					    size_t reply_size, size_t *reply_len)
{
	struct mt6797_g2_bt_test *test = context;

	if (!command || command_len != 4 || expected_opcode != 0x0c03 ||
	    expected_reply_len != 7 || !reply || reply_size < 7 || !reply_len)
		return -EPROTO;
	return test->hci_error;
}

static struct mt6797_g2_bt_proof_ops
mt6797_g2_bt_test_ops(struct mt6797_g2_bt_test *test)
{
	return (struct mt6797_g2_bt_proof_ops) {
		.paldo = mt6797_g2_bt_test_paldo,
		.wmt = mt6797_g2_bt_test_wmt,
		.hci_exchange = mt6797_g2_bt_test_hci_exchange,
		.context = test,
	};
}

static void mt6797_g2d_test_event(struct mt6797_g2d_test *test,
		enum mt6797_g2d_test_event event)
{
	if (test->events < ARRAY_SIZE(test->event))
		test->event[test->events++] = event;
}

static int mt6797_g2d_test_get_voltage(void *context, bool wifi)
{
	struct mt6797_g2d_test *test = context;

	return test->voltage[wifi];
}

static int mt6797_g2d_test_is_enabled(void *context, bool wifi)
{
	struct mt6797_g2d_test *test = context;

	if (test->fail_enabled_call >= 0 &&
	    test->enabled_calls++ == test->fail_enabled_call)
		return -ETIMEDOUT;
	return test->enabled[wifi];
}

static int mt6797_g2d_test_set_voltage(void *context, bool wifi, int uv)
{
	struct mt6797_g2d_test *test = context;

	if (uv == MT6797_G2D_VCN33_UV) {
		mt6797_g2d_test_event(test, wifi ? G2D_TEST_SET_WIFI : G2D_TEST_SET_BT);
		if (test->fail_set == wifi) {
			if (test->set_before_fail) {
				test->voltage[false] = uv;
				test->voltage[true] = uv;
			}
			return -ETIMEDOUT;
		}
	} else {
		mt6797_g2d_test_event(test, G2D_TEST_RESTORE);
		test->restore_calls++;
		test->restore_while_enabled = test->enabled[false] || test->enabled[true];
	}
	test->voltage[false] = uv;
	test->voltage[true] = uv;
	return 0;
}

static int mt6797_g2d_test_enable(void *context, bool wifi)
{
	struct mt6797_g2d_test *test = context;

	mt6797_g2d_test_event(test, wifi ? G2D_TEST_ENABLE_WIFI : G2D_TEST_ENABLE_BT);
	if (test->fail_enable == wifi) {
		if (test->enable_before_fail)
			test->enabled[wifi] = true;
		return -ETIMEDOUT;
	}
	test->enabled[wifi] = true;
	return 0;
}

static int mt6797_g2d_test_disable(void *context, bool wifi)
{
	struct mt6797_g2d_test *test = context;

	mt6797_g2d_test_event(test, wifi ? G2D_TEST_DISABLE_WIFI : G2D_TEST_DISABLE_BT);
	if (test->fail_disable == wifi)
		return -ETIMEDOUT;
	test->enabled[wifi] = false;
	return 0;
}

static int mt6797_g2d_test_wmt(void *context, u8 opcode, const u8 *data,
		size_t data_len, const struct mtk_wmt_match *expected)
{
	struct mt6797_g2d_test *test = context;
	unsigned int call = test->wmt_calls++;

	if (!test->delay_calls)
		test->wmt_before_delay = true;
	if (data == g2d_lte_ext)
		mt6797_g2d_test_event(test, G2D_TEST_LTE_EXT);
	else if (data == g2d_lte_filter)
		mt6797_g2d_test_event(test, G2D_TEST_LTE_FILTER);
	else if (data == g2d_lte_freq)
		mt6797_g2d_test_event(test, G2D_TEST_LTE_FREQ);
	else if (data == g2d_lte_unsafe)
		mt6797_g2d_test_event(test, G2D_TEST_LTE_UNSAFE);
	else if (data == g2d_lte_l)
		mt6797_g2d_test_event(test, G2D_TEST_LTE_L);
	else if (data == g2d_calibrate)
		mt6797_g2d_test_event(test, G2D_TEST_CALIBRATE);
	else if (data == g2d_coex)
		mt6797_g2d_test_event(test, G2D_TEST_COEX);
	else if (data == g2d_fm_strap)
		mt6797_g2d_test_event(test, G2D_TEST_FM_STRAP);
	if (call < ARRAY_SIZE(test->command))
		test->command[call] = (struct mt6797_g2d_test_command) {
			.opcode = opcode, .data = data, .data_len = data_len,
			.expected = expected,
		};
	if (test->fail_wmt >= 0 && call == test->fail_wmt)
		return -ETIMEDOUT;
	return 0;
}

static int mt6797_g2d_test_wmt_capture(void *context, u8 opcode,
		const u8 *data, size_t data_len, u8 expected_opcode,
		size_t min_data_len, size_t max_data_len, u8 *reply,
		size_t reply_len, size_t *actual_data_len)
{
	struct mt6797_g2d_test *test = context;
	unsigned int call = test->wmt_calls++;
	size_t event_len = test->calibration_event_len ?:
			   sizeof(test->calibration_reply);

	if (opcode != 0x14 || data != g2d_calibrate ||
	    data_len != sizeof(g2d_calibrate) || expected_opcode != 0x14 ||
	    min_data_len != sizeof(test->calibration_reply) ||
	    max_data_len != MTK_WMT_MAX_DATA ||
	    reply_len != sizeof(test->calibration_reply) || !actual_data_len ||
	    event_len < min_data_len || event_len > max_data_len)
		return -EPROTO;
	mt6797_g2d_test_event(test, G2D_TEST_CALIBRATE);
	if (call < ARRAY_SIZE(test->command))
		test->command[call] = (struct mt6797_g2d_test_command) {
			.opcode = opcode, .data = data, .data_len = data_len,
			.expected = NULL,
		};
	if (test->fail_wmt >= 0 && call == test->fail_wmt)
		return -ETIMEDOUT;
	memcpy(reply, test->calibration_reply, reply_len);
	*actual_data_len = event_len;
	return 0;
}

static void mt6797_g2d_test_delay(void *context, unsigned int milliseconds)
{
	struct mt6797_g2d_test *test = context;

	test->delay_calls++;
	test->delay_ms = milliseconds;
	mt6797_g2d_test_event(test, G2D_TEST_DELAY);
}

static struct mt6797_g2d_ops mt6797_g2d_test_ops(struct mt6797_g2d_test *test)
{
	return (struct mt6797_g2d_ops) {
		.get_voltage = mt6797_g2d_test_get_voltage,
		.is_enabled = mt6797_g2d_test_is_enabled,
		.set_voltage = mt6797_g2d_test_set_voltage,
		.enable = mt6797_g2d_test_enable,
		.disable = mt6797_g2d_test_disable,
		.wmt = mt6797_g2d_test_wmt,
		.wmt_capture = mt6797_g2d_test_wmt_capture,
		.delay_ms = mt6797_g2d_test_delay,
		.context = test,
	};
}

static void mt6797_g2d_test_expect_command(struct kunit *test,
		const struct mt6797_g2d_test_command *command, u8 opcode,
		const u8 *data, size_t data_len, const struct mtk_wmt_match *expected)
{
	KUNIT_EXPECT_EQ(test, command->opcode, opcode);
	KUNIT_EXPECT_EQ(test, command->data_len, data_len);
	KUNIT_EXPECT_PTR_EQ(test, command->expected, expected);
	KUNIT_EXPECT_MEMEQ(test, command->data, data, data_len);
}

static void mt6797_g2d_test_exact_order(struct kunit *test)
{
	struct mt6797_g2d_test context = {
		.voltage = { 3500000, 3500000 },
		.fail_set = -1, .fail_enable = -1, .fail_disable = -1,
		.fail_wmt = -1, .fail_enabled_call = -1,
		.calibration_reply = { 0x7a, 0xc3 },
		.calibration_event_len = 378,
	};
	struct mt6797_g2d_state state = { };
	struct mt6797_g2d_ops ops = mt6797_g2d_test_ops(&context);

	KUNIT_ASSERT_EQ(test, mt6797_g2d_postpatch_run(&ops, &state), 0);
	KUNIT_EXPECT_EQ(test, context.delay_calls, 1U);
	KUNIT_EXPECT_EQ(test, context.delay_ms, MT6797_G2D_COEX_DELAY_MS);
	KUNIT_EXPECT_FALSE(test, context.wmt_before_delay);
	KUNIT_EXPECT_EQ(test, context.wmt_calls, 8U);
	mt6797_g2d_test_expect_command(test, &context.command[0], 0x10,
		g2d_lte_ext, sizeof(g2d_lte_ext), &g2d_ok_10);
	mt6797_g2d_test_expect_command(test, &context.command[1], 0x10,
		g2d_lte_filter, sizeof(g2d_lte_filter), &g2d_ok_10);
	mt6797_g2d_test_expect_command(test, &context.command[2], 0x10,
		g2d_lte_freq, sizeof(g2d_lte_freq), &g2d_ok_10);
	mt6797_g2d_test_expect_command(test, &context.command[3], 0x10,
		g2d_lte_unsafe, sizeof(g2d_lte_unsafe), &g2d_ok_10);
	mt6797_g2d_test_expect_command(test, &context.command[4], 0x10,
		g2d_lte_l, sizeof(g2d_lte_l), &g2d_ok_10);
	mt6797_g2d_test_expect_command(test, &context.command[5], 0x14,
		g2d_calibrate, sizeof(g2d_calibrate), NULL);
	KUNIT_EXPECT_TRUE(test, state.calibration_reply_valid);
	KUNIT_EXPECT_EQ(test, state.calibration_event_len, (size_t)378);
	KUNIT_EXPECT_MEMEQ(test, state.calibration_reply, context.calibration_reply,
			   sizeof(state.calibration_reply));
	mt6797_g2d_test_expect_command(test, &context.command[6], 0x10,
		g2d_coex, sizeof(g2d_coex), &g2d_ok_10);
	mt6797_g2d_test_expect_command(test, &context.command[7], 0x05,
		g2d_fm_strap, sizeof(g2d_fm_strap), &g2d_fm_strap_match);
	KUNIT_EXPECT_EQ(test, context.events, 16U);
	KUNIT_EXPECT_EQ(test, context.event[0], G2D_TEST_DELAY);
	KUNIT_EXPECT_EQ(test, context.event[1], G2D_TEST_LTE_EXT);
	KUNIT_EXPECT_EQ(test, context.event[2], G2D_TEST_LTE_FILTER);
	KUNIT_EXPECT_EQ(test, context.event[3], G2D_TEST_LTE_FREQ);
	KUNIT_EXPECT_EQ(test, context.event[4], G2D_TEST_LTE_UNSAFE);
	KUNIT_EXPECT_EQ(test, context.event[5], G2D_TEST_LTE_L);
	KUNIT_EXPECT_EQ(test, context.event[6], G2D_TEST_SET_BT);
	KUNIT_EXPECT_EQ(test, context.event[7], G2D_TEST_SET_WIFI);
	KUNIT_EXPECT_EQ(test, context.event[8], G2D_TEST_ENABLE_BT);
	KUNIT_EXPECT_EQ(test, context.event[9], G2D_TEST_ENABLE_WIFI);
	KUNIT_EXPECT_EQ(test, context.event[10], G2D_TEST_CALIBRATE);
	KUNIT_EXPECT_EQ(test, context.event[11], G2D_TEST_DISABLE_BT);
	KUNIT_EXPECT_EQ(test, context.event[12], G2D_TEST_DISABLE_WIFI);
	KUNIT_EXPECT_EQ(test, context.event[13], G2D_TEST_RESTORE);
	KUNIT_EXPECT_EQ(test, context.event[14], G2D_TEST_COEX);
	KUNIT_EXPECT_EQ(test, context.event[15], G2D_TEST_FM_STRAP);
	KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
}

static void mt6797_g2d_test_every_wmt_failure_unwinds(struct kunit *test)
{
	int fail;

	for (fail = 0; fail < 8; fail++) {
		struct mt6797_g2d_test context = {
			.voltage = { 3500000, 3500000 }, .fail_set = -1,
			.fail_enable = -1, .fail_disable = -1, .fail_wmt = fail,
			.fail_enabled_call = -1,
		};
		struct mt6797_g2d_state state = { };
		struct mt6797_g2d_ops ops = mt6797_g2d_test_ops(&context);

		KUNIT_EXPECT_EQ_MSG(test, mt6797_g2d_postpatch_run(&ops, &state),
			-ETIMEDOUT, "WMT command %d", fail);
		KUNIT_EXPECT_FALSE(test, context.enabled[false]);
		KUNIT_EXPECT_FALSE(test, context.enabled[true]);
		KUNIT_EXPECT_EQ(test, context.restore_calls, fail < 5 ? 0U : 1U);
		KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
	}
}

static void mt6797_g2d_test_setup_and_cleanup_failures(struct kunit *test)
{
	int fail_set, fail_enable, fail_disable;

	for (fail_set = 0; fail_set < 2; fail_set++) {
		struct mt6797_g2d_test context = {
			.voltage = { 3500000, 3500000 }, .fail_set = fail_set,
			.fail_enable = -1, .fail_disable = -1, .fail_wmt = -1,
			.fail_enabled_call = -1,
		};
		struct mt6797_g2d_state state = { };
		struct mt6797_g2d_ops ops = mt6797_g2d_test_ops(&context);

		KUNIT_EXPECT_EQ(test, mt6797_g2d_postpatch_run(&ops, &state), -ETIMEDOUT);
		KUNIT_EXPECT_FALSE(test, context.enabled[false]);
		KUNIT_EXPECT_FALSE(test, context.enabled[true]);
		KUNIT_EXPECT_EQ(test, context.restore_calls, 1U);
	}
	for (fail_enable = 0; fail_enable < 2; fail_enable++) {
		struct mt6797_g2d_test context = {
			.voltage = { 3500000, 3500000 }, .fail_set = -1,
			.fail_enable = fail_enable, .fail_disable = -1, .fail_wmt = -1,
			.fail_enabled_call = -1,
		};
		struct mt6797_g2d_state state = { };
		struct mt6797_g2d_ops ops = mt6797_g2d_test_ops(&context);

		KUNIT_EXPECT_EQ(test, mt6797_g2d_postpatch_run(&ops, &state), -ETIMEDOUT);
		KUNIT_EXPECT_FALSE(test, context.enabled[false]);
		KUNIT_EXPECT_FALSE(test, context.enabled[true]);
		KUNIT_EXPECT_EQ(test, context.restore_calls, 1U);
	}
	for (fail_disable = 0; fail_disable < 2; fail_disable++) {
		struct mt6797_g2d_test context = {
			.voltage = { 3500000, 3500000 }, .fail_set = -1,
			.fail_enable = -1, .fail_disable = fail_disable, .fail_wmt = 5,
			.fail_enabled_call = -1,
		};
		struct mt6797_g2d_state state = { };
		struct mt6797_g2d_ops ops = mt6797_g2d_test_ops(&context);

		KUNIT_EXPECT_EQ(test, mt6797_g2d_postpatch_run(&ops, &state), -ETIMEDOUT);
		KUNIT_EXPECT_EQ(test, context.restore_calls, 0U);
		KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
		KUNIT_EXPECT_EQ(test, context.event[11], G2D_TEST_DISABLE_BT);
		KUNIT_EXPECT_EQ(test, context.event[12], G2D_TEST_DISABLE_WIFI);
	}
	for (fail_disable = 2; fail_disable < 4; fail_disable++) {
		struct mt6797_g2d_test context = {
			.voltage = { 3500000, 3500000 }, .fail_set = -1,
			.fail_enable = -1, .fail_disable = -1, .fail_wmt = 5,
			.fail_enabled_call = fail_disable,
		};
		struct mt6797_g2d_state state = { };
		struct mt6797_g2d_ops ops = mt6797_g2d_test_ops(&context);

		KUNIT_EXPECT_EQ(test, mt6797_g2d_postpatch_run(&ops, &state), -ETIMEDOUT);
		KUNIT_EXPECT_EQ(test, context.restore_calls, 0U);
		KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
	}
}

static void mt6797_g2d_test_partial_mutations_unwind(struct kunit *test)
{
	struct mt6797_g2d_test context = {
		.voltage = { 3500000, 3500000 }, .fail_set = 0,
		.fail_enable = -1, .fail_disable = -1, .fail_wmt = -1,
		.fail_enabled_call = -1, .set_before_fail = true,
	};
	struct mt6797_g2d_state state = { };
	struct mt6797_g2d_ops ops = mt6797_g2d_test_ops(&context);

	KUNIT_EXPECT_EQ(test, mt6797_g2d_postpatch_run(&ops, &state), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, context.restore_calls, 1U);
	KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
	KUNIT_EXPECT_EQ(test, context.voltage[false], 3500000);

	context = (struct mt6797_g2d_test) {
		.voltage = { 3500000, 3500000 }, .fail_set = -1,
		.fail_enable = 0, .fail_disable = -1, .fail_wmt = -1,
		.fail_enabled_call = -1, .enable_before_fail = true,
	};
	state = (struct mt6797_g2d_state) { };
	ops = mt6797_g2d_test_ops(&context);
	KUNIT_EXPECT_EQ(test, mt6797_g2d_postpatch_run(&ops, &state), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, context.enabled[false]);
	KUNIT_EXPECT_FALSE(test, context.enabled[true]);
	KUNIT_EXPECT_EQ(test, context.restore_calls, 1U);
	KUNIT_EXPECT_FALSE(test, context.restore_while_enabled);
}

static void mt6797_g2d_test_rejects_unsafe_entry(struct kunit *test)
{
	struct mt6797_g2d_test context = {
		.voltage = { 3300000, 3400000 }, .fail_set = -1,
		.fail_enable = -1, .fail_disable = -1, .fail_wmt = -1,
		.fail_enabled_call = -1,
	};
	struct mt6797_g2d_state state = { };
	struct mt6797_g2d_ops ops = mt6797_g2d_test_ops(&context);

	KUNIT_EXPECT_EQ(test, mt6797_g2d_postpatch_run(&ops, &state), -EUCLEAN);
	KUNIT_EXPECT_EQ(test, context.events, 0U);
	context.voltage[true] = 3300000;
	context.enabled[true] = true;
	KUNIT_EXPECT_EQ(test, mt6797_g2d_postpatch_run(&ops, &state), -EBUSY);
	KUNIT_EXPECT_EQ(test, context.events, 0U);
}

static void mt6797_g2_bt_test_partial_on_failures_unwind(struct kunit *test)
{
	struct mt6797_g2_bt_test context = {
		.paldo_on_before_fail = true,
		.paldo_on_error = -ETIMEDOUT,
	};
	struct mt6797_g2_bt_proof_state state;
	struct mt6797_g2_bt_proof_ops ops = mt6797_g2_bt_test_ops(&context);

	KUNIT_EXPECT_EQ(test, mt6797_g2_bt_proof_run(&ops, false, &state),
			-ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, context.paldo_enabled);
	KUNIT_EXPECT_EQ(test, context.paldo_on_calls, 1U);
	KUNIT_EXPECT_EQ(test, context.paldo_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, context.func_on_calls, 0U);
	KUNIT_EXPECT_EQ(test, context.func_off_calls, 0U);
	KUNIT_EXPECT_EQ(test, state.causal_stage, G2_BT_PALDO_ON);
	KUNIT_EXPECT_EQ(test, state.causal_error, -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, state.cleanup_attempted);
	KUNIT_EXPECT_EQ(test, state.paldo_off_error, 0);

	context = (struct mt6797_g2_bt_test) {
		.func_on_before_fail = true,
		.func_on_error = -ETIMEDOUT,
	};
	ops = mt6797_g2_bt_test_ops(&context);
	KUNIT_EXPECT_EQ(test, mt6797_g2_bt_proof_run(&ops, false, &state),
			-ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, context.paldo_enabled);
	KUNIT_EXPECT_FALSE(test, context.function_enabled);
	KUNIT_EXPECT_EQ(test, context.paldo_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, context.func_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, state.causal_stage, G2_BT_FUNC_ON);
	KUNIT_EXPECT_EQ(test, state.causal_error, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, state.func_off_error, 0);
	KUNIT_EXPECT_EQ(test, state.paldo_off_error, 0);
}

static void mt6797_g2_bt_test_causal_and_cleanup_errors(struct kunit *test)
{
	struct mt6797_g2_bt_test context = {
		.hci_error = -EIO,
		.func_off_error = -ENOSPC,
		.paldo_off_error = -EUCLEAN,
	};
	struct mt6797_g2_bt_proof_state state;
	struct mt6797_g2_bt_proof_ops ops = mt6797_g2_bt_test_ops(&context);

	KUNIT_EXPECT_EQ(test, mt6797_g2_bt_proof_run(&ops, true, &state), -EIO);
	KUNIT_EXPECT_FALSE(test, context.paldo_enabled);
	KUNIT_EXPECT_FALSE(test, context.function_enabled);
	KUNIT_EXPECT_EQ(test, context.func_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, context.paldo_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, state.causal_stage, G2_BT_HCI_RESET);
	KUNIT_EXPECT_EQ(test, state.causal_error, -EIO);
	KUNIT_EXPECT_EQ(test, state.func_off_error, -ENOSPC);
	KUNIT_EXPECT_EQ(test, state.paldo_off_error, -EUCLEAN);
	KUNIT_EXPECT_TRUE(test, state.cleanup_attempted);
}

static struct kunit_case mt6797_g2d_postpatch_cases[] = {
	KUNIT_CASE(mt6797_g2d_test_exact_order),
	KUNIT_CASE(mt6797_g2d_test_every_wmt_failure_unwinds),
	KUNIT_CASE(mt6797_g2d_test_setup_and_cleanup_failures),
	KUNIT_CASE(mt6797_g2d_test_partial_mutations_unwind),
	KUNIT_CASE(mt6797_g2d_test_rejects_unsafe_entry),
	KUNIT_CASE(mt6797_g2_bt_test_partial_on_failures_unwind),
	KUNIT_CASE(mt6797_g2_bt_test_causal_and_cleanup_errors),
	{}
};

static struct kunit_suite mt6797_g2d_postpatch_suite = {
	.name = "mt6797-consys-g2d-postpatch",
	.test_cases = mt6797_g2d_postpatch_cases,
};
kunit_test_suite(mt6797_g2d_postpatch_suite);
#endif
