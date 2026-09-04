// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 common WMT post-patch initialization and VCN33 transaction. */

#include <linux/errno.h>
#include <linux/module.h>

#include <linux/soc/mediatek/mt6797-consys-postpatch.h>
#include <linux/soc/mediatek/mtk-wmt.h>

#define MT6797_CONSYS_VCN33_UV		3300000
#define MT6797_CONSYS_COEX_DELAY_MS	5

static const u8 mt6797_lte_ext[] = { 0x0d, 0x00, 0x00 };
static const u8 mt6797_lte_filter[] = {
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

static const u8 mt6797_lte_frequency[] = {
	0x12, 0xfc, 0x08, 0x15, 0x09, 0x2e, 0x09, 0x47,
	0x09, 0xc4, 0x09, 0xd4, 0x09, 0xe3, 0x09, 0x5a,
	0x0a, 0x14, 0x09, 0x2d, 0x09, 0x46, 0x09, 0x60,
	0x09, 0xd3, 0x09, 0xe2, 0x09, 0x59, 0x0a, 0x8b,
	0x0a,
};

static const u8 mt6797_lte_unsafe[] = { 0x13, 0x00 };
static const u8 mt6797_lte_l[] = { 0x21, 0x01 };
static const u8 mt6797_calibrate[] = { 0x01 };
static const u8 mt6797_coex[] = { 0x01, 0x01 };
static const u8 mt6797_fm_strap[] = { 0x02, 0x02 };
static const u8 mt6797_ok[] = { 0x00 };
static const u8 mt6797_fm_reply[] = { 0x00, 0x02 };

static const struct mtk_wmt_match mt6797_coex_reply = {
	.opcode = 0x10,
	.data = mt6797_ok,
	.data_len = sizeof(mt6797_ok),
};

static const struct mtk_wmt_match mt6797_fm_strap_reply = {
	.opcode = 0x05,
	.data = mt6797_fm_reply,
	.data_len = sizeof(mt6797_fm_reply),
};

struct mt6797_consys_postpatch_state {
	int original_uv;
	bool selector_dirty;
	bool bt_enabled;
	bool wifi_enabled;
};

static int
mt6797_consys_postpatch_wmt(const struct mt6797_consys_postpatch_ops *ops,
			    const u8 *data, size_t data_len)
{
	return ops->wmt(ops->context, 0x10, data, data_len,
			&mt6797_coex_reply);
}

static int
mt6797_consys_postpatch_enable(const struct mt6797_consys_postpatch_ops *ops,
			       struct mt6797_consys_postpatch_state *state,
			       bool wifi)
{
	bool *enabled = wifi ? &state->wifi_enabled : &state->bt_enabled;
	int on;
	int ret;

	ret = ops->enable(ops->context, wifi);
	if (!ret) {
		*enabled = true;
		return 0;
	}
	on = ops->is_enabled(ops->context, wifi);
	if (on > 0)
		*enabled = true;
	return ret;
}

static int
mt6797_consys_postpatch_cleanup(const struct mt6797_consys_postpatch_ops *ops,
				struct mt6797_consys_postpatch_state *state)
{
	int bt_on;
	int wifi_on;
	int ret = 0;
	int tmp;

	if (state->bt_enabled) {
		tmp = ops->disable(ops->context, false);
		if (tmp)
			ret = tmp;
		else
			state->bt_enabled = false;
	}
	if (state->wifi_enabled) {
		tmp = ops->disable(ops->context, true);
		if (!ret && tmp)
			ret = tmp;
		else if (!tmp)
			state->wifi_enabled = false;
	}
	if (!state->selector_dirty)
		return ret;

	bt_on = ops->is_enabled(ops->context, false);
	wifi_on = ops->is_enabled(ops->context, true);
	if (ret || bt_on || wifi_on)
		return ret ?: (bt_on < 0 ? bt_on :
			       (wifi_on < 0 ? wifi_on : -EBUSY));
	tmp = ops->set_voltage(ops->context, false, state->original_uv);
	if (tmp)
		return tmp;
	state->selector_dirty = false;
	return 0;
}

int mt6797_consys_postpatch_run(const struct mt6797_consys_postpatch_ops *ops)
{
	struct mt6797_consys_postpatch_state state = { };
	size_t calibration_len;
	u8 calibration_prefix[2];
	int cleanup_ret;
	int wifi_uv;
	int ret;

	if (!ops || !ops->get_voltage || !ops->is_enabled ||
	    !ops->set_voltage || !ops->enable || !ops->disable || !ops->wmt ||
	    !ops->wmt_capture || !ops->delay_ms)
		return -EINVAL;
	state.original_uv = ops->get_voltage(ops->context, false);
	if (state.original_uv <= 0)
		return state.original_uv ?: -EINVAL;
	wifi_uv = ops->get_voltage(ops->context, true);
	if (wifi_uv <= 0)
		return wifi_uv ?: -EINVAL;
	if (wifi_uv != state.original_uv)
		return -EUCLEAN;
	ret = ops->is_enabled(ops->context, false);
	if (ret)
		return ret < 0 ? ret : -EBUSY;
	ret = ops->is_enabled(ops->context, true);
	if (ret)
		return ret < 0 ? ret : -EBUSY;

	ops->delay_ms(ops->context, MT6797_CONSYS_COEX_DELAY_MS);
	ret = mt6797_consys_postpatch_wmt(ops, mt6797_lte_ext,
					  sizeof(mt6797_lte_ext));
	if (ret)
		goto cleanup;
	ret = mt6797_consys_postpatch_wmt(ops, mt6797_lte_filter,
					  sizeof(mt6797_lte_filter));
	if (ret)
		goto cleanup;
	ret = mt6797_consys_postpatch_wmt(ops, mt6797_lte_frequency,
					  sizeof(mt6797_lte_frequency));
	if (ret)
		goto cleanup;
	ret = mt6797_consys_postpatch_wmt(ops, mt6797_lte_unsafe,
					  sizeof(mt6797_lte_unsafe));
	if (ret)
		goto cleanup;
	ret = mt6797_consys_postpatch_wmt(ops, mt6797_lte_l,
					  sizeof(mt6797_lte_l));
	if (ret)
		goto cleanup;

	state.selector_dirty = true;
	ret = ops->set_voltage(ops->context, false, MT6797_CONSYS_VCN33_UV);
	if (ret)
		goto cleanup;
	ret = ops->set_voltage(ops->context, true, MT6797_CONSYS_VCN33_UV);
	if (ret)
		goto cleanup;
	ret = mt6797_consys_postpatch_enable(ops, &state, false);
	if (ret)
		goto cleanup;
	ret = mt6797_consys_postpatch_enable(ops, &state, true);
	if (ret)
		goto cleanup;
	ret = ops->wmt_capture(ops->context, 0x14, mt6797_calibrate,
			       sizeof(mt6797_calibrate), 0x14, 2,
			       MTK_WMT_MAX_DATA, calibration_prefix,
			       sizeof(calibration_prefix), &calibration_len);
	if (!ret && (calibration_prefix[0] || calibration_prefix[1] != 1))
		ret = -EPROTO;

cleanup:
	cleanup_ret = mt6797_consys_postpatch_cleanup(ops, &state);
	if (ret)
		return ret;
	if (cleanup_ret)
		return cleanup_ret;
	ret = mt6797_consys_postpatch_wmt(ops, mt6797_coex,
					  sizeof(mt6797_coex));
	if (ret)
		return ret;
	return ops->wmt(ops->context, 0x05, mt6797_fm_strap,
			sizeof(mt6797_fm_strap), &mt6797_fm_strap_reply);
}
EXPORT_SYMBOL_GPL(mt6797_consys_postpatch_run);

MODULE_DESCRIPTION("MediaTek MT6797 common WMT post-patch initialization");
MODULE_LICENSE("GPL");
