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
#include <linux/of_platform.h>
#include <linux/soc/mediatek/mt6797-cpuclk.h>
#include <linux/soc/mediatek/mt6797-topckgen.h>
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

/* Snapshot selectors above the per-domain read-only observation range. */
#define MT6797_CPUCLK_FULL_SNAPSHOT (MT6797_CPUCLK_NR_DOMAINS + 1)

#define MT6797_CPUCLK_SMC_MCUSYS_WRITE 0x82000201UL

#define MT6797_ARMPLL_CHG BIT(31)

/* MCU frequency-hopping controller, vendor mach/mt_fhreg.h. */
#define MT6797_FHCTL_HP_EN 0xf00
#define MT6797_FHCTL_CLK_CON 0xf04
#define MT6797_FHCTL_RST_CON 0xf08
#define MT6797_FHCTL_SLOPE0 0xf0c
#define MT6797_FHCTL_CFG(ch) (0xf34 + 0x14 * (ch))
#define MT6797_FHCTL_UPDNLMT(ch) (0xf38 + 0x14 * (ch))
#define MT6797_FHCTL_DDS(ch) (0xf3c + 0x14 * (ch))
#define MT6797_FHCTL_DVFS(ch) (0xf40 + 0x14 * (ch))
#define MT6797_FHCTL_MON(ch) (0xf44 + 0x14 * (ch))
#define MT6797_FHCTL_CFG_FHCTL_EN BIT(0)
#define MT6797_FHCTL_CFG_SFSTR_EN BIT(2)
#define MT6797_FHCTL_DDS_EN BIT(31)
#define MT6797_FHCTL_DDS_MASK GENMASK(20, 0)
#define MT6797_FHCTL_SLOPE0_VALUE 0x6003c97
#define MT6797_FHCTL_ACCESS_NS 200
#define MT6797_FHCTL_READ_TRIES 3
#define MT6797_FHCTL_WRITE_TRIES 4
#define MT6797_FHCTL_POLL_US 10
#define MT6797_FHCTL_POLLS 100

struct mt6797_cpuclk_observer {
	struct device *dev;
	void __iomem *mcumixed;
	void __iomem *mcucfg;
	struct mt6797_dvfsp_handoff *handoff;
	struct mt6797_topckgen *topckgen;
	struct clk *mainpll;
	bool mainpll_rate_locked;
	struct clk *i2c_clk;
	/* Serializes snapshots and complete reconfiguration transactions. */
	struct mutex lock;
	bool faulted;
	bool probe_snapshot_valid;
	/* FHCTL channels initialized by this driver. */
	u32 fhctl_ready;
	struct mt6797_cpuclk_snapshot probe_snapshot;
	/* Saved only by successful last-core DCM disable, under lock. */
	struct mt6797_cpuclk_snapshot mp2_running;
	bool mp2_restart_valid;
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
	struct mt6797_cpuclk_pll_hw branches[3];
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
	int ret;

	ret = clk_prepare_enable(observer->i2c_clk);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_cspm_acquire(cspm);
	if (ret)
		clk_disable_unprepare(observer->i2c_clk);
	return ret;
}

static int
mt6797_cpuclk_lease_release(struct mt6797_cpuclk_observer *observer,
			    void __iomem *cspm)
{
	int ret = mt6797_dvfsp_cspm_release(cspm);

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

/* A53/CCI transitions own the AP PLLs and shared mux/divider/DCM words.
 * MP2 PLL words belong to firmware and are not transition-plan inputs.
 * Retain every shared field, including MP2 mux/divider bits, without issuing
 * secure reads into the A72 block while its power owner may hold reset.
 */
static int mt6797_cpuclk_read_ap_once(struct mt6797_cpuclk_observer *observer,
				     struct mt6797_cpuclk_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->muxsel = readl_relaxed(observer->mcumixed + MT6797_CPUCLK_MUXSEL);
	snapshot->clkdiv = readl_relaxed(observer->mcumixed + MT6797_CPUCLK_CLKDIV);
	snapshot->armpll_con1[MT6797_CPUCLK_MP0] =
		readl_relaxed(observer->mcumixed + MT6797_CPUCLK_MP0_PLL_CON1);
	snapshot->armpll_con1[MT6797_CPUCLK_MP1] =
		readl_relaxed(observer->mcumixed + MT6797_CPUCLK_MP1_PLL_CON1);
	snapshot->armpll_con1[MT6797_CPUCLK_CCI] =
		readl_relaxed(observer->mcumixed + MT6797_CPUCLK_CCI_PLL_CON1);
	snapshot->sync_dcm = readl_relaxed(observer->mcucfg + MT6797_CPUCLK_SYNC_DCM);
	return 0;
}

static int mt6797_cpuclk_read_ap_stable(struct mt6797_cpuclk_observer *observer,
				       struct mt6797_cpuclk_snapshot *snapshot)
{
	struct mt6797_cpuclk_snapshot second;

	mt6797_cpuclk_read_ap_once(observer, snapshot);
	mt6797_cpuclk_read_ap_once(observer, &second);
	return mt6797_cpuclk_snapshots_equal(snapshot, &second) ? 0 : -EAGAIN;
}

/* Read only the registers used to decode this domain. In particular, A53
 * and CCI observations must not enter firmware to sample the MP2 PLL while
 * the A72 power owner may be resetting that block.
 */
static int mt6797_cpuclk_read_domain(struct mt6797_cpuclk_observer *observer,
				    enum mt6797_cpuclk_domain domain,
				    struct mt6797_cpuclk_snapshot *snapshot)
{
	u32 reg;
	int ret;

	if (domain >= MT6797_CPUCLK_NR_DOMAINS)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->muxsel = readl_relaxed(observer->mcumixed + MT6797_CPUCLK_MUXSEL);
	snapshot->clkdiv = readl_relaxed(observer->mcumixed + MT6797_CPUCLK_CLKDIV);
	if (domain == MT6797_CPUCLK_MP2) {
		ret = mt6797_cpuclk_secure_read(MT6797_CPUCLK_SMC_REG_READ,
				MT6797_CPUCLK_MP2_PLL_CON1, &snapshot->mp2_armpll_con1);
		if (ret)
			return ret;
		return mt6797_cpuclk_secure_read(MT6797_CPUCLK_SMC_REG_READ,
				MT6797_CPUCLK_MP2_PLL_CON0, &snapshot->mp2_armpll_con0);
	}
	switch (domain) {
	case MT6797_CPUCLK_MP0:
		reg = MT6797_CPUCLK_MP0_PLL_CON1;
		break;
	case MT6797_CPUCLK_MP1:
		reg = MT6797_CPUCLK_MP1_PLL_CON1;
		break;
	case MT6797_CPUCLK_CCI:
		reg = MT6797_CPUCLK_CCI_PLL_CON1;
		break;
	default:
		return -EINVAL;
	}
	snapshot->armpll_con1[domain] = readl_relaxed(observer->mcumixed + reg);
	return 0;
}

/* Firmware may change MP2 during CPU_ON without taking this driver's mutex.
 * An A53/CCI observation must not depend on that unrelated path settling.
 * AP transitions retain all shared words; A72 admission retains full snapshots.
 */
static int mt6797_cpuclk_read_observation(struct mt6797_cpuclk_observer *observer,
					enum mt6797_cpuclk_domain domain,
					struct mt6797_cpuclk_snapshot *snapshot)
{
	struct mt6797_cpuclk_snapshot second;
	int ret;

	ret = mt6797_cpuclk_read_domain(observer, domain, snapshot);
	if (ret)
		return ret;
	ret = mt6797_cpuclk_read_domain(observer, domain, &second);
	if (ret)
		return ret;
	return mt6797_cpuclk_observations_equal(domain, snapshot, &second) ?
		0 : -EAGAIN;
}

struct mt6797_cpuclk_snapshot_transaction {
	enum mt6797_cpuclk_domain domain;
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

	if (transaction->domain == MT6797_CPUCLK_FULL_SNAPSHOT)
		ret = mt6797_cpuclk_read_stable(observer, transaction->snapshot);
	else if (transaction->domain == MT6797_CPUCLK_NR_DOMAINS)
		ret = mt6797_cpuclk_read_ap_stable(observer, transaction->snapshot);
	else
		ret = mt6797_cpuclk_read_observation(observer, transaction->domain,
						   transaction->snapshot);
	release_ret = mt6797_cpuclk_lease_release(observer, cspm);
	if (release_ret) {
		observer->faulted = true;
		return -EUCLEAN;
	}

	return ret;
}

static int
mt6797_cpuclk_snapshot_locked(struct mt6797_cpuclk_observer *observer,
			      struct mt6797_cpuclk_snapshot *snapshot,
			      enum mt6797_cpuclk_domain domain)
{
	struct mt6797_cpuclk_snapshot_transaction transaction = {
		.domain = domain,
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
mt6797_cpuclk_observer_sample(void *context,
			      struct mt6797_cpuclk_snapshot *snapshot,
			      enum mt6797_cpuclk_domain domain)
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
		ret = mt6797_cpuclk_snapshot_locked(observer, snapshot, domain);
	}
	mutex_unlock(&observer->lock);

	return ret;
}

static int
mt6797_cpuclk_observer_snapshot(void *context,
				struct mt6797_cpuclk_snapshot *snapshot)
{
	return mt6797_cpuclk_observer_sample(context, snapshot,
					    MT6797_CPUCLK_NR_DOMAINS);
}

static int
mt6797_cpuclk_observer_observe(void *context, enum mt6797_cpuclk_domain domain,
			       struct mt6797_cpuclk_snapshot *snapshot)
{
	if (domain < MT6797_CPUCLK_MP0 || domain >= MT6797_CPUCLK_NR_DOMAINS)
		return -EINVAL;
	return mt6797_cpuclk_observer_sample(context, snapshot, domain);
}

static int
mt6797_cpuclk_observer_transition(void *context,
			   const struct mt6797_cpuclk_transition_plan *plan);

static unsigned long mt6797_cpuclk_observer_mainpll_rate(void *context)
{
	struct mt6797_cpuclk_observer *observer = context;

	return observer->topckgen && observer->mainpll ?
		clk_get_rate(observer->mainpll) : 0;
}

static const struct mt6797_cpuclk_backend_ops mt6797_cpuclk_observer_ops = {
	.mainpll_rate = mt6797_cpuclk_observer_mainpll_rate,
	.snapshot = mt6797_cpuclk_observer_snapshot,
	.observe = mt6797_cpuclk_observer_observe,
	.transition = mt6797_cpuclk_observer_transition,
};

static u32 mt6797_cpuclk_pll_offset(enum mt6797_cpuclk_domain domain)
{
	switch (domain) {
	case MT6797_CPUCLK_MP0:
		return MT6797_CPUCLK_MP0_PLL_CON1;
	case MT6797_CPUCLK_MP1:
		return MT6797_CPUCLK_MP1_PLL_CON1;
	case MT6797_CPUCLK_CCI:
		return MT6797_CPUCLK_CCI_PLL_CON1;
	default:
		return 0;
	}
}

/*
 * MCU frequency-hopping controller registers.  They share the MCUMIXED window
 * and its CSPM hardware semaphore with the ARMPLLs.  Vendor channels 0, 1 and 2
 * (MCU_FH_PLL0/1/2) retune the LL, L and CCI ARMPLLs.
 */
static u32 mt6797_cpuclk_fhctl_channel(enum mt6797_cpuclk_domain domain)
{
	return domain == MT6797_CPUCLK_CCI ? 2 :
	       domain == MT6797_CPUCLK_MP0 ? 0 : 1;
}

/* The vendor driver reads MCU FHCTL registers twice after a settle delay. */
static u32 mt6797_cpuclk_fhctl_read(struct mt6797_cpuclk_observer *observer,
				    u32 offset)
{
	void __iomem *reg = observer->mcumixed + offset;
	unsigned int i;
	u32 value = 0;

	ndelay(MT6797_FHCTL_ACCESS_NS);
	for (i = 0; i < MT6797_FHCTL_READ_TRIES && !value; i++) {
		readl(reg);
		value = readl(reg);
	}

	return value;
}

static int mt6797_cpuclk_fhctl_write(struct mt6797_cpuclk_observer *observer,
				     u32 offset, u32 value, u32 check_mask)
{
	void __iomem *reg = observer->mcumixed + offset;
	unsigned int i;

	for (i = 0; i < MT6797_FHCTL_WRITE_TRIES; i++) {
		writel(value, reg);
		ndelay(MT6797_FHCTL_ACCESS_NS);
		if (!((mt6797_cpuclk_fhctl_read(observer, offset) ^ value) &
		      check_mask))
			return 0;
	}

	return -EIO;
}

static int mt6797_cpuclk_fhctl_update(struct mt6797_cpuclk_observer *observer,
				      u32 offset, u32 mask, u32 value)
{
	u32 current_value = mt6797_cpuclk_fhctl_read(observer, offset);

	return mt6797_cpuclk_fhctl_write(observer, offset,
					 (current_value & ~mask) | value, mask);
}

/* One-time channel setup, as done by the vendor hopping driver at init. */
static int mt6797_cpuclk_fhctl_init(struct mt6797_cpuclk_observer *observer,
				    u32 channel)
{
	u32 bit = BIT(channel);
	int ret;

	if (observer->fhctl_ready & bit)
		return 0;

	/* A channel which already owns its PLL is not ours to reset. */
	if (mt6797_cpuclk_fhctl_read(observer, MT6797_FHCTL_HP_EN) & bit)
		return -EBUSY;

	ret = mt6797_cpuclk_fhctl_update(observer, MT6797_FHCTL_CLK_CON, bit,
					 bit);
	if (!ret)
		ret = mt6797_cpuclk_fhctl_update(observer, MT6797_FHCTL_RST_CON,
						 bit, 0);
	if (!ret)
		ret = mt6797_cpuclk_fhctl_update(observer, MT6797_FHCTL_RST_CON,
						 bit, bit);
	if (ret)
		return ret;

	writel(0, observer->mcumixed + MT6797_FHCTL_CFG(channel));
	writel(0, observer->mcumixed + MT6797_FHCTL_UPDNLMT(channel));
	writel(0, observer->mcumixed + MT6797_FHCTL_DDS(channel));
	ndelay(MT6797_FHCTL_ACCESS_NS);

	observer->fhctl_ready |= bit;
	return 0;
}

/*
 * Slew one ARMPLL to a new DDS with the frequency-hopping controller, then
 * hand the reached DDS back to the PLL register.  This follows the vendor
 * mt_fh_hal_dfs_armpll() and mt_fh_hal_hopping_mcu() sequence.  Whenever the
 * controller took the PLL over, the hand-back copies the DDS it drives into
 * the PLL register first, so returning to register control never changes
 * the running rate, even when the slew stopped short of its target.
 */
static int mt6797_cpuclk_fhctl_hop(struct mt6797_cpuclk_observer *observer,
				   enum mt6797_cpuclk_domain domain, u32 dds)
{
	u32 channel = mt6797_cpuclk_fhctl_channel(domain);
	u32 con1_offset = mt6797_cpuclk_pll_offset(domain);
	u32 bit = BIT(channel);
	u32 con1, mon, initial;
	unsigned long flags;
	unsigned int i;
	int ret, hand_back = 0;

	local_irq_save(flags);
	/* Disable SSC, DVFS and hopping, then load the running DDS. */
	ret = mt6797_cpuclk_fhctl_write(observer, MT6797_FHCTL_CFG(channel), 0,
					~0U);
	con1 = mt6797_cpuclk_fhctl_read(observer, con1_offset);
	initial = con1 & MT6797_FHCTL_DDS_MASK;
	if (!ret)
		ret = mt6797_cpuclk_fhctl_write(observer,
			MT6797_FHCTL_DDS(channel),
			initial | MT6797_FHCTL_DDS_EN, MT6797_FHCTL_DDS_MASK);
	if (!ret)
		ret = mt6797_cpuclk_fhctl_write(observer,
			MT6797_FHCTL_CFG(channel),
			MT6797_FHCTL_CFG_SFSTR_EN | MT6797_FHCTL_CFG_FHCTL_EN,
			~0U);
	/* The MCU controller has a single slope setting. */
	if (!ret)
		ret = mt6797_cpuclk_fhctl_write(observer, MT6797_FHCTL_SLOPE0,
						MT6797_FHCTL_SLOPE0_VALUE,
						~0U);
	/* Hand the PLL to the controller and request the new DDS. */
	if (!ret)
		ret = mt6797_cpuclk_fhctl_update(observer, MT6797_FHCTL_HP_EN,
						 bit, bit);
	/* The vendor sequence completes the hand-over before the request. */
	mb();
	if (!ret)
		ret = mt6797_cpuclk_fhctl_write(observer,
			MT6797_FHCTL_DVFS(channel), dds | MT6797_FHCTL_DDS_EN,
			MT6797_FHCTL_DDS_MASK);
	local_irq_restore(flags);

	for (i = 0; !ret && i < MT6797_FHCTL_POLLS; i++) {
		mon = mt6797_cpuclk_fhctl_read(observer,
			MT6797_FHCTL_MON(channel)) & MT6797_FHCTL_DDS_MASK;
		if (mon == dds)
			break;
		udelay(MT6797_FHCTL_POLL_US);
	}

	local_irq_save(flags);
	mon = initial;
	if (mt6797_cpuclk_fhctl_read(observer, MT6797_FHCTL_HP_EN) & bit) {
		mon = mt6797_cpuclk_fhctl_read(observer,
			MT6797_FHCTL_MON(channel)) & MT6797_FHCTL_DDS_MASK;
		con1 = mt6797_cpuclk_fhctl_read(observer, con1_offset);
		hand_back = mt6797_cpuclk_fhctl_write(observer, con1_offset,
			(con1 & ~MT6797_FHCTL_DDS_MASK) | mon |
			MT6797_ARMPLL_CHG, MT6797_FHCTL_DDS_MASK);
		if (mt6797_cpuclk_fhctl_update(observer, MT6797_FHCTL_HP_EN,
					       bit, 0))
			hand_back = -EIO;
		/* Complete the hand-back before the channel is disabled. */
		mb();
	}
	if (mt6797_cpuclk_fhctl_write(observer, MT6797_FHCTL_CFG(channel), 0,
				      ~0U))
		hand_back = -EIO;
	local_irq_restore(flags);

	if (hand_back)
		return hand_back;
	if (ret)
		return ret;

	return mon == dds ? 0 : -ETIMEDOUT;
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

static int mt6797_cpuclk_write_divider(struct mt6797_cpuclk_observer *observer,
				      u32 value, u32 mask)
{
	unsigned int attempt;

	/* Vendor adjust_clkdiv(): settle for 2 us, read back, retry once. */
	for (attempt = 0; attempt < 2; attempt++) {
		writel(value, observer->mcumixed + MT6797_CPUCLK_CLKDIV);
		udelay(2);
		if (!((readl(observer->mcumixed + MT6797_CPUCLK_CLKDIV) ^ value) & mask))
			return 0;
	}
	return -EIO;
}

static int mt6797_cpuclk_write_mux(struct mt6797_cpuclk_observer *observer,
				  u32 value, u32 mask);

/* Called under the CPU lease, with MAINPLL prepared and rate-protected. */
static int mt6797_cpuclk_postdivider_switch(struct mt6797_cpuclk_observer *observer,
				 const struct mt6797_cpuclk_transition_plan *plan)
{
	u32 offset = mt6797_cpuclk_pll_offset(plan->domain);
	u32 mask = MT6797_FHCTL_DDS_MASK | GENMASK(26, 24);
	int ret;

	ret = mt6797_cpuclk_write_divider(observer, plan->temporary_clkdiv,
					plan->clkdiv_mask);
	if (ret)
		return ret;
	ret = mt6797_cpuclk_write_mux(observer, plan->mainpll_mux, plan->mux_mask);
	if (ret)
		return ret;

	/* The CPU no longer uses ARMPLL. Never switch back after a bad retune. */
	writel(plan->target_con1, observer->mcumixed + offset);
	udelay(20); /* Vendor PLL_SETTLE_TIME for a DDS write. */
	if ((readl(observer->mcumixed + offset) ^ plan->target_con1) & mask)
		return -EIO;
	ret = mt6797_cpuclk_write_mux(observer, plan->snapshot.muxsel, plan->mux_mask);
	if (ret)
		return ret;
	return mt6797_cpuclk_write_divider(observer, plan->clkdiv, plan->clkdiv_mask);
}

struct mt6797_cpuclk_transition_transaction {
	struct mt6797_cpuclk_observer *observer;
	const struct mt6797_cpuclk_transition_plan *plan;
};

static int mt6797_cpuclk_transition_transaction(void __iomem *cspm, void *context)
{
	struct mt6797_cpuclk_transition_transaction *transaction = context;
	struct mt6797_cpuclk_observer *observer = transaction->observer;
	const struct mt6797_cpuclk_transition_plan *plan = transaction->plan;
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_state state;
	u8 fallback_saved = 0;
	int release_ret;
	int ret;

	ret = mt6797_cpuclk_lease_acquire(observer, cspm);
	if (ret)
		return ret;

	ret = mt6797_cpuclk_read_ap_stable(observer, &snapshot);
	if (ret)
		goto out_release;
	if (!mt6797_cpuclk_snapshots_equal(&snapshot, &plan->snapshot)) {
		ret = -EAGAIN;
		goto out_release;
	}

	if (plan->changes_postdiv) {
		unsigned int domain;

		if (!observer->topckgen || !observer->mainpll_rate_locked) {
			ret = -EOPNOTSUPP;
			goto out_release;
		}
		if (clk_get_rate(observer->mainpll) != plan->mainpll_rate) {
			ret = -EAGAIN;
			goto out_release;
		}
		/* Check on every direct retune, even after channel initialization. */
		if (mt6797_cpuclk_fhctl_read(observer, MT6797_FHCTL_HP_EN) &
		    BIT(mt6797_cpuclk_fhctl_channel(plan->domain))) {
			ret = -EBUSY;
			goto out_release;
		}
		ret = mt6797_topckgen_cpu_read(observer->topckgen, &fallback_saved);
		if (ret)
			goto out_release;
		for (domain = 0; domain < MT6797_CPUCLK_NR_DOMAINS; domain++) {
			if (((snapshot.muxsel >> (domain * 2)) & 3) >= 2 &&
			    fallback_saved != 3) {
				ret = -EUCLEAN;
				goto out_release;
			}
		}
	} else {
		ret = mt6797_cpuclk_fhctl_init(observer,
				       mt6797_cpuclk_fhctl_channel(plan->domain));
		if (ret)
			goto out_release;
	}

	/*
	 * DCM covers the lowest intermediate rate, including CKDIV steps.
	 * Once writes begin, fence failures rather than raising a divider or
	 * attempting a speculative PLL rollback after a partially completed hop.
	 */
	ret = mt6797_cpuclk_write_dcm(observer, &plan->before);
	if (ret)
		goto fault;
	if (plan->changes_postdiv) {
		ret = mt6797_topckgen_cpu_update(observer->topckgen, fallback_saved, 3);
		if (ret)
			goto fault;
		ret = mt6797_cpuclk_postdivider_switch(observer, plan);
		if (ret)
			goto fault;
		goto final_dcm;
	}
	if (plan->divider_before_hop) {
		ret = mt6797_cpuclk_write_divider(observer, plan->clkdiv,
						plan->clkdiv_mask);
		if (ret)
			goto fault;
	}

	ret = mt6797_cpuclk_fhctl_hop(observer, plan->domain, plan->dds);
	if (ret)
		goto fault;

	if (!plan->divider_before_hop && plan->clkdiv != plan->snapshot.clkdiv) {
		ret = mt6797_cpuclk_write_divider(observer, plan->clkdiv,
						plan->clkdiv_mask);
		if (ret)
			goto fault;
	}
final_dcm:
	ret = mt6797_cpuclk_write_dcm(observer, &plan->after);
	if (ret)
		goto fault;

	ret = mt6797_cpuclk_read_ap_stable(observer, &snapshot);
	if (!ret)
		ret = mt6797_cpuclk_decode_state(plan->domain, &snapshot,
						 plan->clk26m_rate, &state);
	if (!ret && (state.armpll_rate != plan->pll_rate ||
		     snapshot.muxsel != plan->snapshot.muxsel ||
		     snapshot.clkdiv != plan->clkdiv ||
		     snapshot.sync_dcm != plan->after.value[2]))
		ret = -EIO;
	/* Restore the shared setting only after proving every mux is back. */
	if (!ret && plan->changes_postdiv)
		ret = mt6797_topckgen_cpu_update(observer->topckgen, 3, fallback_saved);
	if (!ret)
		goto out_release;

fault:
	observer->faulted = true;
	dev_err(observer->dev,
		"domain %u hop %llu -> %lu Hz failed: %d; fencing CPU clock changes\n",
		plan->domain, plan->initial_rate, plan->rate, ret);

out_release:
	release_ret = mt6797_cpuclk_lease_release(observer, cspm);
	if (release_ret) {
		observer->faulted = true;
		ret = -EUCLEAN;
	}
	return ret;
}

static int
mt6797_cpuclk_observer_transition(void *context,
			   const struct mt6797_cpuclk_transition_plan *plan)
{
	struct mt6797_cpuclk_observer *observer = context;
	struct mt6797_cpuclk_transition_transaction transaction = {
		.observer = observer,
		.plan = plan,
	};
	int ret;

	if (!observer || !plan)
		return -EINVAL;

	mutex_lock(&observer->lock);
	if (observer->faulted) {
		ret = -EUCLEAN;
		goto out_unlock;
	}
	if (plan->changes_postdiv) {
		if (!observer->mainpll || !observer->topckgen) {
			ret = -EOPNOTSUPP;
			goto out_unlock;
		}
		ret = clk_rate_exclusive_get(observer->mainpll);
		if (ret)
			goto out_unlock;
		observer->mainpll_rate_locked = true;
	}
	ret = mt6797_dvfsp_cspm_transaction(observer->handoff,
						    mt6797_cpuclk_transition_transaction,
						    &transaction);
	/* A fault may leave the CPU on MAINPLL: retain its rate protection. */
	if (observer->mainpll_rate_locked && !observer->faulted) {
		clk_rate_exclusive_put(observer->mainpll);
		observer->mainpll_rate_locked = false;
	}
out_unlock:
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

static int mt6797_cpuclk_observe(struct mt6797_cpuclk *provider,
			       enum mt6797_cpuclk_domain domain,
			       struct mt6797_cpuclk_snapshot *snapshot)
{
	if (!provider->backend.ops || !provider->backend.ops->observe)
		return -EOPNOTSUPP;
	return provider->backend.ops->observe(provider->backend.context, domain,
					      snapshot);
}

/* Report the existing sample/decode failure without taking another sample. */
static int mt6797_cpuclk_sample_decode(struct mt6797_cpuclk *provider,
				      enum mt6797_cpuclk_domain domain,
				      unsigned long reference_rate,
				      struct mt6797_cpuclk_state *state,
				      const char *callback)
{
	struct mt6797_cpuclk_snapshot snapshot;
	int ret;

	ret = mt6797_cpuclk_observe(provider, domain, &snapshot);
	if (ret) {
		dev_info_ratelimited(provider->dev,
			"CPUCLK_OBSERVE callback=%s domain=%u stage=snapshot status=%d ref_hz=%lu\n",
			callback, domain, ret, reference_rate);
		return ret;
	}

	ret = mt6797_cpuclk_decode_state(domain, &snapshot, reference_rate, state);
	if (ret)
		dev_info_ratelimited(provider->dev,
			"CPUCLK_OBSERVE callback=%s domain=%u stage=decode status=%d ref_hz=%lu mux=%08x div=%08x pll0=%08x pll1=%08x pllcci=%08x mp2con0=%08x mp2con1=%08x\n",
			callback, domain, ret, reference_rate, snapshot.muxsel,
			snapshot.clkdiv, snapshot.armpll_con1[MT6797_CPUCLK_MP0],
			snapshot.armpll_con1[MT6797_CPUCLK_MP1],
			snapshot.armpll_con1[MT6797_CPUCLK_CCI],
			snapshot.mp2_armpll_con0, snapshot.mp2_armpll_con1);

	return ret;
}

static unsigned long mt6797_cpuclk_pll_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct mt6797_cpuclk_pll_hw *pll =
		container_of(hw, struct mt6797_cpuclk_pll_hw, hw);
	struct mt6797_cpuclk_state state;

	if (mt6797_cpuclk_sample_decode(pll->provider, pll->domain, parent_rate,
					&state, "pll-rate"))
		return 0;

	return state.armpll_rate;
}

/*
 * LL/L expose a compound ARMPLL+CKDIV branch to CCF. The raw PLL clocks are
 * read-only observation taps, not rate-changing parents: CCF must never run
 * a parent PLL set_rate before the CKDIV part of this transaction.
 */
static unsigned long mt6797_cpuclk_branch_recalc_rate(struct clk_hw *hw,
						      unsigned long parent_rate)
{
	struct mt6797_cpuclk_pll_hw *branch =
		container_of(hw, struct mt6797_cpuclk_pll_hw, hw);
	struct mt6797_cpuclk_state state;

	if (mt6797_cpuclk_sample_decode(branch->provider, branch->domain,
					parent_rate, &state, "branch-rate"))
		return 0;
	return mt6797_cpuclk_apply_divider(state.armpll_rate, state.divider);
}

static int mt6797_cpuclk_provider_plan(struct mt6797_cpuclk *provider,
				     enum mt6797_cpuclk_domain domain,
				     const struct mt6797_cpuclk_snapshot *snapshot,
				     unsigned long parent_rate, unsigned long rate,
				     struct mt6797_cpuclk_transition_plan *plan)
{
	struct mt6797_cpuclk_backend *backend = &provider->backend;
	unsigned long mainpll_rate = 0;

	if (backend->ops && backend->ops->mainpll_rate)
		mainpll_rate = backend->ops->mainpll_rate(backend->context);
	return mt6797_cpuclk_plan_transition_mainpll(domain, snapshot, parent_rate,
						   rate, mainpll_rate, plan);
}

static int mt6797_cpuclk_branch_determine_rate(struct clk_hw *hw,
					       struct clk_rate_request *req)
{
	struct mt6797_cpuclk_pll_hw *branch =
		container_of(hw, struct mt6797_cpuclk_pll_hw, hw);
	struct mt6797_cpuclk_transition_plan plan;
	struct mt6797_cpuclk_snapshot snapshot;
	int ret;

	ret = mt6797_cpuclk_snapshot(branch->provider, &snapshot);
	if (ret)
		return ret;
	ret = mt6797_cpuclk_provider_plan(branch->provider, branch->domain, &snapshot,
				 req->best_parent_rate, req->rate, &plan);
	if (ret)
		return ret;
	if (plan.rate < req->min_rate || plan.rate > req->max_rate)
		return -ERANGE;
	req->rate = plan.rate;
	return 0;
}

static int mt6797_cpuclk_branch_set_rate(struct clk_hw *hw, unsigned long rate,
					 unsigned long parent_rate)
{
	struct mt6797_cpuclk_pll_hw *branch =
		container_of(hw, struct mt6797_cpuclk_pll_hw, hw);
	struct mt6797_cpuclk_backend *backend = &branch->provider->backend;
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_transition_plan plan;
	int ret;

	if (!backend->ops || !backend->ops->transition)
		return -EOPNOTSUPP;
	ret = mt6797_cpuclk_snapshot(branch->provider, &snapshot);
	if (ret)
		return ret;
	ret = mt6797_cpuclk_provider_plan(branch->provider, branch->domain, &snapshot,
					    parent_rate, rate, &plan);
	if (ret)
		return ret;
	return backend->ops->transition(backend->context, &plan);
}

static const struct clk_ops mt6797_cpuclk_pll_ops = {
	.recalc_rate = mt6797_cpuclk_pll_recalc_rate,
};

static const struct clk_ops mt6797_cpuclk_branch_ops = {
	.recalc_rate = mt6797_cpuclk_branch_recalc_rate,
	.determine_rate = mt6797_cpuclk_branch_determine_rate,
	.set_rate = mt6797_cpuclk_branch_set_rate,
};

static u8 mt6797_cpuclk_path_get_parent(struct clk_hw *hw)
{
	struct mt6797_cpuclk_path_hw *path =
		container_of(hw, struct mt6797_cpuclk_path_hw, hw);
	struct mt6797_cpuclk_state state;
	struct clk_hw *clk26m;
	unsigned long clk26m_rate;

	clk26m = clk_hw_get_parent_by_index(hw, MT6797_CPUCLK_CLK26M);
	if (!clk26m) {
		dev_info_ratelimited(path->provider->dev,
			"CPUCLK_OBSERVE callback=get-parent domain=%u stage=reference-parent status=%d\n",
			path->domain, -ENOENT);
		return -EINVAL;
	}

	clk26m_rate = clk_hw_get_rate(clk26m);
	if (mt6797_cpuclk_sample_decode(path->provider, path->domain, clk26m_rate,
					&state, "get-parent"))
		return -EINVAL;

	return state.source;
}

static unsigned long mt6797_cpuclk_path_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	struct mt6797_cpuclk_path_hw *path =
		container_of(hw, struct mt6797_cpuclk_path_hw, hw);
	struct mt6797_cpuclk_state state;
	struct clk_hw *clk26m, *parent;
	unsigned long clk26m_rate;

	clk26m = clk_hw_get_parent_by_index(hw, MT6797_CPUCLK_CLK26M);
	if (!clk26m) {
		dev_info_ratelimited(path->provider->dev,
			"CPUCLK_OBSERVE callback=path-rate domain=%u stage=reference-parent status=%d\n",
			path->domain, -ENOENT);
		return 0;
	}

	clk26m_rate = clk_hw_get_rate(clk26m);
	if (mt6797_cpuclk_sample_decode(path->provider, path->domain, clk26m_rate,
					&state, "path-rate"))
		return 0;

	if (state.source == MT6797_CPUCLK_ARMPLL) {
		parent_rate = state.armpll_rate;
	} else {
		parent = clk_hw_get_parent_by_index(hw, state.source);
		if (!parent) {
			dev_info_ratelimited(path->provider->dev,
				"CPUCLK_OBSERVE callback=path-rate domain=%u stage=selected-parent source=%u status=%d\n",
				path->domain, state.source, -ENOENT);
			return 0;
		}
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

	ret = mt6797_cpuclk_read_ap_stable(observer, &snapshot);
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

	ret = mt6797_cpuclk_read_ap_stable(observer, &snapshot);
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

static int mt6797_cpuclk_register_branches(struct mt6797_cpuclk *provider)
{
	static const char *const names[] = {
		"mt6797_mp0_armpll_div", "mt6797_mp1_armpll_div",
		"mt6797_cci_armpll_div",
	};
	struct clk_parent_data parent = { .fw_name = "clk26m" };
	struct clk_init_data init = {
		.ops = &mt6797_cpuclk_branch_ops,
		.parent_data = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(provider->branches); i++) {
		provider->branches[i].provider = provider;
		provider->branches[i].domain = i == 2 ? MT6797_CPUCLK_CCI : i;
		provider->branches[i].hw.init = &init;
		init.name = names[i];
		ret = devm_clk_hw_register(provider->dev, &provider->branches[i].hw);
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
		path->parents[MT6797_CPUCLK_ARMPLL].hw =
			i == MT6797_CPUCLK_CCI ? &provider->branches[2].hw :
			i < 2 ? &provider->branches[i].hw : &provider->plls[i].hw;
		path->parents[MT6797_CPUCLK_MAINPLL].fw_name = "mainpll";
		path->parents[MT6797_CPUCLK_UNIVPLL].fw_name = "univpll";
		path->hw.init = &init;
		init.name = mt6797_cpuclk_path_names[i];
		/* LL/L/CCI requests use the compound PLL+CKDIV branch. */
		init.flags = CLK_GET_RATE_NOCACHE;
		if (i == MT6797_CPUCLK_MP0 || i == MT6797_CPUCLK_MP1 ||
		    i == MT6797_CPUCLK_CCI)
			init.flags |= CLK_SET_RATE_PARENT;
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

	ret = mt6797_cpuclk_register_branches(provider);
	if (ret)
		return ret;

	ret = mt6797_cpuclk_register_paths(provider);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(provider->dev, of_clk_hw_onecell_get,
					   provider->clk_data);
}

static void mt6797_cpuclk_release_mainpll_rate(void *context)
{
	struct mt6797_cpuclk_observer *observer = context;

	if (observer->mainpll_rate_locked)
		clk_rate_exclusive_put(observer->mainpll);
}

static int mt6797_cpuclk_backend_init(struct platform_device *pdev,
				      struct mt6797_cpuclk *provider)
{
	struct mt6797_cpuclk_observer *observer = &provider->observer;
	int ret;

	observer->dev = &pdev->dev;
	observer->topckgen = mt6797_topckgen_get(&pdev->dev);
	if (IS_ERR(observer->topckgen)) {
		if (PTR_ERR(observer->topckgen) != -ENOENT)
			return dev_err_probe(&pdev->dev, PTR_ERR(observer->topckgen),
					     "cannot acquire TOPCKGEN owner\n");
		observer->topckgen = NULL;
	}

	if (observer->topckgen) {
		observer->mainpll = devm_clk_get_enabled(&pdev->dev, "mainpll");
		if (IS_ERR(observer->mainpll))
			return dev_err_probe(&pdev->dev, PTR_ERR(observer->mainpll),
					     "cannot prepare MAINPLL fallback\n");
		ret = devm_add_action_or_reset(&pdev->dev,
				mt6797_cpuclk_release_mainpll_rate, observer);
		if (ret)
			return ret;
	}
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
					    &observer->probe_snapshot,
					    MT6797_CPUCLK_FULL_SNAPSHOT);
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
	if (!ret)
		platform_set_drvdata(pdev, provider);

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
enum mt6797_cpuclk_mp2_operation {
	MT6797_MP2_CHECK_BOOT,
	MT6797_MP2_CHECK_RESTART,
	MT6797_MP2_DCM_ENABLE,
	MT6797_MP2_DCM_DISABLE,
	MT6797_MP2_SET_RATE,
};

struct mt6797_cpuclk_mp2_transaction {
	struct mt6797_cpuclk_observer *observer;
	enum mt6797_cpuclk_mp2_operation operation;
	unsigned long reference_rate;
	unsigned long requested_rate, mainpll_rate;
	unsigned int divisor;
};

static int mt6797_cpuclk_mp2_write_dcm(u32 value)
{
	struct arm_smccc_res result;
	s32 status;

	arm_smccc_smc(MT6797_CPUCLK_SMC_MCUSYS_WRITE,
		      MT6797_CPUCLK_MP2_SYNC_DCM, value, 0,
		      0, 0, 0, 0, &result);
	status = result.a0;
	if (result.a0)
		return status < 0 ? status : -EIO;
	return 0;
}

/* All callbacks run under the observer mutex and the CSPM hardware lease. */
static int mt6797_cpuclk_mp2_snapshot(void *context,
		struct mt6797_cpuclk_snapshot *snapshot, u32 *dcm)
{
	u32 before, after;
	int ret;

	ret = mt6797_cpuclk_secure_read(MT6797_CPUCLK_SMC_REG_READ,
				       MT6797_CPUCLK_MP2_SYNC_DCM, &before);
	if (ret)
		return ret;
	ret = mt6797_cpuclk_read_stable(context, snapshot);
	if (ret)
		return ret;
	ret = mt6797_cpuclk_secure_read(MT6797_CPUCLK_SMC_REG_READ,
				       MT6797_CPUCLK_MP2_SYNC_DCM, &after);
	if (ret)
		return ret;
	if (before != after)
		return -EAGAIN;
	*dcm = after;
	return 0;
}

static int mt6797_cpuclk_mp2_divider(void *context, u32 value)
{
	return mt6797_cpuclk_write_divider(context, value, GENMASK(4, 0));
}

static int mt6797_cpuclk_mp2_mux(void *context, u32 value)
{
	return mt6797_cpuclk_write_mux(context, value, GENMASK(1, 0));
}

static int mt6797_cpuclk_mp2_pll(void *context, u32 mhz)
{
	struct arm_smccc_res result;
	s32 status;

	if (!context)
		return -EINVAL;
	/* Firmware does not validate this argument itself. */
	if (mhz < 250 || mhz > 3000)
		return -ERANGE;
	arm_smccc_smc(0xc20003b8UL, mhz, 0, 0, 0, 0, 0, 0, &result);
	status = result.a0;
	return result.a0 ? (status < 0 ? status : -EIO) : 0;
}

static const struct mt6797_cpuclk_mp2_io mt6797_cpuclk_mp2_io = {
	.snapshot = mt6797_cpuclk_mp2_snapshot,
	.write_divider = mt6797_cpuclk_mp2_divider,
	.write_mux = mt6797_cpuclk_mp2_mux,
	.set_pll = mt6797_cpuclk_mp2_pll,
};

static int mt6797_cpuclk_mp2_toggle_dcm(u32 target)
{
	u32 value, actual;
	int i, ret;

	for (i = 0; i < 2; i++) {
		value = target | (i ? 0 : BIT(1));
		ret = mt6797_cpuclk_mp2_write_dcm(value);
		if (ret)
			return ret;
		ret = mt6797_cpuclk_secure_read(MT6797_CPUCLK_SMC_REG_READ,
					       MT6797_CPUCLK_MP2_SYNC_DCM, &actual);
		if (ret)
			return ret;
		if (actual != value)
			return -EIO;
	}
	return 0;
}

static int mt6797_cpuclk_mp2_change_rate(
		struct mt6797_cpuclk_mp2_transaction *transaction, bool *wrote)
{
	struct mt6797_cpuclk_observer *observer = transaction->observer;
	struct mt6797_cpuclk_mp2_plan plan;
	struct mt6797_cpuclk_snapshot snapshot;
	u32 dcm, restored_dcm;
	int ret;

	if (observer->mp2_restart_valid)
		return -EBUSY;
	ret = mt6797_cpuclk_mp2_snapshot(observer, &snapshot, &dcm);
	if (ret)
		return ret;
	/* Do not adopt an in-progress DCM toggle as a restore target. */
	if (dcm & BIT(1))
		return -EBUSY;
	ret = mt6797_cpuclk_plan_mp2_transition(&snapshot,
		transaction->reference_rate, transaction->requested_rate,
		transaction->divisor, transaction->mainpll_rate, &plan);
	if (ret)
		return ret;
	*wrote = true;
	ret = mt6797_cpuclk_mp2_toggle_dcm(dcm & ~GENMASK(6, 0));
	if (ret)
		return ret;
	ret = mt6797_cpuclk_execute_mp2_plan(&plan, &mt6797_cpuclk_mp2_io,
		observer, transaction->mainpll_rate, &observer->faulted);
	if (ret)
		return ret;
	/* Preserve the owner's DCM policy, without creating a restart receipt. */
	ret = mt6797_cpuclk_mp2_toggle_dcm(dcm);
	if (ret)
		return ret;
	ret = mt6797_cpuclk_mp2_snapshot(observer, &snapshot, &restored_dcm);
	if (ret)
		return ret;
	return restored_dcm != dcm ||
		memcmp(&snapshot, &plan.step[MT6797_MP2_RESTORE_DIVIDER],
		       sizeof(snapshot)) ? -EIO : 0;
}

/* The caller enables the interface clock before taking either mutex. */
static int mt6797_cpuclk_mp2_transaction(void __iomem *cspm, void *context)
{
	struct mt6797_cpuclk_mp2_transaction *transaction = context;
	struct mt6797_cpuclk_observer *observer = transaction->observer;
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_state state;
	u32 first, second, target, value;
	bool wrote = false;
	int ret, release_ret, i;

	ret = mt6797_dvfsp_cspm_acquire(cspm);
	if (ret)
		return ret;
	if (transaction->operation == MT6797_MP2_SET_RATE) {
		ret = mt6797_cpuclk_mp2_change_rate(transaction, &wrote);
		goto release;
	}
	ret = mt6797_cpuclk_read_stable(observer, &snapshot);
	if (ret)
		goto release;
	if (transaction->operation == MT6797_MP2_CHECK_BOOT) {
		ret = mt6797_cpuclk_validate_cold_mp2(&snapshot,
						transaction->reference_rate);
		goto release;
	}
	if (transaction->operation == MT6797_MP2_CHECK_RESTART) {
		ret = observer->mp2_restart_valid ?
			mt6797_cpuclk_validate_stopped_mp2(&observer->mp2_running,
				&snapshot, transaction->reference_rate) : -EAGAIN;
		goto release;
	}
	if (transaction->operation == MT6797_MP2_DCM_DISABLE &&
	    observer->mp2_restart_valid) {
		ret = -EALREADY;
		goto release;
	}
	ret = mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2, &snapshot,
					transaction->reference_rate, &state);
	if (ret)
		goto release;
	if (state.source != MT6797_CPUCLK_ARMPLL ||
	    !(snapshot.mp2_armpll_con0 & BIT(0))) {
		ret = -EBUSY;
		goto release;
	}
	ret = mt6797_cpuclk_secure_read(MT6797_CPUCLK_SMC_REG_READ,
				       MT6797_CPUCLK_MP2_SYNC_DCM, &first);
	if (ret)
		goto release;
	ret = mt6797_cpuclk_secure_read(MT6797_CPUCLK_SMC_REG_READ,
				       MT6797_CPUCLK_MP2_SYNC_DCM, &second);
	if (ret)
		goto release;
	if (first != second) {
		ret = -EAGAIN;
		goto release;
	}
	/* Vendor dcm_mcusys_mp2_sync_dcm(): divide by four, toggle high/low. */
	target = first & ~GENMASK(6, 0);
	if (transaction->operation == MT6797_MP2_DCM_ENABLE)
		target |= BIT(0) | (3 << 2);
	for (i = 0; i < 2; i++) {
		value = target | (i ? 0 : BIT(1));
		wrote = true;
		ret = mt6797_cpuclk_mp2_write_dcm(value);
		if (ret)
			goto release;
		ret = mt6797_cpuclk_secure_read(MT6797_CPUCLK_SMC_REG_READ,
					       MT6797_CPUCLK_MP2_SYNC_DCM, &second);
		if (ret)
			goto release;
		if (second != value) {
			ret = -EIO;
			goto release;
		}
	}
release:
	release_ret = mt6797_dvfsp_cspm_release(cspm);
	if (release_ret || (wrote && ret)) {
		observer->faulted = true;
		/* The handoff transaction also fences the shared hardware owner. */
		return -EUCLEAN;
	}
	if (!ret && transaction->operation == MT6797_MP2_DCM_DISABLE) {
		observer->mp2_running = snapshot;
		observer->mp2_restart_valid = true;
	} else if (!ret && transaction->operation == MT6797_MP2_DCM_ENABLE) {
		observer->mp2_restart_valid = false;
	}
	return ret;
}

static int mt6797_cpuclk_mp2_access(struct device *consumer, struct clk *clk,
				  enum mt6797_cpuclk_mp2_operation operation,
				  unsigned long rate, unsigned int divisor)
{
	struct mt6797_cpuclk_mp2_transaction transaction = {
		.operation = operation,
		.requested_rate = rate,
		.divisor = divisor,
	};
	struct mt6797_cpuclk_observer *observer;
	struct of_phandle_args args;
	struct platform_device *pdev;
	struct mt6797_cpuclk *provider;
	struct clk_hw *reference;
	bool pinned = false;
	int index, ret;

	if (!consumer || !consumer->of_node || IS_ERR_OR_NULL(clk))
		return -EINVAL;
	index = of_property_match_string(consumer->of_node, "clock-names", "mp2");
	if (index < 0)
		return index;
	ret = of_parse_phandle_with_args(consumer->of_node, "clocks",
					"#clock-cells", index, &args);
	if (ret)
		return ret;
	pdev = of_find_device_by_node(args.np);
	of_node_put(args.np);
	if (!pdev)
		return -EPROBE_DEFER;

	/* Hold the supplier against unbind while using its devm-owned state. */
	device_lock(&pdev->dev);
	provider = platform_get_drvdata(pdev);
	if (pdev->dev.driver != &mt6797_cpuclk_driver.driver || !provider) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	if (args.args_count != 1 || args.args[0] != CLK_MCU_MP2 ||
	    __clk_get_hw(clk) != &provider->paths[MT6797_CPUCLK_MP2].hw) {
		ret = -EINVAL;
		goto out;
	}
	reference = clk_hw_get_parent_by_index(
		&provider->paths[MT6797_CPUCLK_MP2].hw, MT6797_CPUCLK_CLK26M);
	if (!reference) {
		ret = -ENOENT;
		goto out;
	}
	transaction.reference_rate = clk_hw_get_rate(reference);
	if (transaction.reference_rate != 26000000) {
		ret = -ERANGE;
		goto out;
	}
	observer = &provider->observer;
	transaction.observer = observer;
	/* CCF callbacks take the prepare lock before observer/CSPM locks. */
	ret = clk_prepare_enable(observer->i2c_clk);
	if (ret)
		goto out;
	if (operation == MT6797_MP2_SET_RATE) {
		if (!observer->mainpll || !observer->topckgen) {
			ret = -EOPNOTSUPP;
			goto disable;
		}
		ret = clk_rate_exclusive_get(observer->mainpll);
		if (ret)
			goto disable;
		pinned = true;
		transaction.mainpll_rate = clk_get_rate(observer->mainpll);
	}
	mutex_lock(&observer->lock);
	if (observer->faulted)
		ret = -EUCLEAN;
	else {
		ret = mt6797_dvfsp_cspm_transaction(observer->handoff,
						  mt6797_cpuclk_mp2_transaction,
						  &transaction);
		if (pinned && observer->faulted) {
			/* The CPU may still be parked on MAINPLL. */
			observer->mainpll_rate_locked = true;
			pinned = false;
		}
	}
	mutex_unlock(&observer->lock);
	if (pinned)
		clk_rate_exclusive_put(observer->mainpll);
disable:
	clk_disable_unprepare(observer->i2c_clk);
out:
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}

int mt6797_cpuclk_check_mp2_boot(struct device *consumer, struct clk *clk)
{
	return mt6797_cpuclk_mp2_access(consumer, clk, MT6797_MP2_CHECK_BOOT, 0, 0);
}
EXPORT_SYMBOL_GPL(mt6797_cpuclk_check_mp2_boot);

int mt6797_cpuclk_set_mp2_sync_dcm(struct device *consumer, struct clk *clk,
				 bool enable)
{
	if (!consumer || !consumer->of_node ||
	    !of_device_is_compatible(consumer->of_node, "mediatek,mt6797-a72-power"))
		return -EINVAL;
	return mt6797_cpuclk_mp2_access(consumer, clk,
			enable ? MT6797_MP2_DCM_ENABLE : MT6797_MP2_DCM_DISABLE, 0, 0);
}
EXPORT_SYMBOL_GPL(mt6797_cpuclk_set_mp2_sync_dcm);

int mt6797_cpuclk_check_mp2_restart(struct device *consumer, struct clk *clk)
{
	if (!consumer || !consumer->of_node ||
	    !of_device_is_compatible(consumer->of_node, "mediatek,mt6797-a72-power"))
		return -EINVAL;
	return mt6797_cpuclk_mp2_access(consumer, clk, MT6797_MP2_CHECK_RESTART, 0, 0);
}
EXPORT_SYMBOL_GPL(mt6797_cpuclk_check_mp2_restart);

int mt6797_cpuclk_set_mp2_rate(struct device *consumer, struct clk *clk,
			      unsigned long rate, unsigned int divisor)
{
	if (!consumer || !consumer->of_node ||
	    !of_device_is_compatible(consumer->of_node, "mediatek,mt6797-a72-power"))
		return -EINVAL;
	return mt6797_cpuclk_mp2_access(consumer, clk, MT6797_MP2_SET_RATE,
				       rate, divisor);
}
EXPORT_SYMBOL_GPL(mt6797_cpuclk_set_mp2_rate);

module_platform_driver(mt6797_cpuclk_driver);

MODULE_DESCRIPTION("MediaTek MT6797 CPU clock provider foundation");
MODULE_LICENSE("GPL");
