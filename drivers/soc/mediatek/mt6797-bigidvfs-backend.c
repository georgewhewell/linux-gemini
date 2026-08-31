// SPDX-License-Identifier: GPL-2.0-only
/*
 * Disabled-only MT6797 BigiDVFS secure-register readback transport.
 *
 * The retained firmware audit proves only REG_READ (0xc200035f) for this
 * boundary. Keep the exact whitelist and raw sample separate from the future
 * calibrated state owner; advertised getter FIDs are not assumed present.
 */

#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mt6797-bigidvfs-backend.h>

#include "mt6797-bigidvfs-sram-internal.h"
#include "mt6797-protected-readback-internal.h"

struct mt6797_bigidvfs_backend {
	struct mutex operation_lock;
	bool faulted;
	u64 sample_generation;
#if IS_ENABLED(CONFIG_MTK_MT6797_A72_BIGIDVFS_SRAM_OWNER)
	struct mt6797_bigidvfs_sram_owner sram_owner;
#endif
};

struct mt6797_bigidvfs_raw_sample {
	u32 word[4];
};

static const u32 mt6797_bigidvfs_addresses[] = {
	MT6797_BIGIDVFS_PLL_PCW,
	MT6797_BIGIDVFS_PLL_ENABLE_POSDIV,
	MT6797_BIGIDVFS_SRAM_SELECTOR,
	MT6797_BIGIDVFS_CONTROL,
};

static bool mt6797_bigidvfs_address_allowed(u32 address)
{
	switch (address) {
	case MT6797_BIGIDVFS_PLL_PCW:
	case MT6797_BIGIDVFS_PLL_ENABLE_POSDIV:
	case MT6797_BIGIDVFS_SRAM_SELECTOR:
#if IS_ENABLED(CONFIG_MTK_MT6797_A72_BIGIDVFS_SRAM_OWNER)
	case MT6797_BIGIDVFS_SRAM_CALIBRATION:
#endif
	case MT6797_BIGIDVFS_CONTROL:
		return true;
	default:
		return false;
	}
}

static int mt6797_bigidvfs_secure_read(void *context, u32 address, u32 *value)
{
	struct arm_smccc_res result;

	if (!value || !mt6797_bigidvfs_address_allowed(address))
		return -EINVAL;

	arm_smccc_smc(MT6797_BIGIDVFS_FID_READ, address, 0, 0, 0, 0, 0, 0,
		      &result);
	/* REG_READ returns a zero-extended raw word; reject all error forms. */
	if (result.a0 >> 32)
		return -EIO;

	*value = (u32)result.a0;
	return 0;
}

static const struct mt6797_bigidvfs_transport_ops bigidvfs_ops = {
	.read = mt6797_bigidvfs_secure_read,
};

static int
mt6797_bigidvfs_read_sample(const struct mt6797_bigidvfs_transport_ops *ops,
			    void *context,
			    struct mt6797_bigidvfs_raw_sample *sample)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(mt6797_bigidvfs_addresses); i++) {
		ret = ops->read(context, mt6797_bigidvfs_addresses[i],
				&sample->word[i]);
		if (ret)
			return ret;
	}

	return 0;
}

int
mt6797_bigidvfs_snapshot(const struct mt6797_bigidvfs_transport_ops *ops,
			 void *context,
			 struct mt6797_bigidvfs_readback *readback)
{
	struct mt6797_bigidvfs_raw_sample first = { };
	struct mt6797_bigidvfs_raw_sample second = { };
	struct mt6797_bigidvfs_readback observed = { };
	int ret;

	if (!readback)
		return -EINVAL;
	memset(readback, 0, sizeof(*readback));
	if (!ops || !ops->read)
		return -EINVAL;

	ret = mt6797_bigidvfs_read_sample(ops, context, &first);
	if (ret)
		return ret;
	ret = mt6797_bigidvfs_read_sample(ops, context, &second);
	if (ret)
		return ret;
	if (memcmp(&first, &second, sizeof(first)))
		return -EAGAIN;

	observed.pll_pcw = second.word[0];
	observed.pll_enable_posdiv = second.word[1];
	observed.sram_selector = second.word[2];
	observed.control = second.word[3];
	*readback = observed;
	return 0;
}

#if IS_ENABLED(CONFIG_MTK_MT6797_A72_BIGIDVFS_SRAM_OWNER)
static int mt6797_bigidvfs_secure_sram_set(void *context, u32 mv_x100)
{
	struct arm_smccc_res result;

	(void)context;
	arm_smccc_smc(MT6797_BIGIDVFS_FID_SRAM_LDO_SET, mv_x100,
		      0, 0, 0, 0, 0, 0, &result);
	if (result.a0)
		return -EIO;

	return 0;
}

static void mt6797_bigidvfs_sram_delay(void *context, unsigned int min_us,
				       unsigned int max_us)
{
	(void)context;
	usleep_range(min_us, max_us);
}

static const struct mt6797_bigidvfs_sram_ops mt6797_bigidvfs_sram_ops = {
	.set = mt6797_bigidvfs_secure_sram_set,
	.read = mt6797_bigidvfs_secure_read,
	.delay = mt6797_bigidvfs_sram_delay,
};

static bool
mt6797_bigidvfs_sram_ops_valid(const struct mt6797_bigidvfs_sram_ops *ops)
{
	return ops && ops->set && ops->read && ops->delay;
}

static bool
mt6797_bigidvfs_sram_request_valid(const struct mt6797_bigidvfs_sram_request *request)
{
	return request && request->abi == MT6797_BIGIDVFS_SRAM_OWNER_ABI &&
	       request->attempt_id && request->cookie;
}

static bool
mt6797_bigidvfs_sram_request_matches(const struct mt6797_bigidvfs_sram_request *left,
				     const struct mt6797_bigidvfs_sram_request *right)
{
	return mt6797_bigidvfs_sram_request_valid(left) &&
	       mt6797_bigidvfs_sram_request_valid(right) &&
	       left->attempt_id == right->attempt_id &&
	       left->cookie == right->cookie;
}

static void
mt6797_bigidvfs_sram_output(const struct mt6797_bigidvfs_sram_owner *owner,
			    struct mt6797_bigidvfs_sram_result *result)
{
	*result = owner->result;
}

static int
mt6797_bigidvfs_sram_fail(struct mt6797_bigidvfs_sram_owner *owner, int error,
			  struct mt6797_bigidvfs_sram_result *result)
{
	owner->state = MT6797_BIGIDVFS_SRAM_OWNER_FAULTED;
	owner->result.error = error;
	owner->result.sealed = true;
	mt6797_bigidvfs_sram_output(owner, result);
	return error;
}

static int
mt6797_bigidvfs_sram_read(struct mt6797_bigidvfs_sram_owner *owner,
			  const struct mt6797_bigidvfs_sram_ops *ops,
			  void *context, u32 address, u32 step, u32 *value,
			  struct mt6797_bigidvfs_sram_result *result)
{
	int ret;

	owner->result.attempted_steps |= step;
	ret = ops->read(context, address, value);
	if (ret)
		return mt6797_bigidvfs_sram_fail(owner, ret, result);
	owner->result.completed_steps |= step;
	return 0;
}

int
mt6797_bigidvfs_sram_owner_execute(struct mt6797_bigidvfs_sram_owner *owner,
				   const struct mt6797_bigidvfs_sram_ops *ops,
				   void *context,
				   const struct mt6797_bigidvfs_sram_request *request,
				   struct mt6797_bigidvfs_sram_result *result)
{
	int ret;

	if (!result)
		return -EINVAL;
	*result = (struct mt6797_bigidvfs_sram_result){};
	if (!owner || !mt6797_bigidvfs_sram_ops_valid(ops) ||
	    !mt6797_bigidvfs_sram_request_valid(request))
		return -EINVAL;
	if (owner->state != MT6797_BIGIDVFS_SRAM_OWNER_UNUSED) {
		mt6797_bigidvfs_sram_output(owner, result);
		if (!mt6797_bigidvfs_sram_request_matches(&owner->request,
							  request))
			return -EPERM;
		return -EALREADY;
	}
	if (request->cpu != 8 || !request->provider_held ||
	    !request->isolation_crossed || request->cpu8_online ||
	    request->cpu9_online)
		return -EPERM;

	owner->state = MT6797_BIGIDVFS_SRAM_OWNER_INFLIGHT;
	owner->request = *request;
	owner->result = (struct mt6797_bigidvfs_sram_result) {
		.abi = MT6797_BIGIDVFS_SRAM_OWNER_ABI,
		.requested_mv_x100 = MT6797_BIGIDVFS_SRAM_TARGET_MV_X100,
		.attempt_id = request->attempt_id,
		.cookie = request->cookie,
		.effect_attempted = true,
	};
	owner->result.attempted_steps |= MT6797_BIGIDVFS_SRAM_SERVICE;
	ret = ops->set(context, MT6797_BIGIDVFS_SRAM_TARGET_MV_X100);
	if (ret)
		return mt6797_bigidvfs_sram_fail(owner, ret, result);
	owner->result.completed_steps |= MT6797_BIGIDVFS_SRAM_SERVICE;

	owner->result.attempted_steps |= MT6797_BIGIDVFS_SRAM_SETTLE;
	ops->delay(context, MT6797_BIGIDVFS_SRAM_SETTLE_MIN_US,
		   MT6797_BIGIDVFS_SRAM_SETTLE_MAX_US);
	owner->result.completed_steps |= MT6797_BIGIDVFS_SRAM_SETTLE;

	ret = mt6797_bigidvfs_sram_read(owner, ops, context,
					MT6797_BIGIDVFS_SRAM_SELECTOR,
					MT6797_BIGIDVFS_SRAM_SELECTOR_FIRST,
					&owner->result.selector_first, result);
	if (ret)
		return ret;
	ret = mt6797_bigidvfs_sram_read(owner, ops, context,
					MT6797_BIGIDVFS_SRAM_CALIBRATION,
					MT6797_BIGIDVFS_SRAM_CALIBRATION_FIRST,
					&owner->result.calibration_first, result);
	if (ret)
		return ret;
	ret = mt6797_bigidvfs_sram_read(owner, ops, context,
					MT6797_BIGIDVFS_SRAM_SELECTOR,
					MT6797_BIGIDVFS_SRAM_SELECTOR_SECOND,
					&owner->result.selector_second, result);
	if (ret)
		return ret;
	ret = mt6797_bigidvfs_sram_read(owner, ops, context,
					MT6797_BIGIDVFS_SRAM_CALIBRATION,
					MT6797_BIGIDVFS_SRAM_CALIBRATION_SECOND,
					&owner->result.calibration_second, result);
	if (ret)
		return ret;

	if (owner->result.selector_first != owner->result.selector_second ||
	    owner->result.calibration_first != owner->result.calibration_second)
		return mt6797_bigidvfs_sram_fail(owner, -EAGAIN, result);
	owner->result.attempted_steps |= MT6797_BIGIDVFS_SRAM_SELECTOR_VALID;
	if ((owner->result.selector_second &
	     MT6797_BIGIDVFS_SRAM_SELECTOR_MASK) !=
	    MT6797_BIGIDVFS_SRAM_SELECTOR_EXPECTED)
		return mt6797_bigidvfs_sram_fail(owner, -ERANGE, result);
	owner->result.completed_steps |= MT6797_BIGIDVFS_SRAM_SELECTOR_VALID;
	owner->result.attempted_steps |= MT6797_BIGIDVFS_SRAM_CALIBRATION_VALID;
	if ((owner->result.calibration_second &
	     ~MT6797_BIGIDVFS_SRAM_CALIBRATION_MASK) ||
	    !(owner->result.calibration_second &
	      MT6797_BIGIDVFS_SRAM_CALIBRATION_MASK))
		return mt6797_bigidvfs_sram_fail(owner, -ERANGE, result);
	owner->result.completed_steps |= MT6797_BIGIDVFS_SRAM_CALIBRATION_VALID;
	owner->state = MT6797_BIGIDVFS_SRAM_OWNER_VERIFIED;
	owner->result.verified = true;
	owner->result.sealed = true;
	mt6797_bigidvfs_sram_output(owner, result);
	return 0;
}
#endif

static void
mt6797_bigidvfs_mark_fault(struct mt6797_bigidvfs_backend *backend)
{
	backend->faulted = true;
}

int mt6797_bigidvfs_backend_read(struct device *dev,
				 struct mt6797_bigidvfs_readback *readback)
{
	struct mt6797_bigidvfs_readback observed = { };
	struct mt6797_bigidvfs_backend *backend;
	int ret;

	if (!readback)
		return -EINVAL;
	memset(readback, 0, sizeof(*readback));
	if (!dev)
		return -EINVAL;

	backend = dev_get_drvdata(dev);
	if (!backend)
		return -ENODEV;

	mutex_lock(&backend->operation_lock);
	if (backend->faulted) {
		ret = -EIO;
		goto out_unlock;
	}

	ret = mt6797_bigidvfs_snapshot(&bigidvfs_ops, backend, &observed);
	if (ret) {
		if (ret != -EAGAIN)
			mt6797_bigidvfs_mark_fault(backend);
		goto out_unlock;
	}

	if (++backend->sample_generation == 0)
		backend->sample_generation = 1;
	observed.abi = MT6797_BIGIDVFS_BACKEND_ABI;
	observed.sample_generation = backend->sample_generation;
	*readback = observed;

out_unlock:
	mutex_unlock(&backend->operation_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_bigidvfs_backend_read);

#if IS_ENABLED(CONFIG_MTK_MT6797_A72_BIGIDVFS_SRAM_OWNER)
int
mt6797_bigidvfs_sram_enable(struct device *dev,
			    const struct mt6797_bigidvfs_sram_request *request,
			    struct mt6797_bigidvfs_sram_result *result)
{
	struct mt6797_bigidvfs_backend *backend;
	int ret;

	if (!result)
		return -EINVAL;
	*result = (struct mt6797_bigidvfs_sram_result){};
	if (!dev)
		return -EINVAL;
	backend = dev_get_drvdata(dev);
	if (!backend)
		return -ENODEV;

	mutex_lock(&backend->operation_lock);
	if (backend->faulted) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = mt6797_bigidvfs_sram_owner_execute(&backend->sram_owner,
						 &mt6797_bigidvfs_sram_ops,
						 backend, request, result);
	if (ret && result->error)
		mt6797_bigidvfs_mark_fault(backend);

out_unlock:
	mutex_unlock(&backend->operation_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_bigidvfs_sram_enable);
#endif
static int mt6797_bigidvfs_backend_probe(struct platform_device *pdev)
{
	struct mt6797_bigidvfs_backend *backend;
	const char *method;

	if (!pdev->dev.of_node ||
	    of_property_read_string(pdev->dev.of_node, "method", &method) ||
	    strcmp(method, "smc"))
		return -EINVAL;

	backend = devm_kzalloc(&pdev->dev, sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return -ENOMEM;

	mutex_init(&backend->operation_lock);
	platform_set_drvdata(pdev, backend);
#if IS_ENABLED(CONFIG_MTK_MT6797_A72_BIGIDVFS_SRAM_OWNER)
	dev_info(&pdev->dev,
		 "secure readback and one-shot SRAM owner ready; no caller\n");
#else
	dev_info(&pdev->dev,
		 "secure readback transport ready; owner unregistered\n");
#endif

	return 0;
}

static const struct of_device_id mt6797_bigidvfs_backend_of_match[] = {
	{ .compatible = "mediatek,mt6797-bigidvfs-backend" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_bigidvfs_backend_of_match);

static struct platform_driver mt6797_bigidvfs_backend_driver = {
	.probe = mt6797_bigidvfs_backend_probe,
	.driver = {
		.name = "mt6797-bigidvfs-backend",
		.of_match_table = mt6797_bigidvfs_backend_of_match,
	},
};
module_platform_driver(mt6797_bigidvfs_backend_driver);

MODULE_DESCRIPTION("Disabled MT6797 BigiDVFS secure readback transport");
MODULE_LICENSE("GPL");
