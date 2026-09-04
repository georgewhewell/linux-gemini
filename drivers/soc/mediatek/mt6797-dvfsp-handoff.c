// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-way handoff receiver for stopped MT6797 DVFSP firmware
 *
 * Copyright (c) 2026 Julien Etienne
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/devm-helpers.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>
#include <linux/lockdep.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "mt6797-dvfsp-cspm-access.h"

#define CSPM_PCM_CON0			0x018
#define CSPM_PCM_CON1			0x01c
#define CSPM_PCM_PWR_IO_EN		0x02c
#define CSPM_PCM_REG15_DATA		0x13c
#define CSPM_PCM_TIMER_OUT		0x150
#define CSPM_PCM_FSM_STA		0x178
#define CSPM_SW_RSV0			0x608

#define INFRACFG_INFRA1_PDN_STA		0x094
#define INFRACFG_INFRA2_PDN_STA		0x0b0

#define PCM_CON0_PCM_KICK		BIT(0)
#define PCM_CON0_IM_KICK		BIT(1)
#define PCM_CON0_PCM_SW_RESET		BIT(15)
#define PCM_CON0_ACTIVITY_MASK		(PCM_CON0_PCM_KICK | \
					 PCM_CON0_IM_KICK | \
					 PCM_CON0_PCM_SW_RESET)
#define PCM_CON1_TIMER_EN		BIT(5)
#define PCM_CON1_WDT_EN			BIT(8)
#define PCM_CON1_ACTIVITY_MASK		(PCM_CON1_TIMER_EN | PCM_CON1_WDT_EN)
#define PCM_CON1_AN_STOPPED		0x00006c00
#define PCM_FSM_STA_RESET		0x00048490
#define PCM_SW_RSV_AN_STOPPED		0xbabebabe
#define INFRA_AP_DMA_GATED		BIT(18)
#define INFRA_I2C_APPM_GATED		BIT(1)

#define MT6797_DVFSP_SAMPLE_COUNT	10
#define MT6797_DVFSP_CONSUMER_POST_COUNT	32
#define MT6797_DVFSP_POST_DELAY_MIN_US	1000
#define MT6797_DVFSP_POST_DELAY_MAX_US	1250
#define MT6797_DVFSP_SW_RSV_COUNT	7
#define MT6797_DVFSP_LATE_DELAY_MS	45000
#define MT6797_DVFSP_PROBE_TIMEOUT_MS	50000
#define MT6797_DVFSP_TRANSFER_COOKIE_XOR	0x4d54363739495452ULL

enum mt6797_dvfsp_sample_id {
	MT6797_DVFSP_PRE0,
	MT6797_DVFSP_PRE1,
	MT6797_DVFSP_PRE2,
	MT6797_DVFSP_ENABLED,
	MT6797_DVFSP_POST,
	MT6797_DVFSP_LATE,
	MT6797_DVFSP_CONSUMER_HELD,
	MT6797_DVFSP_CONSUMER_POST,
	MT6797_DVFSP_SUSPEND,
	MT6797_DVFSP_RESUME,
};

enum mt6797_dvfsp_handoff_state {
	MT6797_DVFSP_UNCLAIMED,
	MT6797_DVFSP_VALIDATING,
	MT6797_DVFSP_NORMALIZING,
	MT6797_DVFSP_PROVISIONAL,
	MT6797_DVFSP_READY,
	MT6797_DVFSP_INCONCLUSIVE,
	MT6797_DVFSP_FAULTED,
};

enum mt6797_dvfsp_pm_fault {
	MT6797_DVFSP_PM_OK,
	MT6797_DVFSP_PM_SUSPEND_INVALID,
	MT6797_DVFSP_PM_SUSPEND_REVALIDATION,
	MT6797_DVFSP_PM_RESUME_INVALID,
	MT6797_DVFSP_PM_RESUME_REVALIDATION,
	MT6797_DVFSP_PM_CONSUMER_NOT_READY,
	MT6797_DVFSP_PM_CONSUMER_CLOCK,
};

enum mt6797_dvfsp_late_state {
	MT6797_DVFSP_LATE_NOT_SCHEDULED,
	MT6797_DVFSP_LATE_PENDING,
	MT6797_DVFSP_LATE_PASSED,
	MT6797_DVFSP_LATE_FAILED,
};

enum mt6797_dvfsp_initial_gate {
	MT6797_DVFSP_GATE_UNKNOWN,
	MT6797_DVFSP_GATE_UNGATED,
	MT6797_DVFSP_GATE_GATED,
};

struct mt6797_dvfsp_snapshot {
	u32 timer_before;
	u32 pcm_con0;
	u32 pcm_con1;
	u32 pcm_pwr_io_en;
	u32 pcm_reg15_data;
	u32 pcm_fsm_sta;
	u32 sw_rsv[MT6797_DVFSP_SW_RSV_COUNT];
	u32 infra1_pdn_sta;
	u32 infra2_pdn_sta;
	u32 timer_after;
	bool infra1_pdn_sta_valid;
	bool infra2_pdn_sta_valid;
};

struct mt6797_dvfsp_handoff {
	struct device *dev;
	void __iomem *cspm;
	struct regmap *infracfg;
	struct clk *i2c_clk;
	/* Protects state, samples, non-atomic counters, and late work. */
	struct mutex lock;
	/*
	 * Sole transaction gate for the owned CSPM mapping. It is held across
	 * validation, PM revalidation, I2C6 admission, and CPU-clock callbacks.
	 */
	struct mt6797_dvfsp_cspm_access cspm_access;
	u64 transfer_generation;
	u64 transfer_cookie;
	int last_transfer_result;
	bool transfer_active;
	struct mutex fw_owner_lock;
	const struct mt6797_dvfsp_i2c6_fw_ops *fw_ops;
	void *fw_context;
	bool fw_lease_active;
	struct mt6797_dvfsp_i2c6_fw_lease fw_lease;
	struct mutex state_owner_lock;
	const struct mt6797_dvfsp_state_owner_ops *state_owner_ops;
	void *state_owner_context;
	struct mt6797_dvfsp_state_owner_identity state_owner_identity;
	bool state_hold_active;
	struct mt6797_dvfsp_state_hold state_hold;
	struct mutex pcm_adapter_lock;
	const struct mt6797_dvfsp_pcm_adapter_ops *pcm_adapter_ops;
	void *pcm_adapter_context;
	enum mt6797_dvfsp_pcm_adapter_phase pcm_adapter_phase;
	bool pcm_adapter_sticky_fault;
	struct mt6797_dvfsp_pcm_adapter_identity pcm_adapter_identity;
	struct mt6797_dvfsp_state_snapshot pcm_adapter_snapshot;
	struct completion terminal;
	struct delayed_work late_work;
	struct mt6797_dvfsp_snapshot samples[MT6797_DVFSP_SAMPLE_COUNT];
	struct mt6797_dvfsp_snapshot
		consumer_post[MT6797_DVFSP_CONSUMER_POST_COUNT];
	bool sample_valid[MT6797_DVFSP_SAMPLE_COUNT];
	unsigned int consumer_post_sequences;
	unsigned int consumer_post_samples;
	unsigned int consumer_post_pcm_failures;
	unsigned int consumer_post_main_failures;
	unsigned int consumer_post_dma_invalid;
	unsigned int consumer_post_dma_gated;
	unsigned int consumer_post_dma_unchanged;
	unsigned int consumer_post_selected;
	bool consumer_post_passed;
	atomic_t terminal_grant;
	atomic_t pm_fault;
	enum mt6797_dvfsp_handoff_state state;
	enum mt6797_dvfsp_late_state late_state;
	enum mt6797_dvfsp_initial_gate initial_gate;
	const char *reason;
	unsigned int transition_attempts;
	unsigned int enable_successes;
	unsigned int disable_count;
	unsigned int late_checks;
	unsigned int fault_count;
	atomic_t suspend_checks;
	atomic_t suspend_failures;
	atomic_t resume_checks;
	atomic_t resume_failures;
	atomic_t consumer_ungated_checks;
	atomic_t consumer_gated_checks;
	atomic_t consumer_validation_failures;
};

static const char * const mt6797_dvfsp_sample_names[] = {
	[MT6797_DVFSP_PRE0] = "pre0",
	[MT6797_DVFSP_PRE1] = "pre1",
	[MT6797_DVFSP_PRE2] = "pre2",
	[MT6797_DVFSP_ENABLED] = "enabled",
	[MT6797_DVFSP_POST] = "post",
	[MT6797_DVFSP_LATE] = "late",
	[MT6797_DVFSP_CONSUMER_HELD] = "consumer-held",
	[MT6797_DVFSP_CONSUMER_POST] = "consumer-post",
	[MT6797_DVFSP_SUSPEND] = "suspend",
	[MT6797_DVFSP_RESUME] = "resume",
};

static const char *const mt6797_dvfsp_state_names[] = {
	[MT6797_DVFSP_UNCLAIMED] = "unclaimed",
	[MT6797_DVFSP_VALIDATING] = "validating",
	[MT6797_DVFSP_NORMALIZING] = "normalizing",
	[MT6797_DVFSP_PROVISIONAL] = "provisional",
	[MT6797_DVFSP_READY] = "ready",
	[MT6797_DVFSP_INCONCLUSIVE] = "inconclusive",
	[MT6797_DVFSP_FAULTED] = "faulted",
};

static const char *const mt6797_dvfsp_late_names[] = {
	[MT6797_DVFSP_LATE_NOT_SCHEDULED] = "not-scheduled",
	[MT6797_DVFSP_LATE_PENDING] = "pending",
	[MT6797_DVFSP_LATE_PASSED] = "passed",
	[MT6797_DVFSP_LATE_FAILED] = "failed",
};

static const char *const mt6797_dvfsp_gate_names[] = {
	[MT6797_DVFSP_GATE_UNKNOWN] = "unknown",
	[MT6797_DVFSP_GATE_UNGATED] = "ungated",
	[MT6797_DVFSP_GATE_GATED] = "gated",
};

static int mt6797_dvfsp_permission(struct mt6797_dvfsp_handoff *handoff)
{
	return mt6797_dvfsp_cspm_access_read(&handoff->cspm_access);
}

static int mt6797_dvfsp_set_permission(struct mt6797_dvfsp_handoff *handoff,
				       int permission)
{
	return mt6797_dvfsp_cspm_access_set(&handoff->cspm_access, permission);
}

static int mt6797_dvfsp_change_permission(struct mt6797_dvfsp_handoff *handoff,
					  int old, int new)
{
	return mt6797_dvfsp_cspm_access_cmpxchg(&handoff->cspm_access, old,
						new);
}

static const char *mt6797_dvfsp_access_grant_name(int permission)
{
	return permission == MT6797_DVFSP_PERMISSION_READY ? "ready" : "denied";
}

static const char *mt6797_dvfsp_pm_fault_name(int fault)
{
	switch (fault) {
	case MT6797_DVFSP_PM_OK:
		return "none";
	case MT6797_DVFSP_PM_SUSPEND_INVALID:
		return "suspend-permission-invalid";
	case MT6797_DVFSP_PM_SUSPEND_REVALIDATION:
		return "suspend-revalidation-failed";
	case MT6797_DVFSP_PM_RESUME_INVALID:
		return "resume-permission-invalid";
	case MT6797_DVFSP_PM_RESUME_REVALIDATION:
		return "resume-revalidation-failed";
	case MT6797_DVFSP_PM_CONSUMER_NOT_READY:
		return "consumer-permission-invalid";
	default:
		return "consumer-clock-validation-failed";
	}
}

static void
mt6797_dvfsp_read_snapshot(struct mt6797_dvfsp_handoff *handoff,
			   struct mt6797_dvfsp_snapshot *snapshot)
{
	unsigned int value;
	int i;

	mt6797_dvfsp_cspm_access_assert_held(&handoff->cspm_access);

	*snapshot = (struct mt6797_dvfsp_snapshot) { };
	snapshot->timer_before =
		readl(handoff->cspm + CSPM_PCM_TIMER_OUT);
	snapshot->pcm_con0 = readl(handoff->cspm + CSPM_PCM_CON0);
	snapshot->pcm_con1 = readl(handoff->cspm + CSPM_PCM_CON1);
	snapshot->pcm_pwr_io_en =
		readl(handoff->cspm + CSPM_PCM_PWR_IO_EN);
	snapshot->pcm_reg15_data =
		readl(handoff->cspm + CSPM_PCM_REG15_DATA);
	snapshot->pcm_fsm_sta =
		readl(handoff->cspm + CSPM_PCM_FSM_STA);

	for (i = 0; i < MT6797_DVFSP_SW_RSV_COUNT; i++)
		snapshot->sw_rsv[i] =
			readl(handoff->cspm + CSPM_SW_RSV0 + i * sizeof(u32));

	if (!regmap_read(handoff->infracfg, INFRACFG_INFRA1_PDN_STA,
			 &value)) {
		snapshot->infra1_pdn_sta = value;
		snapshot->infra1_pdn_sta_valid = true;
	}

	if (!regmap_read(handoff->infracfg, INFRACFG_INFRA2_PDN_STA,
			 &value)) {
		snapshot->infra2_pdn_sta = value;
		snapshot->infra2_pdn_sta_valid = true;
	}

	snapshot->timer_after =
		readl(handoff->cspm + CSPM_PCM_TIMER_OUT);
}

static void
mt6797_dvfsp_log_snapshot(struct mt6797_dvfsp_handoff *handoff,
			  enum mt6797_dvfsp_sample_id id)
{
	struct mt6797_dvfsp_snapshot *snapshot = &handoff->samples[id];

	dev_info(handoff->dev,
		 "sample=%s timer=%08x/%08x con0=%08x con1=%08x "
		 "pwr_io=%08x r15=%08x fsm=%08x "
		 "rsv=%08x,%08x,%08x,%08x,%08x,%08x,%08x "
		 "gate_valid=%u gate=%08x "
		 "dma_gate_valid=%u dma_gate=%08x\n",
		 mt6797_dvfsp_sample_names[id],
		 snapshot->timer_before, snapshot->timer_after,
		 snapshot->pcm_con0, snapshot->pcm_con1,
		 snapshot->pcm_pwr_io_en, snapshot->pcm_reg15_data,
		 snapshot->pcm_fsm_sta,
		 snapshot->sw_rsv[0], snapshot->sw_rsv[1],
		 snapshot->sw_rsv[2], snapshot->sw_rsv[3],
		 snapshot->sw_rsv[4], snapshot->sw_rsv[5],
		 snapshot->sw_rsv[6],
		 snapshot->infra2_pdn_sta_valid,
		 snapshot->infra2_pdn_sta,
		 snapshot->infra1_pdn_sta_valid,
		 snapshot->infra1_pdn_sta);
}

static void
mt6797_dvfsp_take_snapshot(struct mt6797_dvfsp_handoff *handoff,
			   enum mt6797_dvfsp_sample_id id)
{
	mt6797_dvfsp_read_snapshot(handoff, &handoff->samples[id]);
	handoff->sample_valid[id] = true;
	mt6797_dvfsp_log_snapshot(handoff, id);
}

static bool
mt6797_dvfsp_snapshot_matches_an(const struct mt6797_dvfsp_snapshot *snapshot)
{
	int i;

	if (snapshot->pcm_con0 & PCM_CON0_ACTIVITY_MASK ||
	    snapshot->pcm_con1 != PCM_CON1_AN_STOPPED ||
	    snapshot->pcm_con1 & PCM_CON1_ACTIVITY_MASK ||
	    snapshot->pcm_pwr_io_en != 0 ||
	    snapshot->pcm_reg15_data != 0 ||
	    snapshot->pcm_fsm_sta != PCM_FSM_STA_RESET ||
	    snapshot->timer_before != 0 ||
	    snapshot->timer_after != 0)
		return false;

	for (i = 0; i < MT6797_DVFSP_SW_RSV_COUNT; i++)
		if (snapshot->sw_rsv[i] != PCM_SW_RSV_AN_STOPPED)
			return false;

	return true;
}

static bool
mt6797_dvfsp_pcm_equal(const struct mt6797_dvfsp_snapshot *left,
		       const struct mt6797_dvfsp_snapshot *right)
{
	int i;

	if (left->timer_before != right->timer_before ||
	    left->timer_after != right->timer_after ||
	    left->pcm_con0 != right->pcm_con0 ||
	    left->pcm_con1 != right->pcm_con1 ||
	    left->pcm_pwr_io_en != right->pcm_pwr_io_en ||
	    left->pcm_reg15_data != right->pcm_reg15_data ||
	    left->pcm_fsm_sta != right->pcm_fsm_sta)
		return false;

	for (i = 0; i < MT6797_DVFSP_SW_RSV_COUNT; i++)
		if (left->sw_rsv[i] != right->sw_rsv[i])
			return false;

	return true;
}

static bool
mt6797_dvfsp_gate_is(const struct mt6797_dvfsp_snapshot *snapshot,
		     bool gated)
{
	if (!snapshot->infra2_pdn_sta_valid)
		return false;

	return !!(snapshot->infra2_pdn_sta & INFRA_I2C_APPM_GATED) == gated;
}

static bool
mt6797_dvfsp_dma_gate_is(const struct mt6797_dvfsp_snapshot *snapshot,
			 bool gated)
{
	if (!snapshot->infra1_pdn_sta_valid)
		return false;

	return !!(snapshot->infra1_pdn_sta & INFRA_AP_DMA_GATED) == gated;
}

static bool
mt6797_dvfsp_dma_gate_matches(
	const struct mt6797_dvfsp_snapshot *baseline,
	const struct mt6797_dvfsp_snapshot *snapshot)
{
	if (!baseline->infra1_pdn_sta_valid ||
	    !snapshot->infra1_pdn_sta_valid)
		return false;

	return (baseline->infra1_pdn_sta & INFRA_AP_DMA_GATED) ==
		(snapshot->infra1_pdn_sta & INFRA_AP_DMA_GATED);
}

static bool
mt6797_dvfsp_pre_signature_stable(struct mt6797_dvfsp_handoff *handoff)
{
	const struct mt6797_dvfsp_snapshot *baseline =
		&handoff->samples[MT6797_DVFSP_PRE0];
	int i;

	for (i = MT6797_DVFSP_PRE0; i <= MT6797_DVFSP_PRE2; i++) {
		if (!mt6797_dvfsp_snapshot_matches_an(&handoff->samples[i]))
			return false;
		if (!mt6797_dvfsp_pcm_equal(baseline, &handoff->samples[i]))
			return false;
	}

	return true;
}

static enum mt6797_dvfsp_initial_gate
mt6797_dvfsp_classify_initial_gate(struct mt6797_dvfsp_handoff *handoff)
{
	bool gated;
	int i;

	for (i = MT6797_DVFSP_PRE0; i <= MT6797_DVFSP_PRE2; i++)
		if (!handoff->samples[i].infra2_pdn_sta_valid)
			return MT6797_DVFSP_GATE_UNKNOWN;

	gated = !!(handoff->samples[MT6797_DVFSP_PRE0].infra2_pdn_sta &
		   INFRA_I2C_APPM_GATED);
	for (i = MT6797_DVFSP_PRE1; i <= MT6797_DVFSP_PRE2; i++)
		if (!!(handoff->samples[i].infra2_pdn_sta &
		       INFRA_I2C_APPM_GATED) != gated)
			return MT6797_DVFSP_GATE_UNKNOWN;

	return gated ? MT6797_DVFSP_GATE_GATED :
		       MT6797_DVFSP_GATE_UNGATED;
}

static bool mt6797_dvfsp_snapshot_main_valid(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_snapshot *snapshot, bool gated)
{
	const struct mt6797_dvfsp_snapshot *baseline =
		&handoff->samples[MT6797_DVFSP_PRE0];

	return mt6797_dvfsp_snapshot_matches_an(snapshot) &&
	       mt6797_dvfsp_pcm_equal(baseline, snapshot) &&
	       mt6797_dvfsp_gate_is(snapshot, gated);
}

static bool
mt6797_dvfsp_sample_consumer_post(struct mt6797_dvfsp_handoff *handoff)
{
	const struct mt6797_dvfsp_snapshot *baseline =
		&handoff->samples[MT6797_DVFSP_PRE0];
	int first_dma_gated = -1;
	int first_dma_unchanged = -1;
	int i;

	handoff->consumer_post_sequences++;
	handoff->consumer_post_samples = 0;
	handoff->consumer_post_pcm_failures = 0;
	handoff->consumer_post_main_failures = 0;
	handoff->consumer_post_dma_invalid = 0;
	handoff->consumer_post_dma_gated = 0;
	handoff->consumer_post_dma_unchanged = 0;

	for (i = 0; i < MT6797_DVFSP_CONSUMER_POST_COUNT; i++) {
		struct mt6797_dvfsp_snapshot *snapshot =
			&handoff->consumer_post[i];

		mt6797_dvfsp_read_snapshot(handoff, snapshot);
		handoff->consumer_post_samples++;
		if (!mt6797_dvfsp_snapshot_matches_an(snapshot) ||
		    !mt6797_dvfsp_pcm_equal(baseline, snapshot))
			handoff->consumer_post_pcm_failures++;
		if (!mt6797_dvfsp_gate_is(snapshot, true))
			handoff->consumer_post_main_failures++;
		if (!snapshot->infra1_pdn_sta_valid) {
			handoff->consumer_post_dma_invalid++;
		} else if (mt6797_dvfsp_dma_gate_is(snapshot, true)) {
			if (first_dma_gated < 0)
				first_dma_gated = i;
			handoff->consumer_post_dma_gated++;
		}
		if (mt6797_dvfsp_dma_gate_matches(baseline, snapshot)) {
			if (first_dma_unchanged < 0)
				first_dma_unchanged = i;
			handoff->consumer_post_dma_unchanged++;
		}

		if (i + 1 < MT6797_DVFSP_CONSUMER_POST_COUNT)
			usleep_range(MT6797_DVFSP_POST_DELAY_MIN_US,
				     MT6797_DVFSP_POST_DELAY_MAX_US);
	}

	handoff->consumer_post_passed =
		handoff->consumer_post_samples ==
			MT6797_DVFSP_CONSUMER_POST_COUNT &&
		!handoff->consumer_post_pcm_failures &&
		!handoff->consumer_post_main_failures &&
		!handoff->consumer_post_dma_invalid &&
		handoff->consumer_post_dma_unchanged ==
			MT6797_DVFSP_CONSUMER_POST_COUNT;
	handoff->consumer_post_selected =
		handoff->consumer_post_passed ?
			(first_dma_gated >= 0 ? first_dma_gated :
						first_dma_unchanged) :
			MT6797_DVFSP_CONSUMER_POST_COUNT - 1;
	handoff->samples[MT6797_DVFSP_CONSUMER_POST] =
		handoff->consumer_post[handoff->consumer_post_selected];
	handoff->sample_valid[MT6797_DVFSP_CONSUMER_POST] = true;

	return handoff->consumer_post_passed;
}

static const char *
mt6797_dvfsp_consumer_cleanup_result(const struct mt6797_dvfsp_handoff *handoff)
{
	if (!handoff->consumer_post_sequences)
		return "not-run";

	return handoff->consumer_post_passed ? "passed" : "failed";
}

static void __mt6797_dvfsp_fault_locked(struct mt6797_dvfsp_handoff *handoff,
					const char *reason, bool report)
{
	if (handoff->state == MT6797_DVFSP_FAULTED)
		return;

	handoff->state = MT6797_DVFSP_FAULTED;
	handoff->reason = reason;
	handoff->fault_count++;
	mt6797_dvfsp_set_permission(handoff, MT6797_DVFSP_PERMISSION_FAULTED);
	complete_all(&handoff->terminal);
	if (report)
		dev_err(handoff->dev,
			"state=faulted reason=%s i2c6_policy=requires-ready\n",
			reason);
}

static void mt6797_dvfsp_fault_locked(struct mt6797_dvfsp_handoff *handoff,
				      const char *reason)
{
	__mt6797_dvfsp_fault_locked(handoff, reason, true);
}

static void mt6797_dvfsp_late_work(struct work_struct *work)
{
	struct mt6797_dvfsp_handoff *handoff =
		container_of(to_delayed_work(work),
			     struct mt6797_dvfsp_handoff, late_work);
	const struct mt6797_dvfsp_snapshot *baseline =
		&handoff->samples[MT6797_DVFSP_PRE0];
	const struct mt6797_dvfsp_snapshot *late =
		&handoff->samples[MT6797_DVFSP_LATE];
	int ret;

	mt6797_dvfsp_cspm_access_lock(&handoff->cspm_access);
	mutex_lock(&handoff->lock);
	if (handoff->state != MT6797_DVFSP_PROVISIONAL ||
	    handoff->late_state != MT6797_DVFSP_LATE_PENDING)
		goto unlock;

	handoff->late_checks++;
	mt6797_dvfsp_take_snapshot(handoff, MT6797_DVFSP_LATE);
	if (!mt6797_dvfsp_snapshot_matches_an(late) ||
	    !mt6797_dvfsp_pcm_equal(baseline, late) ||
	    !mt6797_dvfsp_gate_is(late, true)) {
		handoff->late_state = MT6797_DVFSP_LATE_FAILED;
		mt6797_dvfsp_fault_locked(handoff, "late-revalidation-failed");
		goto unlock;
	}

	ret = mt6797_dvfsp_set_permission(handoff,
					 MT6797_DVFSP_PERMISSION_READY);
	if (ret) {
		handoff->late_state = MT6797_DVFSP_LATE_FAILED;
		mt6797_dvfsp_fault_locked(handoff,
					  "cspm-access-permission-faulted");
		goto unlock;
	}

	handoff->state = MT6797_DVFSP_READY;
	handoff->late_state = MT6797_DVFSP_LATE_PASSED;
	handoff->reason = "late-validation-passed";
	atomic_set(&handoff->terminal_grant, 1);
	complete_all(&handoff->terminal);
	dev_info(handoff->dev,
		 "state=ready normalization=ungated-to-gated "
		 "late_validation=passed i2c6_policy=requires-ready\n");

unlock:
	mutex_unlock(&handoff->lock);
	mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
}

static void mt6797_dvfsp_run_handoff(struct mt6797_dvfsp_handoff *handoff)
{
	const struct mt6797_dvfsp_snapshot *baseline =
		&handoff->samples[MT6797_DVFSP_PRE0];
	const struct mt6797_dvfsp_snapshot *enabled =
		&handoff->samples[MT6797_DVFSP_ENABLED];
	const struct mt6797_dvfsp_snapshot *post =
		&handoff->samples[MT6797_DVFSP_POST];
	const char *failure = NULL;
	int ret;

	mt6797_dvfsp_cspm_access_lock(&handoff->cspm_access);
	mutex_lock(&handoff->lock);
	handoff->state = MT6797_DVFSP_VALIDATING;
	handoff->reason = "initial-validation";
	dev_info(handoff->dev,
		 "state=validating operation=one-way-handoff "
		 "i2c6_policy=requires-ready\n");

	mt6797_dvfsp_take_snapshot(handoff, MT6797_DVFSP_PRE0);
	usleep_range(2000, 2500);
	mt6797_dvfsp_take_snapshot(handoff, MT6797_DVFSP_PRE1);
	usleep_range(18000, 19000);
	mt6797_dvfsp_take_snapshot(handoff, MT6797_DVFSP_PRE2);

	if (!mt6797_dvfsp_pre_signature_stable(handoff)) {
		mt6797_dvfsp_fault_locked(handoff, "initial-signature-mismatch");
		goto unlock;
	}

	handoff->initial_gate = mt6797_dvfsp_classify_initial_gate(handoff);
	if (handoff->initial_gate == MT6797_DVFSP_GATE_UNKNOWN) {
		mt6797_dvfsp_fault_locked(handoff, "initial-gate-invalid-or-changed");
		goto unlock;
	}

	if (handoff->initial_gate == MT6797_DVFSP_GATE_GATED) {
		handoff->state = MT6797_DVFSP_INCONCLUSIVE;
		handoff->reason = "initial-gate-already-gated";
		complete_all(&handoff->terminal);
		dev_warn(handoff->dev,
			 "state=inconclusive "
			 "reason=initial-gate-already-gated "
			 "transition_attempts=0 i2c6_policy=requires-ready\n");
		goto unlock;
	}

	handoff->state = MT6797_DVFSP_NORMALIZING;
	handoff->reason = "ccf-balance-in-progress";
	handoff->transition_attempts++;
	dev_info(handoff->dev,
		 "state=normalizing transition=ccf-temporary-reference attempt=1\n");

	ret = clk_prepare_enable(handoff->i2c_clk);
	if (ret) {
		mt6797_dvfsp_fault_locked(handoff, "ccf-enable-failed");
		goto unlock;
	}
	handoff->enable_successes++;

	mt6797_dvfsp_take_snapshot(handoff, MT6797_DVFSP_ENABLED);
	if (!mt6797_dvfsp_snapshot_matches_an(enabled) ||
	    !mt6797_dvfsp_pcm_equal(baseline, enabled))
		failure = "pcm-changed-while-clock-held";
	else if (!mt6797_dvfsp_gate_is(enabled, false))
		failure = "clock-not-ungated-while-held";

	clk_disable_unprepare(handoff->i2c_clk);
	handoff->disable_count++;
	mt6797_dvfsp_take_snapshot(handoff, MT6797_DVFSP_POST);

	if (!failure &&
	    (!mt6797_dvfsp_snapshot_matches_an(post) ||
	     !mt6797_dvfsp_pcm_equal(baseline, post)))
		failure = "pcm-changed-after-clock-balance";
	if (!failure && !mt6797_dvfsp_gate_is(post, true))
		failure = "clock-remained-ungated-after-balance";

	if (failure) {
		mt6797_dvfsp_fault_locked(handoff, failure);
		goto unlock;
	}

	handoff->state = MT6797_DVFSP_PROVISIONAL;
	handoff->late_state = MT6797_DVFSP_LATE_PENDING;
	handoff->reason = "normalization-complete-late-pending";
	if (!schedule_delayed_work(&handoff->late_work,
		    msecs_to_jiffies(MT6797_DVFSP_LATE_DELAY_MS))) {
		handoff->late_state = MT6797_DVFSP_LATE_NOT_SCHEDULED;
		mt6797_dvfsp_fault_locked(handoff,
					  "late-validation-not-scheduled");
		goto unlock;
	}

	dev_info(handoff->dev,
		 "state=provisional normalization=ungated-to-gated "
		 "enable_successes=1 disable_count=1 "
		 "late_validation=pending delay_ms=%u "
		 "i2c6_policy=requires-ready\n",
		 MT6797_DVFSP_LATE_DELAY_MS);

unlock:
	mutex_unlock(&handoff->lock);
	mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
}

static int mt6797_dvfsp_handoff_suspend_late(struct device *dev)
{
	struct mt6797_dvfsp_handoff *handoff = dev_get_drvdata(dev);
	struct mt6797_dvfsp_snapshot *snapshot =
		&handoff->samples[MT6797_DVFSP_SUSPEND];
	bool valid;

	atomic_inc(&handoff->suspend_checks);
	if (!atomic_read(&handoff->terminal_grant))
		return 0;

	mt6797_dvfsp_cspm_access_lock(&handoff->cspm_access);
	if (mt6797_dvfsp_change_permission(handoff,
					   MT6797_DVFSP_PERMISSION_READY,
					   MT6797_DVFSP_PERMISSION_BLOCKED) !=
	    MT6797_DVFSP_PERMISSION_READY) {
		atomic_inc(&handoff->suspend_failures);
		atomic_set(&handoff->pm_fault, MT6797_DVFSP_PM_SUSPEND_INVALID);
		mt6797_dvfsp_set_permission(handoff,
					    MT6797_DVFSP_PERMISSION_FAULTED);
		mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
		return -EHOSTDOWN;
	}

	mutex_lock(&handoff->lock);
	mt6797_dvfsp_read_snapshot(handoff, snapshot);
	handoff->sample_valid[MT6797_DVFSP_SUSPEND] = true;
	valid = mt6797_dvfsp_snapshot_main_valid(handoff, snapshot, true);
	mutex_unlock(&handoff->lock);
	if (!valid) {
		atomic_inc(&handoff->suspend_failures);
		atomic_set(&handoff->pm_fault,
			   MT6797_DVFSP_PM_SUSPEND_REVALIDATION);
		mt6797_dvfsp_set_permission(handoff,
					    MT6797_DVFSP_PERMISSION_FAULTED);
		mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
		return -EIO;
	}

	mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
	return 0;
}

static int mt6797_dvfsp_handoff_resume_early(struct device *dev)
{
	struct mt6797_dvfsp_handoff *handoff = dev_get_drvdata(dev);
	struct mt6797_dvfsp_snapshot *snapshot =
		&handoff->samples[MT6797_DVFSP_RESUME];
	bool valid;

	atomic_inc(&handoff->resume_checks);
	if (!atomic_read(&handoff->terminal_grant))
		return 0;

	mt6797_dvfsp_cspm_access_lock(&handoff->cspm_access);
	if (mt6797_dvfsp_permission(handoff) !=
	    MT6797_DVFSP_PERMISSION_BLOCKED) {
		atomic_inc(&handoff->resume_failures);
		atomic_set(&handoff->pm_fault, MT6797_DVFSP_PM_RESUME_INVALID);
		mt6797_dvfsp_set_permission(handoff,
					    MT6797_DVFSP_PERMISSION_FAULTED);
		mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
		return -EHOSTDOWN;
	}

	mutex_lock(&handoff->lock);
	mt6797_dvfsp_read_snapshot(handoff, snapshot);
	handoff->sample_valid[MT6797_DVFSP_RESUME] = true;
	valid = mt6797_dvfsp_snapshot_main_valid(handoff, snapshot, true);
	mutex_unlock(&handoff->lock);
	if (!valid) {
		atomic_inc(&handoff->resume_failures);
		atomic_set(&handoff->pm_fault,
			   MT6797_DVFSP_PM_RESUME_REVALIDATION);
		mt6797_dvfsp_set_permission(handoff,
					    MT6797_DVFSP_PERMISSION_FAULTED);
		mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
		return -EIO;
	}

	if (mt6797_dvfsp_change_permission(handoff,
					   MT6797_DVFSP_PERMISSION_BLOCKED,
					   MT6797_DVFSP_PERMISSION_READY) !=
	    MT6797_DVFSP_PERMISSION_BLOCKED) {
		atomic_inc(&handoff->resume_failures);
		atomic_set(&handoff->pm_fault, MT6797_DVFSP_PM_RESUME_INVALID);
		mt6797_dvfsp_set_permission(handoff,
					    MT6797_DVFSP_PERMISSION_FAULTED);
		mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
		return -EHOSTDOWN;
	}

	mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
	return 0;
}

static const struct dev_pm_ops mt6797_dvfsp_handoff_pm_ops = {
	LATE_SYSTEM_SLEEP_PM_OPS(mt6797_dvfsp_handoff_suspend_late,
				 mt6797_dvfsp_handoff_resume_early)
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct mt6797_dvfsp_handoff *handoff = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&handoff->lock);
	length = sysfs_emit(buf, "%s\n",
			    mt6797_dvfsp_state_names[handoff->state]);
	mutex_unlock(&handoff->lock);

	return length;
}
static DEVICE_ATTR_RO(state);

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct mt6797_dvfsp_handoff *handoff = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&handoff->lock);
	length = sysfs_emit(buf,
			    "state=%s reason=%s initial_gate=%s "
			    "supplier_bound=yes access_grant=%s "
			    "transition_attempts=%u enable_successes=%u "
			    "disable_count=%u late=%s late_checks=%u faults=%u "
			    "suspend_checks=%d suspend_failures=%d "
			    "resume_checks=%d resume_failures=%d "
			    "pm_fault=%s consumer_ungated_checks=%d "
			    "consumer_gated_checks=%d "
			    "consumer_validation_failures=%d "
			    "cleanup_attempts=%u cleanup_samples=%u "
			    "cleanup_pcm_failures=%u "
			    "cleanup_main_failures=%u "
			    "cleanup_dma_invalid=%u cleanup_dma_gated=%u "
			    "cleanup_selected=%u cleanup_result=%s "
			    "i2c6_policy=requires-ready\n",
			    mt6797_dvfsp_state_names[handoff->state],
			    handoff->reason,
			    mt6797_dvfsp_gate_names[handoff->initial_gate],
			    mt6797_dvfsp_access_grant_name(
				    mt6797_dvfsp_permission(handoff)),
			    handoff->transition_attempts,
			    handoff->enable_successes, handoff->disable_count,
			    mt6797_dvfsp_late_names[handoff->late_state],
			    handoff->late_checks, handoff->fault_count,
			    atomic_read(&handoff->suspend_checks),
			    atomic_read(&handoff->suspend_failures),
			    atomic_read(&handoff->resume_checks),
			    atomic_read(&handoff->resume_failures),
			    mt6797_dvfsp_pm_fault_name(
				    atomic_read(&handoff->pm_fault)),
			    atomic_read(&handoff->consumer_ungated_checks),
			    atomic_read(&handoff->consumer_gated_checks),
			    atomic_read(
				    &handoff->consumer_validation_failures),
			    handoff->consumer_post_sequences,
			    handoff->consumer_post_samples,
			    handoff->consumer_post_pcm_failures,
			    handoff->consumer_post_main_failures,
			    handoff->consumer_post_dma_invalid,
			    handoff->consumer_post_dma_gated,
			    handoff->consumer_post_selected,
			    mt6797_dvfsp_consumer_cleanup_result(handoff));
	mutex_unlock(&handoff->lock);

	return length;
}
static DEVICE_ATTR_RO(status);

static ssize_t snapshots_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct mt6797_dvfsp_handoff *handoff = dev_get_drvdata(dev);
	ssize_t length = 0;
	int i;

	mutex_lock(&handoff->lock);
	for (i = 0; i < MT6797_DVFSP_SAMPLE_COUNT; i++) {
		const struct mt6797_dvfsp_snapshot *snapshot =
			&handoff->samples[i];

		if (!handoff->sample_valid[i])
			continue;

		length += sysfs_emit_at(buf, length,
			"sample=%s timer=%08x/%08x "
			"con0=%08x con1=%08x pwr_io=%08x "
			"r15=%08x fsm=%08x "
			"rsv=%08x,%08x,%08x,%08x,%08x,%08x,%08x "
			"gate_valid=%u gate=%08x "
			"dma_gate_valid=%u dma_gate=%08x\n",
			mt6797_dvfsp_sample_names[i],
			snapshot->timer_before, snapshot->timer_after,
			snapshot->pcm_con0, snapshot->pcm_con1,
			snapshot->pcm_pwr_io_en, snapshot->pcm_reg15_data,
			snapshot->pcm_fsm_sta,
			snapshot->sw_rsv[0], snapshot->sw_rsv[1],
			snapshot->sw_rsv[2], snapshot->sw_rsv[3],
			snapshot->sw_rsv[4], snapshot->sw_rsv[5],
			snapshot->sw_rsv[6],
			snapshot->infra2_pdn_sta_valid,
			snapshot->infra2_pdn_sta,
			snapshot->infra1_pdn_sta_valid,
			snapshot->infra1_pdn_sta);
	}
	mutex_unlock(&handoff->lock);

	return length;
}
static DEVICE_ATTR_RO(snapshots);

static ssize_t consumer_cleanup_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct mt6797_dvfsp_handoff *handoff = dev_get_drvdata(dev);
	ssize_t length;
	unsigned int i;

	mutex_lock(&handoff->lock);
	length = sysfs_emit(
		buf,
		"attempts=%u samples=%u pcm_failures=%u main_failures=%u "
		"dma_invalid=%u dma_gated=%u dma_unchanged=%u selected=%u result=%s\n",
		handoff->consumer_post_sequences,
		handoff->consumer_post_samples,
		handoff->consumer_post_pcm_failures,
		handoff->consumer_post_main_failures,
		handoff->consumer_post_dma_invalid,
		handoff->consumer_post_dma_gated,
		handoff->consumer_post_dma_unchanged,
		handoff->consumer_post_selected,
		mt6797_dvfsp_consumer_cleanup_result(handoff));

	for (i = 0; i < handoff->consumer_post_samples; i++) {
		const struct mt6797_dvfsp_snapshot *snapshot =
			&handoff->consumer_post[i];

		length += sysfs_emit_at(
			buf, length,
			"i=%02u main_valid=%d main=%08x "
			"dma_valid=%d dma=%08x\n",
			i, snapshot->infra2_pdn_sta_valid,
			snapshot->infra2_pdn_sta,
			snapshot->infra1_pdn_sta_valid,
			snapshot->infra1_pdn_sta);
	}
	mutex_unlock(&handoff->lock);

	return length;
}
static DEVICE_ATTR_RO(consumer_cleanup);

static struct attribute *mt6797_dvfsp_handoff_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_status.attr,
	&dev_attr_snapshots.attr,
	&dev_attr_consumer_cleanup.attr,
	NULL,
};

static const struct attribute_group mt6797_dvfsp_handoff_group = {
	.attrs = mt6797_dvfsp_handoff_attrs,
};

int
mt6797_dvfsp_handoff_begin_i2c6_transfer(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_i2c6_lease *lease)
{
	int ret = 0;

	if (!handoff || !lease)
		return -EINVAL;

	mt6797_dvfsp_cspm_access_lock(&handoff->cspm_access);
	mutex_lock(&handoff->lock);
	if (handoff->state != MT6797_DVFSP_READY ||
	    mt6797_dvfsp_permission(handoff) !=
		    MT6797_DVFSP_PERMISSION_READY) {
		ret = -EHOSTDOWN;
		goto out_unlock;
	}
	if (handoff->transfer_active) {
		ret = -EBUSY;
		goto out_unlock;
	}

	handoff->transfer_generation++;
	if (!handoff->transfer_generation) {
		mt6797_dvfsp_fault_locked(handoff,
					  "i2c6-transfer-generation-exhausted");
		ret = -EHOSTDOWN;
		goto out_unlock;
	}
	handoff->transfer_cookie = handoff->transfer_generation ^
		MT6797_DVFSP_TRANSFER_COOKIE_XOR;
	handoff->transfer_active = true;
	lease->generation = handoff->transfer_generation;
	lease->cookie = handoff->transfer_cookie;

out_unlock:
	mutex_unlock(&handoff->lock);
	if (ret)
		mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_begin_i2c6_transfer);

int
mt6797_dvfsp_handoff_end_i2c6_transfer(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_lease *lease,
	int transfer_result)
{
	int ret = 0;

	if (!handoff || !lease)
		return -EINVAL;

	mutex_lock(&handoff->lock);
	if (!handoff->transfer_active ||
	    lease->generation != handoff->transfer_generation ||
	    lease->cookie != handoff->transfer_cookie) {
		mt6797_dvfsp_fault_locked(handoff,
					  "i2c6-transfer-lease-mismatch");
		ret = -EPERM;
	} else {
		handoff->last_transfer_result = transfer_result;
		handoff->transfer_active = false;
		handoff->transfer_cookie = 0;
	}
	mutex_unlock(&handoff->lock);
	mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_end_i2c6_transfer);

static void
mt6797_dvfsp_handoff_i2c6_fw_fault(struct mt6797_dvfsp_handoff *handoff,
					  const char *reason)
{
	mutex_lock(&handoff->lock);
	mt6797_dvfsp_fault_locked(handoff, reason);
	mutex_unlock(&handoff->lock);
}

static void
mt6797_dvfsp_i2c6_fw_request_init(
	struct mt6797_dvfsp_i2c6_fw_request *request,
	const struct mt6797_dvfsp_i2c6_lease *lease,
	const struct mt6797_dvfsp_i2c6_fw_lease *fw_lease)
{
	memset(request, 0, sizeof(*request));
	request->abi = MT6797_DVFSP_I2C6_FW_ABI;
	request->user = MT6797_DVFSP_I2C6_FW_USER;
	request->pause_source = MT6797_DVFSP_I2C6_FW_PAUSE_SOURCE;
	request->sw_pause_bit = MT6797_DVFSP_I2C6_FW_SW_PAUSE_BIT;
	request->fw_done_bit = MT6797_DVFSP_I2C6_FW_DONE_BIT;
	request->timeout_us = MT6797_DVFSP_I2C6_FW_TIMEOUT_US;
	request->generation = lease->generation;
	request->cookie = lease->cookie;
	if (fw_lease)
		request->owner_handle = fw_lease->owner_handle;
}

static void
mt6797_dvfsp_i2c6_fw_refusal(
	const struct mt6797_dvfsp_i2c6_fw_request *request,
	struct mt6797_dvfsp_i2c6_fw_response *response)
{
	memset(response, 0, sizeof(*response));
	response->abi = request->abi;
	response->user = request->user;
	response->status = -EOPNOTSUPP;
	response->returned = 1;
	response->generation = request->generation;
	response->cookie = request->cookie;
}

static bool
mt6797_dvfsp_i2c6_fw_refusal_valid(
	const struct mt6797_dvfsp_i2c6_fw_request *request,
	const struct mt6797_dvfsp_i2c6_fw_response *response)
{
	return response->abi == request->abi &&
		response->user == request->user &&
		response->status == -EOPNOTSUPP &&
		response->returned == 1 &&
		response->generation == request->generation &&
		response->cookie == request->cookie &&
		!response->pause_source_map && !response->sw_pause_words &&
		!response->fw_done_words && !response->owner_handle &&
		!response->reserved;
}

static int
mt6797_dvfsp_i2c6_fw_response_validate(
	const struct mt6797_dvfsp_i2c6_fw_request *request,
	const struct mt6797_dvfsp_i2c6_fw_response *response,
	bool release)
{
	if (response->abi != request->abi || response->user != request->user ||
	    response->status || response->returned != 1 ||
	    response->generation != request->generation ||
	    response->cookie != request->cookie || response->reserved)
		return -EPROTO;

	if (!release &&
	    (response->pause_source_map != MT6797_DVFSP_I2C6_FW_PAUSE_SOURCE ||
	     response->sw_pause_words != MT6797_DVFSP_I2C6_FW_WORDS ||
	     response->fw_done_words != MT6797_DVFSP_I2C6_FW_WORDS ||
     !response->owner_handle))
		return -EPROTO;

	if (release &&
	    (response->owner_handle != request->owner_handle ||
	     response->pause_source_map || response->sw_pause_words))
		return -EPROTO;

	return 0;
}

int mt6797_dvfsp_handoff_i2c6_fw_register(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_fw_ops *ops, void *context)
{
	int ret = 0;

	if (!handoff || !ops || !ops->acquire || !ops->release)
		return -EINVAL;

	mutex_lock(&handoff->fw_owner_lock);
	if (handoff->fw_ops) {
		ret = -EBUSY;
	} else {
		handoff->fw_ops = ops;
		handoff->fw_context = context;
	}
	mutex_unlock(&handoff->fw_owner_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_i2c6_fw_register);

int mt6797_dvfsp_handoff_i2c6_fw_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_fw_ops *ops, void *context)
{
	int ret = 0;

	if (!handoff || !ops)
		return -EINVAL;

	mutex_lock(&handoff->fw_owner_lock);
	if (handoff->fw_lease_active) {
		ret = -EBUSY;
	} else if (handoff->fw_ops != ops || handoff->fw_context != context) {
		ret = -ENOENT;
	} else {
		handoff->fw_ops = NULL;
		handoff->fw_context = NULL;
	}
	mutex_unlock(&handoff->fw_owner_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_i2c6_fw_unregister);

int mt6797_dvfsp_handoff_i2c6_fw_acquire(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_lease *lease,
	struct mt6797_dvfsp_i2c6_fw_lease *fw_lease,
	struct mt6797_dvfsp_i2c6_fw_response *response)
{
	const struct mt6797_dvfsp_i2c6_fw_ops *ops;
	struct mt6797_dvfsp_i2c6_fw_request request;
	void *context;
	const char *fault_reason = NULL;
	int ret;

	if (!handoff || !lease || !fw_lease || !response)
		return -EINVAL;

	mt6797_dvfsp_cspm_access_assert_held(&handoff->cspm_access);
	memset(fw_lease, 0, sizeof(*fw_lease));

	mutex_lock(&handoff->fw_owner_lock);
	mutex_lock(&handoff->lock);
	if (handoff->state != MT6797_DVFSP_READY ||
	    !handoff->transfer_active ||
	    lease->generation != handoff->transfer_generation ||
	    lease->cookie != handoff->transfer_cookie) {
		mutex_unlock(&handoff->lock);
		mutex_unlock(&handoff->fw_owner_lock);
		return -EHOSTDOWN;
	}
	if (handoff->fw_lease_active) {
		mutex_unlock(&handoff->lock);
		mutex_unlock(&handoff->fw_owner_lock);
		return -EBUSY;
	}

	mt6797_dvfsp_i2c6_fw_request_init(&request, lease, NULL);
	ops = handoff->fw_ops;
	context = handoff->fw_context;
	if (!ops) {
		mt6797_dvfsp_i2c6_fw_refusal(&request, response);
		mutex_unlock(&handoff->lock);
		mutex_unlock(&handoff->fw_owner_lock);
		return -EOPNOTSUPP;
	}
	mutex_unlock(&handoff->lock);

	memset(response, 0, sizeof(*response));
	ret = ops->acquire(context, &request, response);
	if (ret == -EOPNOTSUPP) {
		if (!mt6797_dvfsp_i2c6_fw_refusal_valid(&request, response)) {
			ret = -EPROTO;
			fault_reason = "i2c6-fw-acquire-refusal-invalid";
		}
		goto out_callback;
	}
	if (ret) {
		fault_reason = "i2c6-fw-acquire-failed";
		goto out_callback;
	}

	mutex_lock(&handoff->lock);
	if (handoff->state != MT6797_DVFSP_READY ||
	    !handoff->transfer_active) {
		ret = -EHOSTDOWN;
		fault_reason = "i2c6-fw-acquire-state-changed";
	} else {
		ret = mt6797_dvfsp_i2c6_fw_response_validate(
			&request, response, false);
		if (ret) {
			fault_reason = "i2c6-fw-acquire-response-invalid";
		} else {
			fw_lease->generation = lease->generation;
			fw_lease->cookie = lease->cookie;
			fw_lease->owner_handle = response->owner_handle;
			handoff->fw_lease = *fw_lease;
			handoff->fw_lease_active = true;
		}
	}
	mutex_unlock(&handoff->lock);

out_callback:
	mutex_unlock(&handoff->fw_owner_lock);
	if (fault_reason)
		mt6797_dvfsp_handoff_i2c6_fw_fault(handoff, fault_reason);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_i2c6_fw_acquire);

int mt6797_dvfsp_handoff_i2c6_fw_release(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_lease *lease,
	const struct mt6797_dvfsp_i2c6_fw_lease *fw_lease,
	struct mt6797_dvfsp_i2c6_fw_response *response)
{
	const struct mt6797_dvfsp_i2c6_fw_ops *ops;
	struct mt6797_dvfsp_i2c6_fw_request request;
	void *context;
	const char *fault_reason = NULL;
	int ret;

	if (!handoff || !lease || !fw_lease || !response)
		return -EINVAL;

	mt6797_dvfsp_cspm_access_assert_held(&handoff->cspm_access);

	mutex_lock(&handoff->fw_owner_lock);
	mutex_lock(&handoff->lock);
	if (!handoff->fw_lease_active ||
	    memcmp(&handoff->fw_lease, fw_lease, sizeof(*fw_lease)) ||
	    fw_lease->generation != lease->generation ||
	    fw_lease->cookie != lease->cookie ||
	    lease->generation != handoff->transfer_generation ||
	    lease->cookie != handoff->transfer_cookie) {
		mutex_unlock(&handoff->lock);
		mutex_unlock(&handoff->fw_owner_lock);
		return -EPERM;
	}

	mt6797_dvfsp_i2c6_fw_request_init(&request, lease, fw_lease);
	ops = handoff->fw_ops;
	context = handoff->fw_context;
	if (!ops) {
		mutex_unlock(&handoff->lock);
		mutex_unlock(&handoff->fw_owner_lock);
		mt6797_dvfsp_handoff_i2c6_fw_fault(
			handoff, "i2c6-fw-release-owner-missing");
		return -EHOSTDOWN;
	}
	mutex_unlock(&handoff->lock);

	memset(response, 0, sizeof(*response));
	ret = ops->release(context, &request, response);
	if (ret == -EOPNOTSUPP) {
		fault_reason = "i2c6-fw-release-refused";
		goto release_out_callback;
	}
	if (ret) {
		fault_reason = "i2c6-fw-release-failed";
		goto release_out_callback;
	}

	mutex_lock(&handoff->lock);
	ret = mt6797_dvfsp_i2c6_fw_response_validate(&request, response, true);
	if (ret) {
		fault_reason = "i2c6-fw-release-response-invalid";
	} else {
		handoff->fw_lease_active = false;
		memset(&handoff->fw_lease, 0, sizeof(handoff->fw_lease));
	}
	mutex_unlock(&handoff->lock);

release_out_callback:
	mutex_unlock(&handoff->fw_owner_lock);
	if (fault_reason)
		mt6797_dvfsp_handoff_i2c6_fw_fault(handoff, fault_reason);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_i2c6_fw_release);

int mt6797_dvfsp_handoff_require_ready(struct mt6797_dvfsp_handoff *handoff)
{
	int ret = 0;

	if (!handoff)
		return -EINVAL;

	mutex_lock(&handoff->lock);
	if (handoff->state != MT6797_DVFSP_READY ||
	    mt6797_dvfsp_permission(handoff) != MT6797_DVFSP_PERMISSION_READY)
		ret = -EHOSTDOWN;
	mutex_unlock(&handoff->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_require_ready);

int mt6797_dvfsp_cspm_transaction(struct mt6797_dvfsp_handoff *handoff,
				  mt6797_dvfsp_cspm_transaction_fn operation,
				  void *context)
{
	if (!handoff || !operation)
		return -EINVAL;

	return mt6797_dvfsp_cspm_access_transaction(&handoff->cspm_access,
						handoff->cspm, operation, context);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_cspm_transaction);

bool mt6797_dvfsp_handoff_is_ready_atomic(struct mt6797_dvfsp_handoff *handoff)
{
	return handoff &&
	       mt6797_dvfsp_permission(handoff) ==
		       MT6797_DVFSP_PERMISSION_READY;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_is_ready_atomic);

int
mt6797_dvfsp_handoff_validate_clock(
	struct mt6797_dvfsp_handoff *handoff, bool gated)
{
	struct mt6797_dvfsp_snapshot *snapshot;
	int ret = 0;

	if (!handoff)
		return -EINVAL;

	snapshot = &handoff->samples[MT6797_DVFSP_CONSUMER_HELD];
	mt6797_dvfsp_cspm_access_lock(&handoff->cspm_access);
	mutex_lock(&handoff->lock);
	/*
	 * An ungated observation authorizes initialization and therefore
	 * requires a live grant. A gated observation is the mandatory cleanup
	 * proof and remains available after the first observation has faulted
	 * the grant.
	 */
	if (!gated &&
	    (handoff->state != MT6797_DVFSP_READY ||
	     mt6797_dvfsp_permission(handoff) !=
		     MT6797_DVFSP_PERMISSION_READY)) {
		ret = -EHOSTDOWN;
		goto unlock;
	}

	if (gated)
		atomic_inc(&handoff->consumer_gated_checks);
	else
		atomic_inc(&handoff->consumer_ungated_checks);

	if (gated) {
		bool cleanup_passed =
			mt6797_dvfsp_sample_consumer_post(handoff);

		mt6797_dvfsp_log_snapshot(
			handoff, MT6797_DVFSP_CONSUMER_POST);
		if (!cleanup_passed) {
			atomic_inc(&handoff->consumer_validation_failures);
			dev_err(handoff->dev,
				"consumer_clock_check=cleanup "
				"clocks=i2c-appm shared-ap-dma=preserved "
				"validation=failed "
				"samples=%u pcm_failures=%u "
				"main_failures=%u dma_invalid=%u "
				"dma_gated=%u dma_unchanged=%u selected=%u "
				"i2c6_policy=requires-ready\n",
				handoff->consumer_post_samples,
				handoff->consumer_post_pcm_failures,
				handoff->consumer_post_main_failures,
				handoff->consumer_post_dma_invalid,
				handoff->consumer_post_dma_gated,
				handoff->consumer_post_dma_unchanged,
				handoff->consumer_post_selected);
			mt6797_dvfsp_fault_locked(
				handoff, "consumer-cleanup-validation-failed");
			ret = -EIO;
			goto unlock;
		}
	} else {
		mt6797_dvfsp_take_snapshot(
			handoff, MT6797_DVFSP_CONSUMER_HELD);
		if (!mt6797_dvfsp_snapshot_main_valid(
			    handoff, snapshot, false) ||
		    !mt6797_dvfsp_dma_gate_is(snapshot, false)) {
			atomic_inc(&handoff->consumer_validation_failures);
			mt6797_dvfsp_fault_locked(
				handoff, "consumer-held-clocks-not-ungated");
			ret = -EIO;
			goto unlock;
		}
	}

	if (gated)
		dev_info(handoff->dev,
			 "consumer_clock_check=cleanup "
			 "clocks=i2c-appm shared-ap-dma=preserved "
			 "validation=passed samples=%u dma_gated=%u "
			 "dma_unchanged=%u selected=%u "
			 "i2c6_policy=requires-ready\n",
			 handoff->consumer_post_samples,
			 handoff->consumer_post_dma_gated,
			 handoff->consumer_post_dma_unchanged,
			 handoff->consumer_post_selected);
	else
		dev_info(handoff->dev,
			 "consumer_clock_check=held "
			 "clocks=i2c-appm,ap-dma validation=passed "
			 "i2c6_policy=requires-ready\n");

unlock:
	mutex_unlock(&handoff->lock);
	mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_validate_clock);

int mt6797_dvfsp_handoff_validate_clock_pm(struct mt6797_dvfsp_handoff *handoff,
					   bool gated)
{
	struct mt6797_dvfsp_snapshot *snapshot;
	int ret = 0;

	if (!handoff)
		return -EINVAL;

	mt6797_dvfsp_cspm_access_lock(&handoff->cspm_access);
	mutex_lock(&handoff->lock);
	if (!gated && !mt6797_dvfsp_handoff_is_ready_atomic(handoff)) {
		atomic_set(&handoff->pm_fault,
			   MT6797_DVFSP_PM_CONSUMER_NOT_READY);
		mt6797_dvfsp_set_permission(handoff,
					    MT6797_DVFSP_PERMISSION_FAULTED);
		ret = -EHOSTDOWN;
		goto unlock_pm;
	}

	if (gated)
		atomic_inc(&handoff->consumer_gated_checks);
	else
		atomic_inc(&handoff->consumer_ungated_checks);

	if (gated) {
		if (mt6797_dvfsp_sample_consumer_post(handoff))
			goto unlock_pm;
	} else {
		snapshot = &handoff->samples[MT6797_DVFSP_CONSUMER_HELD];
		mt6797_dvfsp_read_snapshot(handoff, snapshot);
		handoff->sample_valid[MT6797_DVFSP_CONSUMER_HELD] = true;
		if (mt6797_dvfsp_snapshot_main_valid(
			    handoff, snapshot, false) &&
		    mt6797_dvfsp_dma_gate_is(snapshot, false))
			goto unlock_pm;
	}

	atomic_inc(&handoff->consumer_validation_failures);
	atomic_set(&handoff->pm_fault, MT6797_DVFSP_PM_CONSUMER_CLOCK);
	__mt6797_dvfsp_fault_locked(
		handoff, gated ? "consumer-cleanup-validation-failed" :
				 "consumer-held-clocks-not-ungated",
		false);
	ret = -EIO;

unlock_pm:
	mutex_unlock(&handoff->lock);
	mt6797_dvfsp_cspm_access_unlock(&handoff->cspm_access);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_validate_clock_pm);

struct mt6797_dvfsp_handoff *
mt6797_dvfsp_handoff_get(struct device *consumer)
{
	struct mt6797_dvfsp_handoff *handoff;
	struct of_phandle_args args;
	struct platform_device *supplier;
	struct device_link *link;
	int count;
	int ret;

	count = of_count_phandle_with_args(consumer->of_node,
					   "access-controllers",
					   "#access-controller-cells");
	if (count != 1)
		return ERR_PTR(-EINVAL);

	ret = of_parse_phandle_with_args(consumer->of_node,
					 "access-controllers",
					 "#access-controller-cells", 0, &args);
	if (ret)
		return ERR_PTR(ret);

	if (args.args_count ||
	    !of_device_is_compatible(args.np,
				     "mediatek,mt6797-dvfsp-handoff")) {
		of_node_put(args.np);
		return ERR_PTR(-EINVAL);
	}

	supplier = of_find_device_by_node(args.np);
	of_node_put(args.np);
	if (!supplier)
		return ERR_PTR(-EPROBE_DEFER);

	device_lock(&supplier->dev);
	if (!device_is_bound(&supplier->dev)) {
		ret = -EPROBE_DEFER;
		handoff = NULL;
	} else {
		handoff = platform_get_drvdata(supplier);
		ret = handoff ? 0 : -EPROBE_DEFER;
	}
	device_unlock(&supplier->dev);

	if (ret)
		goto put_supplier;

	/*
	 * The access-controllers property lets fw_devlink prevent the first
	 * probe before the supplier binds. Convert that inferred relationship
	 * into an explicit link so PM ordering does not depend on the fw_devlink
	 * command-line policy after this point. Deduplicating an fw_devlink=rpm
	 * link can retain DL_FLAG_AUTOPROBE_CONSUMER after INFERRED is cleared,
	 * so built-in devices may still report auto_remove_on=never. Validate
	 * the PM-runtime flag directly instead of inferring it from that text.
	 */
	link = device_link_add(consumer, &supplier->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER |
			       DL_FLAG_PM_RUNTIME);
	if (!link || !(READ_ONCE(link->flags) & DL_FLAG_PM_RUNTIME)) {
		ret = -EINVAL;
		goto put_supplier;
	}

	put_device(&supplier->dev);
	return handoff;

put_supplier:
	put_device(&supplier->dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_get);
static int
mt6797_dvfsp_state_provenance_check(
	const struct mt6797_dvfsp_state_provenance *provenance)
{
	if (!provenance ||
	    provenance->abi != MT6797_DVFSP_STATE_PROVENANCE_ABI ||
	    provenance->source_mask !=
		MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL ||
	    !provenance->variant_id || !provenance->table_epoch ||
	    !provenance->calibration_handle)
		return -EPROTO;

	return 0;
}

static bool
mt6797_dvfsp_state_provenance_equal(
	const struct mt6797_dvfsp_state_provenance *left,
	const struct mt6797_dvfsp_state_provenance *right)
{
	return left->abi == right->abi &&
		left->source_mask == right->source_mask &&
		left->variant_id == right->variant_id &&
		left->table_epoch == right->table_epoch &&
		left->calibration_handle == right->calibration_handle;
}

static int
mt6797_dvfsp_calibration_state_check(
	const struct mt6797_dvfsp_calibration_state *state)
{
	unsigned int i;

	if (!state || state->abi != MT6797_DVFSP_CALIBRATION_STATE_ABI ||
	    state->phase != MT6797_DVFSP_CALIBRATION_STATE_PHASE_MON ||
	    state->bank_mask != MT6797_DVFSP_CALIBRATION_STATE_BANK_ALL ||
	    !state->thermal_generation || !state->clock_owner_generation ||
	    !state->rail_owner_generation ||
	    !state->source_generation ||
	    mt6797_dvfsp_state_provenance_check(&state->provenance))
		return -EPROTO;

	for (i = 0; i < MT6797_DVFSP_STATE_CLUSTER_COUNT; i++) {
		const struct mt6797_dvfsp_calibration_cluster *cluster =
			&state->cluster[i];
		unsigned int j;
		u32 previous_frequency = 0;

		if (!cluster->table_count ||
		    cluster->table_count > MT6797_DVFSP_CALIBRATION_TABLE_MAX)
			return -EPROTO;
		for (j = 0; j < cluster->table_count; j++) {
			const struct mt6797_dvfsp_calibration_table_entry *entry =
				&cluster->table[j];

			if (!entry->frequency_khz ||
			    entry->frequency_khz <= previous_frequency ||
			    !entry->vproc_uv || !entry->vsram_uv ||
			    !entry->ppm_limit_khz || entry->vsram_uv < entry->vproc_uv ||
			    entry->vsram_uv - entry->vproc_uv < 10000 ||
			    entry->vsram_uv - entry->vproc_uv > 30000 ||
			    entry->vsram_uv < 1000000 || entry->vsram_uv > 1200000)
				return -EPROTO;
			previous_frequency = entry->frequency_khz;
		}
	}

	return 0;
}

static int
mt6797_dvfsp_state_snapshot_check(
	const struct mt6797_dvfsp_state_snapshot *snapshot)
{
	unsigned int i;
	int ret;

	ret = mt6797_dvfsp_state_provenance_check(&snapshot->provenance);
	if (ret)
		return ret;

	if (snapshot->abi != MT6797_DVFSP_STATE_OWNER_ABI ||
	    !snapshot->generation ||
	    !snapshot->cluster_mask ||
	    snapshot->cluster_mask & ~MT6797_DVFSP_STATE_CLUSTER_MASK)
		return -EPROTO;

	for (i = 0; i < MT6797_DVFSP_STATE_CLUSTER_COUNT; i++) {
		const struct mt6797_dvfsp_state_cluster *cluster =
			&snapshot->cluster[i];

		if (!(snapshot->cluster_mask & BIT(i)))
			continue;
		if (cluster->valid_fields != MT6797_DVFSP_STATE_FIELD_ALL)
			return -EPROTO;
		if (cluster->state_flags & ~(MT6797_DVFSP_STATE_FLAG_PRESENT |
					      MT6797_DVFSP_STATE_FLAG_ON))
			return -EPROTO;
	}

	return 0;
}
static int
mt6797_dvfsp_state_owner_identity_check(
	const struct mt6797_dvfsp_state_owner_identity *identity)
{
	if (!identity ||
	    identity->abi != MT6797_DVFSP_STATE_OWNER_IDENTITY_ABI ||
	    identity->resource_mask != MT6797_DVFSP_STATE_OWNER_RESOURCE_ALL ||
	    identity->cpu_pll_backend !=
		MT6797_DVFSP_STATE_OWNER_BACKEND_MCUMIXED_DVFSP ||
	    identity->big_cluster_backend !=
		MT6797_DVFSP_STATE_OWNER_BACKEND_BIGIDVFS_SMCCC ||
	    identity->reserved || !identity->owner_handle ||
	    !identity->transition_handle)
		return -EPROTO;
	return mt6797_dvfsp_state_provenance_check(&identity->provenance);
}

int mt6797_dvfsp_handoff_state_owner_register(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_owner_ops *ops, void *context)
{
	int ret = 0;
	struct mt6797_dvfsp_state_owner_identity identity;

	if (!handoff || !ops || !ops->snapshot || !ops->validate ||
	    !ops->hold || !ops->release || !ops->invalidate)
		return -EINVAL;
	if (!ops->identify)
		return -EINVAL;
	memset(&identity, 0, sizeof(identity));
	ret = ops->identify(context, &identity);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_state_owner_identity_check(&identity);
	if (ret)
		return ret;

	mutex_lock(&handoff->state_owner_lock);
	if (handoff->state_owner_ops) {
		ret = -EBUSY;
	} else {
		handoff->state_owner_ops = ops;
		handoff->state_owner_context = context;
		handoff->state_owner_identity = identity;
	}
	mutex_unlock(&handoff->state_owner_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_state_owner_register);

int mt6797_dvfsp_handoff_state_owner_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_owner_ops *ops, void *context)
{
	int ret = 0;

	if (!handoff || !ops)
		return -EINVAL;

	mutex_lock(&handoff->state_owner_lock);
	if (handoff->state_hold_active) {
		ret = -EBUSY;
	} else if (handoff->state_owner_ops != ops ||
	    handoff->state_owner_context != context) {
		ret = -ENOENT;
	} else {
		handoff->state_owner_ops->invalidate(
			handoff->state_owner_context,
			MT6797_DVFSP_STATE_INVALID_OWNER_REMOVED);
		handoff->state_owner_ops = NULL;
		handoff->state_owner_context = NULL;
		memset(&handoff->state_owner_identity, 0,
				sizeof(handoff->state_owner_identity));
	}
	mutex_unlock(&handoff->state_owner_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_state_owner_unregister);

int mt6797_dvfsp_handoff_state_owner_identity(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_state_owner_identity *identity)
{
	int ret = 0;

	if (!handoff || !identity)
		return -EINVAL;

	mutex_lock(&handoff->state_owner_lock);
	if (!handoff->state_owner_ops)
		ret = -EOPNOTSUPP;
	else
		*identity = handoff->state_owner_identity;
	mutex_unlock(&handoff->state_owner_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_state_owner_identity);

static int
mt6797_dvfsp_state_backend_check(
	const struct mt6797_dvfsp_state_backend_snapshot *snapshot,
	const struct mt6797_dvfsp_state_backend_ops *ops)
{
	unsigned int i;

	if (!snapshot || !ops ||
	    snapshot->abi != MT6797_DVFSP_STATE_BACKEND_ABI ||
	    snapshot->abi != ops->abi ||
	    snapshot->backend != ops->backend ||
	    snapshot->resource_mask != ops->resource_mask ||
	    snapshot->cluster_mask != ops->cluster_mask ||
	    !snapshot->generation || !snapshot->owner_handle ||
	    !snapshot->transition_handle ||
	    snapshot->cluster_mask & ~MT6797_DVFSP_STATE_CLUSTER_MASK)
		return -EPROTO;
	if (mt6797_dvfsp_state_provenance_check(&snapshot->provenance))
		return -EPROTO;

	for (i = 0; i < MT6797_DVFSP_STATE_CLUSTER_COUNT; i++) {
		const struct mt6797_dvfsp_state_cluster *cluster =
			&snapshot->cluster[i];

		if (!(snapshot->cluster_mask & BIT(i)))
			continue;
		if (cluster->valid_fields != MT6797_DVFSP_STATE_FIELD_ALL ||
		    cluster->state_flags &
			~(MT6797_DVFSP_STATE_FLAG_PRESENT |
			  MT6797_DVFSP_STATE_FLAG_ON))
			return -EPROTO;
	}

	return 0;
}

static int
mt6797_dvfsp_protected_state_owner_transition_lock(
	const struct mt6797_dvfsp_protected_state_owner *owner)
{
	if (!owner->transition_lock_ops ||
	    owner->transition_lock_ops->abi !=
		MT6797_DVFSP_STATE_TRANSITION_LOCK_ABI ||
	    !owner->transition_lock_ops->lock ||
	    !owner->transition_lock_ops->unlock)
		return -EINVAL;

	return owner->transition_lock_ops->lock(
		owner->transition_lock_context);
}

static void
mt6797_dvfsp_protected_state_owner_transition_unlock(
	const struct mt6797_dvfsp_protected_state_owner *owner)
{
	owner->transition_lock_ops->unlock(owner->transition_lock_context);
}

static int
mt6797_dvfsp_protected_state_owner_check(
	const struct mt6797_dvfsp_protected_state_owner *owner)
{
	const struct mt6797_dvfsp_state_backend_ops *cpu;
	const struct mt6797_dvfsp_state_backend_ops *big;

	if (!owner || !owner->cpu_pll_ops || !owner->big_cluster_ops ||
	    !owner->owner_handle || !owner->transition_handle)
		return -EINVAL;
	if (mt6797_dvfsp_state_provenance_check(&owner->provenance))
		return -EINVAL;
	if (!owner->calibration_ops || !owner->calibration_ops->snapshot ||
	    !owner->calibration_ops->snapshot_state ||
	    !owner->calibration_ops->validate ||
	    !owner->calibration_ops->hold ||
	    !owner->calibration_ops->release ||
	    !owner->calibration_ops->invalidate ||
	    owner->calibration_ops->abi !=
		MT6797_DVFSP_STATE_CALIBRATION_ABI)
		return -EINVAL;
	if (!owner->transition_lock_ops ||
	    owner->transition_lock_ops->abi !=
		MT6797_DVFSP_STATE_TRANSITION_LOCK_ABI ||
	    !owner->transition_lock_ops->lock ||
	    !owner->transition_lock_ops->unlock)
		return -EINVAL;

	cpu = owner->cpu_pll_ops;
	big = owner->big_cluster_ops;
	if (!cpu->snapshot || !cpu->validate || !cpu->hold ||
	    !cpu->release || !cpu->invalidate ||
	    !big->snapshot || !big->validate || !big->hold ||
	    !big->release || !big->invalidate)
		return -EINVAL;
	if (cpu->abi != MT6797_DVFSP_STATE_BACKEND_ABI ||
	    cpu->backend != MT6797_DVFSP_STATE_OWNER_BACKEND_MCUMIXED_DVFSP ||
	    cpu->resource_mask != MT6797_DVFSP_STATE_OWNER_RESOURCE_CPU_PLL ||
	    cpu->cluster_mask != MT6797_DVFSP_STATE_BACKEND_CPU_PLL_CLUSTERS ||
	    big->abi != MT6797_DVFSP_STATE_BACKEND_ABI ||
	    big->backend != MT6797_DVFSP_STATE_OWNER_BACKEND_BIGIDVFS_SMCCC ||
	    big->resource_mask != MT6797_DVFSP_STATE_OWNER_RESOURCE_BIG_CLUSTER ||
	    big->cluster_mask != MT6797_DVFSP_STATE_BACKEND_BIG_CLUSTER_CLUSTERS)
		return -EPROTO;

	return 0;
}

static bool
mt6797_dvfsp_state_snapshot_equal(
	const struct mt6797_dvfsp_state_snapshot *left,
	const struct mt6797_dvfsp_state_snapshot *right)
{
	return left->abi == right->abi &&
		left->cluster_mask == right->cluster_mask &&
		left->generation == right->generation &&
		mt6797_dvfsp_state_provenance_equal(&left->provenance,
			&right->provenance) &&
		!memcmp(left->cluster, right->cluster,
			sizeof(left->cluster));
}

static int
mt6797_dvfsp_protected_state_owner_collect_locked(
	const struct mt6797_dvfsp_protected_state_owner *owner,
	struct mt6797_dvfsp_state_backend_snapshot *cpu_snapshot,
	struct mt6797_dvfsp_state_backend_snapshot *big_snapshot,
	struct mt6797_dvfsp_state_snapshot *snapshot)
{
	const struct mt6797_dvfsp_state_backend_ops *cpu;
	const struct mt6797_dvfsp_state_backend_ops *big;
	struct mt6797_dvfsp_state_provenance provenance;
	struct mt6797_dvfsp_calibration_state calibration_state;
	int ret;

	ret = mt6797_dvfsp_protected_state_owner_check(owner);
	if (ret)
		return ret;

	memset(&calibration_state, 0, sizeof(calibration_state));
	ret = owner->calibration_ops->snapshot_state(
		owner->calibration_context, &calibration_state);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_calibration_state_check(&calibration_state);
	if (ret || !mt6797_dvfsp_state_provenance_equal(
			&calibration_state.provenance, &owner->provenance))
		return -EPROTO;

	memset(&provenance, 0, sizeof(provenance));
	ret = owner->calibration_ops->snapshot(owner->calibration_context,
						&provenance);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_state_provenance_check(&provenance);
	if (ret || !mt6797_dvfsp_state_provenance_equal(
			&provenance, &owner->provenance))
		return -EPROTO;

	cpu = owner->cpu_pll_ops;
	big = owner->big_cluster_ops;
	memset(cpu_snapshot, 0, sizeof(*cpu_snapshot));
	ret = cpu->snapshot(owner->cpu_pll_context, cpu_snapshot);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_state_backend_check(cpu_snapshot, cpu);
	if (ret)
		return ret;

	memset(big_snapshot, 0, sizeof(*big_snapshot));
	ret = big->snapshot(owner->big_cluster_context, big_snapshot);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_state_backend_check(big_snapshot, big);
	if (ret)
		return ret;

	if (cpu_snapshot->owner_handle != owner->owner_handle ||
	    big_snapshot->owner_handle != owner->owner_handle)
		return -EPROTO;
	if (cpu_snapshot->transition_handle != owner->transition_handle ||
	    big_snapshot->transition_handle != owner->transition_handle)
		return -EPROTO;
	if (!mt6797_dvfsp_state_provenance_equal(&cpu_snapshot->provenance,
			&big_snapshot->provenance) ||
	    !mt6797_dvfsp_state_provenance_equal(&cpu_snapshot->provenance,
			&owner->provenance))
		return -EPROTO;
	if (cpu_snapshot->cluster_mask & big_snapshot->cluster_mask ||
	    (cpu_snapshot->cluster_mask | big_snapshot->cluster_mask) !=
		MT6797_DVFSP_STATE_CLUSTER_MASK)
		return -EPROTO;
	if (cpu_snapshot->generation != big_snapshot->generation)
		return -EAGAIN;
	if (!mt6797_dvfsp_state_provenance_equal(&cpu_snapshot->provenance,
			&provenance) ||
	    !mt6797_dvfsp_state_provenance_equal(&big_snapshot->provenance,
			&provenance))
		return -EPROTO;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->abi = MT6797_DVFSP_STATE_OWNER_ABI;
	snapshot->cluster_mask = cpu_snapshot->cluster_mask |
		big_snapshot->cluster_mask;
	snapshot->generation = cpu_snapshot->generation;
	snapshot->provenance = provenance;
	snapshot->cluster[MT6797_DVFSP_STATE_CLUSTER_LL] =
		cpu_snapshot->cluster[MT6797_DVFSP_STATE_CLUSTER_LL];
	snapshot->cluster[MT6797_DVFSP_STATE_CLUSTER_L] =
		cpu_snapshot->cluster[MT6797_DVFSP_STATE_CLUSTER_L];
	snapshot->cluster[MT6797_DVFSP_STATE_CLUSTER_CCI] =
		cpu_snapshot->cluster[MT6797_DVFSP_STATE_CLUSTER_CCI];
	snapshot->cluster[MT6797_DVFSP_STATE_CLUSTER_B] =
		big_snapshot->cluster[MT6797_DVFSP_STATE_CLUSTER_B];

	return mt6797_dvfsp_state_snapshot_check(snapshot);
}

static int
mt6797_dvfsp_protected_state_owner_collect(
	const struct mt6797_dvfsp_protected_state_owner *owner,
	struct mt6797_dvfsp_state_backend_snapshot *cpu_snapshot,
	struct mt6797_dvfsp_state_backend_snapshot *big_snapshot,
	struct mt6797_dvfsp_state_snapshot *snapshot)
{
	int ret;

	ret = mt6797_dvfsp_protected_state_owner_transition_lock(owner);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_protected_state_owner_collect_locked(
		owner, cpu_snapshot, big_snapshot, snapshot);
	mt6797_dvfsp_protected_state_owner_transition_unlock(owner);

	return ret;
}
static int
mt6797_dvfsp_protected_state_owner_identify(
	void *context, struct mt6797_dvfsp_state_owner_identity *identity)
{
	const struct mt6797_dvfsp_protected_state_owner *owner = context;
	int ret;

	if (!identity)
		return -EINVAL;
	ret = mt6797_dvfsp_protected_state_owner_check(owner);
	if (ret)
		return ret;

	memset(identity, 0, sizeof(*identity));
	identity->abi = MT6797_DVFSP_STATE_OWNER_IDENTITY_ABI;
	identity->resource_mask = MT6797_DVFSP_STATE_OWNER_RESOURCE_ALL;
	identity->cpu_pll_backend = owner->cpu_pll_ops->backend;
	identity->big_cluster_backend = owner->big_cluster_ops->backend;
	identity->owner_handle = owner->owner_handle;
	identity->transition_handle = owner->transition_handle;
	identity->provenance = owner->provenance;

	return 0;
}

static int
mt6797_dvfsp_protected_state_owner_snapshot(
	void *context, struct mt6797_dvfsp_state_snapshot *snapshot)
{
	struct mt6797_dvfsp_state_backend_snapshot cpu_snapshot;
	struct mt6797_dvfsp_state_backend_snapshot big_snapshot;

	if (!snapshot)
		return -EINVAL;

	return mt6797_dvfsp_protected_state_owner_collect(
		context, &cpu_snapshot, &big_snapshot, snapshot);
}

static int
mt6797_dvfsp_protected_state_owner_validate(
	void *context, const struct mt6797_dvfsp_state_snapshot *snapshot)
{
	struct mt6797_dvfsp_state_backend_snapshot cpu_snapshot;
	struct mt6797_dvfsp_state_backend_snapshot big_snapshot;
	struct mt6797_dvfsp_state_snapshot fresh;
	const struct mt6797_dvfsp_protected_state_owner *owner = context;
	int ret;

	if (!snapshot)
		return -EINVAL;
	ret = mt6797_dvfsp_state_snapshot_check(snapshot);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_protected_state_owner_transition_lock(owner);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_protected_state_owner_collect_locked(
		owner, &cpu_snapshot, &big_snapshot, &fresh);
	if (ret)
		goto out_unlock;
	if (!mt6797_dvfsp_state_snapshot_equal(snapshot, &fresh)) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	ret = owner->calibration_ops->validate(owner->calibration_context,
						&fresh.provenance);
	if (ret)
		goto out_unlock;

	ret = owner->cpu_pll_ops->validate(owner->cpu_pll_context,
					    &cpu_snapshot);
	if (ret)
		goto out_unlock;
	ret = owner->big_cluster_ops->validate(owner->big_cluster_context,
						&big_snapshot);
out_unlock:
	mt6797_dvfsp_protected_state_owner_transition_unlock(owner);
	return ret;
}

static int
mt6797_dvfsp_protected_state_owner_release_pair(
	const struct mt6797_dvfsp_protected_state_owner *owner,
	const struct mt6797_dvfsp_state_hold *cpu_hold, bool cpu_held,
	const struct mt6797_dvfsp_state_hold *big_hold, bool big_held)
{
	int ret = 0;
	int release_ret;
	struct mt6797_dvfsp_state_calibration_hold calibration_hold;

	if (big_held) {
		release_ret = owner->big_cluster_ops->release(
			owner->big_cluster_context, big_hold);
		if (release_ret)
			ret = release_ret;
	}
	if (cpu_held) {
		release_ret = owner->cpu_pll_ops->release(
			owner->cpu_pll_context, cpu_hold);
		if (!ret && release_ret)
			ret = release_ret;
	}

	if (cpu_held || big_held) {
		memset(&calibration_hold, 0, sizeof(calibration_hold));
		if (cpu_held)
			calibration_hold = (struct mt6797_dvfsp_state_calibration_hold) {
				.generation = cpu_hold->generation,
				.transition_handle = cpu_hold->transition_handle,
				.owner_handle = cpu_hold->owner_handle,
				.provenance = cpu_hold->provenance,
			};
		else if (big_held)
			calibration_hold = (struct mt6797_dvfsp_state_calibration_hold) {
				.generation = big_hold->generation,
				.transition_handle = big_hold->transition_handle,
				.owner_handle = big_hold->owner_handle,
				.provenance = big_hold->provenance,
			};
		release_ret = owner->calibration_ops->release(
			owner->calibration_context, &calibration_hold);
		if (!ret && release_ret)
			ret = release_ret;
	}

	return ret;
}

static int
mt6797_dvfsp_protected_state_owner_hold(
	void *context, const struct mt6797_dvfsp_state_snapshot *snapshot,
	struct mt6797_dvfsp_state_hold *hold)
{
	struct mt6797_dvfsp_state_backend_snapshot cpu_snapshot;
	struct mt6797_dvfsp_state_backend_snapshot big_snapshot;
	struct mt6797_dvfsp_state_snapshot fresh;
	struct mt6797_dvfsp_state_hold cpu_hold;
	struct mt6797_dvfsp_state_hold big_hold;
	const struct mt6797_dvfsp_protected_state_owner *owner = context;
	struct mt6797_dvfsp_state_calibration_hold calibration_hold;
	bool calibration_held = false;
	bool cpu_held = false;
	bool big_held = false;
	int rollback_ret;
	int ret;

	if (!snapshot || !hold)
		return -EINVAL;
	ret = mt6797_dvfsp_state_snapshot_check(snapshot);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_protected_state_owner_transition_lock(owner);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_protected_state_owner_collect_locked(
		owner, &cpu_snapshot, &big_snapshot, &fresh);
	if (ret)
		goto out_unlock;
	if (!mt6797_dvfsp_state_snapshot_equal(snapshot, &fresh)) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	memset(&calibration_hold, 0, sizeof(calibration_hold));
	ret = owner->calibration_ops->hold(owner->calibration_context, &fresh,
						&calibration_hold);
	if (ret)
		goto out_unlock;
	calibration_held = true;
	if (calibration_hold.generation != fresh.generation ||
	    calibration_hold.transition_handle != owner->transition_handle ||
	    calibration_hold.owner_handle != owner->owner_handle ||
	    !mt6797_dvfsp_state_provenance_equal(
			&calibration_hold.provenance, &fresh.provenance)) {
		ret = -EPROTO;
		goto rollback;
	}

	memset(&cpu_hold, 0, sizeof(cpu_hold));
	ret = owner->cpu_pll_ops->hold(owner->cpu_pll_context,
					&cpu_snapshot, &cpu_hold);
	if (ret)
		goto rollback;
	cpu_held = true;
	if (cpu_hold.generation != cpu_snapshot.generation ||
	    cpu_hold.cluster_mask != cpu_snapshot.cluster_mask ||
	    cpu_hold.reserved || cpu_hold.owner_handle != owner->owner_handle ||
	    cpu_hold.transition_handle != owner->transition_handle ||
	    !mt6797_dvfsp_state_provenance_equal(&cpu_hold.provenance,
			&fresh.provenance)) {
		ret = -EPROTO;
		goto rollback;
	}

	memset(&big_hold, 0, sizeof(big_hold));
	ret = owner->big_cluster_ops->hold(owner->big_cluster_context,
					&big_snapshot, &big_hold);
	if (ret)
		goto rollback;
	big_held = true;
	if (big_hold.generation != big_snapshot.generation ||
	    big_hold.cluster_mask != big_snapshot.cluster_mask ||
	    big_hold.reserved || big_hold.owner_handle != owner->owner_handle ||
	    big_hold.transition_handle != owner->transition_handle ||
	    !mt6797_dvfsp_state_provenance_equal(&big_hold.provenance,
			&fresh.provenance)) {
		ret = -EPROTO;
		goto rollback;
	}

	memset(hold, 0, sizeof(*hold));
	hold->generation = snapshot->generation;
	hold->cluster_mask = snapshot->cluster_mask;
	hold->owner_handle = owner->owner_handle;
	hold->transition_handle = owner->transition_handle;
	hold->provenance = fresh.provenance;
	ret = 0;
	goto out_unlock;

rollback:
	rollback_ret = mt6797_dvfsp_protected_state_owner_release_pair(
		owner, &cpu_hold, cpu_held, &big_hold, big_held);
	if (calibration_held && !cpu_held && !big_held)
		owner->calibration_ops->release(owner->calibration_context,
						&calibration_hold);
	owner->cpu_pll_ops->invalidate(
		owner->cpu_pll_context,
		MT6797_DVFSP_STATE_INVALID_PCM_FAULT);
	owner->big_cluster_ops->invalidate(
		owner->big_cluster_context,
		MT6797_DVFSP_STATE_INVALID_PCM_FAULT);
	if (rollback_ret)
		ret = -EIO;
out_unlock:
	mt6797_dvfsp_protected_state_owner_transition_unlock(owner);
	return ret;
}

static int
mt6797_dvfsp_protected_state_owner_release(
	void *context, const struct mt6797_dvfsp_state_hold *hold)
{
	const struct mt6797_dvfsp_protected_state_owner *owner = context;
	struct mt6797_dvfsp_state_hold cpu_hold;
	struct mt6797_dvfsp_state_hold big_hold;
	int ret;

	ret = mt6797_dvfsp_protected_state_owner_check(owner);
	if (ret)
		return ret;
	if (!hold || !hold->generation ||
	    hold->cluster_mask != MT6797_DVFSP_STATE_CLUSTER_MASK ||
	    hold->reserved || hold->owner_handle != owner->owner_handle ||
	    hold->transition_handle != owner->transition_handle)
		return -EPROTO;
	ret = mt6797_dvfsp_protected_state_owner_transition_lock(owner);
	if (ret)
		return ret;

	cpu_hold = *hold;
	cpu_hold.cluster_mask = owner->cpu_pll_ops->cluster_mask;
	big_hold = *hold;
	big_hold.cluster_mask = owner->big_cluster_ops->cluster_mask;
	ret = mt6797_dvfsp_protected_state_owner_release_pair(
		owner, &cpu_hold, true, &big_hold, true);
	if (ret) {
		owner->cpu_pll_ops->invalidate(
			owner->cpu_pll_context,
			MT6797_DVFSP_STATE_INVALID_PCM_FAULT);
		owner->big_cluster_ops->invalidate(
			owner->big_cluster_context,
			MT6797_DVFSP_STATE_INVALID_PCM_FAULT);
	}
	mt6797_dvfsp_protected_state_owner_transition_unlock(owner);

	return ret;
}

static void
mt6797_dvfsp_protected_state_owner_invalidate(
	void *context, enum mt6797_dvfsp_state_invalidation_reason reason)
{
	const struct mt6797_dvfsp_protected_state_owner *owner = context;
	bool locked = false;

	if (!owner)
		return;
	if (!mt6797_dvfsp_protected_state_owner_transition_lock(owner))
		locked = true;
	if (owner->cpu_pll_ops && owner->cpu_pll_ops->invalidate)
		owner->cpu_pll_ops->invalidate(owner->cpu_pll_context, reason);
	if (owner->big_cluster_ops && owner->big_cluster_ops->invalidate)
		owner->big_cluster_ops->invalidate(owner->big_cluster_context, reason);
	if (owner->calibration_ops && owner->calibration_ops->invalidate)
		owner->calibration_ops->invalidate(owner->calibration_context, reason);
	if (locked)
		mt6797_dvfsp_protected_state_owner_transition_unlock(owner);
}

static const struct mt6797_dvfsp_state_owner_ops
mt6797_dvfsp_protected_state_owner_ops = {
	.identify = mt6797_dvfsp_protected_state_owner_identify,
	.snapshot = mt6797_dvfsp_protected_state_owner_snapshot,
	.validate = mt6797_dvfsp_protected_state_owner_validate,
	.hold = mt6797_dvfsp_protected_state_owner_hold,
	.release = mt6797_dvfsp_protected_state_owner_release,
	.invalidate = mt6797_dvfsp_protected_state_owner_invalidate,
};

int mt6797_dvfsp_protected_state_owner_register(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_protected_state_owner *owner)
{
	int ret;

	if (!handoff || !owner)
		return -EINVAL;
	ret = mt6797_dvfsp_protected_state_owner_check(owner);
	if (ret)
		return ret;

	return mt6797_dvfsp_handoff_state_owner_register(
		handoff, &mt6797_dvfsp_protected_state_owner_ops, owner);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_protected_state_owner_register);

int mt6797_dvfsp_protected_state_owner_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_protected_state_owner *owner)
{
	if (!handoff || !owner)
		return -EINVAL;

	return mt6797_dvfsp_handoff_state_owner_unregister(
		handoff, &mt6797_dvfsp_protected_state_owner_ops, owner);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_protected_state_owner_unregister);

int mt6797_dvfsp_handoff_state_snapshot(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_state_snapshot *snapshot)
{
	int ret;

	if (!handoff || !snapshot)
		return -EINVAL;

	mutex_lock(&handoff->state_owner_lock);
	if (!handoff->state_owner_ops) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	ret = handoff->state_owner_ops->snapshot(
		handoff->state_owner_context, snapshot);
	if (!ret)
		ret = mt6797_dvfsp_state_snapshot_check(snapshot);

out_unlock:
	mutex_unlock(&handoff->state_owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_state_snapshot);

int mt6797_dvfsp_handoff_state_validate(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_snapshot *snapshot)
{
	int ret;

	if (!handoff || !snapshot)
		return -EINVAL;

	ret = mt6797_dvfsp_state_snapshot_check(snapshot);
	if (ret)
		return ret;

	mutex_lock(&handoff->state_owner_lock);
	if (!handoff->state_owner_ops)
		ret = -EOPNOTSUPP;
	else
		ret = handoff->state_owner_ops->validate(
			handoff->state_owner_context, snapshot);
	mutex_unlock(&handoff->state_owner_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_state_validate);

static int
mt6797_dvfsp_state_hold_check(
	const struct mt6797_dvfsp_state_snapshot *snapshot,
	const struct mt6797_dvfsp_state_hold *hold)
{
	if (!hold->generation || hold->generation != snapshot->generation ||
	    hold->cluster_mask != snapshot->cluster_mask ||
	    hold->reserved || !hold->owner_handle ||
	    !hold->transition_handle)
		return -EPROTO;
	if (!mt6797_dvfsp_state_provenance_equal(&hold->provenance,
			&snapshot->provenance))
		return -EPROTO;

	return 0;
}

int mt6797_dvfsp_handoff_state_hold(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_snapshot *snapshot,
	struct mt6797_dvfsp_state_hold *hold)
{
	const struct mt6797_dvfsp_state_owner_ops *ops;
	void *context;
	int ret;

	if (!handoff || !snapshot || !hold)
		return -EINVAL;

	ret = mt6797_dvfsp_state_snapshot_check(snapshot);
	if (ret)
		return ret;

	mutex_lock(&handoff->state_owner_lock);
	if (!handoff->state_owner_ops) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}
	if (handoff->state_hold_active) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ops = handoff->state_owner_ops;
	context = handoff->state_owner_context;
	memset(hold, 0, sizeof(*hold));
	ret = ops->hold(context, snapshot, hold);
	if (!ret) {
		ret = mt6797_dvfsp_state_hold_check(snapshot, hold);
		if (!ret) {
			handoff->state_hold = *hold;
			handoff->state_hold_active = true;
		}
	}

out_unlock:
	mutex_unlock(&handoff->state_owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_state_hold);

int mt6797_dvfsp_handoff_state_release(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_hold *hold)
{
	const struct mt6797_dvfsp_state_owner_ops *ops;
	void *context;
	int ret;

	if (!handoff || !hold)
		return -EINVAL;

	mutex_lock(&handoff->state_owner_lock);
	if (!handoff->state_hold_active) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (memcmp(&handoff->state_hold, hold, sizeof(*hold))) {
		ret = -EPERM;
		goto out_unlock;
	}
	if (!handoff->state_owner_ops) {
		ret = -EHOSTDOWN;
		goto out_unlock;
	}

	ops = handoff->state_owner_ops;
	context = handoff->state_owner_context;
	ret = ops->release(context, hold);
	if (!ret) {
		memset(&handoff->state_hold, 0, sizeof(handoff->state_hold));
		handoff->state_hold_active = false;
	} else {
		ops->invalidate(context, MT6797_DVFSP_STATE_INVALID_PCM_FAULT);
	}

out_unlock:
	mutex_unlock(&handoff->state_owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_state_release);

void mt6797_dvfsp_handoff_state_invalidate(
	struct mt6797_dvfsp_handoff *handoff,
	enum mt6797_dvfsp_state_invalidation_reason reason)
{
	if (!handoff)
		return;

	mutex_lock(&handoff->state_owner_lock);
	if (handoff->state_owner_ops)
		handoff->state_owner_ops->invalidate(
			handoff->state_owner_context, reason);
	mutex_unlock(&handoff->state_owner_lock);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_state_invalidate);

static bool
mt6797_dvfsp_pcm_bytes_present(const u8 *bytes, size_t length)
{
	size_t i;

	for (i = 0; i < length; i++)
		if (bytes[i])
			return true;

	return false;
}

static int
mt6797_dvfsp_pcm_image_check(
	const struct mt6797_dvfsp_pcm_image_identity *image)
{
	if (!image || image->abi != MT6797_DVFSP_PCM_ADAPTER_ABI ||
	    !image->target_revision || !image->loader_domain ||
	    !image->physical_base || !image->length || !image->alignment ||
	    !mt6797_dvfsp_pcm_bytes_present(
		image->sha256, MT6797_DVFSP_PCM_IMAGE_HASH_BYTES))
		return -EPROTO;

	return 0;
}

static int
mt6797_dvfsp_pcm_resource_check(
	const struct mt6797_dvfsp_pcm_resource_identity *resources)
{
	if (!resources || resources->abi != MT6797_DVFSP_PCM_ADAPTER_ABI ||
	    resources->resource_mask != MT6797_DVFSP_PCM_RESOURCE_ALL ||
	    resources->cspm_base != MT6797_DVFSP_PCM_CSPM_BASE ||
	    resources->cspm_length != MT6797_DVFSP_PCM_CSPM_LENGTH ||
	    resources->csram_base != MT6797_DVFSP_PCM_CSRAM_BASE ||
	    resources->csram_length != MT6797_DVFSP_PCM_CSRAM_LENGTH ||
	    resources->reserved || resources->reserved2 ||
	    !resources->owner_handle)
		return -EPROTO;

	return 0;
}

static bool
mt6797_dvfsp_pcm_image_equal(
	const struct mt6797_dvfsp_pcm_image_identity *left,
	const struct mt6797_dvfsp_pcm_image_identity *right)
{
	return left->abi == right->abi &&
		left->target_revision == right->target_revision &&
		left->loader_domain == right->loader_domain &&
		left->reserved == right->reserved &&
		left->physical_base == right->physical_base &&
		left->length == right->length &&
		left->alignment == right->alignment &&
		!memcmp(left->sha256, right->sha256,
			MT6797_DVFSP_PCM_IMAGE_HASH_BYTES);
}

static bool
mt6797_dvfsp_pcm_resource_equal(
	const struct mt6797_dvfsp_pcm_resource_identity *left,
	const struct mt6797_dvfsp_pcm_resource_identity *right)
{
	return left->abi == right->abi &&
		left->resource_mask == right->resource_mask &&
		left->cspm_base == right->cspm_base &&
		left->cspm_length == right->cspm_length &&
		left->reserved == right->reserved &&
		left->csram_base == right->csram_base &&
		left->csram_length == right->csram_length &&
		left->reserved2 == right->reserved2 &&
		left->owner_handle == right->owner_handle;
}

static int
mt6797_dvfsp_pcm_identity_check(
	const struct mt6797_dvfsp_pcm_adapter_identity *identity)
{
	if (!identity || identity->abi != MT6797_DVFSP_PCM_ADAPTER_ABI ||
	    !identity->cluster_mask ||
	    identity->cluster_mask & ~MT6797_DVFSP_STATE_CLUSTER_MASK ||
	    !identity->state_generation || !identity->state_owner_handle)
		return -EPROTO;

	return mt6797_dvfsp_pcm_image_check(&identity->image);
}

static int
mt6797_dvfsp_pcm_adapter_fault_locked(
	struct mt6797_dvfsp_handoff *handoff,
	enum mt6797_dvfsp_pcm_adapter_fault fault, int error)
{
	const struct mt6797_dvfsp_pcm_adapter_ops *ops =
		handoff->pcm_adapter_ops;
	int ret = error;

	handoff->pcm_adapter_sticky_fault = true;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_FAULTED;
	if (ops)
		ops->invalidate(handoff->pcm_adapter_context, fault);

	return ret;
}

static int
mt6797_dvfsp_pcm_adapter_revalidate_locked(
	struct mt6797_dvfsp_handoff *handoff)
{
	return mt6797_dvfsp_handoff_state_validate(
		handoff, &handoff->pcm_adapter_snapshot);
}

static int
mt6797_dvfsp_pcm_adapter_checkpoint_locked(
	struct mt6797_dvfsp_handoff *handoff,
	int (*checkpoint)(void *context,
			 const struct mt6797_dvfsp_pcm_adapter_identity *identity),
	enum mt6797_dvfsp_pcm_adapter_fault fault,
	int unused_error)
{
	int ret;
	(void)unused_error;
	(void)fault;

	ret = mt6797_dvfsp_pcm_adapter_revalidate_locked(handoff);
	if (ret)
		return ret;

	return checkpoint(handoff->pcm_adapter_context,
			   &handoff->pcm_adapter_identity);
}

int mt6797_dvfsp_handoff_pcm_adapter_register(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_pcm_adapter_ops *ops, void *context)
{
	int ret = 0;

	if (!handoff || !ops || !ops->resources_acquire ||
	    !ops->image_prepare || !ops->reset_initialize ||
	    !ops->image_acknowledge || !ops->control_initialize ||
	    !ops->run || !ops->lease_register || !ops->lease_unregister ||
	    !ops->teardown || !ops->invalidate)
		return -EINVAL;

	mutex_lock(&handoff->pcm_adapter_lock);
	if (handoff->pcm_adapter_ops || handoff->pcm_adapter_sticky_fault) {
		ret = -EBUSY;
	} else {
		handoff->pcm_adapter_ops = ops;
		handoff->pcm_adapter_context = context;
	}
	mutex_unlock(&handoff->pcm_adapter_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_pcm_adapter_register);

int mt6797_dvfsp_handoff_pcm_adapter_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_pcm_adapter_ops *ops, void *context)
{
	int ret = 0;

	if (!handoff || !ops)
		return -EINVAL;

	mutex_lock(&handoff->pcm_adapter_lock);
	if (handoff->pcm_adapter_phase != MT6797_DVFSP_PCM_PHASE_UNAVAILABLE)
		ret = -EBUSY;
	else if (handoff->pcm_adapter_ops != ops ||
		 handoff->pcm_adapter_context != context)
		ret = -ENOENT;
	else {
		handoff->pcm_adapter_ops = NULL;
		handoff->pcm_adapter_context = NULL;
	}
	mutex_unlock(&handoff->pcm_adapter_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_pcm_adapter_unregister);

int mt6797_dvfsp_handoff_pcm_adapter_start(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_pcm_image_identity *image)
{
	struct mt6797_dvfsp_state_snapshot snapshot;
	struct mt6797_dvfsp_state_hold hold;
	struct mt6797_dvfsp_pcm_resource_identity resources;
	const struct mt6797_dvfsp_pcm_adapter_ops *ops;
	enum mt6797_dvfsp_pcm_adapter_phase phase_before_fault;
	bool hold_active = false;
	int ret;

	if (!handoff || !image)
		return -EINVAL;

	ret = mt6797_dvfsp_pcm_image_check(image);
	if (ret)
		return ret;

	mutex_lock(&handoff->pcm_adapter_lock);
	ops = handoff->pcm_adapter_ops;
	if (!ops) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}
	if (handoff->pcm_adapter_sticky_fault ||
	    handoff->pcm_adapter_phase != MT6797_DVFSP_PCM_PHASE_UNAVAILABLE) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = mt6797_dvfsp_handoff_state_snapshot(handoff, &snapshot);
	if (ret)
		goto out_unlock;
	handoff->pcm_adapter_snapshot = snapshot;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_SNAPSHOTTED;

	ret = mt6797_dvfsp_handoff_state_hold(handoff, &snapshot, &hold);
	if (ret)
		goto fault_state;
	hold_active = true;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_STATE_HELD;

	memset(&handoff->pcm_adapter_identity, 0,
	       sizeof(handoff->pcm_adapter_identity));
	handoff->pcm_adapter_identity.abi = MT6797_DVFSP_PCM_ADAPTER_ABI;
	handoff->pcm_adapter_identity.cluster_mask = snapshot.cluster_mask;
	handoff->pcm_adapter_identity.state_generation = snapshot.generation;
	handoff->pcm_adapter_identity.state_owner_handle = hold.owner_handle;
	handoff->pcm_adapter_identity.image = *image;
	ret = mt6797_dvfsp_pcm_identity_check(
		&handoff->pcm_adapter_identity);
	if (ret)
		goto fault_state;

	memset(&resources, 0, sizeof(resources));
	ret = ops->resources_acquire(handoff->pcm_adapter_context,
				     &handoff->pcm_adapter_identity, &resources);
	if (ret)
		goto fault_state;
	ret = mt6797_dvfsp_pcm_resource_check(&resources);
	if (ret)
		goto fault_resource;
	handoff->pcm_adapter_identity.resources = resources;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_RESOURCES_HELD;

	ret = ops->image_prepare(handoff->pcm_adapter_context,
				 &handoff->pcm_adapter_identity,
				 &handoff->pcm_adapter_identity.image);
	if (ret)
		goto fault_image;
	if (!mt6797_dvfsp_pcm_image_equal(image,
					  &handoff->pcm_adapter_identity.image)) {
		ret = -EPROTO;
		goto fault_image;
	}
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_IMAGE_READY;

	ret = mt6797_dvfsp_pcm_adapter_checkpoint_locked(
		handoff, ops->reset_initialize,
		MT6797_DVFSP_PCM_FAULT_CHECKPOINT, -ETIMEDOUT);
	if (ret)
		goto fault_checkpoint;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_RESET_INITIALIZED;

	ret = mt6797_dvfsp_pcm_adapter_checkpoint_locked(
		handoff, ops->image_acknowledge,
		MT6797_DVFSP_PCM_FAULT_CHECKPOINT, -ETIMEDOUT);
	if (ret)
		goto fault_checkpoint;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_IMAGE_ACKED;

	ret = mt6797_dvfsp_pcm_adapter_checkpoint_locked(
		handoff, ops->control_initialize,
		MT6797_DVFSP_PCM_FAULT_CHECKPOINT, -ETIMEDOUT);
	if (ret)
		goto fault_checkpoint;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_CONTROL_INITIALIZED;

	ret = mt6797_dvfsp_pcm_adapter_checkpoint_locked(
		handoff, ops->run, MT6797_DVFSP_PCM_FAULT_CHECKPOINT,
		-ETIMEDOUT);
	if (ret)
		goto fault_checkpoint;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_RUNNING;

	ret = mt6797_dvfsp_pcm_adapter_checkpoint_locked(
		handoff, ops->lease_register,
		MT6797_DVFSP_PCM_FAULT_CHECKPOINT, -ETIMEDOUT);
	if (ret)
		goto fault_checkpoint;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_LEASE_REGISTERED;
	ret = 0;
	goto out_unlock;

fault_checkpoint:
	phase_before_fault = handoff->pcm_adapter_phase;
	ret = mt6797_dvfsp_pcm_adapter_fault_locked(
		handoff, MT6797_DVFSP_PCM_FAULT_CHECKPOINT, ret);
	ops->teardown(handoff->pcm_adapter_context,
		      &handoff->pcm_adapter_identity, phase_before_fault);
	if (hold_active)
		mt6797_dvfsp_handoff_state_release(handoff, &hold);
	goto out_unlock;

fault_image:
	handoff->pcm_adapter_sticky_fault = true;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_FAULTED;
	ops->teardown(handoff->pcm_adapter_context,
		      &handoff->pcm_adapter_identity,
		      MT6797_DVFSP_PCM_PHASE_RESOURCES_HELD);
	ops->invalidate(handoff->pcm_adapter_context,
			MT6797_DVFSP_PCM_FAULT_IMAGE_IDENTITY);
	if (hold_active)
		mt6797_dvfsp_handoff_state_release(handoff, &hold);
	goto out_unlock;

fault_resource:
	ret = mt6797_dvfsp_pcm_adapter_fault_locked(
		handoff, MT6797_DVFSP_PCM_FAULT_RESOURCE_IDENTITY, ret);
	ops->teardown(handoff->pcm_adapter_context,
		      &handoff->pcm_adapter_identity,
		      MT6797_DVFSP_PCM_PHASE_STATE_HELD);
	if (hold_active)
		mt6797_dvfsp_handoff_state_release(handoff, &hold);
	goto out_unlock;

fault_state:
	phase_before_fault = handoff->pcm_adapter_phase;
	ret = mt6797_dvfsp_pcm_adapter_fault_locked(
		handoff, MT6797_DVFSP_PCM_FAULT_STATE_CHANGED, ret);
	if (hold_active && phase_before_fault >=
	    MT6797_DVFSP_PCM_PHASE_RESOURCES_HELD)
		ops->teardown(handoff->pcm_adapter_context,
			      &handoff->pcm_adapter_identity,
			      phase_before_fault);
	if (hold_active)
		mt6797_dvfsp_handoff_state_release(handoff, &hold);

out_unlock:
	mutex_unlock(&handoff->pcm_adapter_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_pcm_adapter_start);

int mt6797_dvfsp_handoff_pcm_adapter_stop(
	struct mt6797_dvfsp_handoff *handoff)
{
	const struct mt6797_dvfsp_pcm_adapter_ops *ops;
	int ret;

	if (!handoff)
		return -EINVAL;

	mutex_lock(&handoff->pcm_adapter_lock);
	ops = handoff->pcm_adapter_ops;
	if (!ops || handoff->pcm_adapter_phase !=
	    MT6797_DVFSP_PCM_PHASE_LEASE_REGISTERED) {
		ret = -EHOSTDOWN;
		goto out_unlock;
	}
	ret = ops->lease_unregister(handoff->pcm_adapter_context,
				   &handoff->pcm_adapter_identity);
	if (ret)
		goto fault;
	ret = ops->teardown(handoff->pcm_adapter_context,
				&handoff->pcm_adapter_identity,
				MT6797_DVFSP_PCM_PHASE_LEASE_REGISTERED);
	if (ret)
		goto fault;
	ret = mt6797_dvfsp_handoff_state_release(
		handoff, &handoff->state_hold);
	if (ret)
		goto fault;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_UNAVAILABLE;
	memset(&handoff->pcm_adapter_identity, 0,
	       sizeof(handoff->pcm_adapter_identity));
	goto out_unlock;

fault:
	ret = mt6797_dvfsp_pcm_adapter_fault_locked(
		handoff, MT6797_DVFSP_PCM_FAULT_TEARDOWN, ret);
out_unlock:
	mutex_unlock(&handoff->pcm_adapter_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_pcm_adapter_stop);

int mt6797_dvfsp_handoff_pcm_adapter_invalidate(
	struct mt6797_dvfsp_handoff *handoff,
	enum mt6797_dvfsp_pcm_adapter_fault fault)
{
	if (!handoff)
		return -EINVAL;

	mutex_lock(&handoff->pcm_adapter_lock);
	if (handoff->pcm_adapter_phase ==
	    MT6797_DVFSP_PCM_PHASE_UNAVAILABLE) {
		mutex_unlock(&handoff->pcm_adapter_lock);
		return -ENOENT;
	}
	handoff->pcm_adapter_sticky_fault = true;
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_INVALIDATED;
	if (handoff->pcm_adapter_ops)
		handoff->pcm_adapter_ops->invalidate(
			handoff->pcm_adapter_context, fault);
	mutex_unlock(&handoff->pcm_adapter_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_pcm_adapter_invalidate);

enum mt6797_dvfsp_pcm_adapter_phase
mt6797_dvfsp_handoff_pcm_adapter_phase(
	struct mt6797_dvfsp_handoff *handoff)
{
	enum mt6797_dvfsp_pcm_adapter_phase phase;

	if (!handoff)
		return MT6797_DVFSP_PCM_PHASE_FAULTED;

	mutex_lock(&handoff->pcm_adapter_lock);
	phase = handoff->pcm_adapter_phase;
	mutex_unlock(&handoff->pcm_adapter_lock);
	return phase;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_handoff_pcm_adapter_phase);


static int mt6797_dvfsp_handoff_probe(struct platform_device *pdev)
{
	struct mt6797_dvfsp_handoff *handoff;
	unsigned long timeout;
	int ret;

	handoff = devm_kzalloc(&pdev->dev, sizeof(*handoff), GFP_KERNEL);
	if (!handoff)
		return -ENOMEM;

	handoff->dev = &pdev->dev;
	handoff->state = MT6797_DVFSP_UNCLAIMED;
	handoff->late_state = MT6797_DVFSP_LATE_NOT_SCHEDULED;
	handoff->initial_gate = MT6797_DVFSP_GATE_UNKNOWN;
	handoff->reason = "resources-not-ready";
	mutex_init(&handoff->lock);
	mutex_init(&handoff->state_owner_lock);
	mt6797_dvfsp_cspm_access_init(&handoff->cspm_access);
	mutex_init(&handoff->fw_owner_lock);
	mutex_init(&handoff->pcm_adapter_lock);
	handoff->pcm_adapter_phase = MT6797_DVFSP_PCM_PHASE_UNAVAILABLE;
	init_completion(&handoff->terminal);
	atomic_set(&handoff->terminal_grant, 0);
	atomic_set(&handoff->pm_fault, MT6797_DVFSP_PM_OK);
	atomic_set(&handoff->suspend_checks, 0);
	atomic_set(&handoff->suspend_failures, 0);
	atomic_set(&handoff->resume_checks, 0);
	atomic_set(&handoff->resume_failures, 0);
	atomic_set(&handoff->consumer_ungated_checks, 0);
	atomic_set(&handoff->consumer_gated_checks, 0);
	atomic_set(&handoff->consumer_validation_failures, 0);

	handoff->i2c_clk = devm_clk_get(&pdev->dev, "i2c");
	if (IS_ERR(handoff->i2c_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(handoff->i2c_clk),
				     "cannot acquire I2C_APPM clock\n");

	handoff->infracfg =
		syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						"mediatek,infracfg");
	if (IS_ERR(handoff->infracfg))
		return dev_err_probe(&pdev->dev, PTR_ERR(handoff->infracfg),
				     "cannot resolve infracfg syscon\n");

	handoff->cspm = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(handoff->cspm))
		return dev_err_probe(&pdev->dev, PTR_ERR(handoff->cspm),
				     "cannot map CSPM validation window\n");

	ret = devm_delayed_work_autocancel(&pdev->dev, &handoff->late_work,
					   mt6797_dvfsp_late_work);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot allocate late validation work\n");

	platform_set_drvdata(pdev, handoff);
	ret = devm_device_add_group(&pdev->dev, &mt6797_dvfsp_handoff_group);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot publish read-only handoff state\n");

	mt6797_dvfsp_run_handoff(handoff);
	if (!completion_done(&handoff->terminal)) {
		timeout = wait_for_completion_timeout(
			&handoff->terminal,
			msecs_to_jiffies(MT6797_DVFSP_PROBE_TIMEOUT_MS));
		if (!timeout) {
			cancel_delayed_work_sync(&handoff->late_work);
			mutex_lock(&handoff->lock);
			if (handoff->state != MT6797_DVFSP_READY)
				mt6797_dvfsp_fault_locked(
					handoff, "late-validation-timeout");
			mutex_unlock(&handoff->lock);
		}
	}

	mutex_lock(&handoff->lock);
	if (handoff->state != MT6797_DVFSP_READY &&
	    handoff->state != MT6797_DVFSP_INCONCLUSIVE &&
	    handoff->state != MT6797_DVFSP_FAULTED)
		mt6797_dvfsp_fault_locked(handoff,
					  "terminal-state-invalid");

	if (handoff->state == MT6797_DVFSP_READY)
		dev_info(&pdev->dev,
			 "supplier_bound=yes access_grant=ready state=ready "
			 "late_validation=passed access_controller=enabled\n");
	else
		dev_warn(&pdev->dev,
			 "supplier_bound=yes access_grant=denied "
			 "state=%s reason=%s access_controller=enabled\n",
			 mt6797_dvfsp_state_names[handoff->state],
			 handoff->reason);
	mutex_unlock(&handoff->lock);

	dev_pm_set_driver_flags(&pdev->dev, DPM_FLAG_NO_DIRECT_COMPLETE);
	return 0;
}

static const struct of_device_id mt6797_dvfsp_handoff_of_match[] = {
	{ .compatible = "mediatek,mt6797-dvfsp-handoff" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_dvfsp_handoff_of_match);

static struct platform_driver mt6797_dvfsp_handoff_driver = {
	.probe = mt6797_dvfsp_handoff_probe,
	.driver = {
		.name = "mt6797-dvfsp-handoff",
		.of_match_table = mt6797_dvfsp_handoff_of_match,
		.pm = pm_sleep_ptr(&mt6797_dvfsp_handoff_pm_ops),
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6797_dvfsp_handoff_driver);

MODULE_DESCRIPTION("MediaTek MT6797 stopped DVFSP one-way handoff");
MODULE_LICENSE("GPL");
