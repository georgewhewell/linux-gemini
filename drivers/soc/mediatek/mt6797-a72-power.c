// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 Cortex-A72 cluster power coordination. */

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
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

#define MT6797_VPROC_UV			1000000

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
	void __iomem *spm;
	void __iomem *mcucfg;
	struct mutex lock; /* Serializes cluster state and hardware operations. */
	cpumask_t cpus;
	cpumask_t powered;
	unsigned int cpu8;
	unsigned int cpu9;
	enum mt6797_a72_state state;
	bool allow_cpu9;
	bool allow_cpu_off;
	bool rail_vote;
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

static int mt6797_a72_prepare_first(struct mt6797_a72_power *power)
{
	u32 isolation, poweron;
	bool reset_asserted = false;
	int old_uv;
	int ret;

	poweron = readl(power->spm + SPM_POWERON_CONFIG_EN);
	if (poweron != SPM_POWERON_PROOF_OFF &&
	    poweron != SPM_POWERON_PROOF_ON)
		return -ERANGE;

	writel(poweron | BIT(0), power->spm + SPM_POWERON_CONFIG_EN);
	if (readl(power->spm + SPM_POWERON_CONFIG_EN) !=
	    SPM_POWERON_PROOF_ON) {
		ret = -EIO;
		goto restore_poweron;
	}

	/* Vendor ordering read from BIG_ARMPLL_CON0 before latching PWRAP. */
	readl(power->mcucfg + MCUCFG_BIG_ARMPLL_CON0);
	ret = reset_control_assert(power->pwrap_reset);
	if (ret)
		goto restore_poweron;
	reset_asserted = true;

	old_uv = regulator_get_voltage(power->vproc);
	if (old_uv < 0) {
		ret = old_uv;
		goto rollback;
	}
	ret = regulator_set_voltage(power->vproc, MT6797_VPROC_UV,
				    MT6797_VPROC_UV);
	if (ret)
		goto rollback;
	ret = regulator_enable(power->vproc);
	if (ret)
		goto restore_voltage;
	power->rail_vote = true;
	usleep_range(1000, 1250);
	ret = regulator_is_enabled(power->vproc);
	if (ret <= 0) {
		ret = ret ?: -EIO;
		goto disable_rail;
	}
	ret = regulator_get_voltage(power->vproc);
	if (ret != MT6797_VPROC_UV) {
		ret = ret < 0 ? ret : -ERANGE;
		goto disable_rail;
	}

	isolation = readl(power->spm + SPM_EXT_BUCK_ISO);
	if ((isolation & SPM_EXT_BUCK_ISO_MASK) != BIT(1) &&
	    (isolation & SPM_EXT_BUCK_ISO_MASK)) {
		ret = -ERANGE;
		goto disable_rail;
	}
	writel(isolation & ~SPM_EXT_BUCK_ISO_MASK,
	       power->spm + SPM_EXT_BUCK_ISO);
	if (readl(power->spm + SPM_EXT_BUCK_ISO) &
	    SPM_EXT_BUCK_ISO_MASK) {
		ret = -EIO;
		goto restore_isolation;
	}
	ret = reset_control_deassert(power->pwrap_reset);
	if (ret)
		goto restore_isolation;
	reset_asserted = false;

	usleep_range(240, 260);
	ret = mt6797_a72_sram_enable();
	if (ret)
		goto faulted;
	dev_info(power->dev,
		 "CPU%u power ready: VPROC=1000000uV SRAM=1100000uV\n",
		 power->cpu8);
	return 0;

faulted:
	/* No firmware ABI exists to undo the SRAM write safely. Keep rails on. */
	dev_err(power->dev, "SRAM LDO sequence failed after mutation: %d\n", ret);
	power->state = MT6797_A72_FAULT;
	return ret;

restore_isolation:
	writel(isolation, power->spm + SPM_EXT_BUCK_ISO);
disable_rail:
	regulator_disable(power->vproc);
	power->rail_vote = false;
restore_voltage:
	regulator_set_voltage(power->vproc, old_uv, old_uv);
rollback:
	if (reset_asserted)
		reset_control_deassert(power->pwrap_reset);
restore_poweron:
	writel(poweron, power->spm + SPM_POWERON_CONFIG_EN);
	return ret;
}

static void mt6797_a72_dcm_set(struct mt6797_a72_power *power, bool on)
{
	u32 value = readl(power->mcucfg + MCUCFG_MP2_SYNC_DCM);

	value &= ~MCUCFG_MP2_SYNC_DCM_MASK;
	if (on) {
		writel(value | MCUCFG_MP2_SYNC_DCM_TOGGLE,
		       power->mcucfg + MCUCFG_MP2_SYNC_DCM);
		writel(value | MCUCFG_MP2_SYNC_DCM_ON,
		       power->mcucfg + MCUCFG_MP2_SYNC_DCM);
	} else {
		writel(value | BIT(1), power->mcucfg + MCUCFG_MP2_SYNC_DCM);
		writel(value, power->mcucfg + MCUCFG_MP2_SYNC_DCM);
	}
}

int mt6797_a72_power_prepare(unsigned int cpu)
{
	struct mt6797_a72_power *power;
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
	if (power->state == MT6797_A72_FAULT ||
	    power->state == MT6797_A72_STARTING) {
		ret = -EIO;
		goto out;
	}
	if (power->state == MT6797_A72_OFF) {
		if (cpu != power->cpu8) {
			ret = -EPERM;
			goto out;
		}
		power->state = MT6797_A72_STARTING;
		ret = mt6797_a72_prepare_first(power);
		if (ret && power->state != MT6797_A72_FAULT)
			power->state = MT6797_A72_OFF;
	}
out:
	mutex_unlock(&power->lock);
out_owner:
	mutex_unlock(&mt6797_a72_owner_lock);
	return ret;
}

void mt6797_a72_power_complete(unsigned int cpu, int psci_ret)
{
	struct mt6797_a72_power *power;

	mutex_lock(&mt6797_a72_owner_lock);
	power = mt6797_a72_owner;
	if (!power)
		goto out_owner;
	mutex_lock(&power->lock);
	if (psci_ret) {
		if (power->state == MT6797_A72_STARTING) {
			power->state = MT6797_A72_FAULT;
			dev_err(power->dev,
				"PSCI CPU_ON failed after power-up: CPU%u: %d\n",
				cpu, psci_ret);
		}
		goto out;
	}
	if (power->state == MT6797_A72_STARTING) {
		mt6797_a72_dcm_set(power, true);
		power->state = MT6797_A72_ON;
	}
	cpumask_set_cpu(cpu, &power->powered);
	dev_info(power->dev, "PSCI accepted CPU%u; cluster mask=%*pbl\n",
		 cpu, cpumask_pr_args(&power->powered));
out:
	mutex_unlock(&power->lock);
out_owner:
	mutex_unlock(&mt6797_a72_owner_lock);
}

int mt6797_a72_power_can_disable(unsigned int cpu)
{
	struct mt6797_a72_power *power;
	int ret = -EPERM;

	mutex_lock(&mt6797_a72_owner_lock);
	power = mt6797_a72_owner;
	if (!power)
		goto out;
	mutex_lock(&power->lock);
	if (cpumask_test_cpu(cpu, &power->cpus) && power->allow_cpu_off &&
	    power->state == MT6797_A72_ON)
		ret = 0;
	mutex_unlock(&power->lock);
out:
	mutex_unlock(&mt6797_a72_owner_lock);
	return ret;
}

void mt6797_a72_power_off(unsigned int cpu)
{
	struct mt6797_a72_power *power;

	mutex_lock(&mt6797_a72_owner_lock);
	power = mt6797_a72_owner;
	if (!power)
		goto out_owner;
	mutex_lock(&power->lock);
	cpumask_clear_cpu(cpu, &power->powered);
	if (power->state == MT6797_A72_ON && cpumask_empty(&power->powered)) {
		mt6797_a72_dcm_set(power, false);
		if (power->rail_vote) {
			int ret = regulator_disable(power->vproc);

			if (ret) {
				power->state = MT6797_A72_FAULT;
				dev_err(power->dev,
					"failed to disable VPROC: %d\n", ret);
				goto out;
			}
			power->rail_vote = false;
		}
		power->state = MT6797_A72_OFF;
	}
out:
	mutex_unlock(&power->lock);
out_owner:
	mutex_unlock(&mt6797_a72_owner_lock);
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
	struct device_node *spm_np;
	struct resource spm_res;
	struct mt6797_a72_power *power;
	struct device *dev = &pdev->dev;
	int ret;

	power = devm_kzalloc(dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;
	power->dev = dev;
	mutex_init(&power->lock);
	ret = mt6797_a72_parse_cpus(dev, power);
	if (ret)
		return ret;
	if (cpumask_intersects(cpu_online_mask, &power->cpus))
		return dev_err_probe(dev, -EBUSY,
				     "A72 CPU online before power owner\n");
	power->allow_cpu9 = of_property_read_bool(dev->of_node,
						  "mediatek,allow-cpu9");
	power->allow_cpu_off = of_property_read_bool(dev->of_node,
						     "mediatek,allow-cpu-off");
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
	spm_np = of_parse_phandle(dev->of_node, "mediatek,spm", 0);
	if (!spm_np)
		return dev_err_probe(dev, -EINVAL, "missing SPM phandle\n");
	ret = of_address_to_resource(spm_np, 0, &spm_res);
	of_node_put(spm_np);
	if (ret)
		return dev_err_probe(dev, ret, "invalid SPM resource\n");
	power->spm = devm_ioremap(dev, spm_res.start, resource_size(&spm_res));
	if (!power->spm)
		return -ENOMEM;

	mutex_lock(&mt6797_a72_owner_lock);
	if (mt6797_a72_owner)
		ret = -EBUSY;
	else
		mt6797_a72_owner = power;
	mutex_unlock(&mt6797_a72_owner_lock);
	if (ret)
		return ret;

	dev_info(dev, "CPU8 admission ready; CPU9 %s; CPU_OFF %s\n",
		 power->allow_cpu9 ? "enabled" : "vetoed",
		 power->allow_cpu_off ? "enabled" : "vetoed");
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
