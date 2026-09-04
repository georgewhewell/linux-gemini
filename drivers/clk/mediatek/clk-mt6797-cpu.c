// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 George Whewell
 */

#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/mediatek-wdt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>
#include <linux/string.h>

#include <linux/arm-smccc.h>

#include <dt-bindings/clock/mt6797-clk.h>

#include "clk-mt6797-cpu.h"

struct mt6797_cpuclk;

#define MT6797_CPUCLK_MP0_PLL_CON1 0x204
#define MT6797_CPUCLK_MP1_PLL_CON1 0x214
#define MT6797_CPUCLK_CCI_PLL_CON1 0x224
#define MT6797_CPUCLK_MUXSEL 0x270
#define MT6797_CPUCLK_CLKDIV 0x274
#define MT6797_CPUCLK_SYNC_DCM 0x744
#define MT6797_CPUCLK_SYNC_DCM_PHYS 0x10220744

#define MT6797_CPUCLK_CSPM_POWERON 0x000
#define MT6797_CPUCLK_CSPM_SEMAPHORE 0x440
#define MT6797_CPUCLK_CSPM_POWERON_VALUE 0x0b160001
#define MT6797_CPUCLK_CSPM_SEMAPHORE_REQUEST 1
#define MT6797_CPUCLK_CSPM_SEMAPHORE_HELD BIT(0)
#define MT6797_CPUCLK_CSPM_POLL_US 10
#define MT6797_CPUCLK_CSPM_RETRIES 200

#define MT6797_CPUCLK_SMC_MCUSYS_WRITE 0x82000201UL
#define MT6797_CPUCLK_PLL_SETTLE_US 2

struct mt6797_cpuclk_observer {
	void __iomem *mcumixed;
	void __iomem *mcucfg;
	struct mt6797_dvfsp_handoff *handoff;
	struct clk *i2c_clk;
	/* Serializes snapshots and complete reconfiguration transactions. */
	struct mutex lock;
	bool faulted;
	bool probe_snapshot_valid;
	struct mt6797_cpuclk_snapshot probe_snapshot;
};

struct mt6797_cpuclk_pll_hw {
	struct clk_hw hw;
	struct mt6797_cpuclk *provider;
	enum mt6797_cpuclk_domain domain;
};

struct mt6797_cpuclk_path_hw {
	struct clk_hw hw;
	struct mt6797_cpuclk *provider;
	enum mt6797_cpuclk_domain domain;
	struct clk_parent_data parents[MT6797_CPUCLK_NR_SOURCES];
};

struct mt6797_cpuclk {
	struct device *dev;
	struct mt6797_cpuclk_backend backend;
	struct mt6797_cpuclk_observer observer;
	struct mt6797_cpuclk_pll_hw plls[MT6797_CPUCLK_NR_DOMAINS];
	struct mt6797_cpuclk_path_hw paths[MT6797_CPUCLK_NR_DOMAINS];
	struct clk_hw_onecell_data *clk_data;
};

static const char *const mt6797_cpuclk_pll_names[] = {
	[MT6797_CPUCLK_MP0] = "mt6797_mp0_armpll",
	[MT6797_CPUCLK_MP1] = "mt6797_mp1_armpll",
	[MT6797_CPUCLK_MP2] = "mt6797_mp2_armpll",
	[MT6797_CPUCLK_CCI] = "mt6797_cci_armpll",
};

static const char *const mt6797_cpuclk_path_names[] = {
	[MT6797_CPUCLK_MP0] = "mt6797_mp0",
	[MT6797_CPUCLK_MP1] = "mt6797_mp1",
	[MT6797_CPUCLK_MP2] = "mt6797_mp2",
	[MT6797_CPUCLK_CCI] = "mt6797_cci",
};

static int mt6797_cpuclk_secure_read(unsigned long fid, u32 address, u32 *value)
{
	struct arm_smccc_res result;

	if (!value || !mt6797_cpuclk_secure_read_allowed(fid, address))
		return -EINVAL;

	arm_smccc_smc(fid, address, 0, 0, 0, 0, 0, 0, &result);
	if (result.a0 >> 32)
		return -EIO;

	*value = result.a0;
	return 0;
}

static int
mt6797_cpuclk_lease_acquire(struct mt6797_cpuclk_observer *observer,
			    void __iomem *cspm)
{
	unsigned int i;
	int ret;

	ret = clk_prepare_enable(observer->i2c_clk);
	if (ret)
		return ret;

	writel(MT6797_CPUCLK_CSPM_POWERON_VALUE,
	       cspm + MT6797_CPUCLK_CSPM_POWERON);
	readl(cspm + MT6797_CPUCLK_CSPM_POWERON);

	for (i = 0; i < MT6797_CPUCLK_CSPM_RETRIES; i++) {
		writel(MT6797_CPUCLK_CSPM_SEMAPHORE_REQUEST,
		       cspm + MT6797_CPUCLK_CSPM_SEMAPHORE);
		if (readl(cspm + MT6797_CPUCLK_CSPM_SEMAPHORE) &
		    MT6797_CPUCLK_CSPM_SEMAPHORE_HELD)
			return 0;
		udelay(MT6797_CPUCLK_CSPM_POLL_US);
	}

	clk_disable_unprepare(observer->i2c_clk);
	return -ETIMEDOUT;
}

static int
mt6797_cpuclk_lease_release(struct mt6797_cpuclk_observer *observer,
			    void __iomem *cspm)
{
	unsigned int i;
	int ret = -ETIMEDOUT;

	writel(MT6797_CPUCLK_CSPM_SEMAPHORE_REQUEST,
	       cspm + MT6797_CPUCLK_CSPM_SEMAPHORE);
	for (i = 0; i < MT6797_CPUCLK_CSPM_RETRIES; i++) {
		if (!(readl(cspm + MT6797_CPUCLK_CSPM_SEMAPHORE) &
		      MT6797_CPUCLK_CSPM_SEMAPHORE_HELD)) {
			ret = 0;
			break;
		}
		udelay(MT6797_CPUCLK_CSPM_POLL_US);
	}

	clk_disable_unprepare(observer->i2c_clk);
	return ret;
}

static int mt6797_cpuclk_secure_write_dcm(u32 value)
{
	struct arm_smccc_res result;
	s32 status;

	arm_smccc_smc(MT6797_CPUCLK_SMC_MCUSYS_WRITE,
		      MT6797_CPUCLK_SYNC_DCM_PHYS, value, 0,
		      0, 0, 0, 0, &result);
	status = result.a0;
	if (status)
		return status < 0 ? status : -EIO;

	return 0;
}

static int mt6797_cpuclk_read_once(struct mt6797_cpuclk_observer *observer,
				   struct mt6797_cpuclk_snapshot *snapshot)
{
	int ret;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->armpll_con1[MT6797_CPUCLK_MP0] =
		readl_relaxed(observer->mcumixed + MT6797_CPUCLK_MP0_PLL_CON1);
	snapshot->armpll_con1[MT6797_CPUCLK_MP1] =
		readl_relaxed(observer->mcumixed + MT6797_CPUCLK_MP1_PLL_CON1);
	snapshot->armpll_con1[MT6797_CPUCLK_CCI] =
		readl_relaxed(observer->mcumixed + MT6797_CPUCLK_CCI_PLL_CON1);
	snapshot->muxsel =
		readl_relaxed(observer->mcumixed + MT6797_CPUCLK_MUXSEL);
	snapshot->clkdiv =
		readl_relaxed(observer->mcumixed + MT6797_CPUCLK_CLKDIV);
	snapshot->sync_dcm =
		readl_relaxed(observer->mcucfg + MT6797_CPUCLK_SYNC_DCM);

	ret = mt6797_cpuclk_secure_read(MT6797_CPUCLK_SMC_REG_READ,
					MT6797_CPUCLK_MP2_PLL_CON1,
					&snapshot->mp2_armpll_con1);
	if (ret)
		return ret;

	return mt6797_cpuclk_secure_read(MT6797_CPUCLK_SMC_REG_READ,
					 MT6797_CPUCLK_MP2_PLL_CON0,
					 &snapshot->mp2_armpll_con0);
}

static int mt6797_cpuclk_read_stable(struct mt6797_cpuclk_observer *observer,
				     struct mt6797_cpuclk_snapshot *snapshot)
{
	struct mt6797_cpuclk_snapshot second;
	int ret;

	ret = mt6797_cpuclk_read_once(observer, snapshot);
	if (ret)
		return ret;

	ret = mt6797_cpuclk_read_once(observer, &second);
	if (ret)
		return ret;

	return mt6797_cpuclk_snapshots_equal(snapshot, &second) ? 0 : -EAGAIN;
}

struct mt6797_cpuclk_snapshot_transaction {
	struct mt6797_cpuclk_observer *observer;
	struct mt6797_cpuclk_snapshot *snapshot;
};

static int mt6797_cpuclk_snapshot_transaction(void __iomem *cspm, void *context)
{
	struct mt6797_cpuclk_snapshot_transaction *transaction = context;
	struct mt6797_cpuclk_observer *observer = transaction->observer;
	int release_ret;
	int ret;

	ret = mt6797_cpuclk_lease_acquire(observer, cspm);
	if (ret)
		return ret;

	ret = mt6797_cpuclk_read_stable(observer, transaction->snapshot);
	release_ret = mt6797_cpuclk_lease_release(observer, cspm);
	if (release_ret) {
		observer->faulted = true;
		return -EUCLEAN;
	}

	return ret;
}

static int
mt6797_cpuclk_snapshot_locked(struct mt6797_cpuclk_observer *observer,
			      struct mt6797_cpuclk_snapshot *snapshot)
{
	struct mt6797_cpuclk_snapshot_transaction transaction = {
		.observer = observer,
		.snapshot = snapshot,
	};

	if (observer->faulted)
		return -EUCLEAN;

	return mt6797_dvfsp_cspm_transaction(observer->handoff,
					     mt6797_cpuclk_snapshot_transaction,
					     &transaction);
}

static int
mt6797_cpuclk_observer_snapshot(void *context,
				struct mt6797_cpuclk_snapshot *snapshot)
{
	struct mt6797_cpuclk_observer *observer = context;
	int ret;

	if (!observer || !snapshot)
		return -EINVAL;

	mutex_lock(&observer->lock);
	if (observer->probe_snapshot_valid) {
		*snapshot = observer->probe_snapshot;
		ret = 0;
	} else {
		ret = mt6797_cpuclk_snapshot_locked(observer, snapshot);
	}
	mutex_unlock(&observer->lock);

	return ret;
}

static int
mt6797_cpuclk_observer_apply(void *context,
			     const struct mt6797_cpuclk_rate_plan *plan);

static const struct mt6797_cpuclk_backend_ops mt6797_cpuclk_observer_ops = {
	.snapshot = mt6797_cpuclk_observer_snapshot,
	.apply = mt6797_cpuclk_observer_apply,
};

static u32 mt6797_cpuclk_pll_offset(enum mt6797_cpuclk_domain domain)
{
	switch (domain) {
	case MT6797_CPUCLK_MP0:
		return MT6797_CPUCLK_MP0_PLL_CON1;
	case MT6797_CPUCLK_MP1:
		return MT6797_CPUCLK_MP1_PLL_CON1;
	default:
		return 0;
	}
}

static int
mt6797_cpuclk_write_pll(struct mt6797_cpuclk_observer *observer,
			enum mt6797_cpuclk_domain domain, u32 value)
{
	u32 offset = mt6797_cpuclk_pll_offset(domain);
	u32 verify;

	if (!offset)
		return -EOPNOTSUPP;

	writel(value, observer->mcumixed + offset);
	readl(observer->mcumixed + offset);
	ndelay(200);
	udelay(MT6797_CPUCLK_PLL_SETTLE_US);
	verify = readl(observer->mcumixed + offset);

	return ((verify ^ value) & (GENMASK(26, 24) | GENMASK(20, 0))) ?
		-EIO : 0;
}

static int
mt6797_cpuclk_write_dcm(struct mt6797_cpuclk_observer *observer,
			const struct mt6797_cpuclk_dcm_plan *plan)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(plan->value); i++) {
		ret = mt6797_cpuclk_secure_write_dcm(plan->value[i]);
		if (ret)
			return ret;
		if (readl(observer->mcucfg + MT6797_CPUCLK_SYNC_DCM) !=
		    plan->value[i])
			return -EIO;
	}

	return 0;
}

static int
mt6797_cpuclk_rollback_locked(struct mt6797_cpuclk_observer *observer,
			      const struct mt6797_cpuclk_rate_plan *plan)
{
	u32 con1;

	con1 = plan->rollback_snapshot.armpll_con1[plan->domain] | BIT(31);
	return mt6797_cpuclk_write_pll(observer, plan->domain, con1);
}

struct mt6797_cpuclk_apply_transaction {
	struct mt6797_cpuclk_observer *observer;
	const struct mt6797_cpuclk_rate_plan *plan;
};

static int mt6797_cpuclk_apply_transaction(void __iomem *cspm, void *context)
{
	struct mt6797_cpuclk_apply_transaction *transaction = context;
	struct mt6797_cpuclk_observer *observer = transaction->observer;
	const struct mt6797_cpuclk_rate_plan *plan = transaction->plan;
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_state state;
	unsigned long actual_rate;
	u32 target_con1;
	bool leased = false;
	int release_ret;
	int ret;

	ret = mt6797_cpuclk_lease_acquire(observer, cspm);
	if (ret)
		return ret;
	leased = true;

	ret = mt6797_cpuclk_read_stable(observer, &snapshot);
	if (ret)
		goto out_release;
	if (!mt6797_cpuclk_snapshots_equal(&snapshot,
					   &plan->rollback_snapshot)) {
		ret = -EAGAIN;
		goto out_release;
	}

	ret = mt6797_cpuclk_plan_pll_rate(plan->domain,
		plan->target.armpll_rate, plan->clk26m_rate,
		snapshot.armpll_con1[plan->domain], &target_con1, &actual_rate);
	if (ret)
		goto out_release;

	/* Plan validation guarantees that the CPU path is on MAINPLL. */
	ret = mt6797_cpuclk_write_pll(observer, plan->domain, target_con1);
	if (ret)
		goto rollback;

	ret = mt6797_cpuclk_read_stable(observer, &snapshot);
	if (ret)
		goto rollback;
	ret = mt6797_cpuclk_decode_state(plan->domain, &snapshot,
					 plan->clk26m_rate, &state);
	if (ret)
		goto rollback;
	if (state.armpll_rate != actual_rate ||
	    ((snapshot.armpll_con1[plan->domain] ^ target_con1) &
	     (GENMASK(26, 24) | GENMASK(20, 0))) ||
	    snapshot.sync_dcm != plan->rollback_snapshot.sync_dcm) {
		ret = -EIO;
		goto rollback;
	}

	goto out_release;

rollback:
	if (mt6797_cpuclk_rollback_locked(observer, plan)) {
		observer->faulted = true;
		ret = -EUCLEAN;
	}

out_release:
	if (leased) {
		release_ret = mt6797_cpuclk_lease_release(observer, cspm);
		if (release_ret) {
			observer->faulted = true;
			ret = -EUCLEAN;
		}
	}
	return ret;
}

static int
mt6797_cpuclk_observer_apply(void *context,
			     const struct mt6797_cpuclk_rate_plan *plan)
{
	struct mt6797_cpuclk_observer *observer = context;
	struct mt6797_cpuclk_apply_transaction transaction = {
		.observer = observer,
		.plan = plan,
	};
	int ret;

	if (!observer || !plan)
		return -EINVAL;

	mutex_lock(&observer->lock);
	if (observer->faulted)
		ret = -EUCLEAN;
	else
		ret = mt6797_dvfsp_cspm_transaction(observer->handoff,
						    mt6797_cpuclk_apply_transaction,
						    &transaction);
	mutex_unlock(&observer->lock);

	return ret;
}

static int mt6797_cpuclk_snapshot(struct mt6797_cpuclk *provider,
				  struct mt6797_cpuclk_snapshot *snapshot)
{
	if (!provider->backend.ops || !provider->backend.ops->snapshot)
		return -EOPNOTSUPP;

	return provider->backend.ops->snapshot(provider->backend.context,
					       snapshot);
}

static unsigned long mt6797_cpuclk_pll_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct mt6797_cpuclk_pll_hw *pll =
		container_of(hw, struct mt6797_cpuclk_pll_hw, hw);
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_state state;

	if (mt6797_cpuclk_snapshot(pll->provider, &snapshot) ||
	    mt6797_cpuclk_decode_state(pll->domain, &snapshot, parent_rate,
				       &state))
		return 0;

	return state.armpll_rate;
}

static int mt6797_cpuclk_pll_determine_rate(struct clk_hw *hw,
					    struct clk_rate_request *req)
{
	struct mt6797_cpuclk_pll_hw *pll =
		container_of(hw, struct mt6797_cpuclk_pll_hw, hw);
	unsigned long actual_rate;
	u32 con1;

	if (req->rate < req->min_rate || req->rate > req->max_rate)
		return -ERANGE;

	if (mt6797_cpuclk_plan_pll_rate(pll->domain, req->rate,
					req->best_parent_rate, 0,
					&con1, &actual_rate))
		return -EINVAL;

	return 0;
}

static int mt6797_cpuclk_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct mt6797_cpuclk_pll_hw *pll =
		container_of(hw, struct mt6797_cpuclk_pll_hw, hw);
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_rate_plan plan;
	struct mt6797_cpuclk_state target;
	int ret;

	if (pll->domain != MT6797_CPUCLK_MP0 &&
	    pll->domain != MT6797_CPUCLK_MP1)
		return -EOPNOTSUPP;

	ret = mt6797_cpuclk_snapshot(pll->provider, &snapshot);
	if (ret)
		return ret;

	ret = mt6797_cpuclk_decode_state(pll->domain, &snapshot, parent_rate,
					 &target);
	if (ret)
		return ret;

	target.armpll_rate = rate;
	ret = mt6797_cpuclk_build_rate_plan(pll->domain, &snapshot, parent_rate,
					    &target, &plan);
	if (ret)
		return ret;

	return mt6797_cpuclk_apply_rate_plan(&pll->provider->backend, &plan);
}

static const struct clk_ops mt6797_cpuclk_pll_ops = {
	.recalc_rate = mt6797_cpuclk_pll_recalc_rate,
	.determine_rate = mt6797_cpuclk_pll_determine_rate,
	.set_rate = mt6797_cpuclk_pll_set_rate,
};

static u8 mt6797_cpuclk_path_get_parent(struct clk_hw *hw)
{
	struct mt6797_cpuclk_path_hw *path =
		container_of(hw, struct mt6797_cpuclk_path_hw, hw);
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_state state;
	struct clk_hw *clk26m;
	unsigned long clk26m_rate;

	clk26m = clk_hw_get_parent_by_index(hw, MT6797_CPUCLK_CLK26M);
	if (!clk26m)
		return -EINVAL;

	clk26m_rate = clk_hw_get_rate(clk26m);
	if (mt6797_cpuclk_snapshot(path->provider, &snapshot) ||
	    mt6797_cpuclk_decode_state(path->domain, &snapshot, clk26m_rate,
				       &state))
		return -EINVAL;

	return state.source;
}

static unsigned long mt6797_cpuclk_path_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	struct mt6797_cpuclk_path_hw *path =
		container_of(hw, struct mt6797_cpuclk_path_hw, hw);
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_state state;
	struct clk_hw *clk26m, *parent;
	unsigned long clk26m_rate;

	clk26m = clk_hw_get_parent_by_index(hw, MT6797_CPUCLK_CLK26M);
	if (!clk26m)
		return 0;

	clk26m_rate = clk_hw_get_rate(clk26m);
	if (mt6797_cpuclk_snapshot(path->provider, &snapshot) ||
	    mt6797_cpuclk_decode_state(path->domain, &snapshot, clk26m_rate,
				       &state))
		return 0;

	if (state.source == MT6797_CPUCLK_ARMPLL) {
		parent_rate = state.armpll_rate;
	} else {
		parent = clk_hw_get_parent_by_index(hw, state.source);
		if (!parent)
			return 0;
		parent_rate = clk_hw_get_rate(parent);
	}

	return mt6797_cpuclk_apply_divider(parent_rate, state.divider);
}

static int mt6797_cpuclk_path_source_rate(struct clk_hw *hw,
					  const struct mt6797_cpuclk_state *state,
					  u8 source, unsigned long *rate)
{
	struct clk_hw *parent;

	if (source == MT6797_CPUCLK_ARMPLL) {
		*rate = state->armpll_rate;
		return 0;
	}

	parent = clk_hw_get_parent_by_index(hw, source);
	if (!parent)
		return -ENODEV;

	*rate = mt6797_cpuclk_apply_divider(clk_hw_get_rate(parent),
					    state->divider);
	return *rate ? 0 : -EINVAL;
}

static int
mt6797_cpuclk_write_mux(struct mt6797_cpuclk_observer *observer, u32 value,
			u32 mask)
{
	writel(value, observer->mcumixed + MT6797_CPUCLK_MUXSEL);
	readl(observer->mcumixed + MT6797_CPUCLK_MUXSEL);
	ndelay(200);

	return ((readl(observer->mcumixed + MT6797_CPUCLK_MUXSEL) ^ value) &
		mask) ?
		       -EIO :
		       0;
}

struct mt6797_cpuclk_parent_transaction {
	struct mt6797_cpuclk_path_hw *path;
	u8 index;
};

static int mt6797_cpuclk_parent_transaction(void __iomem *cspm, void *context)
{
	struct mt6797_cpuclk_parent_transaction *transaction = context;
	struct mt6797_cpuclk_path_hw *path = transaction->path;
	struct mt6797_cpuclk_observer *observer = &path->provider->observer;
	struct clk_hw *hw = &path->hw;
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_state state;
	struct mt6797_cpuclk_dcm_plan dcm;
	struct mt6797_cpuclk_dcm_plan rollback_dcm;
	struct clk_hw *clk26m;
	unsigned long current_rate;
	unsigned long clk26m_rate;
	unsigned long target_rate;
	u32 final_dcm;
	u32 initial_mux;
	u32 target_mux;
	u32 mux_mask;
	u8 index = transaction->index;
	bool mutated = false;
	bool leased = false;
	int release_ret;
	int rollback_ret;
	int ret;

	ret = mt6797_cpuclk_lease_acquire(observer, cspm);
	if (ret)
		return ret;
	leased = true;

	ret = mt6797_cpuclk_read_stable(observer, &snapshot);
	if (ret)
		goto out_release;
	initial_mux = snapshot.muxsel;
	clk26m = clk_hw_get_parent_by_index(hw, MT6797_CPUCLK_CLK26M);
	if (!clk26m) {
		ret = -ENODEV;
		goto out_release;
	}
	clk26m_rate = clk_hw_get_rate(clk26m);
	ret = mt6797_cpuclk_decode_state(path->domain, &snapshot, clk26m_rate,
					 &state);
	if (ret)
		goto out_release;
	if (state.divider != 8) {
		ret = -EOPNOTSUPP;
		goto out_release;
	}
	if (state.source != MT6797_CPUCLK_ARMPLL &&
	    state.source != MT6797_CPUCLK_MAINPLL) {
		ret = -EOPNOTSUPP;
		goto out_release;
	}
	if (state.source == index)
		goto out_release;

	ret = mt6797_cpuclk_path_source_rate(hw, &state, state.source,
					     &current_rate);
	if (ret)
		goto out_release;
	ret = mt6797_cpuclk_path_source_rate(hw, &state, index, &target_rate);
	if (ret)
		goto out_release;

	ret = mt6797_cpuclk_plan_mux(path->domain, index, snapshot.muxsel,
				     &target_mux);
	if (ret)
		goto out_release;
	ret = mt6797_cpuclk_plan_dcm(path->domain, target_rate,
				     snapshot.sync_dcm, &dcm);
	if (ret)
		goto out_release;
	ret = mt6797_cpuclk_plan_dcm(path->domain, current_rate,
				     snapshot.sync_dcm, &rollback_dcm);
	if (ret)
		goto out_release;
	final_dcm = target_rate == current_rate ? snapshot.sync_dcm :
						  dcm.value[2];

	mux_mask = path->domain == MT6797_CPUCLK_MP0 ? GENMASK(3, 2) :
						       GENMASK(5, 4);
	if (target_rate < current_rate) {
		mutated = true;
		ret = mt6797_cpuclk_write_dcm(observer, &dcm);
		if (ret)
			goto rollback;
	}

	mutated = true;
	ret = mt6797_cpuclk_write_mux(observer, target_mux, mux_mask);
	if (ret)
		goto rollback;

	if (target_rate > current_rate) {
		ret = mt6797_cpuclk_write_dcm(observer, &dcm);
		if (ret)
			goto rollback;
	}

	ret = mt6797_cpuclk_read_stable(observer, &snapshot);
	if (ret)
		goto rollback;
	if (((snapshot.muxsel ^ target_mux) & mux_mask) ||
	    snapshot.sync_dcm != final_dcm) {
		ret = -EIO;
		goto rollback;
	}
	goto out_release;

rollback:
	if (mutated) {
		rollback_ret = mt6797_cpuclk_write_dcm(observer, &rollback_dcm);
		if (mt6797_cpuclk_write_mux(observer, initial_mux, mux_mask))
			rollback_ret = -EIO;
		if (rollback_ret) {
			observer->faulted = true;
			ret = -EUCLEAN;
		}
	}

out_release:
	if (leased) {
		release_ret = mt6797_cpuclk_lease_release(observer, cspm);
		if (release_ret) {
			observer->faulted = true;
			ret = -EUCLEAN;
		}
	}
	return ret;
}

static int mt6797_cpuclk_path_set_parent(struct clk_hw *hw, u8 index)
{
	struct mt6797_cpuclk_path_hw *path =
		container_of(hw, struct mt6797_cpuclk_path_hw, hw);
	struct mt6797_cpuclk_observer *observer = &path->provider->observer;
	struct mt6797_cpuclk_parent_transaction transaction = {
		.path = path,
		.index = index,
	};
	int ret;

	if ((path->domain != MT6797_CPUCLK_MP0 &&
	     path->domain != MT6797_CPUCLK_MP1) ||
	    (index != MT6797_CPUCLK_ARMPLL && index != MT6797_CPUCLK_MAINPLL))
		return -EOPNOTSUPP;

	mutex_lock(&observer->lock);
	if (observer->faulted)
		ret = -EUCLEAN;
	else
		ret = mt6797_dvfsp_cspm_transaction(observer->handoff,
						    mt6797_cpuclk_parent_transaction,
						    &transaction);
	mutex_unlock(&observer->lock);

	return ret;
}

static const struct clk_ops mt6797_cpuclk_path_ops = {
	.determine_rate = clk_hw_determine_rate_no_reparent,
	.get_parent = mt6797_cpuclk_path_get_parent,
	.recalc_rate = mt6797_cpuclk_path_recalc_rate,
	.set_parent = mt6797_cpuclk_path_set_parent,
};

static int mt6797_cpuclk_register_plls(struct mt6797_cpuclk *provider)
{
	struct clk_parent_data parent = { .fw_name = "clk26m" };
	struct clk_init_data init = {
		.ops = &mt6797_cpuclk_pll_ops,
		.parent_data = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < MT6797_CPUCLK_NR_DOMAINS; i++) {
		provider->plls[i].provider = provider;
		provider->plls[i].domain = i;
		provider->plls[i].hw.init = &init;
		init.name = mt6797_cpuclk_pll_names[i];

		ret = devm_clk_hw_register(provider->dev,
					   &provider->plls[i].hw);
		if (ret)
			return ret;
	}

	return 0;
}

static int mt6797_cpuclk_register_paths(struct mt6797_cpuclk *provider)
{
	struct clk_init_data init = {
		.ops = &mt6797_cpuclk_path_ops,
		.num_parents = MT6797_CPUCLK_NR_SOURCES,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < MT6797_CPUCLK_NR_DOMAINS; i++) {
		struct mt6797_cpuclk_path_hw *path = &provider->paths[i];

		path->provider = provider;
		path->domain = i;
		path->parents[MT6797_CPUCLK_CLK26M].fw_name = "clk26m";
		path->parents[MT6797_CPUCLK_ARMPLL].hw = &provider->plls[i].hw;
		path->parents[MT6797_CPUCLK_MAINPLL].fw_name = "mainpll";
		path->parents[MT6797_CPUCLK_UNIVPLL].fw_name = "univpll";
		path->hw.init = &init;
		init.name = mt6797_cpuclk_path_names[i];
		init.parent_data = path->parents;

		ret = devm_clk_hw_register(provider->dev, &path->hw);
		if (ret)
			return ret;

		provider->clk_data->hws[i] = &path->hw;
	}

	return 0;
}

static int mt6797_cpuclk_register(struct mt6797_cpuclk *provider)
{
	int ret;

	provider->clk_data = devm_kzalloc(provider->dev,
					  struct_size(provider->clk_data, hws,
						      MT6797_CPUCLK_NR_DOMAINS),
					  GFP_KERNEL);
	if (!provider->clk_data)
		return -ENOMEM;

	provider->clk_data->num = MT6797_CPUCLK_NR_DOMAINS;

	ret = mt6797_cpuclk_register_plls(provider);
	if (ret)
		return ret;

	ret = mt6797_cpuclk_register_paths(provider);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(provider->dev, of_clk_hw_onecell_get,
					   provider->clk_data);
}

static int mt6797_cpuclk_backend_init(struct platform_device *pdev,
				      struct mt6797_cpuclk *provider)
{
	struct mt6797_cpuclk_observer *observer = &provider->observer;
	int ret;

	observer->handoff = mt6797_dvfsp_handoff_get(&pdev->dev);
	if (IS_ERR(observer->handoff))
		return dev_err_probe(&pdev->dev, PTR_ERR(observer->handoff),
				     "cannot acquire CSPM owner\n");

	ret = mt6797_dvfsp_handoff_require_ready(observer->handoff);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "CSPM owner denied CPU clock access\n");

	observer->mcumixed =
		devm_platform_ioremap_resource_byname(pdev, "mcumixed");
	if (IS_ERR(observer->mcumixed))
		return PTR_ERR(observer->mcumixed);
	observer->mcucfg =
		devm_platform_ioremap_resource_byname(pdev, "mcucfg");
	if (IS_ERR(observer->mcucfg))
		return PTR_ERR(observer->mcucfg);

	observer->i2c_clk = devm_clk_get(&pdev->dev, "i2c");
	if (IS_ERR(observer->i2c_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(observer->i2c_clk),
				     "cannot get CSPM interface clock\n");

	mutex_init(&observer->lock);
	mutex_lock(&observer->lock);
	ret = mt6797_cpuclk_snapshot_locked(observer,
					    &observer->probe_snapshot);
	mutex_unlock(&observer->lock);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot obtain a stable clock snapshot\n");

	observer->probe_snapshot_valid = true;
	provider->backend.ops = &mt6797_cpuclk_observer_ops;
	provider->backend.context = observer;

	return 0;
}

static void mt6797_cpuclk_drop_probe_snapshot(struct mt6797_cpuclk *provider)
{
	struct mt6797_cpuclk_observer *observer = &provider->observer;

	mutex_lock(&observer->lock);
	memzero_explicit(&observer->probe_snapshot,
			 sizeof(observer->probe_snapshot));
	observer->probe_snapshot_valid = false;
	mutex_unlock(&observer->lock);
}

static int mt6797_cpuclk_probe(struct platform_device *pdev)
{
	struct mt6797_cpuclk *provider;
	int ret;

	provider = devm_kzalloc(&pdev->dev, sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;

	provider->dev = &pdev->dev;
	ret = mtk_wdt_require_boot_running(&pdev->dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "boot watchdog is not running before clock observation\n");
	ret = mt6797_cpuclk_backend_init(pdev, provider);
	if (ret)
		return ret;

	ret = mt6797_cpuclk_register(provider);
	mt6797_cpuclk_drop_probe_snapshot(provider);

	return ret;
}

static const struct of_device_id mt6797_cpuclk_of_match[] = {
	{ .compatible = "mediatek,mt6797-mcumixedsys" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6797_cpuclk_of_match);

static struct platform_driver mt6797_cpuclk_driver = {
	.probe = mt6797_cpuclk_probe,
	.driver = {
		.name = "clk-mt6797-cpu",
		.of_match_table = mt6797_cpuclk_of_match,
	},
};
module_platform_driver(mt6797_cpuclk_driver);

MODULE_DESCRIPTION("MediaTek MT6797 CPU clock provider foundation");
MODULE_LICENSE("GPL");
