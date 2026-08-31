/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only evidence from the separately gated MT6797 boot-status observer. */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_BOOT_STATUS_OBSERVE_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_BOOT_STATUS_OBSERVE_H

#include <linux/types.h>

struct mt6797_md1_boot_status_evidence {
	u8 stats0_samples;
	u8 stats1_samples;
	u32 stats0_final;
	u32 stats1_final;
	bool observed;
};

/* Returns -ENODATA until the independent observer has completed all reads. */
int mt6797_md1_boot_status_evidence_get(
	struct mt6797_md1_boot_status_evidence *evidence);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_BOOT_STATUS_OBSERVE_H */
