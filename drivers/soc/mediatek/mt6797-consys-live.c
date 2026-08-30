// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded, explicitly opt-in MT6797 CONSYS identity experiment.
 *
 * This driver is deliberately not a radio driver.  It exercises only the
 * reviewed vendor bring-up prefix needed to read the connectivity chip ID,
 * then unwinds its votes with CONMCU reset still asserted.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/mt6351-regulator.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/soc/mediatek/mt6797-consys.h>

#define MT6797_CONSYS_MCU_CHIP_ID		0x8
#define MT6797_CONSYS_MCU_CFG_ACR		0x110
#define MT6797_CONSYS_MCU_CFG_ACR_MBIST	BIT(18)
#define MT6797_CONSYS_INFRACFG_SLEEP_MASK	0x350
#define MT6797_CONSYS_INFRACFG_SLEEP_MASK_CONN	BIT(8)
#define MT6797_CONSYS_SPM_PWRON_CONFIG_EN	0x0
#define MT6797_CONSYS_SPM_PWRON_CONFIG_VALUE	0x0b160001
#define MT6797_CONSYS_SPM_PWR_STATUS		0x180
#define MT6797_CONSYS_SPM_PWR_STATUS_2ND	0x184
#define MT6797_CONSYS_CHIP_ID			0x0279

#define MT6797_CONSYS_CHIP_POLL_INTERVAL_US	20000
#define MT6797_CONSYS_CHIP_POLL_TIMEOUT_US	200000

static bool identity_experiment;
module_param_named(identity_experiment, identity_experiment, bool, 0400);
MODULE_PARM_DESC(identity_experiment, "allow the MT6797 CONSYS bounded identity experiment");

static bool prepostid_observe_experiment;
module_param_named(prepostid_observe_experiment, prepostid_observe_experiment,
		   bool, 0400);
MODULE_PARM_DESC(prepostid_observe_experiment,
		 "allow the MT6797 CONSYS reset-held pre-post-ID register observation");

struct mt6797_consys_live_resource {
	const char *name;
	resource_size_t start;
	resource_size_t size;
};

struct mt6797_consys_live {
	struct device *dev;
	void __iomem *mcu;
	void __iomem *afe;
	struct regmap *infracfg;
	struct regmap *spm;
	struct regulator *vcn18;
	struct regulator *vcn28;
	struct reset_control *conn_mcu_reset;
	struct resource shared_mem;
	struct mt6797_consys_lifecycle lifecycle;
	bool vcn18_vote;
	bool vcn28_vote;
	bool conn_domain_vote;
	bool runtime_pm_enabled;
	bool conn_domain_attached;
	bool prepostid_observe;
	u32 chip_id;
};

static const struct mt6797_consys_live_resource
	mt6797_consys_live_resources[] = {
	{ "mcu", 0x18070000, SZ_512 },
	{ "ap-rgu", 0x10007000, SZ_256 },
	{ "topckgen", 0x10000000, SZ_8K },
	{ "spm", 0x10006000, SZ_4K },
};

static const struct mt6797_consys_live_resource mt6797_consys_live_afe_resource = {
	"afe", 0x180b6000, SZ_256,
};

/*
 * These are the complete vendor AFE write targets, plus its optional GPS
 * setting. This experiment only reads them after a successful chip-ID poll,
 * while reset remains asserted; it never uses the vendor values or writes.
 */
static const u8 mt6797_consys_live_afe_observed_offsets[] = {
	0x14, 0x20, 0x50, 0x54, 0x60, 0x64, 0x68, 0x6c,
	0x70, 0x84, 0x88, 0x90,
};

static int mt6797_consys_live_map_resource(struct platform_device *pdev,
					  const struct mt6797_consys_live_resource *expected,
					  void __iomem **base)
{
	const struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, expected->name);
	if (!res)
		return dev_err_probe(&pdev->dev, -EINVAL,
					     "missing %s resource\n", expected->name);

	if (res->start != expected->start || resource_size(res) != expected->size)
		return dev_err_probe(&pdev->dev, -EINVAL,
					     "invalid %s resource\n", expected->name);

	if (base) {
		*base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(*base))
			return PTR_ERR(*base);
	}

	return 0;
}

/*
 * Vendor power-on fails closed when its CONSYS EMI reservation is absent.
 * Keep the same prerequisite for this earlier read-only identity slice.  The
 * identity experiment neither maps nor clears the reservation.
 */
static int mt6797_consys_live_acquire_shared_mem(struct mt6797_consys_live *consys)
{
	struct device *dev = consys->dev;
	struct device_node *memory_region;
	int ret;

	memory_region = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!memory_region)
		return dev_err_probe(dev, -EINVAL, "missing CONSYS shared memory\n");

	if (!of_property_read_bool(memory_region, "no-map")) {
		of_node_put(memory_region);
		return dev_err_probe(dev, -EINVAL,
				     "CONSYS shared memory must be no-map\n");
	}
	of_node_put(memory_region);

	ret = of_reserved_mem_region_to_resource_byname(dev->of_node, "shared",
							&consys->shared_mem);
	if (ret)
		return dev_err_probe(dev, ret, "failed to acquire CONSYS shared memory\n");

	if (!mt6797_consys_shared_mem_valid(consys->shared_mem.start,
					 resource_size(&consys->shared_mem)))
		return dev_err_probe(dev, -EINVAL,
				     "invalid CONSYS shared-memory geometry\n");

	return 0;
}

static int mt6797_consys_live_enable_rail(struct regulator *regulator,
					  int microvolts, bool hw_control)
{
	int ret;

	ret = mt6351_regulator_set_hw_control(regulator, hw_control);
	if (ret)
		return ret;

	ret = regulator_set_voltage(regulator, microvolts, microvolts);
	if (ret)
		goto restore_hw_control;

	ret = regulator_enable(regulator);
	if (!ret)
		return 0;

restore_hw_control:
	/* Keep the first failure, but never leave a failed rail in HW control. */
	mt6351_regulator_set_hw_control(regulator, false);
	return ret;
}

static int mt6797_consys_live_disable_rail(struct regulator *regulator,
					   bool *vote)
{
	int ret;

	/* Vendor teardown selects software control before dropping the enable vote. */
	ret = mt6351_regulator_set_hw_control(regulator, false);
	if (*vote) {
		int disable_ret;

		disable_ret = regulator_disable(regulator);
		if (!disable_ret)
			*vote = false;
		if (!ret)
			ret = disable_ret;
	}

	return ret;
}

static int mt6797_consys_live_enable_vcn18(void *context)
{
	struct mt6797_consys_live *consys = context;
	int ret;

	ret = mt6797_consys_live_enable_rail(consys->vcn18, 1800000, false);
	if (!ret)
		consys->vcn18_vote = true;
	return ret;
}

static int mt6797_consys_live_disable_vcn18(void *context)
{
	struct mt6797_consys_live *consys = context;

	return mt6797_consys_live_disable_rail(consys->vcn18,
					       &consys->vcn18_vote);
}

static int mt6797_consys_live_enable_vcn28(void *context)
{
	struct mt6797_consys_live *consys = context;
	int ret;

	ret = mt6797_consys_live_enable_rail(consys->vcn28, 2800000, true);
	if (!ret)
		consys->vcn28_vote = true;
	return ret;
}

static int mt6797_consys_live_disable_vcn28(void *context)
{
	struct mt6797_consys_live *consys = context;

	return mt6797_consys_live_disable_rail(consys->vcn28,
					       &consys->vcn28_vote);
}

static int mt6797_consys_live_configure_sleep_mask(void *context)
{
	struct mt6797_consys_live *consys = context;

	/* Vendor 0x10000000 + 0x1350 is INFRACFG offset 0x350. */
	return regmap_update_bits(consys->infracfg,
				  MT6797_CONSYS_INFRACFG_SLEEP_MASK,
				  MT6797_CONSYS_INFRACFG_SLEEP_MASK_CONN,
				  MT6797_CONSYS_INFRACFG_SLEEP_MASK_CONN);
}

static int mt6797_consys_live_assert_reset(void *context)
{
	struct mt6797_consys_live *consys = context;

	return reset_control_assert(consys->conn_mcu_reset);
}

static int mt6797_consys_live_configure_pwron(void *context)
{
	struct mt6797_consys_live *consys = context;

	return regmap_write(consys->spm, MT6797_CONSYS_SPM_PWRON_CONFIG_EN,
			    MT6797_CONSYS_SPM_PWRON_CONFIG_VALUE);
}

static int mt6797_consys_live_enable_scp_sys_conn(void *context)
{
	struct mt6797_consys_live *consys = context;
	struct of_phandle_args args;
	int ret;

	ret = of_parse_phandle_with_args(consys->dev->of_node,
					 "mediatek,conn-power-domain",
					 "#power-domain-cells", 0, &args);
	if (ret)
		return ret;

	ret = of_genpd_add_device(&args, consys->dev);
	of_node_put(args.np);
	if (ret)
		return ret;
	consys->conn_domain_attached = true;

	pm_runtime_enable(consys->dev);
	consys->runtime_pm_enabled = true;
	ret = pm_runtime_resume_and_get(consys->dev);
	if (!ret)
		consys->conn_domain_vote = true;
	return ret;
}

static int mt6797_consys_live_disable_scp_sys_conn(void *context)
{
	struct mt6797_consys_live *consys = context;
	int ret;

	if (!consys->conn_domain_vote)
		goto detach_domain;

	ret = pm_runtime_put_sync_suspend(consys->dev);
	if (ret < 0) {
		/* put_sync_suspend() drops the count even when suspend fails. */
		pm_runtime_get_noresume(consys->dev);
		return ret;
	}
	consys->conn_domain_vote = false;

detach_domain:
	if (consys->runtime_pm_enabled) {
		pm_runtime_disable(consys->dev);
		consys->runtime_pm_enabled = false;
	}

	if (!consys->conn_domain_attached)
		return 0;

	ret = pm_genpd_remove_device(consys->dev);
	if (!ret)
		consys->conn_domain_attached = false;
	return ret;
}

static int mt6797_consys_live_poll(void *context,
				  enum mt6797_consys_poll poll)
{
	struct mt6797_consys_live *consys = context;

	if (poll != MT6797_CONSYS_POLL_CHIP_READY)
		return -EINVAL;

	consys->chip_id = readl(consys->mcu + MT6797_CONSYS_MCU_CHIP_ID);
	return consys->chip_id == MT6797_CONSYS_CHIP_ID ? 0 : -EAGAIN;
}

static void mt6797_consys_live_delay_us(void *context, u32 min, u32 max)
{
	usleep_range(min, max);
}

static void mt6797_consys_live_log_state(struct mt6797_consys_live *consys,
					 const char *stage)
{
	bool vcn18_hw = false;
	bool vcn28_hw = false;
	unsigned int pwron = U32_MAX;
	unsigned int pwr_status = U32_MAX;
	unsigned int pwr_status_2nd = U32_MAX;
	int reset;
	int ret18;
	int ret28;
	int ret_pwron;
	int ret_status;
	int ret_status_2nd;

	reset = reset_control_status(consys->conn_mcu_reset);
	ret18 = mt6351_regulator_get_hw_control(consys->vcn18, &vcn18_hw);
	ret28 = mt6351_regulator_get_hw_control(consys->vcn28, &vcn28_hw);
	ret_pwron = regmap_read(consys->spm,
				MT6797_CONSYS_SPM_PWRON_CONFIG_EN, &pwron);
	ret_status = regmap_read(consys->spm, MT6797_CONSYS_SPM_PWR_STATUS,
				 &pwr_status);
	ret_status_2nd = regmap_read(consys->spm,
				     MT6797_CONSYS_SPM_PWR_STATUS_2ND,
				     &pwr_status_2nd);
	dev_info(consys->dev,
		 "%s: reset=%d pwron=0x%08x(%d) pwr-status=0x%08x(%d)/0x%08x(%d) "
		 "vcn18=enabled:%d hw:%d(%d) vcn28=enabled:%d hw:%d(%d)\n",
		 stage, reset, pwron, ret_pwron, pwr_status, ret_status,
		 pwr_status_2nd, ret_status_2nd,
		 regulator_is_enabled(consys->vcn18), vcn18_hw, ret18,
		 regulator_is_enabled(consys->vcn28), vcn28_hw, ret28);
}

static void mt6797_consys_live_observe_prepostid(struct mt6797_consys_live *consys)
{
	u32 acr;
	unsigned int i;

	/* The identity lease keeps CONN powered and CONMCU reset asserted here. */
	acr = readl(consys->mcu + MT6797_CONSYS_MCU_CFG_ACR);
	dev_info(consys->dev,
		 "pre-postid observe: mcu[0x%03x]=0x%08x mbist=%u (no writes)\n",
		 MT6797_CONSYS_MCU_CFG_ACR, acr,
		 !!(acr & MT6797_CONSYS_MCU_CFG_ACR_MBIST));

	for (i = 0; i < ARRAY_SIZE(mt6797_consys_live_afe_observed_offsets); i++) {
		u8 offset = mt6797_consys_live_afe_observed_offsets[i];

		dev_info(consys->dev,
			 "pre-postid observe: afe[0x%02x]=0x%08x (no writes)\n",
			 offset, readl(consys->afe + offset));
	}
}

static const struct mt6797_consys_ops mt6797_consys_live_ops = {
	.abi = MT6797_CONSYS_LIFECYCLE_ABI,
	.enable_vcn18 = mt6797_consys_live_enable_vcn18,
	.disable_vcn18 = mt6797_consys_live_disable_vcn18,
	.enable_vcn28 = mt6797_consys_live_enable_vcn28,
	.disable_vcn28 = mt6797_consys_live_disable_vcn28,
	.configure_sleep_mask = mt6797_consys_live_configure_sleep_mask,
	.assert_reset = mt6797_consys_live_assert_reset,
	.configure_pwron = mt6797_consys_live_configure_pwron,
	.enable_scp_sys_conn = mt6797_consys_live_enable_scp_sys_conn,
	.disable_scp_sys_conn = mt6797_consys_live_disable_scp_sys_conn,
	.poll = mt6797_consys_live_poll,
	.delay_us = mt6797_consys_live_delay_us,
};

static void mt6797_consys_live_release_domain(struct mt6797_consys_live *consys)
{
	int ret;

	if (consys->conn_domain_vote || consys->conn_domain_attached) {
		ret = mt6797_consys_live_disable_scp_sys_conn(consys);
		if (ret)
			dev_err(consys->dev, "failed to release CONN domain vote: %d\n",
				ret);
	}

}

static int mt6797_consys_live_probe(struct platform_device *pdev)
{
	struct mt6797_consys_lifecycle *lifecycle;
	struct mt6797_consys_live *consys;
	struct mt6797_consys_timeouts timeouts = {
		.poll_interval_us = MT6797_CONSYS_CHIP_POLL_INTERVAL_US,
		.chip_ready_timeout_us = MT6797_CONSYS_CHIP_POLL_TIMEOUT_US,
	};
	int ret;

	if (of_device_is_compatible(pdev->dev.of_node,
				    "mediatek,mt6797-consys-prepostid-observe") &&
	    !prepostid_observe_experiment)
		return dev_err_probe(&pdev->dev, -EPERM,
					     "pre-postid observation needs explicit boot opt-in\n");

	if (!identity_experiment)
		return dev_err_probe(&pdev->dev, -EPERM,
					     "identity experiment needs explicit boot opt-in\n");

	consys = devm_kzalloc(&pdev->dev, sizeof(*consys), GFP_KERNEL);
	if (!consys)
		return -ENOMEM;
	consys->dev = &pdev->dev;
	consys->prepostid_observe =
		of_device_is_compatible(pdev->dev.of_node,
					"mediatek,mt6797-consys-prepostid-observe");

	ret = mt6797_consys_live_map_resource(pdev,
					     &mt6797_consys_live_resources[0], &consys->mcu);
	if (ret)
		return ret;

	ret = mt6797_consys_live_map_resource(pdev,
					     &mt6797_consys_live_resources[1], NULL);
	if (ret)
		return ret;

	ret = mt6797_consys_live_map_resource(pdev,
					     &mt6797_consys_live_resources[2], NULL);
	if (ret)
		return ret;

	ret = mt6797_consys_live_map_resource(pdev,
					     &mt6797_consys_live_resources[3], NULL);
	if (ret)
		return ret;

	if (consys->prepostid_observe) {
		ret = mt6797_consys_live_map_resource(pdev, &mt6797_consys_live_afe_resource,
						      &consys->afe);
		if (ret)
			return ret;
	}

	ret = mt6797_consys_live_acquire_shared_mem(consys);
	if (ret)
		return ret;

	consys->infracfg = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							   "mediatek,infracfg");
	if (IS_ERR(consys->infracfg))
		return dev_err_probe(&pdev->dev, PTR_ERR(consys->infracfg),
				     "failed to acquire INFRACFG regmap\n");

	consys->spm = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						      "mediatek,spm");
	if (IS_ERR(consys->spm))
		return dev_err_probe(&pdev->dev, PTR_ERR(consys->spm),
				     "failed to acquire SPM regmap\n");

	consys->vcn18 = devm_regulator_get(&pdev->dev, "vcn18");
	if (IS_ERR(consys->vcn18))
		return dev_err_probe(&pdev->dev, PTR_ERR(consys->vcn18),
					     "failed to acquire VCN18\n");

	consys->vcn28 = devm_regulator_get(&pdev->dev, "vcn28");
	if (IS_ERR(consys->vcn28))
		return dev_err_probe(&pdev->dev, PTR_ERR(consys->vcn28),
					     "failed to acquire VCN28\n");

	consys->conn_mcu_reset =
		devm_reset_control_get_exclusive(&pdev->dev, "conn-mcu");
	if (IS_ERR(consys->conn_mcu_reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(consys->conn_mcu_reset),
					     "failed to acquire CONMCU reset\n");

	lifecycle = &consys->lifecycle;
	ret = mt6797_consys_lifecycle_init(lifecycle, &mt6797_consys_live_ops,
					 consys, &timeouts);
	if (ret)
		goto disable_domain;

	mt6797_consys_live_log_state(consys, "pre-identity");
	ret = mt6797_consys_identity_get(lifecycle);
	if (ret) {
		mt6797_consys_live_log_state(consys, "identity-failed");
		goto disable_domain;
	}

	if (consys->prepostid_observe)
		mt6797_consys_live_observe_prepostid(consys);

	ret = mt6797_consys_identity_put(lifecycle);
	if (ret) {
		mt6797_consys_live_log_state(consys, "identity-put-failed");
		goto disable_domain;
	}
	mt6797_consys_live_log_state(consys, "post-identity");

	platform_set_drvdata(pdev, consys);
	dev_info(&pdev->dev,
		 "identity 0x%04x observed; reset remains asserted and votes released\n",
		 consys->chip_id);
	return 0;

disable_domain:
	mt6797_consys_live_release_domain(consys);
	return ret;
}

static void mt6797_consys_live_remove(struct platform_device *pdev)
{
	struct mt6797_consys_live *consys = platform_get_drvdata(pdev);

	mt6797_consys_live_release_domain(consys);
}

static const struct of_device_id mt6797_consys_live_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys-identity" },
	{ .compatible = "mediatek,mt6797-consys-prepostid-observe" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_consys_live_of_match);

static struct platform_driver mt6797_consys_live_driver = {
	.probe = mt6797_consys_live_probe,
	.remove = mt6797_consys_live_remove,
	.driver = {
		.name = "mt6797-consys-live",
		.of_match_table = mt6797_consys_live_of_match,
	},
};
module_platform_driver(mt6797_consys_live_driver);

MODULE_DESCRIPTION("Explicitly opt-in MT6797 CONSYS identity experiment");
MODULE_LICENSE("GPL");
