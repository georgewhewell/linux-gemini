// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 Bluetooth WMT function-control choreography, expressed only as a
 * hardware-neutral KUnit model.
 *
 * The vendor MT6797 WMT path is the SoC path, not the COMBO shortcut:
 *
 *   1. WMT_CTRL_SOC_PALDO_CTRL(BT_PALDO, PALDO_ON)
 *   2. FUNC_CTRL(WMTDRV_TYPE_BT, on), reply status == 0
 *   3. FUNC_CTRL(WMTDRV_TYPE_BT, off), reply status == 0
 *   4. WMT_CTRL_SOC_PALDO_CTRL(BT_PALDO, PALDO_OFF)
 *
 * See gemini-linux-kernel-3.18, drivers/misc/mediatek/connectivity/
 * common/common_main/core/wmt_func.c:293-360 and wmt_core.c:395-489. The
 * function-control command is opcode 6 with SDU { WMTDRV_TYPE_BT == 0,
 * enable }, and its five-byte event has one zero status byte.
 *
 * More importantly, the vendor's post-patch sw_init() does not jump directly
 * to FUNC_CTRL: it turns BT/Wi-Fi PALDO on, runs calibration_table, turns both
 * off, then runs the configuration-derived coexistence script
 * (wmt_ic_soc.c:1168-1197,1765-1868). The current G2c owner intentionally
 * stops immediately after patching. Gemini DT also supplies VCN18 and VCN28
 * only, with no vcn33-bt-supply (arch/arm64/boot/dts/mediatek/
 * mt6797-gemini-pda.dts:230-246). Consequently this file has no production
 * entry point. A future live owner must first establish that whole post-patch
 * PALDO/calibration/coexistence contract and its reset/fatal recovery path.
 * It must never treat a successful unit test as authority to emit this
 * sequence.
 */

#include <kunit/test.h>

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mtk-wmt.h>

#define MT6797_CONSYS_WMT_FUNC_CTRL_OPCODE	6
#define MT6797_CONSYS_WMT_FUNC_CTRL_TIMEOUT_MS	3000
#define MT6797_CONSYS_WMT_BT_TYPE		0

enum mt6797_consys_wmt_bt_function_outcome {
	MT6797_CONSYS_WMT_BT_CLEAN_OFF,
	/* The outer owner must fence BTIF and reset/recover CONSYS. */
	MT6797_CONSYS_WMT_BT_TERMINAL,
};

struct mt6797_consys_wmt_bt_function_transport {
	int (*paldo)(void *context, bool enable);
	int (*transaction)(void *context, u8 opcode, const u8 *data,
			   size_t data_len, const struct mtk_wmt_match *expected,
			   unsigned long timeout);
	void *context;
};

static const u8 mt6797_consys_wmt_bt_status_ok[] = { 0 };

static const struct mtk_wmt_match mt6797_consys_wmt_bt_func_match = {
	.opcode = MT6797_CONSYS_WMT_FUNC_CTRL_OPCODE,
	.data = mt6797_consys_wmt_bt_status_ok,
	.data_len = sizeof(mt6797_consys_wmt_bt_status_ok),
};

static int mt6797_consys_wmt_bt_func_ctrl(
		const struct mt6797_consys_wmt_bt_function_transport *transport,
		bool enable)
{
	const u8 data[] = { MT6797_CONSYS_WMT_BT_TYPE, enable ? 1 : 0 };

	return transport->transaction(transport->context,
			MT6797_CONSYS_WMT_FUNC_CTRL_OPCODE, data, sizeof(data),
			&mt6797_consys_wmt_bt_func_match,
			msecs_to_jiffies(MT6797_CONSYS_WMT_FUNC_CTRL_TIMEOUT_MS));
}

/*
 * Mirror wmt_func_bt_on()/wmt_func_bt_off() without implementing its vendor
 * coredump trigger. Any error is terminal here: a future hardware owner must
 * do its own fenced reset/recovery before it can use this controller again.
 */
static int mt6797_consys_wmt_bt_function_pulse(
		const struct mt6797_consys_wmt_bt_function_transport *transport,
		enum mt6797_consys_wmt_bt_function_outcome *outcome)
{
	int ret;
	int paldo_ret;

	if (!transport || !transport->paldo || !transport->transaction ||
	    !outcome)
		return -EINVAL;

	*outcome = MT6797_CONSYS_WMT_BT_TERMINAL;
	ret = transport->paldo(transport->context, true);
	if (ret)
		return ret;

	ret = mt6797_consys_wmt_bt_func_ctrl(transport, true);
	if (ret)
		goto out_paldo;

	ret = mt6797_consys_wmt_bt_func_ctrl(transport, false);
	if (ret)
		goto out_paldo;

out_paldo:
	/* The vendor off path always requests PALDO off, including on FUNC failure. */
	paldo_ret = transport->paldo(transport->context, false);
	if (ret)
		return ret;
	if (paldo_ret)
		return paldo_ret;

	*outcome = MT6797_CONSYS_WMT_BT_CLEAN_OFF;
	return 0;
}

enum mt6797_consys_wmt_bt_function_step {
	MT6797_CONSYS_WMT_BT_PALDO_ON,
	MT6797_CONSYS_WMT_BT_FUNC_ON,
	MT6797_CONSYS_WMT_BT_FUNC_OFF,
	MT6797_CONSYS_WMT_BT_PALDO_OFF,
};

struct mt6797_consys_wmt_bt_function_test_context {
	enum mt6797_consys_wmt_bt_function_step steps[4];
	unsigned int count;
	int fail_step;
};

static int mt6797_consys_wmt_bt_function_test_paldo(void *context, bool enable)
{
	struct mt6797_consys_wmt_bt_function_test_context *test = context;
	enum mt6797_consys_wmt_bt_function_step step;

	step = enable ? MT6797_CONSYS_WMT_BT_PALDO_ON :
		MT6797_CONSYS_WMT_BT_PALDO_OFF;
	if (test->count < ARRAY_SIZE(test->steps))
		test->steps[test->count] = step;
	test->count++;

	return test->fail_step == step ? -ETIMEDOUT : 0;
}

static int mt6797_consys_wmt_bt_function_test_transaction(void *context,
		u8 opcode, const u8 *data, size_t data_len,
		const struct mtk_wmt_match *expected, unsigned long timeout)
{
	struct mt6797_consys_wmt_bt_function_test_context *test = context;
	enum mt6797_consys_wmt_bt_function_step step;

	if (opcode != MT6797_CONSYS_WMT_FUNC_CTRL_OPCODE || data_len != 2 ||
	    data[0] != MT6797_CONSYS_WMT_BT_TYPE || !expected ||
	    expected->opcode != MT6797_CONSYS_WMT_FUNC_CTRL_OPCODE ||
	    expected->data_len != sizeof(mt6797_consys_wmt_bt_status_ok) ||
	    expected->data[0] != 0 ||
	    timeout != msecs_to_jiffies(MT6797_CONSYS_WMT_FUNC_CTRL_TIMEOUT_MS))
		return -EINVAL;

	step = data[1] ? MT6797_CONSYS_WMT_BT_FUNC_ON :
		MT6797_CONSYS_WMT_BT_FUNC_OFF;
	if (test->count < ARRAY_SIZE(test->steps))
		test->steps[test->count] = step;
	test->count++;

	return test->fail_step == step ? -ETIMEDOUT : 0;
}

static const struct mt6797_consys_wmt_bt_function_transport
		mt6797_consys_wmt_bt_function_test_transport = {
	.paldo = mt6797_consys_wmt_bt_function_test_paldo,
	.transaction = mt6797_consys_wmt_bt_function_test_transaction,
};

static void mt6797_consys_wmt_bt_function_success_test(struct kunit *test)
{
	struct mt6797_consys_wmt_bt_function_test_context context = {
		.fail_step = -1,
	};
	struct mt6797_consys_wmt_bt_function_transport transport =
		mt6797_consys_wmt_bt_function_test_transport;
	enum mt6797_consys_wmt_bt_function_outcome outcome;

	transport.context = &context;
	KUNIT_EXPECT_EQ(test, mt6797_consys_wmt_bt_function_pulse(&transport,
								  &outcome), 0);
	KUNIT_EXPECT_EQ(test, outcome, MT6797_CONSYS_WMT_BT_CLEAN_OFF);
	KUNIT_EXPECT_EQ(test, context.count, 4U);
	KUNIT_EXPECT_EQ(test, context.steps[0], MT6797_CONSYS_WMT_BT_PALDO_ON);
	KUNIT_EXPECT_EQ(test, context.steps[1], MT6797_CONSYS_WMT_BT_FUNC_ON);
	KUNIT_EXPECT_EQ(test, context.steps[2], MT6797_CONSYS_WMT_BT_FUNC_OFF);
	KUNIT_EXPECT_EQ(test, context.steps[3], MT6797_CONSYS_WMT_BT_PALDO_OFF);
}

static void mt6797_consys_wmt_bt_function_on_failure_test(struct kunit *test)
{
	struct mt6797_consys_wmt_bt_function_test_context context = {
		.fail_step = MT6797_CONSYS_WMT_BT_FUNC_ON,
	};
	struct mt6797_consys_wmt_bt_function_transport transport =
		mt6797_consys_wmt_bt_function_test_transport;
	enum mt6797_consys_wmt_bt_function_outcome outcome;

	transport.context = &context;
	KUNIT_EXPECT_EQ(test, mt6797_consys_wmt_bt_function_pulse(&transport,
								  &outcome), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, outcome, MT6797_CONSYS_WMT_BT_TERMINAL);
	KUNIT_EXPECT_EQ(test, context.count, 3U);
	KUNIT_EXPECT_EQ(test, context.steps[0], MT6797_CONSYS_WMT_BT_PALDO_ON);
	KUNIT_EXPECT_EQ(test, context.steps[1], MT6797_CONSYS_WMT_BT_FUNC_ON);
	KUNIT_EXPECT_EQ(test, context.steps[2], MT6797_CONSYS_WMT_BT_PALDO_OFF);
}

static void mt6797_consys_wmt_bt_function_off_failure_test(struct kunit *test)
{
	struct mt6797_consys_wmt_bt_function_test_context context = {
		.fail_step = MT6797_CONSYS_WMT_BT_FUNC_OFF,
	};
	struct mt6797_consys_wmt_bt_function_transport transport =
		mt6797_consys_wmt_bt_function_test_transport;
	enum mt6797_consys_wmt_bt_function_outcome outcome;

	transport.context = &context;
	KUNIT_EXPECT_EQ(test, mt6797_consys_wmt_bt_function_pulse(&transport,
								  &outcome), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, outcome, MT6797_CONSYS_WMT_BT_TERMINAL);
	KUNIT_EXPECT_EQ(test, context.count, 4U);
	KUNIT_EXPECT_EQ(test, context.steps[0], MT6797_CONSYS_WMT_BT_PALDO_ON);
	KUNIT_EXPECT_EQ(test, context.steps[1], MT6797_CONSYS_WMT_BT_FUNC_ON);
	KUNIT_EXPECT_EQ(test, context.steps[2], MT6797_CONSYS_WMT_BT_FUNC_OFF);
	KUNIT_EXPECT_EQ(test, context.steps[3], MT6797_CONSYS_WMT_BT_PALDO_OFF);
}

static void mt6797_consys_wmt_bt_function_paldo_off_failure_test(struct kunit *test)
{
	struct mt6797_consys_wmt_bt_function_test_context context = {
		.fail_step = MT6797_CONSYS_WMT_BT_PALDO_OFF,
	};
	struct mt6797_consys_wmt_bt_function_transport transport =
		mt6797_consys_wmt_bt_function_test_transport;
	enum mt6797_consys_wmt_bt_function_outcome outcome;

	transport.context = &context;
	KUNIT_EXPECT_EQ(test, mt6797_consys_wmt_bt_function_pulse(&transport,
								  &outcome), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, outcome, MT6797_CONSYS_WMT_BT_TERMINAL);
	KUNIT_EXPECT_EQ(test, context.count, 4U);
}

static struct kunit_case mt6797_consys_wmt_bt_function_test_cases[] = {
	KUNIT_CASE(mt6797_consys_wmt_bt_function_success_test),
	KUNIT_CASE(mt6797_consys_wmt_bt_function_on_failure_test),
	KUNIT_CASE(mt6797_consys_wmt_bt_function_off_failure_test),
	KUNIT_CASE(mt6797_consys_wmt_bt_function_paldo_off_failure_test),
	{}
};

static struct kunit_suite mt6797_consys_wmt_bt_function_test_suite = {
	.name = "mt6797-consys-wmt-bt-function",
	.test_cases = mt6797_consys_wmt_bt_function_test_cases,
};
kunit_test_suite(mt6797_consys_wmt_bt_function_test_suite);

MODULE_LICENSE("GPL");
