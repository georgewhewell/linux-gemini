// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-neutral MT6797 MD1 pre-release transaction.
 *
 * Vendor chronology starts with PMIC rails, then MD_INFRA/MODEM_TOP. Its
 * later source-clock, PLL, WDT, boot-vector, CCIF and CLDMA actions are
 * deliberately absent: none belongs before a reviewed firmware/ownership
 * contract. A successful transaction always unwinds to OFF.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/soc/mediatek/mt6797-md1-prerelease.h>

int mt6797_md1_prerelease_init(struct mt6797_md1_prerelease *prerelease,
		const struct mt6797_md1_prerelease_ops *ops, void *context)
{
	if (!prerelease || !ops || ops->abi != MT6797_MD1_PRERELEASE_ABI ||
	    !ops->pmic_on || !ops->pmic_off || !ops->clock_on || !ops->clock_off ||
	    !ops->domain_on ||
	    !ops->domain_off || !ops->poll)
		return -EINVAL;

	prerelease->ops = ops;
	prerelease->context = context;
	mutex_init(&prerelease->lock);
	prerelease->state = MT6797_MD1_PRERELEASE_OFF;
	return 0;
}

static int mt6797_md1_prerelease_unwind(struct mt6797_md1_prerelease *prerelease,
		bool domain_may_be_on, bool clock_may_be_on, bool pmic_may_be_on,
		int ret)
{
	int cleanup;
	int cleanup_error = 0;

	if (domain_may_be_on) {
		cleanup = prerelease->ops->domain_off(prerelease->context);
		if (cleanup)
			cleanup_error = cleanup;
		if (!ret)
			ret = cleanup;
	}
	if (clock_may_be_on) {
		cleanup = prerelease->ops->clock_off(prerelease->context);
		if (cleanup && !cleanup_error)
			cleanup_error = cleanup;
		if (!ret)
			ret = cleanup;
	}
	if (pmic_may_be_on) {
		cleanup = prerelease->ops->pmic_off(prerelease->context);
		if (cleanup && !cleanup_error)
			cleanup_error = cleanup;
		if (!ret)
			ret = cleanup;
	}
	if (cleanup_error)
		prerelease->state = MT6797_MD1_PRERELEASE_ERROR;
	else
		prerelease->state = MT6797_MD1_PRERELEASE_OFF;
	return ret;
}

int mt6797_md1_prerelease_run(struct mt6797_md1_prerelease *prerelease)
{
	bool pmic_may_be_on = false;
	bool clock_may_be_on = false;
	bool domain_may_be_on = false;
	unsigned int attempt;
	int ret;

	if (!prerelease || !prerelease->ops)
		return -EINVAL;

	mutex_lock(&prerelease->lock);
	if (prerelease->state == MT6797_MD1_PRERELEASE_ERROR) {
		ret = -EIO;
		goto out_unlock;
	}

	/* Set ownership before every callback: an error may follow a real write. */
	pmic_may_be_on = true;
	ret = prerelease->ops->pmic_on(prerelease->context);
	if (ret)
		goto out_unwind;

	clock_may_be_on = true;
	ret = prerelease->ops->clock_on(prerelease->context);
	if (ret)
		goto out_unwind;

	domain_may_be_on = true;
	ret = prerelease->ops->domain_on(prerelease->context);
	if (ret)
		goto out_unwind;

	for (attempt = 0; attempt < MT6797_MD1_PRERELEASE_MAX_POLLS; attempt++) {
		ret = prerelease->ops->poll(prerelease->context);
		if (!ret)
			break;
		if (ret != -EAGAIN)
			goto out_unwind;
	}
	if (ret == -EAGAIN)
		ret = -ETIMEDOUT;

out_unwind:
	ret = mt6797_md1_prerelease_unwind(prerelease, domain_may_be_on,
					     clock_may_be_on, pmic_may_be_on, ret);
out_unlock:
	mutex_unlock(&prerelease->lock);
	return ret;
}

enum mt6797_md1_prerelease_state
mt6797_md1_prerelease_state(struct mt6797_md1_prerelease *prerelease)
{
	enum mt6797_md1_prerelease_state state;

	mutex_lock(&prerelease->lock);
	state = prerelease->state;
	mutex_unlock(&prerelease->lock);
	return state;
}
