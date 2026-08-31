// SPDX-License-Identifier: GPL-2.0-only
/* Injected tests for MT6797 A72 platform-state movement attribution. */

#include <kunit/test.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>

#include "mt6797-a72-platform-state-internal.h"

struct mt6797_state_test_context {
	struct mt6797_a72_platform_state samples[2];
	int returns[2];
	unsigned int calls;
};

static int mt6797_state_test_read_once(void *context,
				       struct mt6797_a72_platform_state *sample)
{
	struct mt6797_state_test_context *state = context;
	unsigned int call = state->calls++;

	if (call >= ARRAY_SIZE(state->samples))
		return -EOVERFLOW;
	if (state->returns[call])
		return state->returns[call];
	*sample = state->samples[call];
	return 0;
}

static const struct mt6797_state_capture_ops test_ops = {
	.read_once = mt6797_state_test_read_once,
};

static void mt6797_state_expect_zero(struct kunit *test, const void *actual,
				     size_t size)
{
	u8 zero[sizeof(struct mt6797_a72_platform_state_failure)] = { };

	KUNIT_ASSERT_LE(test, size, sizeof(zero));
	KUNIT_EXPECT_MEMEQ(test, actual, zero, size);
}

static void mt6797_state_stable_test(struct kunit *test)
{
	struct mt6797_state_test_context state = { };
	struct mt6797_a72_platform_state_failure failure;
	struct mt6797_a72_platform_state snapshot;
	int ret;

	state.samples[0].spm_cpu_pwr_status = 0x12345678;
	state.samples[1] = state.samples[0];
	ret = mt6797_a72_platform_state_capture(&test_ops, &state, &snapshot,
						&failure);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state.calls, 2U);
	KUNIT_EXPECT_TRUE(test, snapshot.valid);
	KUNIT_EXPECT_EQ(test, snapshot.spm_cpu_pwr_status, (u32)0x12345678);
	mt6797_state_expect_zero(test, &failure, sizeof(failure));
}

static void mt6797_state_read_errors_test(struct kunit *test)
{
	struct mt6797_state_test_context state = { .returns = { -EIO, 0 } };
	struct mt6797_a72_platform_state_failure failure;
	struct mt6797_a72_platform_state snapshot;
	int ret;

	ret = mt6797_a72_platform_state_capture(&test_ops, &state, &snapshot,
						&failure);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, state.calls, 1U);
	mt6797_state_expect_zero(test, &snapshot, sizeof(snapshot));
	mt6797_state_expect_zero(test, &failure, sizeof(failure));

	state = (struct mt6797_state_test_context){ .returns = { 0, -ENODATA } };
	ret = mt6797_a72_platform_state_capture(&test_ops, &state, &snapshot,
						&failure);
	KUNIT_EXPECT_EQ(test, ret, -ENODATA);
	KUNIT_EXPECT_EQ(test, state.calls, 2U);
	mt6797_state_expect_zero(test, &snapshot, sizeof(snapshot));
	mt6797_state_expect_zero(test, &failure, sizeof(failure));
}

static void mt6797_state_cci_busy_precedence_test(struct kunit *test)
{
	struct mt6797_state_test_context state = { };
	struct mt6797_a72_platform_state_failure failure;
	struct mt6797_a72_platform_state snapshot;
	int ret;

	state.samples[0].cci_status_before = BIT(0);
	state.samples[1].spm_cpu_pwr_status = BIT(6);
	ret = mt6797_a72_platform_state_capture(&test_ops, &state, &snapshot,
						&failure);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_EQ(test, state.calls, 2U);
	KUNIT_EXPECT_TRUE(test, failure.samples_valid);
	KUNIT_EXPECT_EQ(test, failure.movement_mask,
			MT6797_A72_PLATFORM_MOVED_SPM_CPU_PWR_STATUS);
	mt6797_state_expect_zero(test, &snapshot, sizeof(snapshot));
}

static void mt6797_state_set_movement(struct mt6797_a72_platform_state *sample,
				      u32 movement)
{
	switch (movement) {
	case MT6797_A72_PLATFORM_MOVED_SPM_CPU_PWR_STATUS:
		sample->spm_cpu_pwr_status = BIT(6);
		break;
	case MT6797_A72_PLATFORM_MOVED_SPM_CPU_PWR_STATUS_2ND:
		sample->spm_cpu_pwr_status_2nd = BIT(6);
		break;
	case MT6797_A72_PLATFORM_MOVED_MP2_CPUSYS_PWR_CON:
		sample->spm_mp2_cpusys_pwr_con = 1;
		break;
	case MT6797_A72_PLATFORM_MOVED_MP2_CPU0_PWR_CON:
		sample->spm_mp2_cpu0_pwr_con = 1;
		break;
	case MT6797_A72_PLATFORM_MOVED_MP2_CPU1_PWR_CON:
		sample->spm_mp2_cpu1_pwr_con = 1;
		break;
	case MT6797_A72_PLATFORM_MOVED_CPU_EXT_BUCK_ISO:
		sample->spm_cpu_ext_buck_iso = 1;
		break;
	case MT6797_A72_PLATFORM_MOVED_MP2_SYNC_DCM:
		sample->mp2_sync_dcm = BIT(0);
		break;
	case MT6797_A72_PLATFORM_MOVED_CCI_MP2_PORT:
		sample->cci_mp2_port_control = BIT(0);
		break;
	case MT6797_A72_PLATFORM_MOVED_PWRAP_RESET:
		sample->pwrap_reset_asserted = true;
		break;
	}
}

static void mt6797_state_each_movement_test(struct kunit *test)
{
	static const u32 movements[] = {
		MT6797_A72_PLATFORM_MOVED_SPM_CPU_PWR_STATUS,
		MT6797_A72_PLATFORM_MOVED_SPM_CPU_PWR_STATUS_2ND,
		MT6797_A72_PLATFORM_MOVED_MP2_CPUSYS_PWR_CON,
		MT6797_A72_PLATFORM_MOVED_MP2_CPU0_PWR_CON,
		MT6797_A72_PLATFORM_MOVED_MP2_CPU1_PWR_CON,
		MT6797_A72_PLATFORM_MOVED_CPU_EXT_BUCK_ISO,
		MT6797_A72_PLATFORM_MOVED_MP2_SYNC_DCM,
		MT6797_A72_PLATFORM_MOVED_CCI_MP2_PORT,
		MT6797_A72_PLATFORM_MOVED_PWRAP_RESET,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(movements); i++) {
		struct mt6797_state_test_context state = { };
		struct mt6797_a72_platform_state_failure failure;
		struct mt6797_a72_platform_state snapshot;
		int ret;

		mt6797_state_set_movement(&state.samples[1], movements[i]);
		ret = mt6797_a72_platform_state_capture(&test_ops, &state,
							&snapshot, &failure);
		KUNIT_EXPECT_EQ_MSG(test, ret, -EAGAIN, "movement=%x", movements[i]);
		KUNIT_EXPECT_EQ_MSG(test, state.calls, 2U, "movement=%x", movements[i]);
		KUNIT_EXPECT_TRUE(test, failure.samples_valid);
		KUNIT_EXPECT_EQ_MSG(test, failure.movement_mask, movements[i],
				    "movement=%x", movements[i]);
		KUNIT_EXPECT_MEMEQ(test, &failure.first, &state.samples[0],
				   sizeof(failure.first));
		KUNIT_EXPECT_MEMEQ(test, &failure.second, &state.samples[1],
				   sizeof(failure.second));
		mt6797_state_expect_zero(test, &snapshot, sizeof(snapshot));
	}
}

static void mt6797_state_each_a72_identity_bit_test(struct kunit *test)
{
	static const u32 status_bits[] = { BIT(6), BIT(7) };
	unsigned int word;
	unsigned int bit;

	for (word = 0; word < 2; word++) {
		for (bit = 0; bit < ARRAY_SIZE(status_bits); bit++) {
			struct mt6797_state_test_context state = { };
			struct mt6797_a72_platform_state_failure failure;
			struct mt6797_a72_platform_state snapshot;
			u32 expected;
			int ret;

			if (word == 0) {
				state.samples[1].spm_cpu_pwr_status = status_bits[bit];
				expected = MT6797_A72_PLATFORM_MOVED_SPM_CPU_PWR_STATUS;
			} else {
				state.samples[1].spm_cpu_pwr_status_2nd = status_bits[bit];
				expected = MT6797_A72_PLATFORM_MOVED_SPM_CPU_PWR_STATUS_2ND;
			}
			ret = mt6797_a72_platform_state_capture(&test_ops, &state,
								&snapshot, &failure);
			KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
			KUNIT_EXPECT_EQ(test, state.calls, 2U);
			KUNIT_EXPECT_TRUE(test, failure.samples_valid);
			KUNIT_EXPECT_EQ(test, failure.movement_mask, expected);
			mt6797_state_expect_zero(test, &snapshot, sizeof(snapshot));
		}
	}
}

static void mt6797_state_masked_noise_test(struct kunit *test)
{
	struct mt6797_state_test_context state = { };
	struct mt6797_a72_platform_state_failure failure;
	struct mt6797_a72_platform_state snapshot;
	int ret;

	state.samples[0].spm_cpu_pwr_status = 0x003dcf08;
	state.samples[1].spm_cpu_pwr_status = 0x003dc708;
	state.samples[0].spm_cpu_pwr_status_2nd = 0x003defff;
	state.samples[1].spm_cpu_pwr_status_2nd = 0x003dc7ff;
	state.samples[1].spm_pwr_status = 1;
	state.samples[1].spm_pwr_status_2nd = 1;
	state.samples[1].mp2_sync_dcm = BIT(7);
	state.samples[1].cci_mp2_port_control = BIT(2);
	ret = mt6797_a72_platform_state_capture(&test_ops, &state, &snapshot,
						&failure);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state.calls, 2U);
	KUNIT_EXPECT_TRUE(test, snapshot.valid);
	KUNIT_EXPECT_EQ(test, snapshot.spm_cpu_pwr_status,
			(u32)0x003dc708);
	KUNIT_EXPECT_EQ(test, snapshot.spm_cpu_pwr_status_2nd,
			(u32)0x003dc7ff);
	mt6797_state_expect_zero(test, &failure, sizeof(failure));
}

static struct kunit_case mt6797_state_cases[] = {
	KUNIT_CASE(mt6797_state_stable_test),
	KUNIT_CASE(mt6797_state_read_errors_test),
	KUNIT_CASE(mt6797_state_cci_busy_precedence_test),
	KUNIT_CASE(mt6797_state_each_movement_test),
	KUNIT_CASE(mt6797_state_each_a72_identity_bit_test),
	KUNIT_CASE(mt6797_state_masked_noise_test),
	{ }
};

static struct kunit_suite mt6797_state_suite = {
	.name = "mt6797-a72-platform-state-movement",
	.test_cases = mt6797_state_cases,
};

kunit_test_suite(mt6797_state_suite);

MODULE_LICENSE("GPL");
