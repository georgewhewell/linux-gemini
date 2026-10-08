/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __DRV_CLK_MT6797_CPU_H
#define __DRV_CLK_MT6797_CPU_H

#include <linux/types.h>

#define MT6797_CPUCLK_SMC_REG_READ 0xc200035fUL
#define MT6797_CPUCLK_MP2_PLL_CON0 0x102224a0U
#define MT6797_CPUCLK_MP2_PLL_CON1 0x102224a4U
#define MT6797_CPUCLK_MP2_SYNC_DCM 0x10222274U

enum mt6797_cpuclk_domain {
	MT6797_CPUCLK_MP0,
	MT6797_CPUCLK_MP1,
	MT6797_CPUCLK_MP2,
	MT6797_CPUCLK_CCI,
	MT6797_CPUCLK_NR_DOMAINS,
};

enum mt6797_cpuclk_source {
	MT6797_CPUCLK_CLK26M,
	MT6797_CPUCLK_ARMPLL,
	MT6797_CPUCLK_MAINPLL,
	MT6797_CPUCLK_UNIVPLL,
	MT6797_CPUCLK_NR_SOURCES,
};

struct mt6797_cpuclk_snapshot {
	u32 armpll_con1[MT6797_CPUCLK_NR_DOMAINS];
	u32 mp2_armpll_con0;
	u32 mp2_armpll_con1;
	u32 muxsel;
	u32 clkdiv;
	u32 sync_dcm;
};

struct mt6797_cpuclk_state {
	u64 armpll_rate;
	u8 source;
	u8 divider;
};

struct mt6797_cpuclk_dcm_plan {
	u32 value[3];
};

/* One CPU-rate transaction: DCM, optional CKDIV, FHCTL, CKDIV, DCM. */
struct mt6797_cpuclk_transition_plan {
	enum mt6797_cpuclk_domain domain;
	unsigned long clk26m_rate;
	u64 initial_rate;
	unsigned long rate;
	unsigned long pll_rate;
	unsigned long minimum_rate;
	u32 dds;
	bool changes_postdiv;
	u32 target_con1;
	u32 temporary_clkdiv;
	u32 mainpll_mux;
	u32 mux_mask;
	unsigned long mainpll_rate;
	u32 clkdiv_mask;
	u32 clkdiv;
	bool divider_before_hop;
	struct mt6797_cpuclk_dcm_plan before;
	struct mt6797_cpuclk_dcm_plan after;
	struct mt6797_cpuclk_snapshot snapshot;
};

struct mt6797_cpuclk_backend_ops {
	unsigned long (*mainpll_rate)(void *context);
	int (*snapshot)(void *context, struct mt6797_cpuclk_snapshot *snapshot);
	int (*transition)(void *context,
			  const struct mt6797_cpuclk_transition_plan *plan);
};

struct mt6797_cpuclk_backend {
	const struct mt6797_cpuclk_backend_ops *ops;
	void *context;
};

int mt6797_cpuclk_decode_state(enum mt6797_cpuclk_domain domain,
			       const struct mt6797_cpuclk_snapshot *snapshot,
			       unsigned long clk26m_rate,
			       struct mt6797_cpuclk_state *state);
bool mt6797_cpuclk_secure_read_allowed(unsigned long fid, u32 address);
bool mt6797_cpuclk_snapshots_equal(const struct mt6797_cpuclk_snapshot *first,
				   const struct mt6797_cpuclk_snapshot *second);
int mt6797_cpuclk_validate_cold_mp2(const struct mt6797_cpuclk_snapshot *snapshot,
				 unsigned long reference_rate);
int mt6797_cpuclk_validate_stopped_mp2(
		const struct mt6797_cpuclk_snapshot *running,
		const struct mt6797_cpuclk_snapshot *stopped,
		unsigned long reference_rate);
unsigned long mt6797_cpuclk_apply_divider(unsigned long parent_rate,
					  u8 divider);
int mt6797_cpuclk_plan_pll_rate(enum mt6797_cpuclk_domain domain,
				unsigned long rate,
				unsigned long parent_rate, u32 initial_con1,
				u32 *target_con1, unsigned long *actual_rate);
int mt6797_cpuclk_plan_mux(enum mt6797_cpuclk_domain domain, u8 source,
			   u32 initial_mux, u32 *target_mux);
int mt6797_cpuclk_plan_dcm(enum mt6797_cpuclk_domain domain,
			   unsigned long rate, u32 initial_dcm,
			   struct mt6797_cpuclk_dcm_plan *plan);
int mt6797_cpuclk_plan_transition(enum mt6797_cpuclk_domain domain,
			   const struct mt6797_cpuclk_snapshot *snapshot,
			   unsigned long clk26m_rate, unsigned long rate,
			   struct mt6797_cpuclk_transition_plan *plan);
/* A nonzero measured MAINPLL rate permits a bounded temporary handoff. */
int mt6797_cpuclk_plan_transition_mainpll(enum mt6797_cpuclk_domain domain,
			   const struct mt6797_cpuclk_snapshot *snapshot,
			   unsigned long clk26m_rate, unsigned long rate,
			   unsigned long mainpll_rate,
			   struct mt6797_cpuclk_transition_plan *plan);

#endif /* __DRV_CLK_MT6797_CPU_H */
