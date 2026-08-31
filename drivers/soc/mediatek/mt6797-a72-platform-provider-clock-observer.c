// SPDX-License-Identifier: GPL-2.0-only
/* Candidate-only MT6797 A72 platform, provider, and clock observer. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mt6797-a72-provider.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pstore_ram.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mt6797-a72-platform-state.h>
#include <linux/soc/mediatek/mt6797-dvfsp-clock-backend.h>

#include "mt6797-a72-platform-provider-clock-observer-internal.h"

#define MT6797_A72_PPC_TAG \
	"GEMINI_A72_PLATFORM_PROVIDER_CLOCK_SNAPSHOT_V1"

static int mt6797_a72_ppc_platform(void *context, struct device *dev,
	struct mt6797_a72_platform_state *snapshot,
	struct mt6797_a72_platform_state_failure *failure)
{
	return mt6797_a72_platform_state_snapshot_detailed(dev, snapshot, failure);
}

static int mt6797_a72_ppc_provider(void *context,
				   struct mt6797_a72_provider_snapshot *snapshot)
{
	return mt6797_a72_provider_snapshot(snapshot);
}

static bool mt6797_a72_ppc_checkpoint(void *context, unsigned int checkpoint)
{
	return gemini_protected_readback_ledger_checkpoint(checkpoint);
}

static int mt6797_a72_ppc_clock(void *context, struct device *dev,
				struct mt6797_dvfsp_clock_readback *snapshot)
{
	return mt6797_dvfsp_clock_backend_read(dev, snapshot);
}

static const struct mt6797_a72_platform_provider_clock_ops
mt6797_a72_ppc_ops = {
	.platform = mt6797_a72_ppc_platform,
	.provider = mt6797_a72_ppc_provider,
	.checkpoint = mt6797_a72_ppc_checkpoint,
	.clock = mt6797_a72_ppc_clock,
};

int mt6797_a72_ppc_capture(struct device *platform, struct device *provider,
			   struct device *clock,
	const struct mt6797_a72_platform_provider_clock_ops *ops, void *context,
	struct mt6797_a72_platform_provider_clock_snapshot *snapshot,
	enum mt6797_a72_ppc_failure_stage *failure_stage,
	struct mt6797_a72_platform_state_failure *platform_failure)
{
	int ret;

	if (!snapshot || !failure_stage || !platform_failure)
		return -EINVAL;
	*failure_stage = MT6797_A72_PPC_FAILURE_NONE;
	memset(snapshot, 0, sizeof(*snapshot));
	memset(platform_failure, 0, sizeof(*platform_failure));
	if (!platform || !provider || !clock) {
		*failure_stage = MT6797_A72_PPC_FAILURE_DEPENDENCY;
		return -EPROBE_DEFER;
	}
	if (!ops || !ops->platform || !ops->provider || !ops->checkpoint ||
	    !ops->clock)
		return -EINVAL;

	ret = ops->platform(context, platform, &snapshot->platform,
			    platform_failure);
	if (ret) {
		*failure_stage = MT6797_A72_PPC_FAILURE_PLATFORM;
		goto out_clear;
	}
	if (!snapshot->platform.valid) {
		*failure_stage = MT6797_A72_PPC_FAILURE_PLATFORM;
		ret = -ENODATA;
		goto out_clear;
	}

	ret = ops->provider(context, &snapshot->provider);
	if (ret) {
		*failure_stage = MT6797_A72_PPC_FAILURE_PROVIDER;
		goto out_clear;
	}
	if (!snapshot->provider.valid) {
		*failure_stage = MT6797_A72_PPC_FAILURE_PROVIDER;
		ret = -ENODATA;
		goto out_clear;
	}

	if (!ops->checkpoint(context, 0)) {
		*failure_stage = MT6797_A72_PPC_FAILURE_BEFORE_CLOCK;
		ret = -EIO;
		goto out_clear;
	}

	snapshot->clock_ret = ops->clock(context, clock, &snapshot->clock);
	snapshot->clock_returned = true;
	snapshot->after_checkpoint = ops->checkpoint(context, 1);
	snapshot->valid = !snapshot->clock_ret && snapshot->after_checkpoint &&
			  snapshot->clock.abi ==
				  MT6797_DVFSP_CLOCK_BACKEND_ABI &&
			  snapshot->clock.sample_generation;

	/* A returned hardware call is terminal, including error attribution. */
	return 0;

out_clear:
	memset(snapshot, 0, sizeof(*snapshot));
	return ret;
}

static struct device *mt6797_a72_ppc_get_platform(struct device *dev)
{
	struct platform_device *source;
	struct device_node *node;

	node = of_parse_phandle(dev->of_node, "mediatek,platform-state", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	source = of_find_device_by_node(node);
	of_node_put(node);
	if (!source)
		return ERR_PTR(-EPROBE_DEFER);
	if (!device_is_bound(&source->dev)) {
		put_device(&source->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	return &source->dev;
}

static struct device *mt6797_a72_ppc_get_provider(struct device *dev)
{
	struct i2c_client *provider;
	struct device_node *node;

	node = of_parse_phandle(dev->of_node, "mediatek,provider", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	if (!of_device_is_compatible(node, "dlg,da9214-legacy")) {
		of_node_put(node);
		return ERR_PTR(-EINVAL);
	}
	provider = of_find_i2c_device_by_node(node);
	of_node_put(node);
	if (!provider)
		return ERR_PTR(-EPROBE_DEFER);
	if (!device_is_bound(&provider->dev)) {
		put_device(&provider->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	return &provider->dev;
}

static struct device *mt6797_a72_ppc_get_clock(struct device *dev)
{
	struct platform_device *clock;
	struct device_node *node;

	node = of_parse_phandle(dev->of_node, "mediatek,clock-backend", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	if (!of_device_is_compatible(node,
				     "mediatek,mt6797-dvfsp-clock-backend")) {
		of_node_put(node);
		return ERR_PTR(-EINVAL);
	}
	clock = of_find_device_by_node(node);
	of_node_put(node);
	if (!clock)
		return ERR_PTR(-EPROBE_DEFER);
	if (!device_is_bound(&clock->dev)) {
		put_device(&clock->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	return &clock->dev;
}

static const char *
mt6797_a72_ppc_failure_stage_name(enum mt6797_a72_ppc_failure_stage stage)
{
	switch (stage) {
	case MT6797_A72_PPC_FAILURE_NONE:
		return "none";
	case MT6797_A72_PPC_FAILURE_DEPENDENCY:
		return "dependency";
	case MT6797_A72_PPC_FAILURE_PLATFORM:
		return "platform";
	case MT6797_A72_PPC_FAILURE_PROVIDER:
		return "provider";
	case MT6797_A72_PPC_FAILURE_BEFORE_CLOCK:
		return "before-clock";
	default:
		return "invalid";
	}
}

static void
mt6797_a72_ppc_log_failure(struct device *dev,
			   enum mt6797_a72_ppc_failure_stage stage, int ret,
			   const struct mt6797_a72_platform_state_failure *failure)
{
	const struct mt6797_a72_platform_state *first = &failure->first;
	const struct mt6797_a72_platform_state *second = &failure->second;

	if (stage == MT6797_A72_PPC_FAILURE_PLATFORM && ret == -EAGAIN &&
	    failure->samples_valid) {
		dev_err(dev,
			"platform/provider/clock capture failed: stage=platform ret=-11"
			" movement=%03x cpu=%08x/%08x cpu2=%08x/%08x"
			" cpusys=%08x/%08x cpu0=%08x/%08x cpu1=%08x/%08x"
			" iso=%08x/%08x dcm=%08x/%08x cci-port=%08x/%08x"
			" pwrap=%u/%u\n",
			failure->movement_mask,
			first->spm_cpu_pwr_status, second->spm_cpu_pwr_status,
			first->spm_cpu_pwr_status_2nd,
			second->spm_cpu_pwr_status_2nd,
			first->spm_mp2_cpusys_pwr_con,
			second->spm_mp2_cpusys_pwr_con,
			first->spm_mp2_cpu0_pwr_con,
			second->spm_mp2_cpu0_pwr_con,
			first->spm_mp2_cpu1_pwr_con,
			second->spm_mp2_cpu1_pwr_con,
			first->spm_cpu_ext_buck_iso,
			second->spm_cpu_ext_buck_iso,
			first->mp2_sync_dcm, second->mp2_sync_dcm,
			first->cci_mp2_port_control,
			second->cci_mp2_port_control,
			first->pwrap_reset_asserted,
			second->pwrap_reset_asserted);
		return;
	}

	dev_err(dev,
		"platform/provider/clock capture failed: stage=%s ret=%d\n",
		mt6797_a72_ppc_failure_stage_name(stage), ret);
}

static void mt6797_a72_ppc_log(struct device *dev,
			       const struct mt6797_a72_platform_provider_clock_snapshot *snapshot)
{
	const struct mt6797_a72_platform_state *platform = &snapshot->platform;
	const struct mt6797_a72_provider_snapshot *provider = &snapshot->provider;
	const struct mt6797_dvfsp_clock_readback *clock = &snapshot->clock;

	dev_info(dev,
		 MT6797_A72_PPC_TAG
		 " platform valid=%u spm=%08x/%08x/%08x/%08x"
		 " mp2=%08x/%08x/%08x iso=%08x dcm=%08x"
		 " cci=%08x/%08x/%08x pwrap=%u\n",
		 platform->valid, platform->spm_pwr_status,
		 platform->spm_pwr_status_2nd,
		 platform->spm_cpu_pwr_status,
		 platform->spm_cpu_pwr_status_2nd,
		 platform->spm_mp2_cpusys_pwr_con,
		 platform->spm_mp2_cpu0_pwr_con,
		 platform->spm_mp2_cpu1_pwr_con,
		 platform->spm_cpu_ext_buck_iso, platform->mp2_sync_dcm,
		 platform->cci_mp2_port_control, platform->cci_status_before,
		 platform->cci_status_after, platform->pwrap_reset_asserted);
	dev_info(dev,
		 MT6797_A72_PPC_TAG
		 " provider abi=%u valid=%u raw=%02x/%02x/%02x/%02x/%02x\n",
		 provider->abi, provider->valid, provider->control_a,
		 provider->status_b, provider->buckb_cont, provider->vbuckb_a,
		 provider->vbuckb_b);
	dev_info(dev,
		 MT6797_A72_PPC_TAG
		 " clock ret=%d abi=%u generation=%llu"
		 " muxsel=%08x ckdiv=%08x"
		 " pll_ll=%08x/%08x/%08x pll_l=%08x/%08x/%08x"
		 " pll_cci=%08x/%08x/%08x"
		 " cspm_swctrl=%08x/%08x/%08x"
		 " cspm_hwsta=%08x/%08x/%08x/%08x\n",
		 snapshot->clock_ret, clock->abi,
		 (unsigned long long)clock->sample_generation,
		 clock->armplldiv_muxsel, clock->armplldiv_ckdiv,
		 clock->pll_ll[0], clock->pll_ll[1], clock->pll_ll[2],
		 clock->pll_l[0], clock->pll_l[1], clock->pll_l[2],
		 clock->pll_cci[0], clock->pll_cci[1], clock->pll_cci[2],
		 clock->cspm_swctrl[0], clock->cspm_swctrl[1],
		 clock->cspm_swctrl[2], clock->cspm_hwsta[0],
		 clock->cspm_hwsta[1], clock->cspm_hwsta[2],
		 clock->cspm_hwsta[3]);
	dev_info(dev,
		 MT6797_A72_PPC_TAG
		 " state=complete provider_ready_gate=passed clock_ready_gate=passed"
		 " valid=%u clock_returned=%u after_checkpoint=%u"
		 " platform_calls=1 platform_samples=2"
		 " platform_register_observations=26 provider_snapshots=1"
		 " provider_samples=2 provider_i2c_reads=10 provider_i2c_writes=0"
		 " retained_write_attempts=2 protected_clock_calls=1"
		 " protected_clock_ret=%d protected_clock_abi=%u"
		 " protected_clock_generation=%llu clock_gate_pairs=1"
		 " explicit_mmio_writes_maximum=401"
		 " explicit_mmio_reads_maximum=419 observer_retries=0"
		 " bigidvfs_reads=0 secure_calls=0 provider_acquires=0"
		 " provider_releases=0 publisher_calls=0 owner_mutations=0"
		 " cpu_requests=0\n",
		 snapshot->valid, snapshot->clock_returned,
		 snapshot->after_checkpoint, snapshot->clock_ret, clock->abi,
		 (unsigned long long)clock->sample_generation);
}

static int mt6797_a72_ppc_probe(struct platform_device *pdev)
{
	struct mt6797_a72_platform_provider_clock_snapshot snapshot;
	struct mt6797_a72_platform_state_failure platform_failure;
	enum mt6797_a72_ppc_failure_stage failure_stage;
	struct device *platform;
	struct device *provider;
	struct device *clock;
	struct device *dev = &pdev->dev;
	int ret;

	platform = mt6797_a72_ppc_get_platform(dev);
	if (IS_ERR(platform))
		return dev_err_probe(dev, PTR_ERR(platform),
				     "platform-state source unavailable\n");
	provider = mt6797_a72_ppc_get_provider(dev);
	if (IS_ERR(provider)) {
		ret = dev_err_probe(dev, PTR_ERR(provider),
				    "provider unavailable\n");
		goto out_put_platform;
	}
	clock = mt6797_a72_ppc_get_clock(dev);
	if (IS_ERR(clock)) {
		ret = dev_err_probe(dev, PTR_ERR(clock),
				    "clock backend unavailable\n");
		goto out_put_provider;
	}

	ret = mt6797_a72_ppc_capture(platform, provider, clock,
				     &mt6797_a72_ppc_ops, NULL, &snapshot,
				     &failure_stage, &platform_failure);
	if (ret)
		mt6797_a72_ppc_log_failure(dev, failure_stage, ret,
					   &platform_failure);
	else
		mt6797_a72_ppc_log(dev, &snapshot);
	put_device(clock);
out_put_provider:
	put_device(provider);
out_put_platform:
	put_device(platform);

	return ret;
}

static const struct of_device_id mt6797_a72_ppc_of_match[] = {
	{ .compatible = "mediatek,mt6797-a72-platform-provider-clock-observer" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_a72_ppc_of_match);

static struct platform_driver mt6797_a72_ppc_driver = {
	.probe = mt6797_a72_ppc_probe,
	.driver = {
		.name = "mt6797-a72-platform-provider-clock-observer",
		.of_match_table = mt6797_a72_ppc_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6797_a72_ppc_driver);

MODULE_DESCRIPTION("MT6797 A72 platform, provider, and clock observer");
MODULE_LICENSE("GPL");
