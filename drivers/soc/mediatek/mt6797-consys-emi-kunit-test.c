// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 CONSYS shared-EMI owner. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/soc/mediatek/mt6797-consys-emi.h>
#include <kunit/test.h>

enum mt6797_consys_emi_test_call {
	MT6797_CONSYS_EMI_TEST_NONE = -1,
	MT6797_CONSYS_EMI_TEST_PROTECT,
	MT6797_CONSYS_EMI_TEST_REMAP,
	MT6797_CONSYS_EMI_TEST_MAP_CLEAR,
};

struct mt6797_consys_emi_test_backend {
	enum mt6797_consys_emi_test_call calls[4];
	u32 call_count;
	enum mt6797_consys_emi_test_call fail_call;
	struct mt6797_consys_emi_layout seen;
};

static int mt6797_consys_emi_test_record(
		struct mt6797_consys_emi_test_backend *backend,
		enum mt6797_consys_emi_test_call call,
		const struct mt6797_consys_emi_layout *layout)
{
	if (backend->call_count < ARRAY_SIZE(backend->calls))
		backend->calls[backend->call_count++] = call;
	backend->seen = *layout;
	return backend->fail_call == call ? -EIO : 0;
}

static int mt6797_consys_emi_test_protect(
		void *context, const struct mt6797_consys_emi_layout *layout)
{
	return mt6797_consys_emi_test_record(context,
					MT6797_CONSYS_EMI_TEST_PROTECT, layout);
}

static int mt6797_consys_emi_test_remap(
		void *context, const struct mt6797_consys_emi_layout *layout)
{
	return mt6797_consys_emi_test_record(context,
					MT6797_CONSYS_EMI_TEST_REMAP, layout);
}

static int mt6797_consys_emi_test_map_clear(
		void *context, const struct mt6797_consys_emi_layout *layout)
{
	return mt6797_consys_emi_test_record(context,
					MT6797_CONSYS_EMI_TEST_MAP_CLEAR, layout);
}

static const struct mt6797_consys_emi_ops mt6797_consys_emi_test_ops = {
	.abi = MT6797_CONSYS_EMI_ABI,
	.protect = mt6797_consys_emi_test_protect,
	.remap = mt6797_consys_emi_test_remap,
	.map_clear = mt6797_consys_emi_test_map_clear,
};

static void mt6797_consys_emi_test_layout_values(struct kunit *test)
{
	struct mt6797_consys_emi_layout layout;

	KUNIT_ASSERT_EQ(test, mt6797_consys_emi_layout_build(0x40000000,
					MT6797_CONSYS_SHARED_MEM_SIZE, &layout), 0);
	KUNIT_EXPECT_EQ(test, layout.reservation_start, (phys_addr_t)0x40000000);
	KUNIT_EXPECT_EQ(test, layout.reservation_size,
			(resource_size_t)0x200000);
	KUNIT_EXPECT_EQ(test, layout.protection_start, (phys_addr_t)0x40080000);
	KUNIT_EXPECT_EQ(test, layout.protection_end, (phys_addr_t)0x400fffff);
	KUNIT_EXPECT_EQ(test, layout.clear_start, (phys_addr_t)0x40080000);
	KUNIT_EXPECT_EQ(test, layout.clear_size, (resource_size_t)0x55c00);
	KUNIT_EXPECT_EQ(test, layout.protection_region, (u32)19);
	KUNIT_EXPECT_EQ(test, layout.remap_value, (u32)0x1400);

	KUNIT_ASSERT_EQ(test, mt6797_consys_emi_layout_build(0xbfe00000,
					MT6797_CONSYS_SHARED_MEM_SIZE, &layout), 0);
	KUNIT_EXPECT_EQ(test, layout.protection_start, (phys_addr_t)0xbfe80000);
	KUNIT_EXPECT_EQ(test, layout.protection_end, (phys_addr_t)0xbfefffff);
	KUNIT_EXPECT_EQ(test, layout.remap_value, (u32)0x1bfe);
}

static void mt6797_consys_emi_test_layout_rejects_bad_geometry(
		struct kunit *test)
{
	struct mt6797_consys_emi_layout layout;
	phys_addr_t max = (phys_addr_t)~(phys_addr_t)0;

	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_layout_build(0x3fe00000,
					MT6797_CONSYS_SHARED_MEM_SIZE, &layout), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_layout_build(0xc0000000,
					MT6797_CONSYS_SHARED_MEM_SIZE, &layout), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_layout_build(0xbfe00001,
					MT6797_CONSYS_SHARED_MEM_SIZE, &layout), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_layout_build(0x40000000,
					MT6797_CONSYS_SHARED_MEM_SIZE - 1, &layout), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_layout_build(max,
					MT6797_CONSYS_SHARED_MEM_SIZE, &layout), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_layout_build(0x40000000,
					MT6797_CONSYS_SHARED_MEM_SIZE, NULL), -EINVAL);
}

static void mt6797_consys_emi_test_start_order_and_gate(struct kunit *test)
{
	struct mt6797_consys_emi_test_backend backend = {
		.fail_call = MT6797_CONSYS_EMI_TEST_NONE,
	};
	struct mt6797_consys_emi_owner owner;
	struct mt6797_consys_emi_layout layout;

	KUNIT_ASSERT_EQ(test, mt6797_consys_emi_init(&owner,
					&mt6797_consys_emi_test_ops, &backend), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_emi_layout_build(0x40000000,
					MT6797_CONSYS_SHARED_MEM_SIZE, &layout), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_start(&owner, &layout, false),
			-EPERM);
	KUNIT_EXPECT_EQ(test, backend.call_count, (u32)0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_start(&owner, &layout, true), 0);
	KUNIT_EXPECT_EQ(test, backend.call_count, (u32)3);
	KUNIT_EXPECT_EQ(test, backend.calls[0],
			(enum mt6797_consys_emi_test_call)
			MT6797_CONSYS_EMI_TEST_PROTECT);
	KUNIT_EXPECT_EQ(test, backend.calls[1],
			(enum mt6797_consys_emi_test_call)
			MT6797_CONSYS_EMI_TEST_REMAP);
	KUNIT_EXPECT_EQ(test, backend.calls[2],
			(enum mt6797_consys_emi_test_call)
			MT6797_CONSYS_EMI_TEST_MAP_CLEAR);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_state(&owner),
			MT6797_CONSYS_EMI_ACTIVE);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_start(&owner, &layout, true),
			-EBUSY);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_stop(&owner), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, mt6797_consys_emi_state(&owner),
			MT6797_CONSYS_EMI_ACTIVE);
}

static void mt6797_consys_emi_test_failures_are_sticky(struct kunit *test)
{
	static const enum mt6797_consys_emi_test_call failures[] = {
		MT6797_CONSYS_EMI_TEST_PROTECT,
		MT6797_CONSYS_EMI_TEST_REMAP,
		MT6797_CONSYS_EMI_TEST_MAP_CLEAR,
	};
	struct mt6797_consys_emi_layout layout;
	int i;

	KUNIT_ASSERT_EQ(test, mt6797_consys_emi_layout_build(0x40000000,
					MT6797_CONSYS_SHARED_MEM_SIZE, &layout), 0);
	for (i = 0; i < ARRAY_SIZE(failures); i++) {
		struct mt6797_consys_emi_test_backend backend = {
			.fail_call = failures[i],
		};
		struct mt6797_consys_emi_owner owner;

		KUNIT_ASSERT_EQ(test, mt6797_consys_emi_init(&owner,
					&mt6797_consys_emi_test_ops, &backend), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consys_emi_start(&owner, &layout, true),
				-EIO);
		KUNIT_EXPECT_EQ(test, mt6797_consys_emi_state(&owner),
				MT6797_CONSYS_EMI_ERROR);
		KUNIT_EXPECT_EQ(test, backend.call_count, (u32)failures[i] + 1);
		KUNIT_EXPECT_EQ(test, mt6797_consys_emi_start(&owner, &layout, true),
				-EIO);
		KUNIT_EXPECT_EQ(test, mt6797_consys_emi_stop(&owner), -EIO);
	}
}

static struct kunit_case mt6797_consys_emi_test_cases[] = {
	KUNIT_CASE(mt6797_consys_emi_test_layout_values),
	KUNIT_CASE(mt6797_consys_emi_test_layout_rejects_bad_geometry),
	KUNIT_CASE(mt6797_consys_emi_test_start_order_and_gate),
	KUNIT_CASE(mt6797_consys_emi_test_failures_are_sticky),
	{ }
};

static struct kunit_suite mt6797_consys_emi_test_suite = {
	.name = "mt6797-consys-emi",
	.test_cases = mt6797_consys_emi_test_cases,
};
kunit_test_suite(mt6797_consys_emi_test_suite);

MODULE_DESCRIPTION("KUnit tests for the MT6797 CONSYS shared-EMI owner");
MODULE_LICENSE("GPL");
