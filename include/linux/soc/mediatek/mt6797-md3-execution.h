/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD3_EXECUTION_H
#define __MT6797_MD3_EXECUTION_H
#include <linux/types.h>
struct platform_device;
struct reset_control;
struct mt6797_md3_ccif;
struct mt6797_md3_execution;
/* Bounded first-boot observation, NOT a runtime/CSS service provider. All
 * resources are acquired before the existing one-boot power lease mutates HW.
 */
int mt6797_md3_execution_acquire(struct platform_device *pdev,
	struct reset_control *reset, struct mt6797_md3_ccif *ccif,
	struct mt6797_md3_execution **out);
int mt6797_md3_execution_run(struct mt6797_md3_execution *execution);
ssize_t mt6797_md3_execution_show(struct mt6797_md3_execution *execution, char *buf);
void mt6797_md3_execution_shutdown(struct mt6797_md3_execution *execution);
/* Alternative one-shot start: real HS1 validated, IRQ observer drained,
 * reset remains released under the retained lease for the actual service owner.
 * Failure always runs terminal reset. Caller MUST own services before calling.
 */
int mt6797_md3_execution_start(struct mt6797_md3_execution *execution);
int mt6797_md3_execution_query(struct mt6797_md3_execution *execution,
	u8 query[64], u16 *control_sequence);
#endif
