/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_POSTPATCH_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_POSTPATCH_H

struct device;
struct regulator;
struct mt6797_btif_stp_bootstrap_full_wmt_lease;

int mt6797_consys_wmt_postpatch_run(struct device *dev,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		struct regulator *bt_paldo, struct regulator *wifi_paldo);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_POSTPATCH_H */
