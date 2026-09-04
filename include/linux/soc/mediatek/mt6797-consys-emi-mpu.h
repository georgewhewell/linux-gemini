/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6797 CONSYS shared-EMI MPU provider interface. */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_EMI_MPU_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_EMI_MPU_H

struct device;
struct resource;

/*
 * Claim the named MT6797 shared-EMI provider referenced by @consumer's
 * mediatek,emi-mpu phandle. The first claim attempt consumes the provider for
 * this boot, whether it succeeds or fails: vendor provides no inverse for its
 * region protection or remap operation.
 */
int mt6797_consys_emi_mpu_claim(struct device *consumer);

/*
 * Temporarily grant region 18 over the first 512 KiB of the already-claimed
 * shared reservation. Only the opener may revoke the grant. Every SMC error
 * is terminal because the secure monitor may have changed protection before
 * reporting failure.
 */
int mt6797_consys_emi_mpu_wifi_window_open(struct device *consumer,
					   struct resource *window);
int mt6797_consys_emi_mpu_wifi_window_close(struct device *consumer);

/*
 * Record an uncertain post-claim consumer cleanup. This makes the provider
 * terminal for the remainder of the boot before the caller can release any
 * temporary module references.
 */
void mt6797_consys_emi_mpu_latch_fatal(void);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_EMI_MPU_H */
