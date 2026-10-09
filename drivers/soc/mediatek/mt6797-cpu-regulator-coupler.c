// SPDX-License-Identifier: GPL-2.0-only
/* Shared A53/CCI DA9214 BUCKA and eight CPU SRAM LDOs. */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/coupler.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/soc/mediatek/mt6797-cpu-rail-plan.h>
#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>

struct mt6797_cpu_coupler {
	struct regulator_coupler coupler;
	struct regulator_dev *vproc;
	struct regulator_dev *vsram;
	struct mt6797_dvfsp_handoff *handoff;
	bool faulted;
};

static int mt6797_cpu_coupler_fault(struct mt6797_cpu_coupler *cpu)
{
	pr_err("mt6797-cpu-coupler: fencing CPU DVFS: invalid or unreadable VPROC/VSRAM pair\n");
	cpu->faulted = true;
	mt6797_dvfsp_handoff_rail_fault(cpu->handoff);
	return -EUCLEAN;
}

static int mt6797_cpu_coupler_read(struct mt6797_cpu_coupler *cpu,
				 struct mt6797_cpu_rail_pair *pair)
{
	int ret;

	ret = regulator_get_voltage_rdev(cpu->vproc);
	if (ret < 0)
		return ret;
	pair->vproc_uv = ret;
	ret = regulator_get_voltage_rdev(cpu->vsram);
	if (ret < 0)
		return ret;
	pair->vsram_uv = ret;
	return mt6797_cpu_rail_pair_valid(pair) ? 0 : -EPROTO;
}

static int mt6797_cpu_coupler_range(struct regulator_dev *rdev,
				  int *min_uv, int *max_uv, int default_min)
{
	int ret;

	*min_uv = 0;
	*max_uv = INT_MAX;
	ret = regulator_check_consumers(rdev, min_uv, max_uv, PM_SUSPEND_ON);
	if (ret)
		return ret;
	if (!*min_uv)
		*min_uv = default_min;
	*min_uv = max(*min_uv, rdev->constraints->min_uV);
	*max_uv = min(*max_uv, rdev->constraints->max_uV);
	return *min_uv <= *max_uv ? 0 : -EINVAL;
}

static int mt6797_cpu_coupler_balance(struct regulator_coupler *coupler,
				      struct regulator_dev *rdev,
				      suspend_state_t state)
{
	struct mt6797_cpu_coupler *cpu = container_of(coupler,
					struct mt6797_cpu_coupler, coupler);
	struct mt6797_cpu_rail_pair from, next, actual, target;
	struct regulator_dev *changed;
	int pmin, pmax, smin, smax, voltage, high, ret, cci_owned;
	unsigned int step;

	if (state != PM_SUSPEND_ON)
		return -EOPNOTSUPP;
	if (!cpu->vproc || !cpu->vsram || !cpu->handoff ||
	    (rdev != cpu->vproc && rdev != cpu->vsram))
		return -ENODEV;
	if (cpu->faulted)
		return -EHOSTDOWN;
	cci_owned = mt6797_dvfsp_handoff_cci_status(cpu->handoff);
	if (cci_owned < 0)
		return cci_owned;
	ret = mt6797_cpu_coupler_read(cpu, &from);
	if (ret)
		return ret == -EUCLEAN || ret == -EPROTO ?
			mt6797_cpu_coupler_fault(cpu) : ret;
	ret = mt6797_cpu_coupler_range(cpu->vproc, &pmin, &pmax, from.vproc_uv);
	if (ret)
		return ret;
	/* No boot-order assumption: preserve the inherited rail until CCI votes. */
	if (!cci_owned)
		pmin = max(pmin, (int)from.vproc_uv);
	ret = mt6797_cpu_coupler_range(cpu->vsram, &smin, &smax, 0);
	if (ret)
		return ret;
	if (pmin < MT6797_CPU_VPROC_MIN_UV ||
	    pmax > MT6797_CPU_VSRAM_MAX_UV)
		return -EINVAL;
	pmin = DIV_ROUND_UP(pmin, MT6797_CPU_VPROC_STEP_UV) *
		MT6797_CPU_VPROC_STEP_UV;
	for (; pmin <= pmax; pmin += MT6797_CPU_VPROC_STEP_UV) {
		ret = mt6797_cpu_rail_target(pmin, &target);
		if (!ret && target.vsram_uv >= smin && target.vsram_uv <= smax)
			break;
	}
	if (pmin > pmax)
		return -EINVAL;

	/* Check the entire path against machine constraints before any write. */
	actual = from;
	for (step = 0; step < 8; step++) {
		ret = mt6797_cpu_rail_next(&actual, target.vproc_uv, &next);
		if (ret < 0)
			return ret;
		if (!ret)
			break;
		changed = next.vproc_uv != actual.vproc_uv ? cpu->vproc : cpu->vsram;
		voltage = changed == cpu->vproc ? next.vproc_uv : next.vsram_uv;
		high = voltage;
		ret = regulator_check_voltage(changed, &voltage, &high);
		if (ret || voltage != high)
			return -EINVAL;
		actual = next;
	}
	if (step == 8)
		return -ERANGE;

	/* All coupled consumer locks are held by the regulator framework. */
	for (step = 0; step < 8; step++) {
		ret = mt6797_cpu_rail_next(&from, target.vproc_uv, &next);
		if (!ret)
			return 0;
		if (ret < 0)
			return mt6797_cpu_coupler_fault(cpu);
		changed = next.vproc_uv != from.vproc_uv ? cpu->vproc : cpu->vsram;
		voltage = changed == cpu->vproc ? next.vproc_uv : next.vsram_uv;
		/* Providers own settling and readback. Never hold CSPM across I2C. */
		ret = regulator_set_voltage_rdev(changed, voltage, voltage, state);
		if (ret)
			return mt6797_cpu_coupler_fault(cpu);
		ret = mt6797_cpu_coupler_read(cpu, &actual);
		if (ret || actual.vproc_uv != next.vproc_uv ||
		    actual.vsram_uv != next.vsram_uv)
			return mt6797_cpu_coupler_fault(cpu);
		from = actual;
	}
	return mt6797_cpu_coupler_fault(cpu);
}

/* Match physical endpoints, never arbitrary regulator-name strings. */
static int mt6797_cpu_coupler_role(struct device_node *node)
{
	struct device_node *regulators, *chip;
	bool vproc;

	if (!node)
		return 0;
	if (of_device_is_compatible(node, "mediatek,mt6797-cpu-sram-regulator"))
		return 2;
	if (!of_node_name_eq(node, "BUCKA"))
		return 0;
	regulators = of_get_parent(node);
	chip = of_get_parent(regulators);
	vproc = of_node_name_eq(regulators, "regulators") &&
		of_device_is_compatible(chip, "dlg,da9214-legacy");
	of_node_put(chip);
	of_node_put(regulators);
	return vproc ? 1 : 0;
}

static int mt6797_cpu_coupler_attach(struct regulator_coupler *coupler,
				     struct regulator_dev *rdev)
{
	struct mt6797_cpu_coupler *cpu = container_of(coupler,
					struct mt6797_cpu_coupler, coupler);
	struct device_node *peer, *back;
	struct mt6797_dvfsp_handoff *handoff;
	int role = mt6797_cpu_coupler_role(rdev->dev.of_node);
	bool valid;

	if (!role)
		return 1;
	if (rdev->coupling_desc.n_coupled != 2 || !rdev->constraints->always_on ||
	    rdev->constraints->max_spread[0] != MT6797_CPU_VSRAM_MAX_DELTA_UV)
		return -EINVAL;
	peer = of_parse_phandle(rdev->dev.of_node, "regulator-coupled-with", 0);
	back = of_parse_phandle(peer, "regulator-coupled-with", 0);
	valid = mt6797_cpu_coupler_role(peer) == 3 - role && back == rdev->dev.of_node;
	if (role == 1 && cpu->vsram && peer != cpu->vsram->dev.of_node)
		valid = false;
	if (role == 2 && cpu->vproc && peer != cpu->vproc->dev.of_node)
		valid = false;
	of_node_put(back);
	of_node_put(peer);
	if (!valid)
		return -EINVAL;
	if (role == 1) {
		if (cpu->vproc)
			return -EBUSY;
		cpu->vproc = rdev;
	} else {
		if (cpu->vsram)
			return -EBUSY;
		handoff = mt6797_dvfsp_handoff_get(rdev->dev.parent);
		if (IS_ERR(handoff))
			return PTR_ERR(handoff);
		cpu->handoff = handoff;
		cpu->vsram = rdev;
	}
	return 0;
}

static int mt6797_cpu_coupler_detach(struct regulator_coupler *coupler,
				     struct regulator_dev *rdev)
{
	struct mt6797_cpu_coupler *cpu = container_of(coupler,
					struct mt6797_cpu_coupler, coupler);

	if (cpu->vproc == rdev)
		cpu->vproc = NULL;
	if (cpu->vsram == rdev) {
		cpu->vsram = NULL;
		cpu->handoff = NULL;
	}
	return 0;
}

static struct mt6797_cpu_coupler mt6797_cpu_coupler = {
	.coupler = {
		.attach_regulator = mt6797_cpu_coupler_attach,
		.detach_regulator = mt6797_cpu_coupler_detach,
		.balance_voltage = mt6797_cpu_coupler_balance,
	},
};

static int __init mt6797_cpu_coupler_init(void)
{
	if (!of_machine_is_compatible("mediatek,mt6797"))
		return 0;
	return regulator_coupler_register(&mt6797_cpu_coupler.coupler);
}
arch_initcall(mt6797_cpu_coupler_init);

MODULE_DESCRIPTION("MediaTek MT6797 shared CPU and SRAM regulator coupler");
MODULE_LICENSE("GPL");
