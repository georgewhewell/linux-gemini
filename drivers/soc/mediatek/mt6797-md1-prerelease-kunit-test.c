// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free ordering tests for the MT6797 MD1 pre-release core. */

#include <linux/errno.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6797-md1-prerelease.h>
#include <kunit/test.h>

enum mt6797_md1_test_op {
	MD1_TEST_NONE,
	MD1_TEST_PMIC_ON,
	MD1_TEST_CLOCK_ON,
	MD1_TEST_DOMAIN_ON,
	MD1_TEST_POLL,
	MD1_TEST_DOMAIN_OFF,
	MD1_TEST_CLOCK_OFF,
	MD1_TEST_PMIC_OFF,
};

struct mt6797_md1_test_backend {
	enum mt6797_md1_test_op calls[16];
	u32 count;
	u32 pending;
	int fail;
};

static int mt6797_md1_test_record(struct mt6797_md1_test_backend *backend,
		enum mt6797_md1_test_op op)
{
	backend->calls[backend->count++] = op;
	return backend->fail == op ? -EIO : 0;
}

static int mt6797_md1_test_pmic_on(void *context)
{
	return mt6797_md1_test_record(context, MD1_TEST_PMIC_ON);
}

static int mt6797_md1_test_pmic_off(void *context)
{
	return mt6797_md1_test_record(context, MD1_TEST_PMIC_OFF);
}

static int mt6797_md1_test_clock_on(void *context)
{
	return mt6797_md1_test_record(context, MD1_TEST_CLOCK_ON);
}

static int mt6797_md1_test_clock_off(void *context)
{
	return mt6797_md1_test_record(context, MD1_TEST_CLOCK_OFF);
}

static int mt6797_md1_test_domain_on(void *context)
{
	return mt6797_md1_test_record(context, MD1_TEST_DOMAIN_ON);
}

static int mt6797_md1_test_domain_off(void *context)
{
	return mt6797_md1_test_record(context, MD1_TEST_DOMAIN_OFF);
}

static int mt6797_md1_test_poll(void *context)
{
	struct mt6797_md1_test_backend *backend = context;
	int ret;

	ret = mt6797_md1_test_record(backend, MD1_TEST_POLL);
	if (ret)
		return ret;
	if (backend->pending) {
		backend->pending--;
		return -EAGAIN;
	}
	return 0;
}

static const struct mt6797_md1_prerelease_ops mt6797_md1_test_ops = {
	.abi = MT6797_MD1_PRERELEASE_ABI,
	.pmic_on = mt6797_md1_test_pmic_on,
	.pmic_off = mt6797_md1_test_pmic_off,
	.clock_on = mt6797_md1_test_clock_on,
	.clock_off = mt6797_md1_test_clock_off,
	.domain_on = mt6797_md1_test_domain_on,
	.domain_off = mt6797_md1_test_domain_off,
	.poll = mt6797_md1_test_poll,
};

static void mt6797_md1_test_expect_calls(struct kunit *test,
		struct mt6797_md1_test_backend *backend,
		const enum mt6797_md1_test_op *expected, u32 expected_count)
{
	u32 i;

	KUNIT_ASSERT_EQ(test, backend->count, expected_count);
	for (i = 0; i < expected_count; i++)
		KUNIT_EXPECT_EQ(test, backend->calls[i], expected[i]);
}

static void mt6797_md1_test_success_unwinds(struct kunit *test)
{
	static const enum mt6797_md1_test_op expected[] = {
		MD1_TEST_PMIC_ON, MD1_TEST_CLOCK_ON, MD1_TEST_DOMAIN_ON,
		MD1_TEST_POLL, MD1_TEST_DOMAIN_OFF, MD1_TEST_CLOCK_OFF,
		MD1_TEST_PMIC_OFF,
	};
	struct mt6797_md1_test_backend backend = { };
	struct mt6797_md1_prerelease prerelease;

	KUNIT_ASSERT_EQ(test, mt6797_md1_prerelease_init(&prerelease,
						 &mt6797_md1_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_run(&prerelease), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_state(&prerelease),
			MT6797_MD1_PRERELEASE_OFF);
	mt6797_md1_test_expect_calls(test, &backend, expected, ARRAY_SIZE(expected));
}

static void mt6797_md1_test_timeout_is_bounded_and_unwinds(struct kunit *test)
{
	static const enum mt6797_md1_test_op expected[] = {
		MD1_TEST_PMIC_ON, MD1_TEST_CLOCK_ON, MD1_TEST_DOMAIN_ON,
		MD1_TEST_POLL, MD1_TEST_POLL, MD1_TEST_POLL,
		MD1_TEST_DOMAIN_OFF, MD1_TEST_CLOCK_OFF, MD1_TEST_PMIC_OFF,
	};
	struct mt6797_md1_test_backend backend = { .pending = 3 };
	struct mt6797_md1_prerelease prerelease;

	KUNIT_ASSERT_EQ(test, mt6797_md1_prerelease_init(&prerelease,
						 &mt6797_md1_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_run(&prerelease), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_state(&prerelease),
			MT6797_MD1_PRERELEASE_OFF);
	mt6797_md1_test_expect_calls(test, &backend, expected, ARRAY_SIZE(expected));
}

static void mt6797_md1_test_failed_domain_setup_is_unwound(struct kunit *test)
{
	static const enum mt6797_md1_test_op expected[] = {
		MD1_TEST_PMIC_ON, MD1_TEST_CLOCK_ON, MD1_TEST_DOMAIN_ON,
		MD1_TEST_DOMAIN_OFF, MD1_TEST_CLOCK_OFF, MD1_TEST_PMIC_OFF,
	};
	struct mt6797_md1_test_backend backend = { .fail = MD1_TEST_DOMAIN_ON };
	struct mt6797_md1_prerelease prerelease;

	KUNIT_ASSERT_EQ(test, mt6797_md1_prerelease_init(&prerelease,
						 &mt6797_md1_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_run(&prerelease), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_state(&prerelease),
			MT6797_MD1_PRERELEASE_OFF);
	mt6797_md1_test_expect_calls(test, &backend, expected, ARRAY_SIZE(expected));
}

static void mt6797_md1_test_cleanup_failure_is_sticky(struct kunit *test)
{
	struct mt6797_md1_test_backend backend = { .fail = MD1_TEST_DOMAIN_OFF };
	struct mt6797_md1_prerelease prerelease;

	KUNIT_ASSERT_EQ(test, mt6797_md1_prerelease_init(&prerelease,
						 &mt6797_md1_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_run(&prerelease), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_state(&prerelease),
			MT6797_MD1_PRERELEASE_ERROR);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_run(&prerelease), -EIO);
}

struct mt6797_md1_pmic_fake {
	u16 registers[0x4000];
	unsigned int writes;
	unsigned int fail_write;
};

static int mt6797_md1_pmic_fake_read(void *context, unsigned int reg,
				     unsigned int *value)
{
	struct mt6797_md1_pmic_fake *fake = context;

	if (reg & 1 || reg > 0x7ffe)
		return -EINVAL;
	*value = fake->registers[reg / 2];
	return 0;
}

static int mt6797_md1_pmic_fake_write(void *context, unsigned int reg,
				      unsigned int value)
{
	struct mt6797_md1_pmic_fake *fake = context;

	if (reg & 1 || reg > 0x7ffe)
		return -EINVAL;
	if (++fake->writes == fake->fail_write)
		return -EIO;
	fake->registers[reg / 2] = value;
	return 0;
}

static const struct regmap_bus mt6797_md1_pmic_fake_bus = {
	.reg_read = mt6797_md1_pmic_fake_read,
	.reg_write = mt6797_md1_pmic_fake_write,
};

static const struct regmap_config mt6797_md1_pmic_fake_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 2,
	.max_register = 0x7ffe,
};

static void mt6797_md1_pmic_fake_seed(struct mt6797_md1_pmic_fake *fake)
{
	fake->registers[MT6351_BUCK_VMODEM_CON0 / 2] = 0xaaa8;
	fake->registers[MT6351_BUCK_VMODEM_CON2 / 2] = 0xbbb0;
	fake->registers[MT6351_BUCK_VMODEM_CON5 / 2] = 0xccc3;
	fake->registers[MT6351_BUCK_VMODEM_CON9 / 2] = 0xddd0;
	fake->registers[MT6351_BUCK_VMD1_CON0 / 2] = 0x1110;
	fake->registers[MT6351_BUCK_VMD1_CON2 / 2] = 0x2220;
	fake->registers[MT6351_BUCK_VMD1_CON5 / 2] = 0x3334;
	fake->registers[MT6351_BUCK_VMD1_CON9 / 2] = 0x4440;
	fake->registers[MT6351_BUCK_VSRAM_MD_CON0 / 2] = 0x5550;
	fake->registers[MT6351_BUCK_VSRAM_MD_CON2 / 2] = 0x6660;
	fake->registers[MT6351_BUCK_VSRAM_MD_CON5 / 2] = 0x7777;
	fake->registers[MT6351_BUCK_VSRAM_MD_CON9 / 2] = 0x8880;
}

static void mt6797_md1_test_pmic_apply_restores_every_field(struct kunit *test)
{
	struct mt6797_md1_pmic_fake *fake;
	struct mt6797_md1_pmic_snapshot snapshot;
	struct regmap *map;

	fake = kunit_kzalloc(test, sizeof(*fake), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fake);
	mt6797_md1_pmic_fake_seed(fake);
	map = regmap_init(NULL, &mt6797_md1_pmic_fake_bus, fake,
			  &mt6797_md1_pmic_fake_config);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));

	KUNIT_ASSERT_EQ(test, mt6797_md1_pmic_snapshot(map, &snapshot), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_pmic_apply_vendor_values(map, &snapshot), 0);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VMODEM_CON5 / 2] & 0x7f,
			0x40);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VSRAM_MD_CON5 / 2] & 0x7f,
			0x50);
	KUNIT_ASSERT_EQ(test, mt6797_md1_pmic_restore(map, &snapshot), 0);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VMODEM_CON0 / 2], 0xaaa8);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VMODEM_CON2 / 2], 0xbbb0);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VMODEM_CON5 / 2], 0xccc3);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VMODEM_CON9 / 2], 0xddd0);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VMD1_CON0 / 2], 0x1110);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VMD1_CON2 / 2], 0x2220);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VMD1_CON5 / 2], 0x3334);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VMD1_CON9 / 2], 0x4440);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VSRAM_MD_CON0 / 2], 0x5550);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VSRAM_MD_CON2 / 2], 0x6660);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VSRAM_MD_CON5 / 2], 0x7777);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VSRAM_MD_CON9 / 2], 0x8880);
	regmap_exit(map);
}

static void mt6797_md1_test_pmic_write_failure_can_be_rolled_back(struct kunit *test)
{
	struct mt6797_md1_pmic_fake *fake;
	struct mt6797_md1_pmic_snapshot snapshot;
	struct regmap *map;

	fake = kunit_kzalloc(test, sizeof(*fake), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fake);
	mt6797_md1_pmic_fake_seed(fake);
	map = regmap_init(NULL, &mt6797_md1_pmic_fake_bus, fake,
			  &mt6797_md1_pmic_fake_config);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	KUNIT_ASSERT_EQ(test, mt6797_md1_pmic_snapshot(map, &snapshot), 0);
	fake->fail_write = 4;
	KUNIT_EXPECT_EQ(test, mt6797_md1_pmic_apply_vendor_values(map, &snapshot), -EIO);
	fake->fail_write = 0;
	KUNIT_ASSERT_EQ(test, mt6797_md1_pmic_restore(map, &snapshot), 0);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VMODEM_CON5 / 2], 0xccc3);
	KUNIT_EXPECT_EQ(test, fake->registers[MT6351_BUCK_VSRAM_MD_CON5 / 2], 0x7777);
	regmap_exit(map);
}

struct mt6797_md1_spm_fake {
	u32 registers[0x500 / sizeof(u32)];
	unsigned int writes;
	unsigned int fail_write;
	unsigned int write_reg[8];
};

static int mt6797_md1_spm_fake_read(void *context, unsigned int reg,
				    unsigned int *value)
{
	struct mt6797_md1_spm_fake *fake = context;

	if (reg & 3 || reg >= 0x500)
		return -EINVAL;
	*value = fake->registers[reg / sizeof(u32)];
	return 0;
}

static int mt6797_md1_spm_fake_write(void *context, unsigned int reg,
				     unsigned int value)
{
	struct mt6797_md1_spm_fake *fake = context;

	if (reg & 3 || reg >= 0x500)
		return -EINVAL;
	if (++fake->writes == fake->fail_write)
		return -EIO;
	if (fake->writes <= ARRAY_SIZE(fake->write_reg))
		fake->write_reg[fake->writes - 1] = reg;
	fake->registers[reg / sizeof(u32)] = value;
	return 0;
}

static const struct regmap_bus mt6797_md1_spm_fake_bus = {
	.reg_read = mt6797_md1_spm_fake_read,
	.reg_write = mt6797_md1_spm_fake_write,
};

static const struct regmap_config mt6797_md1_spm_fake_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x4fc,
};

static void mt6797_md1_test_clkbuf_set_restores_only_owned_bits(struct kunit *test)
{
	struct mt6797_md1_clkbuf_snapshot snapshot;
	struct mt6797_md1_spm_fake *fake;
	struct regmap *map;

	fake = kunit_kzalloc(test, sizeof(*fake), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fake);
	fake->registers[0x404 / sizeof(u32)] = 0xa5a50003;
	map = regmap_init(NULL, &mt6797_md1_spm_fake_bus, fake,
			  &mt6797_md1_spm_fake_config);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));

	KUNIT_ASSERT_EQ(test, mt6797_md1_clkbuf_snapshot(map, &snapshot), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_clkbuf_set_modem_on(map, &snapshot), 0);
	KUNIT_EXPECT_EQ(test, fake->registers[0x404 / sizeof(u32)], 0xa5a50006);
	/* Simulate an independent SPM update above the vendor-owned low bits. */
	fake->registers[0x404 / sizeof(u32)] ^= BIT(16);
	KUNIT_ASSERT_EQ(test, mt6797_md1_clkbuf_restore(map, &snapshot), 0);
	KUNIT_EXPECT_EQ(test, fake->registers[0x404 / sizeof(u32)], 0xa5a40003);
	regmap_exit(map);
}

static void mt6797_md1_test_clkbuf_write_failure_can_be_rolled_back(struct kunit *test)
{
	struct mt6797_md1_clkbuf_snapshot snapshot;
	struct mt6797_md1_spm_fake *fake;
	struct regmap *map;

	fake = kunit_kzalloc(test, sizeof(*fake), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fake);
	fake->registers[0x404 / sizeof(u32)] = 0x5;
	map = regmap_init(NULL, &mt6797_md1_spm_fake_bus, fake,
			  &mt6797_md1_spm_fake_config);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));

	KUNIT_ASSERT_EQ(test, mt6797_md1_clkbuf_snapshot(map, &snapshot), 0);
	fake->fail_write = 1;
	KUNIT_EXPECT_EQ(test, mt6797_md1_clkbuf_set_modem_on(map, &snapshot), -EIO);
	fake->fail_write = 0;
	KUNIT_ASSERT_EQ(test, mt6797_md1_clkbuf_restore(map, &snapshot), 0);
	KUNIT_EXPECT_EQ(test, fake->registers[0x404 / sizeof(u32)], 0x5);
	regmap_exit(map);
}

static void mt6797_md1_test_spm_timeout_is_explicitly_restored(struct kunit *test)
{
	struct mt6797_md1_spm_snapshot snapshot;
	struct mt6797_md1_spm_fake *fake;
	struct regmap *map;

	fake = kunit_kzalloc(test, sizeof(*fake), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fake);
	/* The status registers remain clear: this is a bounded no-ack simulation. */
	fake->registers[0x0 / sizeof(u32)] = BIT(0);
	fake->registers[0x320 / sizeof(u32)] = BIT(1) | BIT(4);
	fake->registers[0x390 / sizeof(u32)] = BIT(0);
	map = regmap_init(NULL, &mt6797_md1_spm_fake_bus, fake,
			  &mt6797_md1_spm_fake_config);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	KUNIT_ASSERT_EQ(test, mt6797_md1_spm_snapshot(map, &snapshot), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_spm_power_on(map, &snapshot), -ETIMEDOUT);
	KUNIT_ASSERT_EQ(test, mt6797_md1_spm_restore(map, &snapshot), 0);
	KUNIT_EXPECT_EQ(test, fake->registers[0x320 / sizeof(u32)], BIT(1) | BIT(4));
	KUNIT_EXPECT_EQ(test, fake->registers[0x390 / sizeof(u32)], BIT(0));
	/* Vendor order: external buck isolation, PWR_ON, PWR_ON_2ND. */
	KUNIT_EXPECT_EQ(test, fake->write_reg[0], 0x390U);
	KUNIT_EXPECT_EQ(test, fake->write_reg[1], 0x320U);
	KUNIT_EXPECT_EQ(test, fake->write_reg[2], 0x320U);
	regmap_exit(map);
}

static void mt6797_md1_test_spm_rejects_shared_or_live_state(struct kunit *test)
{
	struct mt6797_md1_spm_snapshot snapshot;
	struct mt6797_md1_spm_fake *fake;
	struct regmap *map;

	fake = kunit_kzalloc(test, sizeof(*fake), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fake);
	map = regmap_init(NULL, &mt6797_md1_spm_fake_bus, fake,
			  &mt6797_md1_spm_fake_config);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	KUNIT_EXPECT_EQ(test, mt6797_md1_spm_snapshot(map, &snapshot), -EPERM);
	fake->registers[0x0 / sizeof(u32)] = BIT(0);
	fake->registers[0x320 / sizeof(u32)] = BIT(2);
	KUNIT_EXPECT_EQ(test, mt6797_md1_spm_snapshot(map, &snapshot), -EBUSY);
	regmap_exit(map);
}

static void mt6797_md1_test_spm_restore_attempts_isolation_after_power_failure(struct kunit *test)
{
	struct mt6797_md1_spm_snapshot snapshot;
	struct mt6797_md1_spm_fake *fake;
	struct regmap *map;

	fake = kunit_kzalloc(test, sizeof(*fake), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fake);
	fake->registers[0x0 / sizeof(u32)] = BIT(0);
	fake->registers[0x390 / sizeof(u32)] = BIT(0);
	map = regmap_init(NULL, &mt6797_md1_spm_fake_bus, fake,
			  &mt6797_md1_spm_fake_config);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	KUNIT_ASSERT_EQ(test, mt6797_md1_spm_snapshot(map, &snapshot), 0);
	/* Pretend the dual power ack arrived after both requests. */
	fake->registers[0x180 / sizeof(u32)] = BIT(0);
	fake->registers[0x184 / sizeof(u32)] = BIT(0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_spm_power_on(map, &snapshot), 0);
	fake->registers[0x180 / sizeof(u32)] = 0;
	fake->registers[0x184 / sizeof(u32)] = 0;
	/* The PWR_CON restore fails, but isolation must still be restored. */
	fake->fail_write = fake->writes + 1;
	KUNIT_EXPECT_EQ(test, mt6797_md1_spm_restore(map, &snapshot), -EIO);
	KUNIT_EXPECT_EQ(test, fake->registers[0x390 / sizeof(u32)], BIT(0));
	KUNIT_EXPECT_EQ(test, fake->write_reg[fake->writes - 1], 0x390U);
	regmap_exit(map);
}

static struct kunit_case mt6797_md1_prerelease_test_cases[] = {
	KUNIT_CASE(mt6797_md1_test_success_unwinds),
	KUNIT_CASE(mt6797_md1_test_timeout_is_bounded_and_unwinds),
	KUNIT_CASE(mt6797_md1_test_failed_domain_setup_is_unwound),
	KUNIT_CASE(mt6797_md1_test_cleanup_failure_is_sticky),
	KUNIT_CASE(mt6797_md1_test_pmic_apply_restores_every_field),
	KUNIT_CASE(mt6797_md1_test_pmic_write_failure_can_be_rolled_back),
	KUNIT_CASE(mt6797_md1_test_clkbuf_set_restores_only_owned_bits),
	KUNIT_CASE(mt6797_md1_test_clkbuf_write_failure_can_be_rolled_back),
	KUNIT_CASE(mt6797_md1_test_spm_timeout_is_explicitly_restored),
	KUNIT_CASE(mt6797_md1_test_spm_rejects_shared_or_live_state),
	KUNIT_CASE(mt6797_md1_test_spm_restore_attempts_isolation_after_power_failure),
	{ }
};

static struct kunit_suite mt6797_md1_prerelease_test_suite = {
	.name = "mt6797-md1-prerelease",
	.test_cases = mt6797_md1_prerelease_test_cases,
};
kunit_test_suite(mt6797_md1_prerelease_test_suite);

MODULE_LICENSE("GPL");
