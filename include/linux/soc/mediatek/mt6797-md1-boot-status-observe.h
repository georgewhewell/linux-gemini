/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only evidence from the separately gated MT6797 boot-status observer. */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_BOOT_STATUS_OBSERVE_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_BOOT_STATUS_OBSERVE_H

#include <linux/types.h>

enum mt6797_md1_power_state {
	MT6797_MD1_POWER_INCONSISTENT,
	MT6797_MD1_POWER_COLD_OFF,
	MT6797_MD1_POWERED_DOMAIN_RESET_HELD,
	MT6797_MD1_POWERED_DOMAIN_RESET_DEASSERTED,
};

struct mt6797_md1_power_snapshot {
	u32 poweron_config;
	u32 pwr_status;
	u32 pwr_status_2nd;
	u32 md1_pwr_con;
	u32 md_ext_buck_iso;
	u32 infra_protect_en;
	u32 infra_protect_sta;
	u32 infra_srcclkena;
};

struct mt6797_md1_boot_status_evidence {
	u8 stats0_samples;
	u8 stats1_samples;
	u32 stats0_final;
	u32 stats1_final;
	struct mt6797_md1_power_snapshot power;
	enum mt6797_md1_power_state power_state;
	bool active_pg_cold_consistent;
	bool observed;
};

/* Returns -ENODATA until the independent observer has completed all reads. */
int mt6797_md1_boot_status_evidence_get(
	struct mt6797_md1_boot_status_evidence *evidence);

enum mt6797_md1_power_state
mt6797_md1_power_state_classify(const struct mt6797_md1_power_snapshot *snapshot,
				bool *cold_consistent);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_BOOT_STATUS_OBSERVE_H */
