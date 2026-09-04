// SPDX-License-Identifier: GPL-2.0-only
/* Failure matrix for the hardware-independent MT6797 MD1 power model. */

#include <kunit/test.h>
#include <linux/errno.h>

#include "power_transaction.h"

#define MT6797_MD1_TEST_ACK_ATTEMPTS	3
#define MT6797_MD1_TEST_INITIAL_UV	900000
#define MT6797_MD1_TEST_VSRAM_MD_UV	987500
#define MT6797_MD1_TEST_VMODEM_UV	1000000

enum mt6797_md1_test_step {
	TEST_ENABLE_VSRAM_MD,
	TEST_ENABLE_HW_VSRAM_MD,
	TEST_ENABLE_VMD1,
	TEST_ENABLE_HW_VMD1,
	TEST_ENABLE_VMODEM,
	TEST_ENABLE_HW_VMODEM,
	TEST_VOLTAGE_VSRAM_MD,
	TEST_VOLTAGE_VMODEM,
	TEST_VOLTAGE_HW_VSRAM_MD,
	TEST_VOLTAGE_HW_VMD1,
	TEST_VOLTAGE_HW_VMODEM,
	TEST_DOMAIN_CONTROL_BASE,
};

struct mt6797_md1_power_event {
	unsigned int step;
	bool forward;
};

struct mt6797_md1_power_test_context {
	struct mt6797_md1_power_event
		events[MT6797_MD1_POWER_MUTATION_COUNT * 2];
	bool rails[MT6797_MD1_POWER_RAIL_COUNT];
	bool rail_hw_control[MT6797_MD1_POWER_RAIL_COUNT]
			    [MT6797_MD1_RAIL_HW_CONTROL_COUNT];
	bool controls[MT6797_MD1_POWER_CONTROL_COUNT];
	int voltage_uv[MT6797_MD1_POWER_RAIL_COUNT];
	int target_voltage_uv[MT6797_MD1_POWER_RAIL_COUNT];
	unsigned int event_count;
	unsigned int delay_calls;
	unsigned int settle_calls;
	unsigned int settle_usecs;
	unsigned int force_power_off_reads;
	unsigned int force_bus_on_reads;
	int fail_forward_step;
	int fail_unwind_step;
	int ignore_forward_step;
	int settle_ret;
	bool leave_changed_on_failure;
	bool force_power_on_timeout;
	bool force_bus_release_timeout;
	bool power_was_on;
	bool bus_was_released;
};

static bool
mt6797_md1_test_control_forward(enum mt6797_md1_power_control control)
{
	return control == MT6797_MD1_CTRL_PWR_ON ||
	       control == MT6797_MD1_CTRL_PWR_ON_2ND;
}

static void
mt6797_md1_power_test_init(struct mt6797_md1_power_test_context *context)
{
	unsigned int i;

	memset(context, 0, sizeof(*context));
	for (i = 0; i < MT6797_MD1_POWER_RAIL_COUNT; i++)
		context->voltage_uv[i] = MT6797_MD1_TEST_INITIAL_UV;
	context->target_voltage_uv[MT6797_MD1_RAIL_VSRAM_MD] =
		MT6797_MD1_TEST_VSRAM_MD_UV;
	context->target_voltage_uv[MT6797_MD1_RAIL_VMODEM] =
		MT6797_MD1_TEST_VMODEM_UV;
	context->controls[MT6797_MD1_CTRL_EXT_BUCK_ISO] = true;
	context->controls[MT6797_MD1_CTRL_PWR_CLK_DIS] = true;
	context->controls[MT6797_MD1_CTRL_PWR_ISO] = true;
	context->controls[MT6797_MD1_CTRL_SRAM_PDN] = true;
	context->controls[MT6797_MD1_CTRL_BUS_PROTECT] = true;
	context->controls[MT6797_MD1_CTRL_RESET_ASSERT] = true;
	context->fail_forward_step = -1;
	context->fail_unwind_step = -1;
	context->ignore_forward_step = -1;
}

static int
mt6797_md1_power_test_event(struct mt6797_md1_power_test_context *context,
			    unsigned int step, bool forward, bool *change)
{
	bool fail = (forward && step == context->fail_forward_step) ||
		    (!forward && step == context->fail_unwind_step);

	context->events[context->event_count].step = step;
	context->events[context->event_count++].forward = forward;
	*change = (!fail || !context->leave_changed_on_failure) &&
		  (!forward || step != context->ignore_forward_step);

	if (!fail)
		return 0;
	return forward ? -EIO : -EREMOTEIO;
}

static unsigned int
mt6797_md1_test_rail_step(enum mt6797_md1_power_rail rail)
{
	return rail * 2;
}

static unsigned int
test_hw_step(enum mt6797_md1_power_rail rail,
	     enum mt6797_md1_rail_hw_control control)
{
	if (control == MT6797_MD1_RAIL_ENABLE_HW_CONTROL)
		return rail * 2 + 1;
	return TEST_VOLTAGE_HW_VSRAM_MD + rail;
}

static int mt6797_md1_power_test_set_rail(void *data, enum mt6797_md1_power_rail rail, bool enable)
{
	struct mt6797_md1_power_test_context *context = data;
	bool change;
	int ret;

	ret = mt6797_md1_power_test_event(context,
					  mt6797_md1_test_rail_step(rail),
					  enable, &change);
	if (change)
		context->rails[rail] = enable;
	return ret;
}

static int
mt6797_md1_power_test_set_voltage(void *data,
				  enum mt6797_md1_power_rail rail,
				  int voltage_uv)
{
	struct mt6797_md1_power_test_context *context = data;
	unsigned int step;
	bool change;
	bool forward;
	int ret;

	step = rail == MT6797_MD1_RAIL_VSRAM_MD ?
		TEST_VOLTAGE_VSRAM_MD : TEST_VOLTAGE_VMODEM;
	forward = voltage_uv == context->target_voltage_uv[rail];
	ret = mt6797_md1_power_test_event(context, step, forward, &change);
	if (change)
		context->voltage_uv[rail] = voltage_uv;
	return ret;
}

static int
test_set_hw_control(void *data, enum mt6797_md1_power_rail rail,
		    enum mt6797_md1_rail_hw_control control, bool enable)
{
	struct mt6797_md1_power_test_context *context = data;
	bool change;
	int ret;

	ret = mt6797_md1_power_test_event(context,
					  test_hw_step(rail, control),
					  enable, &change);
	if (change)
		context->rail_hw_control[rail][control] = enable;
	return ret;
}

static int
mt6797_md1_power_test_set_control(void *data,
				  enum mt6797_md1_power_control control,
				  bool assert)
{
	struct mt6797_md1_power_test_context *context = data;
	bool change;
	bool forward;
	int ret;

	forward = assert == mt6797_md1_test_control_forward(control);
	ret = mt6797_md1_power_test_event(context,
					  TEST_DOMAIN_CONTROL_BASE + control,
					  forward, &change);
	if (change)
		context->controls[control] = assert;
	if (control == MT6797_MD1_CTRL_PWR_ON_2ND && forward)
		context->power_was_on = true;
	if (control == MT6797_MD1_CTRL_BUS_PROTECT && forward)
		context->bus_was_released = true;
	return ret;
}

static int test_read_rail(void *data, enum mt6797_md1_power_rail rail,
			  bool *enabled)
{
	struct mt6797_md1_power_test_context *context = data;

	*enabled = context->rails[rail];
	return 0;
}

static int
mt6797_md1_power_test_read_voltage(void *data,
				   enum mt6797_md1_power_rail rail,
				   int *voltage_uv)
{
	struct mt6797_md1_power_test_context *context = data;

	*voltage_uv = context->voltage_uv[rail];
	return 0;
}

static int
test_read_hw_control(void *data, enum mt6797_md1_power_rail rail,
		     enum mt6797_md1_rail_hw_control control, bool *enabled)
{
	struct mt6797_md1_power_test_context *context = data;

	*enabled = context->rail_hw_control[rail][control];
	return 0;
}

static int
mt6797_md1_power_test_read_control(void *data,
				   enum mt6797_md1_power_control control,
				   bool *asserted)
{
	struct mt6797_md1_power_test_context *context = data;

	*asserted = context->controls[control];
	return 0;
}

static int mt6797_md1_power_test_read_power(void *data, bool *primary,
					    bool *secondary)
{
	struct mt6797_md1_power_test_context *context = data;

	*primary = context->controls[MT6797_MD1_CTRL_PWR_ON];
	*secondary = context->controls[MT6797_MD1_CTRL_PWR_ON_2ND];
	if (*primary && *secondary && context->force_power_on_timeout) {
		*primary = false;
		*secondary = false;
	} else if (!*primary && !*secondary && context->power_was_on &&
		   context->force_power_off_reads) {
		context->force_power_off_reads--;
		*primary = true;
		*secondary = true;
	}

	return 0;
}

static int mt6797_md1_power_test_read_bus(void *data, bool *protected)
{
	struct mt6797_md1_power_test_context *context = data;

	*protected = context->controls[MT6797_MD1_CTRL_BUS_PROTECT];
	if (!*protected && context->force_bus_release_timeout) {
		*protected = true;
	} else if (*protected && context->bus_was_released &&
		   context->force_bus_on_reads) {
		context->force_bus_on_reads--;
		*protected = false;
	}

	return 0;
}

static int mt6797_md1_power_test_settle(void *data, unsigned int usec)
{
	struct mt6797_md1_power_test_context *context = data;

	context->settle_calls++;
	context->settle_usecs = usec;
	return context->settle_ret;
}

static void mt6797_md1_power_test_delay(void *data)
{
	struct mt6797_md1_power_test_context *context = data;

	context->delay_calls++;
}

static const struct mt6797_md1_power_ops mt6797_md1_power_test_ops = {
	.set_rail = mt6797_md1_power_test_set_rail,
	.set_rail_voltage = mt6797_md1_power_test_set_voltage,
	.set_rail_hw_control = test_set_hw_control,
	.set_control = mt6797_md1_power_test_set_control,
	.read_rail = test_read_rail,
	.read_rail_voltage = mt6797_md1_power_test_read_voltage,
	.read_rail_hw_control = test_read_hw_control,
	.read_control = mt6797_md1_power_test_read_control,
	.read_power_ack = mt6797_md1_power_test_read_power,
	.read_bus_protected = mt6797_md1_power_test_read_bus,
	.settle_rails = mt6797_md1_power_test_settle,
	.ack_delay = mt6797_md1_power_test_delay,
};

static int
mt6797_md1_power_test_run(struct mt6797_md1_power_test_context *context)
{
	const struct mt6797_md1_power_req requirements = {
		.vsram_md_uv = MT6797_MD1_TEST_VSRAM_MD_UV,
		.vmodem_uv = MT6797_MD1_TEST_VMODEM_UV,
	};

	return mt6797_md1_power_transaction(&requirements,
					    &mt6797_md1_power_test_ops, context,
					    MT6797_MD1_TEST_ACK_ATTEMPTS);
}

static void
mt6797_md1_power_expect_off(struct kunit *test,
			    struct mt6797_md1_power_test_context *context)
{
	bool expected;
	unsigned int control;
	unsigned int rail;

	for (rail = 0; rail < MT6797_MD1_POWER_RAIL_COUNT; rail++) {
		KUNIT_EXPECT_FALSE(test, context->rails[rail]);
		KUNIT_EXPECT_EQ(test, context->voltage_uv[rail],
				MT6797_MD1_TEST_INITIAL_UV);
		KUNIT_EXPECT_FALSE(test,
				   context->rail_hw_control[rail]
						   [MT6797_MD1_RAIL_ENABLE_HW_CONTROL]);
		KUNIT_EXPECT_FALSE(test,
				   context->rail_hw_control[rail]
						   [MT6797_MD1_RAIL_VOLTAGE_HW_CONTROL]);
	}
	for (control = 0; control < MT6797_MD1_POWER_CONTROL_COUNT;
	     control++) {
		expected = !mt6797_md1_test_control_forward(control);
		KUNIT_EXPECT_EQ(test, context->controls[control], expected);
	}
}

static void
mt6797_md1_power_expect_order(struct kunit *test,
			      struct mt6797_md1_power_test_context *context,
			      unsigned int attempted)
{
	unsigned int i;

	KUNIT_ASSERT_EQ(test, context->event_count, attempted * 2);
	for (i = 0; i < attempted; i++) {
		KUNIT_EXPECT_EQ(test, context->events[i].step, i);
		KUNIT_EXPECT_TRUE(test, context->events[i].forward);
		KUNIT_EXPECT_EQ(test,
				context->events[attempted + i].step,
				attempted - i - 1);
		KUNIT_EXPECT_FALSE(test,
				   context->events[attempted + i].forward);
	}
}

static void mt6797_md1_power_invalid_arguments_test(struct kunit *test)
{
	struct mt6797_md1_power_test_context context;
	struct mt6797_md1_power_req requirements = {
		.vsram_md_uv = MT6797_MD1_TEST_VSRAM_MD_UV,
		.vmodem_uv = MT6797_MD1_TEST_VMODEM_UV,
	};
	struct mt6797_md1_power_ops ops;
	int ret;

	mt6797_md1_power_test_init(&context);
	ret = mt6797_md1_power_transaction(&requirements, NULL, &context, 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_md1_power_transaction(&requirements,
					   &mt6797_md1_power_test_ops, NULL, 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_md1_power_transaction(&requirements,
					   &mt6797_md1_power_test_ops, &context, 0);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_md1_power_transaction(NULL, &mt6797_md1_power_test_ops,
					   &context, 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	requirements.vsram_md_uv = 0;
	ret = mt6797_md1_power_transaction(&requirements,
					   &mt6797_md1_power_test_ops, &context, 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	requirements.vsram_md_uv = MT6797_MD1_TEST_VSRAM_MD_UV;
	requirements.vmodem_uv = -1;
	ret = mt6797_md1_power_transaction(&requirements,
					   &mt6797_md1_power_test_ops, &context, 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	requirements.vmodem_uv = MT6797_MD1_TEST_VMODEM_UV;

#define EXPECT_NULL_OP_REJECTED(member) do { \
	ops = mt6797_md1_power_test_ops; \
	ops.member = NULL; \
	ret = mt6797_md1_power_transaction(&requirements, &ops, &context, 1); \
	KUNIT_EXPECT_EQ(test, ret, -EINVAL); \
} while (0)
	EXPECT_NULL_OP_REJECTED(set_rail);
	EXPECT_NULL_OP_REJECTED(set_rail_voltage);
	EXPECT_NULL_OP_REJECTED(set_rail_hw_control);
	EXPECT_NULL_OP_REJECTED(set_control);
	EXPECT_NULL_OP_REJECTED(read_rail);
	EXPECT_NULL_OP_REJECTED(read_rail_voltage);
	EXPECT_NULL_OP_REJECTED(read_rail_hw_control);
	EXPECT_NULL_OP_REJECTED(read_control);
	EXPECT_NULL_OP_REJECTED(read_power_ack);
	EXPECT_NULL_OP_REJECTED(read_bus_protected);
	EXPECT_NULL_OP_REJECTED(settle_rails);
	EXPECT_NULL_OP_REJECTED(ack_delay);
#undef EXPECT_NULL_OP_REJECTED

	KUNIT_EXPECT_EQ(test, context.event_count, 0U);
}

static void mt6797_md1_power_success_test(struct kunit *test)
{
	struct mt6797_md1_power_test_context context;
	int ret;

	mt6797_md1_power_test_init(&context);
	ret = mt6797_md1_power_test_run(&context);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, context.settle_calls, 1U);
	KUNIT_EXPECT_EQ(test, context.settle_usecs,
			MT6797_MD1_RAIL_SETTLE_US);
	mt6797_md1_power_expect_order(test, &context,
				      MT6797_MD1_POWER_MUTATION_COUNT);
	mt6797_md1_power_expect_off(test, &context);
}

static void mt6797_md1_power_settle_failure_test(struct kunit *test)
{
	struct mt6797_md1_power_test_context context;
	int ret;

	mt6797_md1_power_test_init(&context);
	context.settle_ret = -EIO;
	ret = mt6797_md1_power_test_run(&context);

	KUNIT_EXPECT_EQ(test, ret, -EIO);
	mt6797_md1_power_expect_order(test, &context,
				      TEST_VOLTAGE_VMODEM + 1);
	mt6797_md1_power_expect_off(test, &context);
}

static void mt6797_md1_power_contract_readback_test(struct kunit *test)
{
	struct mt6797_md1_power_test_context context;
	int ret;

	mt6797_md1_power_test_init(&context);
	context.ignore_forward_step = TEST_VOLTAGE_VSRAM_MD;
	ret = mt6797_md1_power_test_run(&context);

	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	mt6797_md1_power_expect_order(test, &context,
				      TEST_DOMAIN_CONTROL_BASE);
	mt6797_md1_power_expect_off(test, &context);
}

static void mt6797_md1_power_forward_failure_matrix_test(struct kunit *test)
{
	struct mt6797_md1_power_test_context context;
	unsigned int failure;
	int ret;

	for (failure = 0; failure < MT6797_MD1_POWER_MUTATION_COUNT;
	     failure++) {
		mt6797_md1_power_test_init(&context);
		context.fail_forward_step = failure;
		ret = mt6797_md1_power_test_run(&context);

		KUNIT_EXPECT_EQ_MSG(test, ret, -EIO, "step %u", failure);
		mt6797_md1_power_expect_order(test, &context, failure + 1);
		mt6797_md1_power_expect_off(test, &context);
	}
}

static void mt6797_md1_power_unwind_failure_matrix_test(struct kunit *test)
{
	struct mt6797_md1_power_test_context context;
	unsigned int failure;
	int ret;

	for (failure = 0; failure < MT6797_MD1_POWER_MUTATION_COUNT;
	     failure++) {
		mt6797_md1_power_test_init(&context);
		context.fail_unwind_step = failure;
		ret = mt6797_md1_power_test_run(&context);

		KUNIT_EXPECT_EQ_MSG(test, ret, -EREMOTEIO, "step %u",
				    failure);
		mt6797_md1_power_expect_order(test, &context,
					      MT6797_MD1_POWER_MUTATION_COUNT);
		mt6797_md1_power_expect_off(test, &context);
	}
}

static void mt6797_md1_power_ack_timeout_test(struct kunit *test)
{
	struct mt6797_md1_power_test_context context;
	int ret;

	mt6797_md1_power_test_init(&context);
	context.force_power_on_timeout = true;
	ret = mt6797_md1_power_test_run(&context);
	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, context.delay_calls,
			MT6797_MD1_TEST_ACK_ATTEMPTS - 1);
	mt6797_md1_power_expect_order(test, &context,
				      TEST_DOMAIN_CONTROL_BASE + 3);
	mt6797_md1_power_expect_off(test, &context);

	mt6797_md1_power_test_init(&context);
	context.force_bus_release_timeout = true;
	ret = mt6797_md1_power_test_run(&context);
	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, context.delay_calls,
			MT6797_MD1_TEST_ACK_ATTEMPTS - 1);
	mt6797_md1_power_expect_order(test, &context,
				      MT6797_MD1_POWER_MUTATION_COUNT - 1);
	mt6797_md1_power_expect_off(test, &context);
}

static void mt6797_md1_power_unwind_ack_timeout_test(struct kunit *test)
{
	struct mt6797_md1_power_test_context context;
	int ret;

	mt6797_md1_power_test_init(&context);
	context.force_power_off_reads = MT6797_MD1_TEST_ACK_ATTEMPTS;
	ret = mt6797_md1_power_test_run(&context);
	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	mt6797_md1_power_expect_off(test, &context);

	mt6797_md1_power_test_init(&context);
	context.force_bus_on_reads = MT6797_MD1_TEST_ACK_ATTEMPTS;
	ret = mt6797_md1_power_test_run(&context);
	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	mt6797_md1_power_expect_off(test, &context);
}

static void mt6797_md1_power_final_verify_test(struct kunit *test)
{
	struct mt6797_md1_power_test_context context;
	int ret;

	mt6797_md1_power_test_init(&context);
	context.fail_unwind_step = TEST_ENABLE_VSRAM_MD;
	context.leave_changed_on_failure = true;
	ret = mt6797_md1_power_test_run(&context);

	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_TRUE(test, context.rails[MT6797_MD1_RAIL_VSRAM_MD]);
}

static void mt6797_md1_power_initial_state_test(struct kunit *test)
{
	struct mt6797_md1_power_test_context context;
	int ret;

	mt6797_md1_power_test_init(&context);
	context.rails[MT6797_MD1_RAIL_VMD1] = true;
	ret = mt6797_md1_power_test_run(&context);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_EQ(test, context.event_count, 0U);

	mt6797_md1_power_test_init(&context);
	context.controls[MT6797_MD1_CTRL_PWR_ON] = true;
	ret = mt6797_md1_power_test_run(&context);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_EQ(test, context.event_count, 0U);

	mt6797_md1_power_test_init(&context);
	context.controls[MT6797_MD1_CTRL_BUS_PROTECT] = false;
	ret = mt6797_md1_power_test_run(&context);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_EQ(test, context.event_count, 0U);
}

static struct kunit_case mt6797_md1_power_transaction_cases[] = {
	KUNIT_CASE(mt6797_md1_power_invalid_arguments_test),
	KUNIT_CASE(mt6797_md1_power_success_test),
	KUNIT_CASE(mt6797_md1_power_settle_failure_test),
	KUNIT_CASE(mt6797_md1_power_contract_readback_test),
	KUNIT_CASE(mt6797_md1_power_forward_failure_matrix_test),
	KUNIT_CASE(mt6797_md1_power_unwind_failure_matrix_test),
	KUNIT_CASE(mt6797_md1_power_ack_timeout_test),
	KUNIT_CASE(mt6797_md1_power_unwind_ack_timeout_test),
	KUNIT_CASE(mt6797_md1_power_final_verify_test),
	KUNIT_CASE(mt6797_md1_power_initial_state_test),
	{}
};

static struct kunit_suite mt6797_md1_power_transaction_suite = {
	.name = "mt6797-md1-power-transaction",
	.test_cases = mt6797_md1_power_transaction_cases,
};

kunit_test_suite(mt6797_md1_power_transaction_suite);

MODULE_LICENSE("GPL");
