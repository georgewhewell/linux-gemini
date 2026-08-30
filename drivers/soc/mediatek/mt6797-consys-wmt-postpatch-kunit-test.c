// SPDX-License-Identifier: GPL-2.0-only
/* Exact vendor post-patch ordering model; deliberately no live entry point. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mtk-wmt.h>

#define MT6797_POSTPATCH_TIMEOUT	msecs_to_jiffies(3000)

enum mt6797_postpatch_step { EFUSE, BT_ON, WIFI_ON, CALIBRATE, WIFI_OFF, BT_OFF, COEX };
struct mt6797_postpatch_ops {
	int (*efuse)(void *ctx, u8 *level);
	int (*rail)(void *ctx, bool wifi, bool on, u8 level);
	int (*wmt)(void *ctx, u8 opcode, const u8 *data, size_t len,
		   const struct mtk_wmt_match *match, unsigned long timeout);
	void *ctx;
};
static const u8 calibration_data[] = { 1 }, calibration_reply[] = { 0, 1 };
static const u8 coex_data[] = { 1, 1 }, status_ok[] = { 0 };
static const struct mtk_wmt_match calibration_match = {
	.opcode = 0x14, .data = calibration_reply, .data_len = sizeof(calibration_reply),
};
static const struct mtk_wmt_match coex_match = {
	.opcode = 0x10, .data = status_ok, .data_len = sizeof(status_ok),
};

static int mt6797_postpatch_run(const struct mt6797_postpatch_ops *ops)
{
	u8 level;
	int ret;

	if (!ops || !ops->efuse || !ops->rail || !ops->wmt)
		return -EINVAL;
	ret = ops->efuse(ops->ctx, &level);
	if (ret)
		return ret;
	/* The missing vendor wmt_set_pmic_voltage() owns level-to-uV selection. */
	ret = ops->rail(ops->ctx, false, true, level);
	if (ret)
		return ret;
	ret = ops->rail(ops->ctx, true, true, level);
	if (ret)
		goto bt_off;
	ret = ops->wmt(ops->ctx, 0x14, calibration_data, sizeof(calibration_data),
			       &calibration_match, MT6797_POSTPATCH_TIMEOUT);
	if (ops->rail(ops->ctx, true, false, level) && !ret)
		ret = -EIO;
bt_off:
	if (ops->rail(ops->ctx, false, false, level) && !ret)
		ret = -EIO;
	if (ret)
		return ret;
	return ops->wmt(ops->ctx, 0x10, coex_data, sizeof(coex_data),
			&coex_match, MT6797_POSTPATCH_TIMEOUT);
}

struct mt6797_postpatch_test { enum mt6797_postpatch_step step[7]; unsigned int n; int fail; };
static int postpatch_efuse(void *ctx, u8 *level)
{
	struct mt6797_postpatch_test *t = ctx;
	t->step[t->n++] = EFUSE; *level = 2;
	return t->fail == EFUSE ? -ETIMEDOUT : 0;
}
static int postpatch_rail(void *ctx, bool wifi, bool on, u8 level)
{
	struct mt6797_postpatch_test *t = ctx;
	if (level != 2)
		return -EINVAL;
	t->step[t->n++] = wifi ? (on ? WIFI_ON : WIFI_OFF) : (on ? BT_ON : BT_OFF);
	return t->fail == t->step[t->n - 1] ? -ETIMEDOUT : 0;
}
static int postpatch_wmt(void *ctx, u8 opcode, const u8 *data, size_t len,
		const struct mtk_wmt_match *match, unsigned long timeout)
{
	struct mt6797_postpatch_test *t = ctx;
	enum mt6797_postpatch_step step;
	if (timeout != MT6797_POSTPATCH_TIMEOUT)
		return -EINVAL;
	if (opcode == 0x14 && len == sizeof(calibration_data) &&
	    !memcmp(data, calibration_data, len) && match == &calibration_match)
		step = CALIBRATE;
	else if (opcode == 0x10 && len == sizeof(coex_data) &&
		 !memcmp(data, coex_data, len) && match == &coex_match)
		step = COEX;
	else
		return -EINVAL;
	t->step[t->n++] = step;
	return t->fail == step ? -ETIMEDOUT : 0;
}
static void mt6797_postpatch_order_test(struct kunit *test)
{
	struct mt6797_postpatch_test context = { .fail = -1 };
	struct mt6797_postpatch_ops ops = { postpatch_efuse, postpatch_rail,
		postpatch_wmt, &context };
	KUNIT_EXPECT_EQ(test, mt6797_postpatch_run(&ops), 0);
	KUNIT_EXPECT_EQ(test, context.n, 7U);
	KUNIT_EXPECT_EQ(test, context.step[0], EFUSE);
	KUNIT_EXPECT_EQ(test, context.step[1], BT_ON);
	KUNIT_EXPECT_EQ(test, context.step[2], WIFI_ON);
	KUNIT_EXPECT_EQ(test, context.step[3], CALIBRATE);
	KUNIT_EXPECT_EQ(test, context.step[4], WIFI_OFF);
	KUNIT_EXPECT_EQ(test, context.step[5], BT_OFF);
	KUNIT_EXPECT_EQ(test, context.step[6], COEX);
}
static void mt6797_postpatch_failure_unwinds_test(struct kunit *test)
{
	struct mt6797_postpatch_test context = { .fail = CALIBRATE };
	struct mt6797_postpatch_ops ops = { postpatch_efuse, postpatch_rail,
		postpatch_wmt, &context };
	KUNIT_EXPECT_EQ(test, mt6797_postpatch_run(&ops), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, context.n, 6U);
	KUNIT_EXPECT_EQ(test, context.step[4], WIFI_OFF);
	KUNIT_EXPECT_EQ(test, context.step[5], BT_OFF);
}
static struct kunit_case mt6797_postpatch_cases[] = {
	KUNIT_CASE(mt6797_postpatch_order_test),
	KUNIT_CASE(mt6797_postpatch_failure_unwinds_test), {}
};
static struct kunit_suite mt6797_postpatch_suite = {
	.name = "mt6797-consys-wmt-postpatch", .test_cases = mt6797_postpatch_cases,
};
kunit_test_suite(mt6797_postpatch_suite);
