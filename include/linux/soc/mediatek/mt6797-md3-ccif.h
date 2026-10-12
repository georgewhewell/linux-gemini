/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD3_CCIF_H
#define __MT6797_MD3_CCIF_H
#include <linux/types.h>
struct platform_device;
struct reset_control;
struct mt6797_md3_lk_snapshot;
struct mt6797_md3_ccif;
/* Preallocate exclusive resources; no CCIF/SMEM/reset/power write or clock/IRQ
 * enable. Interrupt-domain allocation may set controller routing/polarity.
 * Caller retains its reset/supplier lease and must stay bound after preparation.
 */
int mt6797_md3_ccif_acquire(struct platform_device *pdev,
			    const struct mt6797_md3_lk_snapshot *snapshot,
			    struct reset_control *reset,
			    struct mt6797_md3_ccif **out);
/* Exactly once after real domain-on admission, with TOPRGU reset held. Does
 * not enable IRQs, transmit, release reset, or publish any runtime readiness.
 */
int mt6797_md3_ccif_prepare(struct mt6797_md3_ccif *ccif);
int mt6797_md3_ccif_status(struct mt6797_md3_ccif *ccif);
/* Optional first-message observer: no ring consumption or runtime response.
 * Handler is hard-IRQ, must not sleep. Both IRQs are one-shot masked after
 * callback. stop() drains both before caller releases callback storage.
 */
typedef void (*mt6797_md3_ccif_observer)(void *data, bool wdt, void __iomem *ap);
int mt6797_md3_ccif_observe_start(struct mt6797_md3_ccif *ccif,
	mt6797_md3_ccif_observer observer, void *data);
void mt6797_md3_ccif_observe_stop(struct mt6797_md3_ccif *ccif);
#endif
