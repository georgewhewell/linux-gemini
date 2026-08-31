// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mt6797-wifi-ahb.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb-w0-phase.h>

struct mt6797_wifi_ahb_w0_test {
	int voltage;
	int bt_on;
	int wifi_on;
	int set_fail;
	int restore_fail;
	int failed_voltage;
	int enable_fail;
	bool enable_sets_on;
	int disable_fail;
	int observe_fail;
	int off_fail;
	unsigned int disables;
	bool fatal;
};

static int mt6797_wifi_ahb_w0_test_voltage(void *context)
{
	return ((struct mt6797_wifi_ahb_w0_test *)context)->voltage;
}
static int mt6797_wifi_ahb_w0_test_enabled(void *context, bool wifi)
{
	struct mt6797_wifi_ahb_w0_test *test = context;

	if (test->off_fail)
		return test->off_fail;
	return wifi ? test->wifi_on : test->bt_on;
}
static int mt6797_wifi_ahb_w0_test_set(void *context, int uv)
{
	struct mt6797_wifi_ahb_w0_test *test = context;

	/* A regulator may change the shared selector before reporting failure. */
	test->voltage = uv;
	if (test->set_fail && uv == 3300000) {
		test->failed_voltage = uv;
		return test->set_fail;
	}
	if (test->restore_fail && uv != 3300000) {
		test->failed_voltage = uv;
		return test->restore_fail;
	}
	return 0;
}
static int mt6797_wifi_ahb_w0_test_enable(void *context)
{
	struct mt6797_wifi_ahb_w0_test *test = context;

	if (test->enable_sets_on)
		test->wifi_on = 1;
	if (test->enable_fail)
		return test->enable_fail;
	test->wifi_on = 1;
	return 0;
}
static int mt6797_wifi_ahb_w0_test_disable(void *context)
{
	struct mt6797_wifi_ahb_w0_test *test = context;

	test->disables++;
	if (test->disable_fail)
		return test->disable_fail;
	test->wifi_on = 0;
	return 0;
}
static int mt6797_wifi_ahb_w0_test_observe(void *context)
{
	return ((struct mt6797_wifi_ahb_w0_test *)context)->observe_fail;
}
static void mt6797_wifi_ahb_w0_test_fatal(void *context, int causal, int cleanup)
{
	((struct mt6797_wifi_ahb_w0_test *)context)->fatal = true;
}

static void mt6797_wifi_ahb_w0_fault_paths(struct kunit *test)
{
	struct mt6797_wifi_ahb_w0_test fake = { .voltage = 1800000 };
	struct mt6797_wifi_ahb_w0_result result;
	struct mt6797_wifi_ahb_w0_ops ops = {
		.get_voltage = mt6797_wifi_ahb_w0_test_voltage,
		.is_enabled = mt6797_wifi_ahb_w0_test_enabled,
		.set_voltage = mt6797_wifi_ahb_w0_test_set,
		.enable = mt6797_wifi_ahb_w0_test_enable,
		.disable = mt6797_wifi_ahb_w0_test_disable,
		.observe = mt6797_wifi_ahb_w0_test_observe,
		.fatal = mt6797_wifi_ahb_w0_test_fatal,
		.context = &fake,
	};

	/* Failed enable can have powered the rail: it is still disabled. */
	fake.enable_fail = -EIO;
	fake.enable_sets_on = true;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w0_run(&ops, &result, 3300000), -EIO);
	KUNIT_EXPECT_EQ(test, fake.disables, 1U);
	KUNIT_EXPECT_FALSE(test, result.fatal);
	KUNIT_EXPECT_FALSE(test, result.selector_dirty);

	/* A causal observation failure is retained despite successful cleanup. */
	memset(&fake, 0, sizeof(fake)); fake.voltage = 1800000; fake.observe_fail = -EPROTO;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w0_run(&ops, &result, 3300000), -EPROTO);
	KUNIT_EXPECT_EQ(test, fake.disables, 1U);
	KUNIT_EXPECT_FALSE(test, result.fatal);

	/* A partial selector failure still proves rails off and restores selector. */
	memset(&fake, 0, sizeof(fake)); fake.voltage = 1800000; fake.set_fail = -EIO;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w0_run(&ops, &result, 3300000), -EIO);
	KUNIT_EXPECT_EQ(test, result.causal, -EIO);
	KUNIT_EXPECT_EQ(test, result.cleanup, 0);
	KUNIT_EXPECT_EQ(test, fake.failed_voltage, 3300000);
	KUNIT_EXPECT_EQ(test, fake.voltage, 1800000);
	KUNIT_EXPECT_FALSE(test, result.fatal);
	KUNIT_EXPECT_FALSE(test, result.selector_dirty);

	/* Failed disable/unknown off state is dirty and asks the owner to reboot. */
	memset(&fake, 0, sizeof(fake)); fake.voltage = 1800000; fake.disable_fail = -EIO;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w0_run(&ops, &result, 3300000), -EIO);
	KUNIT_EXPECT_EQ(test, result.causal, 0);
	KUNIT_EXPECT_EQ(test, result.cleanup, -EIO);
	KUNIT_EXPECT_EQ(test, fake.voltage, 3300000);
	KUNIT_EXPECT_TRUE(test, result.fatal);
	KUNIT_EXPECT_TRUE(test, fake.fatal);

	/* A selector restore failure is also fatal after rails are proven off. */
	memset(&fake, 0, sizeof(fake)); fake.voltage = 1800000; fake.restore_fail = -EIO;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w0_run(&ops, &result, 3300000), -EIO);
	KUNIT_EXPECT_EQ(test, result.causal, 0);
	KUNIT_EXPECT_EQ(test, result.cleanup, -EIO);
	KUNIT_EXPECT_EQ(test, fake.failed_voltage, 1800000);
	KUNIT_EXPECT_EQ(test, fake.voltage, 1800000);
	KUNIT_EXPECT_TRUE(test, result.fatal);
	KUNIT_EXPECT_TRUE(test, result.selector_dirty);
	KUNIT_EXPECT_TRUE(test, fake.fatal);
}

static void mt6797_wifi_ahb_wcir_accepts_only_everest_raw_id(struct kunit *test)
{
	struct mt6797_wifi_ahb_wcir wcir;

	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_wcir_decode(0x00000279, &wcir), 0);
	KUNIT_EXPECT_EQ(test, wcir.chip_id, (u16)0x0279);
	KUNIT_EXPECT_EQ(test, wcir.revision, (u8)0);
	KUNIT_EXPECT_FALSE(test, wcir.por_indicator);
	KUNIT_EXPECT_FALSE(test, wcir.wlan_ready);
}

static void mt6797_wifi_ahb_wcir_accepts_vendor_por_before_firmware(struct kunit *test)
{
	struct mt6797_wifi_ahb_wcir wcir;

	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_wcir_decode(0x00100279, &wcir), 0);
	KUNIT_EXPECT_EQ(test, wcir.chip_id, (u16)0x0279);
	KUNIT_EXPECT_EQ(test, wcir.revision, (u8)0);
	KUNIT_EXPECT_TRUE(test, wcir.por_indicator);
	KUNIT_EXPECT_FALSE(test, wcir.wlan_ready);
}

static void mt6797_wifi_ahb_wcir_separates_ready_from_identity(struct kunit *test)
{
	struct mt6797_wifi_ahb_wcir wcir;

	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_wcir_decode(0x00006582, &wcir),
			-EPROTO);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_wcir_decode(0x00200279, &wcir), 0);
	KUNIT_EXPECT_TRUE(test, wcir.wlan_ready);
}

static void mt6797_wifi_ahb_wcir_rejects_absent_device(struct kunit *test)
{
	struct mt6797_wifi_ahb_wcir wcir;

	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_wcir_decode(0, &wcir), -ENODEV);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_wcir_decode(0x0000ffff, &wcir),
			-ENODEV);
}

static void mt6797_wifi_ahb_wcir_requires_complete_guard(struct kunit *test)
{
	struct mt6797_wifi_ahb_wcir_guard guard = {
		.full_postpatch_session = true,
		.wifi_paldo_held = true,
		.hif_owner = true,
		.hif_mapping = true,
	};

	KUNIT_EXPECT_TRUE(test, mt6797_wifi_ahb_wcir_read_permitted(&guard));
	guard.hif_mapping = false;
	KUNIT_EXPECT_FALSE(test, mt6797_wifi_ahb_wcir_read_permitted(&guard));
}

static void mt6797_wifi_ahb_cmd53_packs_vendor_wcir_read(struct kunit *test)
{
	u32 word;

	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_cmd53_read_pack(0, 4, &word), 0);
	/* read/function-1/fixed-port/byte-mode/count-4 */
	KUNIT_EXPECT_EQ(test, word, 0x10000004U);
}

static void mt6797_wifi_ahb_cmd52_packs_vendor_cccr_access(struct kunit *test)
{
	u32 word;

	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_cmd52_pack(false, 0, 0x02, 0,
							 &word), 0);
	KUNIT_EXPECT_EQ(test, word, 0x00000400U);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_cmd52_pack(true, 0, 0x02, 0x02,
							 &word), 0);
	KUNIT_EXPECT_EQ(test, word, 0x80000402U);
}

static void mt6797_wifi_ahb_command_pack_refuses_invalid_fields(struct kunit *test)
{
	u32 word;

	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_cmd52_pack(false, 8, 0, 0, &word),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_cmd52_pack(false, 0, BIT(17), 0,
							 &word), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_cmd53_read_pack(0, 0, &word),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_cmd53_read_pack(0, 512, &word),
			-EINVAL);
}

static struct kunit_case mt6797_wifi_ahb_wcir_test_cases[] = {
	KUNIT_CASE(mt6797_wifi_ahb_wcir_accepts_only_everest_raw_id),
	KUNIT_CASE(mt6797_wifi_ahb_wcir_accepts_vendor_por_before_firmware),
	KUNIT_CASE(mt6797_wifi_ahb_wcir_rejects_absent_device),
	KUNIT_CASE(mt6797_wifi_ahb_wcir_separates_ready_from_identity),
	KUNIT_CASE(mt6797_wifi_ahb_wcir_requires_complete_guard),
	KUNIT_CASE(mt6797_wifi_ahb_cmd53_packs_vendor_wcir_read),
	KUNIT_CASE(mt6797_wifi_ahb_cmd52_packs_vendor_cccr_access),
	KUNIT_CASE(mt6797_wifi_ahb_command_pack_refuses_invalid_fields),
	KUNIT_CASE(mt6797_wifi_ahb_w0_fault_paths),
	{}
};

static struct kunit_suite mt6797_wifi_ahb_wcir_test_suite = {
	.name = "mt6797-wifi-ahb-wcir",
	.test_cases = mt6797_wifi_ahb_wcir_test_cases,
};

kunit_test_suite(mt6797_wifi_ahb_wcir_test_suite);

MODULE_LICENSE("GPL");
