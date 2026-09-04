// SPDX-License-Identifier: GPL-2.0-only
/*
 * Transactional MT6797 Cortex-A72 resource preparation.
 *
 * CPU core power and CPU_ON remain owned by PSCI firmware. This driver owns
 * only the vendor-evidenced, reversible Linux resource sequence immediately
 * before a standard PSCI CPU_ON call.
 */

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/smp.h>
#include <linux/soc/mediatek/mt6797-a72-power.h>
#include <uapi/linux/psci.h>

#include <asm/cputype.h>

#include "mt6797-a72-power.h"

#define MT6797_A72_CPU_COUNT		2
#define MT6797_A72_FIRST_CPU		8
#define MT6797_A72_FIRST_MPIDR		0x200
#define MT6797_A72_BOOT_CPU_LIMIT	8
#define MT6797_A72_IDVFS_READ_FID	0xc200035fUL
#define MT6797_A72_IDVFS_FENCE_REG	0x102224a0U
#define MT6797_A72_SRAM_SELECTOR_REG	0x102222b0U
#define MT6797_A72_SRAM_BOOT_SELECTOR	11

static_assert(MT6797_A72_R0P1_MIDR ==
	      (MIDR_CORTEX_A72 | MIDR_CPU_VAR_REV(0, 1)));

struct mt6797_a72_preflight {
	struct device *dev;
	struct regulator *vproc_big;
	struct regulator *sram_big;
	struct reset_control *pwrap_reset;
	struct clk *mp2_clk;
	struct regmap *spm;
	unsigned int cpus[MT6797_A72_CPU_COUNT];
	u64 mpidrs[MT6797_A72_CPU_COUNT];
	cpumask_t cpu_mask;
	struct hlist_node cpuhp_node;
	struct hlist_node online_node;
	struct notifier_block overlay_nb;
	int cpuhp_state;
	int online_state;
	int admission_ret;
	struct mt6797_a72_plan plan;
	struct mt6797_a72_transaction transaction;
	/* Excludes every Linux-owned operation during the PWRAP reset window. */
	struct mutex operation_lock;
	u32 fence_value;
	bool guarded;
	bool faulted;
	bool cpu8_armed;
	bool overlay_nb_registered;
};

static DEFINE_MUTEX(mt6797_a72_owner_lock);
static struct mt6797_a72_preflight *mt6797_a72_owner;

static int mt6797_a72_overlay_notify(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	struct mt6797_a72_preflight *preflight =
		container_of(nb, struct mt6797_a72_preflight, overlay_nb);
	const struct of_overlay_notify_data *nd = data;

	if (action != OF_OVERLAY_PRE_REMOVE || nd->target != preflight->dev->of_node)
		return NOTIFY_DONE;

	dev_err(preflight->dev,
		"refusing removal of the active A72 ownership overlay\n");
	return notifier_from_errno(-EBUSY);
}

int mt6797_a72_power_cpu_boot_admit(unsigned int cpu)
{
	struct mt6797_a72_preflight *preflight;
	bool cpu4_online;
	bool armed;
	bool faulted;
	int ret;

	mutex_lock(&mt6797_a72_owner_lock);
	preflight = mt6797_a72_owner;
	if (!preflight) {
		ret = -EAGAIN;
	} else {
		cpu4_online = cpu_online(4);
		armed = preflight->cpu8_armed;
		faulted = preflight->faulted;
		ret = mt6797_a72_validate_cpu8_boot(cpu, cpu4_online, armed, faulted);
	}
	mutex_unlock(&mt6797_a72_owner_lock);

	return ret;
}

static int mt6797_a72_secure_read(u32 address, u32 *value)
{
	struct arm_smccc_res result;

	if (!value || (address != MT6797_A72_IDVFS_FENCE_REG &&
		       address != MT6797_A72_SRAM_SELECTOR_REG))
		return -EINVAL;

	arm_smccc_smc(MT6797_A72_IDVFS_READ_FID, address, 0, 0, 0, 0, 0, 0,
		      &result);
	if (result.a0 >> 32)
		return result.a0 == SMCCC_RET_NOT_SUPPORTED ? -EOPNOTSUPP : -EIO;

	*value = result.a0;
	return 0;
}

static int mt6797_a72_snapshot(void *context, struct mt6797_a72_state *state)
{
	struct mt6797_a72_preflight *preflight = context;
	int ret;

	if (preflight->faulted)
		return -EUCLEAN;

	ret = regmap_read(preflight->spm, MT6797_A72_SPM_MP2_PWR_CON,
			  &state->mp2_pwr_con);
	if (ret)
		return ret;
	ret = regmap_read(preflight->spm, MT6797_A72_SPM_EXT_BUCK_ISO,
			  &state->ext_buck_iso);
	if (ret)
		return ret;
	state->vproc_enabled = regulator_is_enabled(preflight->vproc_big);
	if (state->vproc_enabled < 0)
		return state->vproc_enabled;
	state->vproc_uv = regulator_get_voltage(preflight->vproc_big);
	if (state->vproc_uv < 0)
		return state->vproc_uv;
	state->vsram_uv = regulator_get_voltage(preflight->sram_big);
	if (state->vsram_uv < 0)
		return state->vsram_uv;
	ret = mt6797_a72_secure_read(MT6797_A72_SRAM_SELECTOR_REG,
				     &state->vsram_selector);
	if (ret)
		return ret;
	state->vsram_selector &= 0xf;
	state->pwrap_reset_asserted =
		reset_control_status(preflight->pwrap_reset);
	if (state->pwrap_reset_asserted < 0)
		return state->pwrap_reset_asserted;
	state->transaction_guarded = preflight->guarded;
	state->a72_online =
		cpumask_intersects(&preflight->cpu_mask, cpu_online_mask);

	return 0;
}

static int
mt6797_a72_apply(void *context, const struct mt6797_a72_step *step)
{
	struct mt6797_a72_preflight *preflight = context;
	int ret;

	switch (step->action) {
	case MT6797_A72_TRANSACTION_GUARD_ENTER:
		mutex_lock(&preflight->operation_lock);
		if (preflight->faulted || preflight->guarded) {
			mutex_unlock(&preflight->operation_lock);
			return -EUCLEAN;
		}
		preflight->guarded = true;
		return 0;
	case MT6797_A72_SPM_UPDATE:
		return regmap_update_bits(preflight->spm, step->reg,
					  step->mask, step->value);
	case MT6797_A72_IDVFS_READ_FENCE:
		return mt6797_a72_secure_read(MT6797_A72_IDVFS_FENCE_REG,
					       &preflight->fence_value);
	case MT6797_A72_PWRAP_RESET_ASSERT:
		return reset_control_assert(preflight->pwrap_reset);
	case MT6797_A72_VPROC_ENABLE:
		ret = regulator_is_enabled(preflight->vproc_big);
		return ret < 0 ? ret : ret ? 0 :
			regulator_enable(preflight->vproc_big);
	case MT6797_A72_DELAY_US:
		usleep_range(step->value, step->value + 50);
		return 0;
	case MT6797_A72_PWRAP_RESET_DEASSERT:
		return reset_control_deassert(preflight->pwrap_reset);
	case MT6797_A72_TRANSACTION_GUARD_EXIT:
		if (!preflight->guarded)
			return -EUCLEAN;
		preflight->guarded = false;
		mutex_unlock(&preflight->operation_lock);
		return 0;
	case MT6797_A72_VSRAM_SET_UV:
		return regulator_set_voltage(preflight->sram_big, step->value,
					     step->value);
	case MT6797_A72_NR_ACTIONS:
		return -EINVAL;
	}

	return -EINVAL;
}

static int
mt6797_a72_verify(void *context, const struct mt6797_a72_step *step)
{
	struct mt6797_a72_preflight *preflight = context;
	u32 value;
	int ret;

	switch (step->action) {
	case MT6797_A72_TRANSACTION_GUARD_ENTER:
		return preflight->guarded ? 0 : -EUCLEAN;
	case MT6797_A72_SPM_UPDATE:
		ret = regmap_read(preflight->spm, step->reg, &value);
		return ret ?: ((value & step->mask) == step->value ? 0 : -EIO);
	case MT6797_A72_IDVFS_READ_FENCE:
		ret = mt6797_a72_secure_read(MT6797_A72_IDVFS_FENCE_REG, &value);
		return ret ?: (value == preflight->fence_value ? 0 : -EAGAIN);
	case MT6797_A72_PWRAP_RESET_ASSERT:
		ret = reset_control_status(preflight->pwrap_reset);
		return ret < 0 ? ret : ret == 1 ? 0 : -EIO;
	case MT6797_A72_VPROC_ENABLE:
		ret = regulator_is_enabled(preflight->vproc_big);
		return ret < 0 ? ret : ret == 1 ? 0 : -EIO;
	case MT6797_A72_DELAY_US:
		return 0;
	case MT6797_A72_PWRAP_RESET_DEASSERT:
		ret = reset_control_status(preflight->pwrap_reset);
		return ret < 0 ? ret : ret == 0 ? 0 : -EIO;
	case MT6797_A72_TRANSACTION_GUARD_EXIT:
		return preflight->guarded ? -EUCLEAN : 0;
	case MT6797_A72_VSRAM_SET_UV:
		ret = regulator_get_voltage(preflight->sram_big);
		if (ret < 0 || ret != step->value)
			return ret < 0 ? ret : -EIO;
		ret = mt6797_a72_secure_read(MT6797_A72_SRAM_SELECTOR_REG, &value);
		return ret ?: ((value & 0xf) == MT6797_A72_SRAM_BOOT_SELECTOR ?
				0 : -EIO);
	case MT6797_A72_NR_ACTIONS:
		return -EINVAL;
	}

	return -EINVAL;
}

static int
mt6797_a72_rollback(void *context, const struct mt6797_a72_step *step,
		    const struct mt6797_a72_state *initial)
{
	struct mt6797_a72_preflight *preflight = context;
	u32 value;
	int ret;

	switch (step->action) {
	case MT6797_A72_TRANSACTION_GUARD_ENTER:
		if (!preflight->guarded)
			return -EUCLEAN;
		preflight->guarded = false;
		mutex_unlock(&preflight->operation_lock);
		return 0;
	case MT6797_A72_SPM_UPDATE:
		value = step->reg == MT6797_A72_SPM_MP2_PWR_CON ?
			initial->mp2_pwr_con : initial->ext_buck_iso;
		return regmap_update_bits(preflight->spm, step->reg,
					  step->mask, value);
	case MT6797_A72_IDVFS_READ_FENCE:
	case MT6797_A72_DELAY_US:
		return 0;
	case MT6797_A72_PWRAP_RESET_ASSERT:
		return initial->pwrap_reset_asserted ?
			reset_control_assert(preflight->pwrap_reset) :
			reset_control_deassert(preflight->pwrap_reset);
	case MT6797_A72_VPROC_ENABLE:
		ret = regulator_is_enabled(preflight->vproc_big);
		if (ret < 0)
			return ret;
		if (!initial->vproc_enabled && ret)
			return regulator_disable(preflight->vproc_big);
		return initial->vproc_enabled && !ret ? -EUCLEAN : 0;
	case MT6797_A72_PWRAP_RESET_DEASSERT:
		return reset_control_assert(preflight->pwrap_reset);
	case MT6797_A72_TRANSACTION_GUARD_EXIT:
		if (preflight->guarded)
			return -EUCLEAN;
		mutex_lock(&preflight->operation_lock);
		preflight->guarded = true;
		return 0;
	case MT6797_A72_VSRAM_SET_UV:
		return regulator_set_voltage(preflight->sram_big,
					     initial->vsram_uv,
					     initial->vsram_uv);
	case MT6797_A72_NR_ACTIONS:
		return -EINVAL;
	}

	return -EINVAL;
}

static const struct mt6797_a72_backend_ops mt6797_a72_live_ops = {
	.snapshot = mt6797_a72_snapshot,
	.apply = mt6797_a72_apply,
	.verify = mt6797_a72_verify,
	.rollback = mt6797_a72_rollback,
};

static int mt6797_a72_preflight_parse_cpus(struct device *dev,
					   struct mt6797_a72_preflight *preflight)
{
	struct device_node *cpu_node;
	const char *enable_method;
	int count;
	int cpu;
	int i;

	count = of_count_phandle_with_args(dev->of_node, "cpus", NULL);
	if (count != ARRAY_SIZE(preflight->cpus))
		return dev_err_probe(dev, -EINVAL,
				     "expected exactly two CPU phandles\n");

	cpumask_clear(&preflight->cpu_mask);
	for (i = 0; i < count; i++) {
		cpu_node = of_parse_phandle(dev->of_node, "cpus", i);
		if (!cpu_node)
			return dev_err_probe(dev, -EINVAL,
					     "failed to parse CPU phandle %d\n", i);

		cpu = MT6797_A72_FIRST_CPU + i;

		preflight->mpidrs[i] = of_get_cpu_hwid(cpu_node, 0);
		if (preflight->mpidrs[i] != MT6797_A72_FIRST_MPIDR + i)
			goto invalid_cpu;

		if (of_property_read_string(cpu_node, "enable-method",
					    &enable_method))
			goto invalid_cpu;
		if (strcmp(enable_method, "mediatek,mt6797-psci"))
			goto invalid_cpu;

		of_node_put(cpu_node);
		if (!i && cpu < nr_cpu_ids &&
		    cpumask_test_and_set_cpu(cpu, &preflight->cpu_mask))
			return dev_err_probe(dev, -EINVAL,
					     "duplicate CPU phandle %d\n", i);

		preflight->cpus[i] = cpu;
		continue;

invalid_cpu:
		of_node_put(cpu_node);
		return dev_err_probe(dev, -EINVAL,
				     "CPU phandle %d is not CPU%u/MPIDR 0x%llx\n",
				     i, MT6797_A72_FIRST_CPU + i,
				     (unsigned long long)
				     (MT6797_A72_FIRST_MPIDR + i));
	}
	return 0;
}

static int
mt6797_a72_cpu_on_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct mt6797_a72_preflight *preflight;
	int ret;

	preflight = hlist_entry(node, struct mt6797_a72_preflight, cpuhp_node);
	if (!cpumask_test_cpu(cpu, &preflight->cpu_mask))
		return 0;
	if (cpu != MT6797_A72_FIRST_CPU)
		return -EPERM;
	if (!cpu_online(4))
		return -EAGAIN;
	if (preflight->faulted)
		return -EUCLEAN;
	if (preflight->admission_ret)
		return preflight->admission_ret;

	ret = mt6797_a72_transaction_begin(&preflight->transaction,
					   &preflight->plan,
					   &mt6797_a72_live_ops,
					   preflight,
					   MT6797_A72_REQUIRED_CAPS);
	if (ret == -EUCLEAN) {
		preflight->faulted = true;
		return ret;
	}
	if (ret)
		return ret;

	mutex_lock(&mt6797_a72_owner_lock);
	if (mt6797_a72_owner != preflight) {
		mutex_unlock(&mt6797_a72_owner_lock);
		ret = mt6797_a72_transaction_abort(&preflight->transaction);
		if (ret)
			preflight->faulted = true;
		return ret ?: -ESHUTDOWN;
	}
	preflight->cpu8_armed = true;
	mutex_unlock(&mt6797_a72_owner_lock);

	return ret;
}

static int
mt6797_a72_cpu_on_abort(unsigned int cpu, struct hlist_node *node)
{
	struct mt6797_a72_preflight *preflight;
	int ret;

	preflight = hlist_entry(node, struct mt6797_a72_preflight, cpuhp_node);
	if (cpu != MT6797_A72_FIRST_CPU)
		return 0;

	mutex_lock(&mt6797_a72_owner_lock);
	preflight->cpu8_armed = false;
	mutex_unlock(&mt6797_a72_owner_lock);
	if (!preflight->transaction.active)
		return 0;

	ret = mt6797_a72_transaction_abort(&preflight->transaction);
	if (ret)
		preflight->faulted = true;
	return ret;
}

static int
mt6797_a72_cpu_online_commit(unsigned int cpu, struct hlist_node *node)
{
	struct mt6797_a72_preflight *preflight;
	bool cpu4_online;
	bool faulted;
	bool transaction_active;
	int affinity;
	int ret;

	preflight = hlist_entry(node, struct mt6797_a72_preflight, online_node);
	if (cpu != MT6797_A72_FIRST_CPU)
		return 0;

	cpu4_online = cpu_online(4);
	faulted = preflight->faulted;
	transaction_active = preflight->transaction.active;
	ret = mt6797_a72_validate_cpu8_boot(cpu, cpu4_online, transaction_active, faulted);
	if (ret)
		goto disarm;
	ret = mt6797_a72_validate_cpu8_identity(read_cpuid_id());
	if (ret)
		goto disarm;

	affinity = psci_ops.affinity_info(preflight->mpidrs[0], 0);
	if (affinity != PSCI_0_2_AFFINITY_LEVEL_ON) {
		ret = affinity < 0 ? affinity : -EIO;
		goto disarm;
	}

	ret = mt6797_a72_transaction_commit(&preflight->transaction);
disarm:
	mutex_lock(&mt6797_a72_owner_lock);
	preflight->cpu8_armed = false;
	mutex_unlock(&mt6797_a72_owner_lock);
	if (ret)
		preflight->faulted = true;
	else
		dev_info(preflight->dev,
			 "CPU8 PSCI CPU_ON committed with CPU4 prerequisite\n");

	return ret;
}

static int mt6797_a72_install_cpu_on_gate(struct device *dev,
					  struct mt6797_a72_preflight *preflight,
					  int affinity[MT6797_A72_CPU_COUNT],
					  u32 *version)
{
	int ret;
	int i;

	if (setup_max_cpus != MT6797_A72_BOOT_CPU_LIMIT)
		return dev_err_probe(dev, -EPERM,
				     "PSCI gate requires maxcpus=8, got %u\n",
				     setup_max_cpus);
	if (!cpu_possible(preflight->cpus[0]) ||
	    !cpu_present(preflight->cpus[0]))
		return dev_err_probe(dev, -ENODEV,
				     "CPU8 must be possible and present\n");

	if (!psci_ops.get_version || !psci_ops.cpu_on ||
	    !psci_ops.affinity_info)
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "required PSCI operations are unavailable\n");

	*version = psci_ops.get_version();
	if ((s32)*version < 0)
		return dev_err_probe(dev, (s32)*version,
				     "failed to read PSCI version\n");
	if (PSCI_VERSION_MAJOR(*version) == 0 &&
	    PSCI_VERSION_MINOR(*version) < 2)
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "PSCI v0.2 or newer is required\n");

	for (i = 0; i < MT6797_A72_CPU_COUNT; i++) {
		affinity[i] = psci_ops.affinity_info(preflight->mpidrs[i], 0);
		if (affinity[i] < 0)
			return dev_err_probe(dev, affinity[i],
					     "PSCI does not recognize CPU%u\n",
					     preflight->cpus[i]);
		if (affinity[i] != PSCI_0_2_AFFINITY_LEVEL_OFF)
			return dev_err_probe(dev, -EBUSY,
					     "CPU%u PSCI affinity is not off: %d\n",
					     preflight->cpus[i], affinity[i]);
	}

	preflight->cpuhp_state =
		cpuhp_setup_state_multi(CPUHP_BP_PREPARE_DYN,
					"soc/mediatek:mt6797-a72-prepare",
					mt6797_a72_cpu_on_prepare,
					mt6797_a72_cpu_on_abort);
	if (preflight->cpuhp_state < 0)
		return dev_err_probe(dev, preflight->cpuhp_state,
				     "failed to register CPU_ON preparation state\n");

	ret = cpuhp_state_add_instance_nocalls(preflight->cpuhp_state,
					       &preflight->cpuhp_node);
	if (ret) {
		cpuhp_remove_multi_state(preflight->cpuhp_state);
		preflight->cpuhp_state = CPUHP_INVALID;
		return dev_err_probe(dev, ret,
				     "failed to install CPU_ON preparation gate\n");
	}

	preflight->online_state =
		cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
					"soc/mediatek:mt6797-a72-commit",
					mt6797_a72_cpu_online_commit, NULL);
	if (preflight->online_state < 0) {
		ret = preflight->online_state;
		goto remove_prepare_instance;
	}

	ret = cpuhp_state_add_instance_nocalls(preflight->online_state,
					       &preflight->online_node);
	if (ret)
		goto remove_online_state;

	return 0;

remove_online_state:
	cpuhp_remove_multi_state(preflight->online_state);
	preflight->online_state = CPUHP_INVALID;
remove_prepare_instance:
	cpuhp_state_remove_instance_nocalls(preflight->cpuhp_state,
					    &preflight->cpuhp_node);
	cpuhp_remove_multi_state(preflight->cpuhp_state);
	preflight->cpuhp_state = CPUHP_INVALID;
	return dev_err_probe(dev, ret,
			     "failed to install CPU_ON commit state\n");
}

static void
mt6797_a72_remove_cpu_on_gate(struct mt6797_a72_preflight *preflight)
{
	if (preflight->online_state != CPUHP_INVALID) {
		cpuhp_state_remove_instance_nocalls(preflight->online_state,
						    &preflight->online_node);
		cpuhp_remove_multi_state(preflight->online_state);
		preflight->online_state = CPUHP_INVALID;
	}

	if (preflight->cpuhp_state != CPUHP_INVALID) {
		cpuhp_state_remove_instance_nocalls(preflight->cpuhp_state,
						    &preflight->cpuhp_node);
		cpuhp_remove_multi_state(preflight->cpuhp_state);
		preflight->cpuhp_state = CPUHP_INVALID;
	}
}

static int mt6797_a72_preflight_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_a72_preflight *preflight;
	unsigned long mp2_rate;
	struct mt6797_a72_state state = { };
	struct mt6797_a72_plan plan;
	u32 mp2_power;
	u32 buck_iso;
	u32 sram_selector;
	int vproc_enabled;
	int vproc_uv;
	int sram_uv;
	int pwrap_reset_asserted;
	int transaction_ret;
	int affinity[MT6797_A72_CPU_COUNT] = { };
	u32 psci_version = 0;
	int ret;

	preflight = devm_kzalloc(dev, sizeof(*preflight), GFP_KERNEL);
	if (!preflight)
		return -ENOMEM;
	preflight->cpuhp_state = CPUHP_INVALID;
	preflight->online_state = CPUHP_INVALID;
	preflight->dev = dev;
	mutex_init(&preflight->operation_lock);

	ret = mt6797_a72_preflight_parse_cpus(dev, preflight);
	if (ret)
		return ret;

	ret = 0;
	cpus_read_lock();
	if ((preflight->cpus[0] < nr_cpu_ids &&
	     cpu_online(preflight->cpus[0])) ||
	    (preflight->cpus[1] < nr_cpu_ids &&
	     cpu_online(preflight->cpus[1])))
		ret = -EBUSY;
	cpus_read_unlock();
	if (ret)
		return dev_err_probe(dev, ret,
				     "A72 CPU is already online; refusing preflight\n");

	preflight->vproc_big = devm_regulator_get_exclusive(dev, "vproc-big");
	if (IS_ERR(preflight->vproc_big))
		return dev_err_probe(dev, PTR_ERR(preflight->vproc_big),
				     "failed to get big-cluster regulator\n");

	preflight->pwrap_reset = devm_reset_control_get_exclusive(dev, "pwrap");
	if (IS_ERR(preflight->pwrap_reset))
		return dev_err_probe(dev, PTR_ERR(preflight->pwrap_reset),
				     "failed to get PWRAP reset\n");

	preflight->sram_big = devm_regulator_get_exclusive(dev, "sram-big");
	if (IS_ERR(preflight->sram_big))
		return dev_err_probe(dev, PTR_ERR(preflight->sram_big),
				     "failed to get big-cluster SRAM regulator\n");

	preflight->mp2_clk = devm_clk_get(dev, "mp2");
	if (IS_ERR(preflight->mp2_clk))
		return dev_err_probe(dev, PTR_ERR(preflight->mp2_clk),
				     "failed to get MP2 clock\n");

	preflight->spm =
		syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(preflight->spm))
		return dev_err_probe(dev, PTR_ERR(preflight->spm),
				     "failed to get SPM syscon\n");

	vproc_enabled = regulator_is_enabled(preflight->vproc_big);
	if (vproc_enabled < 0)
		return dev_err_probe(dev, vproc_enabled,
				     "failed to read big-cluster rail state\n");

	vproc_uv = regulator_get_voltage(preflight->vproc_big);
	if (vproc_uv < 0)
		return dev_err_probe(dev, vproc_uv,
				     "failed to read big-cluster voltage\n");

	if (vproc_enabled > 1 || vproc_uv < MT6797_A72_VPROC_MIN_UV ||
	    vproc_uv > MT6797_A72_VPROC_MAX_UV)
		return dev_err_probe(dev, -ERANGE,
				     "big-cluster rail observation is out of range\n");

	sram_uv = regulator_get_voltage(preflight->sram_big);
	if (sram_uv < 0)
		return dev_err_probe(dev, sram_uv,
				     "failed to read big-cluster SRAM voltage\n");

	if (sram_uv < MT6797_A72_SRAM_MIN_UV ||
	    sram_uv > MT6797_A72_SRAM_MAX_UV)
		return dev_err_probe(dev, -ERANGE,
				     "big-cluster SRAM observation is out of range\n");
	ret = mt6797_a72_secure_read(MT6797_A72_SRAM_SELECTOR_REG,
				     &sram_selector);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read big-cluster SRAM selector\n");
	sram_selector &= 0xf;

	mp2_rate = clk_get_rate(preflight->mp2_clk);
	if (!mp2_rate)
		return dev_err_probe(dev, -EINVAL,
				     "MP2 clock rate is unavailable\n");

	ret = regmap_read(preflight->spm, MT6797_A72_SPM_MP2_PWR_CON,
			  &mp2_power);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read MP2 power control\n");

	ret = regmap_read(preflight->spm, MT6797_A72_SPM_EXT_BUCK_ISO,
			  &buck_iso);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read external buck isolation\n");

	pwrap_reset_asserted = reset_control_status(preflight->pwrap_reset);
	if (pwrap_reset_asserted < 0)
		return dev_err_probe(dev, pwrap_reset_asserted,
				     "failed to read PWRAP reset state\n");

	ret = mt6797_a72_build_plan(&plan);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to build A72 transaction model\n");

	state.mp2_pwr_con = mp2_power;
	state.ext_buck_iso = buck_iso;
	state.vproc_enabled = vproc_enabled;
	state.pwrap_reset_asserted = pwrap_reset_asserted;
	state.vproc_uv = vproc_uv;
	state.vsram_uv = sram_uv;
	state.vsram_selector = sram_selector;
	transaction_ret = mt6797_a72_validate_admission(&state,
							MT6797_A72_REQUIRED_CAPS);
	if (transaction_ret)
		return dev_err_probe(dev, transaction_ret,
				     "A72 transaction admission failed\n");
	preflight->plan = plan;
	preflight->admission_ret = transaction_ret;

	mutex_lock(&mt6797_a72_owner_lock);
	if (mt6797_a72_owner) {
		mutex_unlock(&mt6797_a72_owner_lock);
		return dev_err_probe(dev, -EBUSY,
				     "A72 admission owner already exists\n");
	}
	mt6797_a72_owner = preflight;
	mutex_unlock(&mt6797_a72_owner_lock);

	ret = mt6797_a72_install_cpu_on_gate(dev, preflight, affinity, &psci_version);
	if (ret) {
		mutex_lock(&mt6797_a72_owner_lock);
		if (mt6797_a72_owner == preflight)
			mt6797_a72_owner = NULL;
		mutex_unlock(&mt6797_a72_owner_lock);
		return ret;
	}

	preflight->overlay_nb.notifier_call = mt6797_a72_overlay_notify;
	ret = of_overlay_notifier_register(&preflight->overlay_nb);
	if (ret) {
		mutex_lock(&mt6797_a72_owner_lock);
		if (mt6797_a72_owner == preflight)
			mt6797_a72_owner = NULL;
		mutex_unlock(&mt6797_a72_owner_lock);
		mt6797_a72_remove_cpu_on_gate(preflight);
		return dev_err_probe(dev, ret,
				     "failed to protect A72 owner overlay\n");
	}
	preflight->overlay_nb_registered = true;

	platform_set_drvdata(pdev, preflight);
	dev_info(dev,
		 "CPU%u/%u resource-preflight-v2: offline=1 vproc=%duV vproc-enabled=%d sram=%duV sram-selector=%u sram-contract=voltage-only mp2=%luHz pwrap-reset=%d spm-mp2=0x%08x spm-buck-iso=0x%08x plan-steps=%u transaction=ready\n",
		 preflight->cpus[0], preflight->cpus[1], vproc_uv,
		 vproc_enabled, sram_uv, sram_selector, mp2_rate,
		 pwrap_reset_asserted, mp2_power, buck_iso, plan.count);
	dev_info(dev,
		 "cpu-on-gate-v3: method=mediatek,mt6797-psci CPU8-mpidr=0x%llx CPU9-mpidr=0x%llx PSCI=%u.%u affinity8=off affinity9=off prepare-state=%d commit-state=%d CPU4-prerequisite=required CPU8-r0p1=required CPU8-admission=ready CPU9=blocked\n",
		 (unsigned long long)preflight->mpidrs[0],
		 (unsigned long long)preflight->mpidrs[1],
		 PSCI_VERSION_MAJOR(psci_version),
		 PSCI_VERSION_MINOR(psci_version), preflight->cpuhp_state,
		 preflight->online_state);

	return 0;
}

static void mt6797_a72_preflight_remove(struct platform_device *pdev)
{
	struct mt6797_a72_preflight *preflight = platform_get_drvdata(pdev);
	bool transaction_active;
	bool cpu8_online;
	bool faulted;
	bool guarded;
	bool armed;
	int ret;

	if (preflight->overlay_nb_registered) {
		ret = of_overlay_notifier_unregister(&preflight->overlay_nb);
		if (ret)
			panic("MT6797 A72 owner overlay notifier is still live");
		preflight->overlay_nb_registered = false;
	}

	/* Serialize removal against every CPUHP prepare, boot, and unwind path. */
	cpus_write_lock();

	mutex_lock(&mt6797_a72_owner_lock);
	preflight->cpu8_armed = false;
	mutex_unlock(&mt6797_a72_owner_lock);

	if (preflight->transaction.active) {
		ret = mt6797_a72_transaction_abort(&preflight->transaction);
		if (ret)
			preflight->faulted = true;
	}

	cpu8_online = cpu_online(MT6797_A72_FIRST_CPU);
	transaction_active = preflight->transaction.active;
	armed = preflight->cpu8_armed;
	guarded = preflight->guarded;
	faulted = preflight->faulted;
	ret = mt6797_a72_validate_removal(cpu8_online, transaction_active, armed, guarded, faulted);
	if (ret) {
		dev_crit(preflight->dev,
			 "unsafe owner removal after serialization: %d\n", ret);
		panic("MT6797 A72 ownership removal would release live resources");
	}

	mutex_lock(&mt6797_a72_owner_lock);
	if (mt6797_a72_owner == preflight)
		mt6797_a72_owner = NULL;
	mutex_unlock(&mt6797_a72_owner_lock);
	cpus_write_unlock();

	/* New prepares now fail owner matching and self-abort before CPU_ON. */
	mt6797_a72_remove_cpu_on_gate(preflight);
}

static const struct of_device_id mt6797_a72_preflight_of_match[] = {
	{ .compatible = "mediatek,mt6797-a72-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_a72_preflight_of_match);

static struct platform_driver mt6797_a72_preflight_driver = {
	.probe = mt6797_a72_preflight_probe,
	.remove = mt6797_a72_preflight_remove,
	.driver = {
		.name = "mt6797-a72-preflight",
		.of_match_table = mt6797_a72_preflight_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6797_a72_preflight_driver);

MODULE_DESCRIPTION("MediaTek MT6797 Cortex-A72 resource preflight and CPU_ON gate");
MODULE_LICENSE("GPL");
