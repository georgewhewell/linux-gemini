// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only identification driver for legacy direct-address DA9213-DA9215.
 *
 * The default build is identification-only. An explicitly isolated provider
 * build may register read-only resource descriptors after the same fixed
 * transcript; it never exposes a writable regulator operation or A72 consumer.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/driver.h>

#include "da9213-legacy-observer.h"
#if IS_ENABLED(CONFIG_ARM64_MT6797_A72_PROVIDER_OWNER)
#include <linux/mt6797-a72-provider.h>
#endif

#define DA9213_LEGACY_PRIMARY_ADDR	0x68
#define DA9213_LEGACY_PAGE2_ADDR	0x69
#define DA9213_LEGACY_PASSES		2

enum da9213_legacy_bank {
	DA9213_LEGACY_PRIMARY,
	DA9213_LEGACY_PAGE2,
};

struct da9213_legacy_variant {
	const char *name;
};

struct da9213_legacy_sample {
	enum da9213_legacy_bank bank;
	u8 reg;
	u8 expected;
};

struct da9213_legacy {
	struct device *dev;
	struct i2c_client *client;
	struct i2c_client *page2;
	const struct da9213_legacy_variant *variant;
#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_PROVIDER)
	struct regulator_dev *rdev[2];
#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_OBSERVER)
	unsigned int provider_count;
	bool observer_bound;
	struct da9213_legacy_observation observation;
#endif
#endif
};

static const struct da9213_legacy_variant da9213_legacy_da9213 = {
	.name = "DA9213",
};

static const struct da9213_legacy_variant da9213_legacy_da9214 = {
	.name = "DA9214",
};

static const struct da9213_legacy_variant da9213_legacy_da9215 = {
	.name = "DA9215",
};

static const struct da9213_legacy_sample da9213_legacy_samples[] = {
	{ DA9213_LEGACY_PAGE2,   0x05, 0xd9 },
	{ DA9213_LEGACY_PAGE2,   0x06, 0xd0 },
	{ DA9213_LEGACY_PAGE2,   0x47, 0xc0 },
	{ DA9213_LEGACY_PRIMARY, 0xd3, 0x1f },
	{ DA9213_LEGACY_PRIMARY, 0x5e, 0x00 },
	{ DA9213_LEGACY_PRIMARY, 0xd9, 0x46 },
	{ DA9213_LEGACY_PRIMARY, 0xda, 0x46 },
};

#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_PROVIDER)

static const u8 da9213_legacy_vsel_reg[DA9213_LEGACY_BUCK_COUNT] = {
	0xd7, 0xd9,
};

static const u8 da9213_legacy_cont_reg[DA9213_LEGACY_BUCK_COUNT] = {
	0x5d, 0x5e,
};

/*
 * A72 admission consumes a stable, read-only DA9213 state snapshot.  Keep
 * this separate from regulator consumer operations: it does not request a
 * vote, select a voltage, or write any register.
 */
#define DA9213_LEGACY_CONTROL_A_REG	0x56
#define DA9213_LEGACY_STATUS_B_REG	0x51
#define DA9213_LEGACY_PROVIDER_SNAPSHOT_PASSES	2

static const u8 da9213_legacy_provider_snapshot_regs[] = {
	DA9213_LEGACY_CONTROL_A_REG,
	DA9213_LEGACY_STATUS_B_REG,
	0x5e, /* BUCKB_CONT */
	0xd9, /* VBUCKB_A */
	0xda, /* VBUCKB_B */
};

static int da9213_legacy_read_reg_locked(struct da9213_legacy *chip, u8 reg,
					 unsigned int *value)
{
	struct i2c_msg msgs[2] = { };
	u8 pointer = reg;
	u8 data = 0;
	int ret;

	msgs[0].addr = chip->client->addr;
	msgs[0].len = 1;
	msgs[0].buf = &pointer;
	msgs[1].addr = chip->client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = &data;

	ret = __i2c_transfer(chip->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*value = data;
	return 0;
}

static int da9213_legacy_read_reg(struct da9213_legacy *chip, u8 reg,
				  unsigned int *value)
{
	int ret;

	i2c_lock_bus(chip->client->adapter, I2C_LOCK_ROOT_ADAPTER);
	ret = da9213_legacy_read_reg_locked(chip, reg, value);
	i2c_unlock_bus(chip->client->adapter, I2C_LOCK_ROOT_ADAPTER);
	return ret;
}

static int da9213_legacy_get_voltage_sel(struct regulator_dev *rdev)
{
	struct da9213_legacy *chip = rdev_get_drvdata(rdev);
	unsigned int value;
	int ret;

	ret = da9213_legacy_read_reg(chip,
				     da9213_legacy_vsel_reg[rdev_get_id(rdev)],
				     &value);
	if (ret)
		return ret;

	return value & DA9213_LEGACY_VSEL_MASK;
}

static int da9213_legacy_is_enabled(struct regulator_dev *rdev)
{
	struct da9213_legacy *chip = rdev_get_drvdata(rdev);
	unsigned int value;
	int ret;

	ret = da9213_legacy_read_reg(chip,
				     da9213_legacy_cont_reg[rdev_get_id(rdev)],
				     &value);
	if (ret)
		return ret;

	return !!(value & DA9213_LEGACY_ENABLE_MASK);
}

static const struct regulator_ops da9213_legacy_readonly_ops = {
	.get_voltage_sel = da9213_legacy_get_voltage_sel,
	.list_voltage = regulator_list_voltage_linear,
	.is_enabled = da9213_legacy_is_enabled,
};

#define DA9213_LEGACY_BUCK(_id)						\
{								\
	.name = "DA9213-legacy-BUCK" #_id,				\
	.ops = &da9213_legacy_readonly_ops,				\
	.type = REGULATOR_VOLTAGE,					\
	.id = _id,							\
	.n_voltages = (DA9213_LEGACY_MAX_UV -			\
				DA9213_LEGACY_MIN_UV) / DA9213_LEGACY_STEP_UV + 1,\
	.min_uV = DA9213_LEGACY_MIN_UV,				\
	.uV_step = DA9213_LEGACY_STEP_UV,				\
	.owner = THIS_MODULE,					\
}

static const struct regulator_desc da9213_legacy_regulators[] = {
	DA9213_LEGACY_BUCK(0),
	DA9213_LEGACY_BUCK(1),
};

static int da9213_legacy_register_provider(struct da9213_legacy *chip)
{
	struct regulator_config config = {
		.dev = chip->dev,
		.driver_data = chip,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(da9213_legacy_regulators); i++) {
		chip->rdev[i] = devm_regulator_register(chip->dev,
						&da9213_legacy_regulators[i], &config);
		if (IS_ERR(chip->rdev[i]))
			return PTR_ERR(chip->rdev[i]);
#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_OBSERVER)
		chip->provider_count++;
#endif
	}

	return 0;
}

#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_OBSERVER)

int da9213_legacy_observer_collect(
	da9213_legacy_observer_read_fn read, void *context,
	unsigned int identity_reads, unsigned int provider_count,
	struct da9213_legacy_observation *observation)
{
	unsigned int buck;

	if (!observation)
		return -EINVAL;

	memset(observation, 0, sizeof(*observation));
	observation->identity_reads = identity_reads;
	observation->provider_count = provider_count;
	if (!read || identity_reads != DA9213_LEGACY_PASSES *
					 ARRAY_SIZE(da9213_legacy_samples) ||
	    provider_count != DA9213_LEGACY_BUCK_COUNT)
		return -EINVAL;

	for (buck = 0; buck < DA9213_LEGACY_BUCK_COUNT; buck++) {
		unsigned int value;
		int ret;

		observation->provider_read_attempts++;
		ret = read(context, buck, DA9213_LEGACY_OBSERVER_SELECTOR,
			   &value);
		if (ret)
			return ret;
		observation->provider_read_completed++;
		if (value > DA9213_LEGACY_VSEL_MASK)
			return -ERANGE;
		observation->buck[buck].selector = value;
		observation->buck[buck].microvolts =
			DA9213_LEGACY_MIN_UV + value * DA9213_LEGACY_STEP_UV;
		if (observation->buck[buck].microvolts >
		    DA9213_LEGACY_MAX_UV)
			return -ERANGE;

		observation->provider_read_attempts++;
		ret = read(context, buck, DA9213_LEGACY_OBSERVER_ENABLED,
			   &value);
		if (ret)
			return ret;
		observation->provider_read_completed++;
		if (value > 1)
			return -ERANGE;
		observation->buck[buck].enabled = value;
	}

	observation->valid = true;
	return 0;
}

void da9213_legacy_observer_cleanup_state(
	struct da9213_legacy_observation *observation,
	unsigned int *provider_count)
{
	if (!observation || !provider_count)
		return;

	observation->valid = false;
	observation->provider_count = 0;
	*provider_count = 0;
}

static int da9213_legacy_observer_read(
	void *context, unsigned int buck,
	enum da9213_legacy_observer_field field, unsigned int *value)
{
	struct da9213_legacy *chip = context;
	int ret;

	if (!chip || !value || buck >= chip->provider_count)
		return -EINVAL;

	switch (field) {
	case DA9213_LEGACY_OBSERVER_SELECTOR:
		ret = da9213_legacy_get_voltage_sel(chip->rdev[buck]);
		break;
	case DA9213_LEGACY_OBSERVER_ENABLED:
		ret = da9213_legacy_is_enabled(chip->rdev[buck]);
		break;
	default:
		return -EINVAL;
	}
	if (ret < 0)
		return ret;

	*value = ret;
	return 0;
}

static void da9213_legacy_observer_cleanup(void *context)
{
	struct da9213_legacy *chip = context;
	unsigned int providers = chip->provider_count;

	da9213_legacy_observer_cleanup_state(&chip->observation,
				      &chip->provider_count);
	dev_info(chip->dev,
		 "da921x-observer-v1 event=%s providers_released=%u "
		 "register_data_writes=%u\n",
		 chip->observer_bound ? "unbind" : "failed-probe",
		 providers, chip->observation.register_data_writes);
}

static int da9213_legacy_observer_prepare(struct da9213_legacy *chip)
{
	return devm_add_action(chip->dev, da9213_legacy_observer_cleanup,
			       chip);
}

static int da9213_legacy_observer_sample(struct da9213_legacy *chip,
				     unsigned int identity_reads)
{
	return da9213_legacy_observer_collect(
		da9213_legacy_observer_read, chip, identity_reads,
		chip->provider_count, &chip->observation);
}

static void da9213_legacy_observer_publish(struct da9213_legacy *chip)
{
	const struct da9213_legacy_observation *observation =
		&chip->observation;

	chip->observer_bound = true;
	dev_info(chip->dev,
		 "da921x-observer-v1 event=bound valid=%u identity_reads=%u "
		 "providers=%u provider_read_attempts=%u "
		 "provider_read_completed=%u register_data_writes=%u "
		 "buck0_selector=%u buck0_uv=%u buck0_enabled=%u "
		 "buck1_selector=%u buck1_uv=%u buck1_enabled=%u\n",
		 observation->valid, observation->identity_reads,
		 observation->provider_count,
		 observation->provider_read_attempts,
		 observation->provider_read_completed,
		 observation->register_data_writes,
		 observation->buck[0].selector,
		 observation->buck[0].microvolts,
		 observation->buck[0].enabled,
		 observation->buck[1].selector,
		 observation->buck[1].microvolts,
		 observation->buck[1].enabled);
}

#endif

#if IS_ENABLED(CONFIG_ARM64_MT6797_A72_PROVIDER_OWNER)

static int da9213_legacy_provider_snapshot(
	void *context, struct mt6797_a72_provider_snapshot *state)
{
	struct da9213_legacy *chip = context;
	unsigned int sample[DA9213_LEGACY_PROVIDER_SNAPSHOT_PASSES]
		[ARRAY_SIZE(da9213_legacy_provider_snapshot_regs)] = { };
	unsigned int pass;
	unsigned int reg;
	int ret = 0;

	if (!chip || !chip->client || !state)
		return -EINVAL;
	memset(state, 0, sizeof(*state));

	/* One root-adapter lock encloses both samples; each transfer is read-only. */
	i2c_lock_bus(chip->client->adapter, I2C_LOCK_ROOT_ADAPTER);
	for (pass = 0; pass < DA9213_LEGACY_PROVIDER_SNAPSHOT_PASSES; pass++) {
		for (reg = 0; reg < ARRAY_SIZE(da9213_legacy_provider_snapshot_regs);
		     reg++) {
			ret = da9213_legacy_read_reg_locked(chip,
				da9213_legacy_provider_snapshot_regs[reg],
				&sample[pass][reg]);
			if (ret)
				goto out_unlock;
		}
	}
	i2c_unlock_bus(chip->client->adapter, I2C_LOCK_ROOT_ADAPTER);

	for (reg = 0; reg < ARRAY_SIZE(da9213_legacy_provider_snapshot_regs);
	     reg++)
		if (sample[0][reg] != sample[1][reg])
			return -EAGAIN;

	state->abi = MT6797_A72_PROVIDER_STATE_ABI;
	state->valid = 1;
	state->control_a = sample[0][0];
	state->status_b = sample[0][1];
	state->buckb_cont = sample[0][2];
	state->vbuckb_a = sample[0][3];
	state->vbuckb_b = sample[0][4];
	return 0;

out_unlock:
	i2c_unlock_bus(chip->client->adapter, I2C_LOCK_ROOT_ADAPTER);
	memset(state, 0, sizeof(*state));
	return ret;
}

static int da9213_legacy_provider_acquire(void *context,
	const struct mt6797_a72_provider_request *request,
	struct mt6797_a72_provider_response *response)
{
	struct da9213_legacy *chip = context;

	if (!chip || !request || !response)
		return -EINVAL;

	memset(response, 0, sizeof(*response));
	response->abi = MT6797_A72_PROVIDER_CALL_ABI;
	response->returned = 1;
	if (request->abi != MT6797_A72_PROVIDER_CALL_ABI ||
	    request->operation != MT6797_A72_PROVIDER_OPERATION_CPU8_UP ||
	    request->settle_us != MT6797_A72_PROVIDER_CALL_SETTLE_US ||
	    request->da921x_page != MT6797_A72_PROVIDER_CALL_DA921X_PAGE ||
	    request->buckb_vsel != MT6797_A72_PROVIDER_CALL_BUCKB_VSEL ||
	    request->reserved)
		return -EINVAL;

	/* The current provider is deliberately read-only: no vote or rail write. */
	dev_dbg(chip->dev,
		"provider-owner acquire refused: read-only resource boundary\n");
	return -EOPNOTSUPP;
}

static int da9213_legacy_provider_release(void *context,
	const struct mt6797_a72_provider_handle *handle,
	struct mt6797_a72_provider_response *response)
{
	if (!context || !handle || !response)
		return -EINVAL;

	memset(response, 0, sizeof(*response));
	response->abi = MT6797_A72_PROVIDER_CALL_ABI;
	response->returned = 1;
	response->origin = 1;
	response->reserved = 0;

	/* Page ownership, write readback, and inverse proof are still open. */
	dev_dbg(((struct da9213_legacy *)context)->dev,
		"provider-owner release refused: no rollback owner\n");
	return -EOPNOTSUPP;
}

static const struct mt6797_a72_provider_ops da9213_legacy_provider_ops = {
	.acquire = da9213_legacy_provider_acquire,
	.release = da9213_legacy_provider_release,
	.snapshot = da9213_legacy_provider_snapshot,
};

static void da9213_legacy_provider_unregister(void *context)
{
	mt6797_a72_provider_unregister(&da9213_legacy_provider_ops, context);
}

static int da9213_legacy_register_owner(struct da9213_legacy *chip)
{
	int ret;

	ret = mt6797_a72_provider_register(&da9213_legacy_provider_ops, chip);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(chip->dev,
				       da9213_legacy_provider_unregister, chip);
	return ret;
}

#endif

#endif

static int da9213_legacy_validate_node(struct i2c_client *client)
{
	struct device_node *regulators;
	struct device_node *np = client->dev.of_node;
	const char *name;
	u32 addr;
	int i;

	if (!np || client->flags & I2C_CLIENT_TEN)
		return -EINVAL;

	if (client->addr != DA9213_LEGACY_PRIMARY_ADDR)
		return -EINVAL;

	if (of_property_count_u32_elems(np, "reg") != 2 ||
	    of_property_count_strings(np, "reg-names") != 2)
		return -EINVAL;

	for (i = 0; i < 2; i++) {
		u32 expected_addr = i ? DA9213_LEGACY_PAGE2_ADDR :
					 DA9213_LEGACY_PRIMARY_ADDR;
		const char *expected_name = i ? "page2" : "primary";

		if (of_property_read_u32_index(np, "reg", i, &addr) ||
		    addr != expected_addr ||
		    of_property_read_string_index(np, "reg-names", i, &name) ||
		    strcmp(name, expected_name))
			return -EINVAL;
	}

	regulators = of_get_child_by_name(np, "regulators");
	if (regulators) {
		of_node_put(regulators);
		return -EOPNOTSUPP;
	}

	return 0;
}

static int da9213_legacy_read_sample(struct i2c_client *client,
				     struct i2c_client *page2,
				     const struct da9213_legacy_sample *sample)
{
	struct i2c_client *target;
	struct i2c_msg msgs[2];
	u8 reg = sample->reg;
	u8 value = ~sample->expected;
	int ret;

	target = sample->bank == DA9213_LEGACY_PAGE2 ? page2 : client;
	msgs[0].addr = target->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &reg;
	msgs[1].addr = target->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = &value;

	ret = __i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;
	if (value != sample->expected)
		return -ENODEV;

	return 0;
}

static int da9213_legacy_probe(struct i2c_client *client)
{
	struct da9213_legacy *chip;
	const struct da9213_legacy_variant *variant;
	int pass;
	unsigned int sample;
#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_OBSERVER)
	unsigned int identity_reads = 0;
#endif
	int ret = 0;

	variant = i2c_get_match_data(client);
	if (!variant)
		return -ENODEV;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = &client->dev;
	chip->client = client;
	chip->variant = variant;
	i2c_set_clientdata(client, chip);

	ret = da9213_legacy_validate_node(client);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "invalid identification-only description\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(&client->dev, -EOPNOTSUPP,
				     "adapter lacks raw I2C transfers\n");

	chip->page2 = devm_i2c_new_dummy_device(&client->dev, client->adapter,
						DA9213_LEGACY_PAGE2_ADDR);
	if (IS_ERR(chip->page2))
		return dev_err_probe(&client->dev, PTR_ERR(chip->page2),
				     "failed to claim page2 address\n");

	i2c_lock_bus(client->adapter, I2C_LOCK_ROOT_ADAPTER);
	for (pass = 0; pass < DA9213_LEGACY_PASSES; pass++) {
		for (sample = 0; sample < ARRAY_SIZE(da9213_legacy_samples);
		     sample++) {
			ret = da9213_legacy_read_sample(client, chip->page2,
							&da9213_legacy_samples[sample]);
			if (ret)
				goto out_unlock;
#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_OBSERVER)
			identity_reads++;
#endif
		}
	}
out_unlock:
	i2c_unlock_bus(client->adapter, I2C_LOCK_ROOT_ADAPTER);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "read-only identity transcript failed\n");

#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_PROVIDER)
#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_OBSERVER)
	ret = da9213_legacy_observer_prepare(chip);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to prepare read-only observer\n");
#endif
	ret = da9213_legacy_register_provider(chip);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to register read-only provider\n");
#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_OBSERVER)
	ret = da9213_legacy_observer_sample(chip, identity_reads);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "read-only provider observation failed\n");
#endif
#if IS_ENABLED(CONFIG_ARM64_MT6797_A72_PROVIDER_OWNER)
	ret = da9213_legacy_register_owner(chip);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to register provider-owner boundary\n");
#endif
#endif
#if IS_ENABLED(CONFIG_REGULATOR_DA9213_LEGACY_OBSERVER)
	da9213_legacy_observer_publish(chip);
#endif

	dev_info(&client->dev,
		 "%s legacy direct-address identity matched; provider is read-only\n",
		 variant->name);

	return 0;
}

static const struct of_device_id da9213_legacy_of_match[] = {
	{
		.compatible = "dlg,da9213-legacy",
		.data = &da9213_legacy_da9213,
	}, {
		.compatible = "dlg,da9214-legacy",
		.data = &da9213_legacy_da9214,
	}, {
		.compatible = "dlg,da9215-legacy",
		.data = &da9213_legacy_da9215,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, da9213_legacy_of_match);

static struct i2c_driver da9213_legacy_driver = {
	.driver = {
		.name = "da9213-legacy-regulator",
		.of_match_table = da9213_legacy_of_match,
	},
	.probe = da9213_legacy_probe,
};
module_i2c_driver(da9213_legacy_driver);

MODULE_AUTHOR("Julien Etienne <julien.etienne@gmail.com>");
MODULE_DESCRIPTION("Dialog DA9213-DA9215 legacy identification and read-only provider");
MODULE_LICENSE("GPL");
