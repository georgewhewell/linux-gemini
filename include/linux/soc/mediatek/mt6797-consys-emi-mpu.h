/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6797 CONSYS shared-EMI MPU provider interface. */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_EMI_MPU_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_EMI_MPU_H

struct device;

/*
 * Claim the named MT6797 shared-EMI provider referenced by @consumer's
 * mediatek,emi-mpu phandle. A successful claim is intentionally permanent:
 * vendor provides no inverse for its region protection or remap operation.
 */
int mt6797_consys_emi_mpu_claim(struct device *consumer);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_EMI_MPU_H */
