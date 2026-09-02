/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * One-shot MT6797 MD1 cold power/clock ownership transaction.
 *
 * This is deliberately below, and separate from, the future MD1 boot-vector
 * gate.  Vendor CCCI orders CCIF clearing before md_cd_power_on(), then only
 * later writes MD_BOOT_VECTOR_EN.  A successful owner transaction therefore
 * means READY_FOR_BOOT_VECTOR, not that a modem was released.
 *
 * The vendor MT6797 PLL loops do not have a timeout.  The bounded count below
 * is a mainline failure containment rule, not a claim about vendor hardware.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_COLD_OWNER_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_COLD_OWNER_H

#include <linux/mutex.h>
#include <linux/types.h>

#define MT6797_MD1_COLD_OWNER_ABI		1
/*
 * Shared across all PLL stages: the core's attempt counter never resets
 * between stages.  Vendor cldma_platform.c spins unbounded per stage;
 * keep a firm bound at ~330 ms worst case (~33 us per syscon-backed
 * poll), enough for DFEPLL_RDY to assert after the earlier ready bits.
 */
#define MT6797_MD1_COLD_OWNER_MAX_PLL_POLLS	10000

enum mt6797_md1_cold_owner_state {
	MT6797_MD1_COLD_OWNER_OFF,
	MT6797_MD1_COLD_OWNER_PREFLIGHT_ACCEPTED,
	MT6797_MD1_COLD_OWNER_CCIF_CLEARED,
	MT6797_MD1_COLD_OWNER_POWERED,
	MT6797_MD1_COLD_OWNER_SRC_CLK_ENABLED,
	MT6797_MD1_COLD_OWNER_PLL_READY,
	/* The vendor WDT magic writes have no source-backed readback contract. */
	MT6797_MD1_COLD_OWNER_PLL_READY_WDT_UNOWNED,
	MT6797_MD1_COLD_OWNER_FATAL,
};

/* The hardware backend fills these source-visible values before any write. */
struct mt6797_md1_cold_state {
	u32 poweron_config;
	u32 md1_pwr_con;
	u32 pwr_status;
	u32 pwr_status_2nd;
	u32 infra_protect_en;
	u32 infra_protect_sta;
};

/* The values are evidence only: their bit meanings are intentionally absent. */
struct mt6797_md1_boot_status_preflight {
	bool lk_image_loaded;
	bool boot_status_observed;
	u8 stats0_samples;
	u8 stats1_samples;
	u32 stats0_final;
	u32 stats1_final;
};

enum mt6797_md1_cold_owner_pll_stage {
	MT6797_MD1_COLD_OWNER_PLL_PSMCU,
	MT6797_MD1_COLD_OWNER_PLL_L1MCU,
	MT6797_MD1_COLD_OWNER_PLL_DFE_IMC,
	MT6797_MD1_COLD_OWNER_PLL_L1SYS,
	MT6797_MD1_COLD_OWNER_PLL_FLEX1,
	MT6797_MD1_COLD_OWNER_PLL_FLEX2,
	MT6797_MD1_COLD_OWNER_PLL_DCM,
	MT6797_MD1_COLD_OWNER_PLL_DONE,
};

struct mt6797_md1_cold_owner_pll_status {
	enum mt6797_md1_cold_owner_pll_stage stage;
	u32 offset;
	u32 mask;
	u32 last_value;
};

struct mt6797_md1_cold_owner_ccif {
	u32 ap_busy;
	u32 md_busy;
	u32 ap_rchnum;
	u32 md_rchnum;
	u32 ap_ack;
	u32 md_ack;
	u32 ap_sram_nonzero_words;
	u32 md_sram_nonzero_words;
};

/*
 * Read-only AP-side CLDMA evidence.  No descriptor, queue, IRQ or mask is
 * owned by this observer; the values are only sampled after boot-vector GO.
 */
struct mt6797_md1_cold_owner_cldma {
	u32 coda_version;
	u32 ul_status;
	u32 so_status;
	u32 l2_tx_status;
	u32 l2_rx_status;
	u32 irq_mask;
	u32 ip_busy;
};

/*
 * A terminal, AP-visible observation made immediately around the one vendor
 * boot-vector write. It is an activity token only: it is not CCCI HS1, does
 * not decode CCIF channel contents and grants no CLDMA/IRQ ownership.
 */
enum mt6797_md1_post_vector_token {
	MT6797_MD1_POST_VECTOR_UNOBSERVED,
	MT6797_MD1_POST_VECTOR_NO_GO,
	MT6797_MD1_POST_VECTOR_GO,
};

struct mt6797_md1_post_vector_evidence {
	u64 baseline_boottime_ns;
	u64 observed_boottime_ns;
	u32 polls;
	u32 boot_status0_before;
	u32 boot_vector_readback;
	u32 boot_status1_before;
	u32 boot_status0_after;
	u32 boot_status1_after;
	struct mt6797_md1_cold_owner_ccif ccif_before;
	struct mt6797_md1_cold_owner_ccif ccif_after;
	bool boot_status_changed;
	bool ccif_changed;
	enum mt6797_md1_post_vector_token token;
	bool observed;
	/* Bounded, register-only follow-up after the immediate GO oracle. */
	u64 passive_started_boottime_ns;
	u64 passive_observed_boottime_ns;
	u32 passive_polls;
	u32 boot_status0_transitions;
	u32 boot_status1_transitions;
	u32 ccif_register_transitions;
	u32 first_ccif_activity_poll;
	bool ccif_register_activity;
	struct mt6797_md1_cold_owner_cldma cldma_after;
	u32 cldma_register_transitions;
	u32 first_cldma_activity_poll;
	bool cldma_register_activity;
	bool passive_complete;
};

struct mt6797_md1_cold_owner_ops {
	u32 abi;
	int (*verify_cold_reset_held)(void *context,
				       struct mt6797_md1_cold_state *state);
	int (*ccif_observe)(void *context, struct mt6797_md1_cold_owner_ccif *state);
	int (*ccif_clear)(void *context);
	int (*ccif_verify_cleared)(void *context,
			 const struct mt6797_md1_cold_owner_ccif *before,
			 const struct mt6797_md1_cold_owner_ccif *after);
	/* This may alter PMIC/SPM state. Failure is fatal; there is no rollback. */
	int (*power_on)(void *context);
	int (*enable_srcclk)(void *context);
	int (*pll_start)(void *context);
	/* Return -EAGAIN for one not-ready bounded poll and fill @status. */
	int (*pll_poll)(void *context,
			struct mt6797_md1_cold_owner_pll_status *status);
	/* Read-only register evidence after a pll poll timeout; no writes. */
	void (*pll_timeout_dump)(void *context);
};

struct mt6797_md1_cold_owner {
	const struct mt6797_md1_cold_owner_ops *ops;
	void *context;
	struct mutex lock;
	struct mt6797_md1_cold_state cold_state;
	struct mt6797_md1_cold_owner_ccif ccif_before;
	struct mt6797_md1_cold_owner_ccif ccif_after;
	struct mt6797_md1_cold_owner_pll_status last_pll;
	/* A pll stage timed out and was advanced without its ready bits. */
	bool pll_degraded;
	enum mt6797_md1_cold_owner_state state;
	int last_error;
};

int mt6797_md1_cold_owner_init(struct mt6797_md1_cold_owner *owner,
	const struct mt6797_md1_cold_owner_ops *ops, void *context);
int mt6797_md1_cold_owner_run(struct mt6797_md1_cold_owner *owner,
	const struct mt6797_md1_boot_status_preflight *preflight);
enum mt6797_md1_cold_owner_state
mt6797_md1_cold_owner_state(struct mt6797_md1_cold_owner *owner);

/* Pure comparison used by the bounded post-vector AP-only observer. */
bool mt6797_md1_cold_owner_post_vector_activity(
	const struct mt6797_md1_post_vector_evidence *evidence);

/* Pure classifier for the passive CCIF register observer; performs no I/O. */
bool mt6797_md1_cold_owner_ccif_register_activity(
	const struct mt6797_md1_cold_owner_ccif *ccif);

/* Pure classifier for the passive AP-side CLDMA observer; performs no I/O. */
bool
mt6797_md1_cold_owner_cldma_register_activity(
	const struct mt6797_md1_cold_owner_cldma *cldma);

/* Returns -ENODATA until the boot-vector observer reports GO or NO-GO. */
int mt6797_md1_cold_owner_post_vector_evidence_get(
	struct mt6797_md1_post_vector_evidence *evidence);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_COLD_OWNER_H */
