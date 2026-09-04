// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only MT6797 Cortex-A72 resource preflight.
 *
 * CPU core power and CPU_ON remain owned by PSCI firmware. This driver only
 * verifies that Linux can resolve and observe the resources needed before a
 * future, separately reviewed pre-CPU_ON sequence is enabled.
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/smp.h>
#include <uapi/linux/psci.h>

#include "mt6797-a72-power.h"

#define MT6797_A72_VPROC_MIN_UV		800000
#define MT6797_A72_VPROC_MAX_UV		1200000
#define MT6797_A72_SRAM_MIN_UV		600000
#define MT6797_A72_SRAM_MAX_UV		1200000
#define MT6797_A72_CPU_COUNT		2
#define MT6797_A72_FIRST_CPU		8
#define MT6797_A72_FIRST_MPIDR		0x200
#define MT6797_A72_BOOT_CPU_LIMIT	8

struct mt6797_a72_preflight {
	struct regulator *vproc_big;
	struct regulator *sram_big;
	struct reset_control *pwrap_reset;
	struct clk *mp2_clk;
	struct regmap *spm;
	unsigned int cpus[MT6797_A72_CPU_COUNT];
	u64 mpidrs[MT6797_A72_CPU_COUNT];
	cpumask_t cpu_mask;
	struct hlist_node cpuhp_node;
	int cpuhp_state;
	int admission_ret;
	bool psci_gate;
};

static int mt6797_a72_preflight_parse_cpus(struct device *dev,
					   struct mt6797_a72_preflight *preflight)
{
	struct device_node *cpu_node;
	const char *enable_method;
	int count;
	int cpu;
	int i;
	bool psci_gate = true;
	bool this_psci;

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

		cpu = of_cpu_node_to_id(cpu_node);
		if (cpu < 0 || cpu >= nr_cpu_ids)
			goto invalid_cpu;
		if (cpu != MT6797_A72_FIRST_CPU + i)
			goto invalid_cpu;

		preflight->mpidrs[i] = of_get_cpu_hwid(cpu_node, 0);
		if (preflight->mpidrs[i] != MT6797_A72_FIRST_MPIDR + i)
			goto invalid_cpu;

		if (of_property_read_string(cpu_node, "enable-method",
					    &enable_method))
			goto invalid_cpu;
		if (strcmp(enable_method, "psci") &&
		    strcmp(enable_method, "mediatek,mt6797-psci"))
			goto invalid_cpu;
		this_psci = !strcmp(enable_method, "psci");
		if (i && this_psci != psci_gate)
			goto invalid_cpu;
		psci_gate = this_psci;

		of_node_put(cpu_node);
		if (cpumask_test_and_set_cpu(cpu, &preflight->cpu_mask))
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
	preflight->psci_gate = psci_gate;

	return 0;
}

static int
mt6797_a72_cpu_on_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct mt6797_a72_preflight *preflight;

	preflight = hlist_entry(node, struct mt6797_a72_preflight, cpuhp_node);
	if (!cpumask_test_cpu(cpu, &preflight->cpu_mask))
		return 0;

	/*
	 * This is the future transaction boundary immediately before PSCI
	 * CPU_ON.  Never open it from the stale probe-time snapshot: a live
	 * backend must re-snapshot and prepare the resources here, and its
	 * teardown callback must abort them if a later bring-up state fails.
	 */
	return preflight->admission_ret;
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
	    !cpu_possible(preflight->cpus[1]) ||
	    !cpu_present(preflight->cpus[0]) ||
	    !cpu_present(preflight->cpus[1]))
		return dev_err_probe(dev, -ENODEV,
				     "CPU8/9 must be possible and present\n");

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
					mt6797_a72_cpu_on_prepare, NULL);
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

	return 0;
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
	int vproc_enabled;
	int vproc_uv;
	int sram_enabled;
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

	ret = mt6797_a72_preflight_parse_cpus(dev, preflight);
	if (ret)
		return ret;

	ret = 0;
	cpus_read_lock();
	if (cpu_online(preflight->cpus[0]) ||
	    cpu_online(preflight->cpus[1]))
		ret = -EBUSY;
	cpus_read_unlock();
	if (ret)
		return dev_err_probe(dev, ret,
				     "A72 CPU is already online; refusing preflight\n");

	preflight->vproc_big = devm_regulator_get(dev, "vproc-big");
	if (IS_ERR(preflight->vproc_big))
		return dev_err_probe(dev, PTR_ERR(preflight->vproc_big),
				     "failed to get big-cluster regulator\n");

	preflight->pwrap_reset = devm_reset_control_get_exclusive(dev, "pwrap");
	if (IS_ERR(preflight->pwrap_reset))
		return dev_err_probe(dev, PTR_ERR(preflight->pwrap_reset),
				     "failed to get PWRAP reset\n");

	preflight->sram_big = devm_regulator_get(dev, "sram-big");
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

	sram_enabled = regulator_is_enabled(preflight->sram_big);
	if (sram_enabled < 0 && sram_enabled != -EOPNOTSUPP)
		return dev_err_probe(dev, sram_enabled,
				     "failed to read big-cluster SRAM status\n");
	if (sram_enabled > 1 || sram_uv < MT6797_A72_SRAM_MIN_UV ||
	    sram_uv > MT6797_A72_SRAM_MAX_UV)
		return dev_err_probe(dev, -ERANGE,
				     "big-cluster SRAM observation is out of range\n");

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
	state.vsram_enabled = sram_enabled;
	state.pwrap_reset_asserted = pwrap_reset_asserted;
	state.vproc_uv = vproc_uv;
	state.vsram_uv = sram_uv;
	/*
	 * No live backend or OCP guard is wired.  In particular, the secure
	 * SRAM provider cannot report or restore enable state, so admission
	 * must remain closed before the first write.
	 */
	transaction_ret = mt6797_a72_validate_admission(&state, 0);
	if (transaction_ret != -EOPNOTSUPP)
		return dev_err_probe(dev, -EUCLEAN,
				     "A72 transaction gate unexpectedly opened\n");
	preflight->admission_ret = transaction_ret;

	if (preflight->psci_gate) {
		ret = mt6797_a72_install_cpu_on_gate(dev, preflight,
						     affinity, &psci_version);
		if (ret)
			return ret;
	}

	platform_set_drvdata(pdev, preflight);
	dev_info(dev,
		 "CPU%u/%u resource-preflight-v1: offline=1 vproc=%duV vproc-enabled=%d sram=%duV sram-status=%d mp2=%luHz pwrap-reset=%d spm-mp2=0x%08x spm-buck-iso=0x%08x plan-steps=%u transaction=-EOPNOTSUPP CPU_ON=denied\n",
		 preflight->cpus[0], preflight->cpus[1], vproc_uv,
		 vproc_enabled, sram_uv, sram_enabled, mp2_rate,
		 pwrap_reset_asserted, mp2_power, buck_iso, plan.count);
	if (preflight->psci_gate)
		dev_info(dev,
			 "cpu-on-gate-v1: CPU8-mpidr=0x%llx CPU9-mpidr=0x%llx PSCI=%u.%u affinity8=off affinity9=off hp-state=%d admission=-EOPNOTSUPP CPU_ON=blocked\n",
			 (unsigned long long)preflight->mpidrs[0],
			 (unsigned long long)preflight->mpidrs[1],
			 PSCI_VERSION_MAJOR(psci_version),
			 PSCI_VERSION_MINOR(psci_version),
			 preflight->cpuhp_state);

	return 0;
}

static void mt6797_a72_preflight_remove(struct platform_device *pdev)
{
	struct mt6797_a72_preflight *preflight = platform_get_drvdata(pdev);

	if (preflight->cpuhp_state == CPUHP_INVALID)
		return;

	cpuhp_state_remove_instance_nocalls(preflight->cpuhp_state,
					    &preflight->cpuhp_node);
	cpuhp_remove_multi_state(preflight->cpuhp_state);
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
