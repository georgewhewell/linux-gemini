// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 Cortex-A72 cluster power coordination. */

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mt6797-a72-power.h>

#define SPM_POWERON_CONFIG_EN		0x218
#define SPM_EXT_BUCK_ISO		0x290
#define SPM_EXT_BUCK_ISO_MASK		GENMASK(1, 0)
#define SPM_POWERON_PROOF_OFF		0x00010132
#define SPM_POWERON_PROOF_ON		0x00010133

#define MCUCFG_MP2_SYNC_DCM		0x274
#define MCUCFG_BIG_ARMPLL_CON0		0x4a0
#define MCUCFG_MP2_SYNC_DCM_MASK	GENMASK(6, 0)
#define MCUCFG_MP2_SYNC_DCM_TOGGLE	0x0f
#define MCUCFG_MP2_SYNC_DCM_ON		0x0d

#define MT6797_BIGIDVFS_READ		0xc200035f
#define MT6797_BIGIDVFS_SRAM_SET	0xc20003bf
#define MT6797_BIGIDVFS_SRAM_SELECTOR	0x102222b0
#define MT6797_BIGIDVFS_SRAM_CALIBRATION 0x102222b4
#define MT6797_BIGIDVFS_SRAM_MV_X100	110000
#define MT6797_BIGIDVFS_SRAM_SEL_MASK	GENMASK(11, 0)
#define MT6797_BIGIDVFS_SRAM_SEL_1100MV	0x8fb

enum mt6797_a72_state {
	MT6797_A72_OFF,
	MT6797_A72_STARTING,
	MT6797_A72_ON,
	MT6797_A72_FAULT,
};

struct mt6797_a72_power {
	struct device *dev;
	struct regulator *vproc;
	struct reset_control *pwrap_reset;
	struct regmap *spm;
	void __iomem *mcucfg;
	struct mutex lock; /* Serializes cluster state and hardware operations. */
	raw_spinlock_t state_lock;
	cpumask_t cpus;
	cpumask_t accepted;
	cpumask_t powered;
	unsigned int cpu8;
	unsigned int cpu9;
	enum mt6797_a72_state state;
	bool allow_cpu9;
	bool rail_vote;
	int vproc_uv;
};

static DEFINE_MUTEX(mt6797_a72_owner_lock);
static struct mt6797_a72_power *mt6797_a72_owner;

static int mt6797_a72_smc_read(u32 address, u32 *value)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MT6797_BIGIDVFS_READ, address, 0, 0, 0, 0, 0, 0,
		      &res);
	if (res.a0 >> 32)
		return -EIO;
	*value = res.a0;
	return 0;
}

static int mt6797_a72_sram_enable(void)
{
	struct arm_smccc_res res;
	u32 calibration[2];
	u32 selector[2];
	int ret;

	arm_smccc_smc(MT6797_BIGIDVFS_SRAM_SET,
		      MT6797_BIGIDVFS_SRAM_MV_X100, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		return -EIO;

	usleep_range(240, 260);
	ret = mt6797_a72_smc_read(MT6797_BIGIDVFS_SRAM_SELECTOR,
				  &selector[0]);
	if (ret)
		return ret;
	ret = mt6797_a72_smc_read(MT6797_BIGIDVFS_SRAM_CALIBRATION,
				  &calibration[0]);
	if (ret)
		return ret;
	ret = mt6797_a72_smc_read(MT6797_BIGIDVFS_SRAM_SELECTOR,
				  &selector[1]);
	if (ret)
		return ret;
	ret = mt6797_a72_smc_read(MT6797_BIGIDVFS_SRAM_CALIBRATION,
				  &calibration[1]);
	if (ret)
		return ret;
	if (selector[0] != selector[1] || calibration[0] != calibration[1])
		return -EAGAIN;
	if ((selector[1] & MT6797_BIGIDVFS_SRAM_SEL_MASK) !=
	    MT6797_BIGIDVFS_SRAM_SEL_1100MV || !calibration[1] ||
	    calibration[1] > U16_MAX)
		return -ERANGE;

	return 0;
}

static void mt6797_a72_set_state(struct mt6797_a72_power *power,
				 enum mt6797_a72_state state)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&power->state_lock, flags);
	power->state = state;
	raw_spin_unlock_irqrestore(&power->state_lock, flags);
}

static int mt6797_a72_prepare_first(struct mt6797_a72_power *power)
{
	u32 isolation, poweron, value;
	bool reset_asserted = false;
	bool vproc_was_enabled;
	int cleanup_ret = 0;
	int enabled;
	int uv;
	int ret;

	/* Capture the inherited provider state before the first hardware write. */
	enabled = regulator_is_enabled(power->vproc);
	if (enabled < 0)
		return enabled;
	vproc_was_enabled = enabled;
	uv = regulator_get_voltage(power->vproc);
	if (uv <= 0)
		return uv ?: -ERANGE;
	power->vproc_uv = uv;

	ret = regmap_read(power->spm, SPM_POWERON_CONFIG_EN, &poweron);
	if (ret)
		return ret;
	if (poweron != SPM_POWERON_PROOF_OFF &&
	    poweron != SPM_POWERON_PROOF_ON)
		return -ERANGE;

	ret = regmap_update_bits(power->spm, SPM_POWERON_CONFIG_EN, BIT(0),
				 BIT(0));
	if (ret)
		return ret;
	ret = regmap_read(power->spm, SPM_POWERON_CONFIG_EN, &value);
	if (ret || value != SPM_POWERON_PROOF_ON) {
		ret = ret ?: -EIO;
		goto restore_poweron;
	}

	/* The initial PLL, mux and divider remain secure-firmware owned. */
	readl(power->mcucfg + MCUCFG_BIG_ARMPLL_CON0);
	ret = reset_control_assert(power->pwrap_reset);
	if (ret)
		goto restore_poweron;
	reset_asserted = true;

	ret = regulator_enable(power->vproc);
	if (ret)
		goto rollback_preisolation;
	power->rail_vote = true;
	usleep_range(1000, 1250);
	enabled = regulator_is_enabled(power->vproc);
	if (enabled <= 0) {
		ret = enabled ?: -EIO;
		goto rollback_preisolation;
	}
	ret = regulator_get_voltage(power->vproc);
	if (ret != uv) {
		ret = ret < 0 ? ret : -ERANGE;
		goto rollback_preisolation;
	}

	ret = regmap_read(power->spm, SPM_EXT_BUCK_ISO, &isolation);
	if (ret)
		goto rollback_preisolation;
	if ((isolation & SPM_EXT_BUCK_ISO_MASK) != BIT(1) &&
	    (isolation & SPM_EXT_BUCK_ISO_MASK)) {
		ret = -ERANGE;
		goto rollback_preisolation;
	}

	/* No proven inverse exists once the isolation write is attempted. */
	ret = regmap_update_bits(power->spm, SPM_EXT_BUCK_ISO,
				 SPM_EXT_BUCK_ISO_MASK, 0);
	if (ret)
		goto faulted;
	ret = regmap_read(power->spm, SPM_EXT_BUCK_ISO, &value);
	if (ret || (value & SPM_EXT_BUCK_ISO_MASK)) {
		ret = ret ?: -EIO;
		goto faulted;
	}
	ret = reset_control_deassert(power->pwrap_reset);
	if (ret)
		goto faulted;
	reset_asserted = false;

	usleep_range(240, 260);
	ret = mt6797_a72_sram_enable();
	if (ret)
		goto faulted;
	dev_info(power->dev,
		 "CPU%u power ready: inherited VPROC=%duV SRAM=1100000uV\n",
		 power->cpu8, power->vproc_uv);
	return 0;

faulted:
	if (reset_asserted) {
		cleanup_ret = reset_control_deassert(power->pwrap_reset);
		if (cleanup_ret)
			dev_err(power->dev,
				"failed to release PWRAP reset after fault: %d\n",
				cleanup_ret);
	}
	/* Keep the rail, SPM reset release and isolation state as observed. */
	mt6797_a72_set_state(power, MT6797_A72_FAULT);
	dev_err(power->dev,
		"power sequence failed beyond isolation boundary: %d%s\n", ret,
		cleanup_ret ? "; PWRAP reset state uncertain" : "");
	return ret;

rollback_preisolation:
	if (reset_asserted) {
		cleanup_ret = reset_control_deassert(power->pwrap_reset);
		reset_asserted = false;
	}
	if (!cleanup_ret && power->rail_vote) {
		if (vproc_was_enabled) {
			/* Dropping the vote could disable an inherited live rail. */
			cleanup_ret = -EBUSY;
		} else {
			enabled = regulator_is_enabled(power->vproc);
			uv = regulator_get_voltage(power->vproc);
			if (enabled <= 0 || uv != power->vproc_uv)
				cleanup_ret = -EBUSY;
			else
				cleanup_ret = regulator_disable(power->vproc);
			if (!cleanup_ret)
				power->rail_vote = false;
		}
	}
	if (cleanup_ret) {
		mt6797_a72_set_state(power, MT6797_A72_FAULT);
		dev_err(power->dev,
			"pre-isolation cleanup retained power: %d\n", cleanup_ret);
		return ret;
	}

restore_poweron:
	cleanup_ret = regmap_update_bits(power->spm, SPM_POWERON_CONFIG_EN,
					 BIT(0), poweron & BIT(0));
	if (!cleanup_ret)
		cleanup_ret = regmap_read(power->spm, SPM_POWERON_CONFIG_EN,
					  &value);
	if (cleanup_ret || value != poweron) {
		mt6797_a72_set_state(power, MT6797_A72_FAULT);
		return cleanup_ret ?: -EIO;
	}
	return ret;
}

static int mt6797_a72_dcm_enable(struct mt6797_a72_power *power)
{
	u32 base = readl(power->mcucfg + MCUCFG_MP2_SYNC_DCM);
	u32 value;

	base &= ~MCUCFG_MP2_SYNC_DCM_MASK;
	writel(base | MCUCFG_MP2_SYNC_DCM_TOGGLE,
	       power->mcucfg + MCUCFG_MP2_SYNC_DCM);
	value = readl(power->mcucfg + MCUCFG_MP2_SYNC_DCM);
	if ((value & MCUCFG_MP2_SYNC_DCM_MASK) !=
	    MCUCFG_MP2_SYNC_DCM_TOGGLE)
		return -EIO;

	writel(base | MCUCFG_MP2_SYNC_DCM_ON,
	       power->mcucfg + MCUCFG_MP2_SYNC_DCM);
	value = readl(power->mcucfg + MCUCFG_MP2_SYNC_DCM);
	if ((value & MCUCFG_MP2_SYNC_DCM_MASK) != MCUCFG_MP2_SYNC_DCM_ON)
		return -EIO;

	return 0;
}

int mt6797_a72_power_prepare(unsigned int cpu)
{
	struct mt6797_a72_power *power;
	unsigned long flags;
	int ret = 0;

	mutex_lock(&mt6797_a72_owner_lock);
	power = mt6797_a72_owner;
	if (!power) {
		ret = -EPROBE_DEFER;
		goto out_owner;
	}
	mutex_lock(&power->lock);
	if (!cpumask_test_cpu(cpu, &power->cpus)) {
		ret = -EINVAL;
		goto out;
	}
	if (cpu == power->cpu9 && !power->allow_cpu9) {
		ret = -EPERM;
		goto out;
	}
	raw_spin_lock_irqsave(&power->state_lock, flags);
	if (power->state == MT6797_A72_FAULT ||
	    power->state == MT6797_A72_STARTING) {
		ret = -EIO;
		goto out_state;
	}
	if (power->state == MT6797_A72_OFF) {
		if (cpu != power->cpu8) {
			ret = -EPERM;
			goto out_state;
		}
		power->state = MT6797_A72_STARTING;
		cpumask_clear_cpu(cpu, &power->accepted);
		raw_spin_unlock_irqrestore(&power->state_lock, flags);
		ret = mt6797_a72_prepare_first(power);
		if (ret) {
			raw_spin_lock_irqsave(&power->state_lock, flags);
			if (power->state == MT6797_A72_STARTING)
				power->state = MT6797_A72_OFF;
			raw_spin_unlock_irqrestore(&power->state_lock, flags);
		}
		goto out;
	}
	cpumask_clear_cpu(cpu, &power->accepted);
out_state:
	raw_spin_unlock_irqrestore(&power->state_lock, flags);
out:
	mutex_unlock(&power->lock);
out_owner:
	mutex_unlock(&mt6797_a72_owner_lock);
	return ret;
}

void mt6797_a72_power_psci_return(unsigned int cpu, int psci_ret)
{
	struct mt6797_a72_power *power;
	unsigned long flags;

	mutex_lock(&mt6797_a72_owner_lock);
	power = mt6797_a72_owner;
	if (!power)
		goto out_owner;
	raw_spin_lock_irqsave(&power->state_lock, flags);
	if (psci_ret) {
		power->state = MT6797_A72_FAULT;
		raw_spin_unlock_irqrestore(&power->state_lock, flags);
		dev_err(power->dev,
			"PSCI CPU_ON failed after power-up: CPU%u: %d\n",
			cpu, psci_ret);
		goto out_owner;
	}
	cpumask_set_cpu(cpu, &power->accepted);
	raw_spin_unlock_irqrestore(&power->state_lock, flags);
	dev_info(power->dev, "PSCI accepted CPU%u; awaiting postboot\n", cpu);
out_owner:
	mutex_unlock(&mt6797_a72_owner_lock);
}

int mt6797_a72_power_postboot(unsigned int cpu)
{
	struct mt6797_a72_power *power = READ_ONCE(mt6797_a72_owner);
	int ret = 0;

	if (!power || !cpumask_test_cpu(cpu, &power->cpus))
		return -ENODEV;

	raw_spin_lock(&power->state_lock);
	if (!cpumask_test_cpu(cpu, &power->accepted)) {
		ret = -EPROTO;
		goto fault;
	}
	if (cpu == power->cpu8) {
		if (power->state != MT6797_A72_STARTING) {
			ret = -EPROTO;
			goto fault;
		}
		ret = mt6797_a72_dcm_enable(power);
		if (ret)
			goto fault;
		power->state = MT6797_A72_ON;
	} else if (power->state != MT6797_A72_ON || !power->allow_cpu9) {
		ret = -EPERM;
		goto fault;
	}
	cpumask_set_cpu(cpu, &power->powered);
	raw_spin_unlock(&power->state_lock);
	dev_info(power->dev, "CPU%u postboot complete; DCM=0x%02x\n",
		 cpu, MCUCFG_MP2_SYNC_DCM_ON);
	return 0;

fault:
	power->state = MT6797_A72_FAULT;
	raw_spin_unlock(&power->state_lock);
	dev_err(power->dev, "CPU%u postboot completion failed: %d\n", cpu, ret);
	return ret;
}

static int mt6797_a72_parse_cpus(struct device *dev,
				 struct mt6797_a72_power *power)
{
	struct device_node *np;
	int count, cpu, i;

	count = of_count_phandle_with_args(dev->of_node, "cpus", NULL);
	if (count != 2)
		return dev_err_probe(dev, -EINVAL,
				     "expected two CPU phandles\n");
	cpumask_clear(&power->cpus);
	for (i = 0; i < count; i++) {
		np = of_parse_phandle(dev->of_node, "cpus", i);
		if (!np)
			return -EINVAL;
		cpu = of_cpu_node_to_id(np);
		of_node_put(np);
		if (cpu < 0 || cpu >= nr_cpu_ids ||
		    cpumask_test_and_set_cpu(cpu, &power->cpus))
			return -EINVAL;
		if (!i)
			power->cpu8 = cpu;
		else
			power->cpu9 = cpu;
	}
	return 0;
}

static int mt6797_a72_power_probe(struct platform_device *pdev)
{
	struct mt6797_a72_power *power;
	struct device *dev = &pdev->dev;
	int ret;

	power = devm_kzalloc(dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;
	power->dev = dev;
	mutex_init(&power->lock);
	raw_spin_lock_init(&power->state_lock);
	cpumask_clear(&power->accepted);
	cpumask_clear(&power->powered);
	ret = mt6797_a72_parse_cpus(dev, power);
	if (ret)
		return ret;
	if (cpumask_intersects(cpu_online_mask, &power->cpus))
		return dev_err_probe(dev, -EBUSY,
				     "A72 CPU online before power owner\n");
	power->allow_cpu9 = of_property_read_bool(dev->of_node,
						  "mediatek,allow-cpu9");
	power->vproc = devm_regulator_get(dev, "vproc");
	if (IS_ERR(power->vproc))
		return dev_err_probe(dev, PTR_ERR(power->vproc),
				     "failed to get VPROC regulator\n");
	power->pwrap_reset = devm_reset_control_get_exclusive(dev, "pwrap");
	if (IS_ERR(power->pwrap_reset))
		return dev_err_probe(dev, PTR_ERR(power->pwrap_reset),
				     "failed to get PWRAP reset\n");
	power->mcucfg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(power->mcucfg))
		return PTR_ERR(power->mcucfg);
	power->spm = syscon_regmap_lookup_by_phandle(dev->of_node,
						     "mediatek,spm");
	if (IS_ERR(power->spm))
		return dev_err_probe(dev, PTR_ERR(power->spm),
				     "failed to get SPM regmap\n");

	mutex_lock(&mt6797_a72_owner_lock);
	if (mt6797_a72_owner)
		ret = -EBUSY;
	else
		WRITE_ONCE(mt6797_a72_owner, power);
	mutex_unlock(&mt6797_a72_owner_lock);
	if (ret)
		return ret;

	dev_info(dev, "CPU8 admission ready; CPU9 %s; CPU_OFF unavailable\n",
		 power->allow_cpu9 ? "enabled" : "vetoed");
	return 0;
}

static const struct of_device_id mt6797_a72_power_of_match[] = {
	{ .compatible = "mediatek,mt6797-a72-power" },
	{ }
};

static struct platform_driver mt6797_a72_power_driver = {
	.probe = mt6797_a72_power_probe,
	.driver = {
		.name = "mt6797-a72-power",
		.of_match_table = mt6797_a72_power_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6797_a72_power_driver);
