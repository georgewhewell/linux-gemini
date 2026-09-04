/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_REMOTEPROC_MTK_MT6797_CONSYS_H
#define __LINUX_REMOTEPROC_MTK_MT6797_CONSYS_H

struct device;
struct mt6797_consys_fw_bundle;

int mt6797_consys_fw_request(struct device *dev,
			     struct mt6797_consys_fw_bundle **bundle);
void mt6797_consys_fw_release(struct mt6797_consys_fw_bundle *bundle);

#endif
