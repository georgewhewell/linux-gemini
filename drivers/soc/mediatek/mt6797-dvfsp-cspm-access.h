/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_DVFSP_CSPM_ACCESS_H
#define __MT6797_DVFSP_CSPM_ACCESS_H

#include <linux/atomic.h>
#include <linux/io.h>
#include <linux/mutex.h>

enum mt6797_dvfsp_access_permission {
	MT6797_DVFSP_PERMISSION_FAULTED = -1,
	MT6797_DVFSP_PERMISSION_BLOCKED,
	MT6797_DVFSP_PERMISSION_READY,
};

/*
 * One software arbitration point for every client of the CSPM mapping owned
 * by the handoff device.  The permission remains atomic because the I2C PM
 * path has a non-sleeping readiness check; every actual CSPM transaction is
 * nevertheless serialized by lock.
 */
struct mt6797_dvfsp_cspm_access {
	struct mutex lock; /* Serializes every admitted CSPM transaction. */
	atomic_t permission;
};

typedef int (*mt6797_dvfsp_cspm_access_fn)(void __iomem *base, void *context);
/* Called under the access lock after an -EUCLEAN result has been fenced. */
typedef void (*mt6797_dvfsp_cspm_terminal_fn)(void *context);

void mt6797_dvfsp_cspm_access_init(struct mt6797_dvfsp_cspm_access *access);
int mt6797_dvfsp_cspm_access_read(const struct mt6797_dvfsp_cspm_access *access);
int mt6797_dvfsp_cspm_access_set(struct mt6797_dvfsp_cspm_access *access,
					 int permission);
int mt6797_dvfsp_cspm_access_cmpxchg(struct mt6797_dvfsp_cspm_access *access,
				     int old, int new);
void mt6797_dvfsp_cspm_access_lock(struct mt6797_dvfsp_cspm_access *access);
void mt6797_dvfsp_cspm_access_unlock(struct mt6797_dvfsp_cspm_access *access);
void mt6797_dvfsp_cspm_access_assert_held(
	struct mt6797_dvfsp_cspm_access *access);
int mt6797_dvfsp_cspm_access_transaction(
	struct mt6797_dvfsp_cspm_access *access, void __iomem *base,
	mt6797_dvfsp_cspm_access_fn operation, void *context,
	mt6797_dvfsp_cspm_terminal_fn terminal, void *terminal_context);

#endif /* __MT6797_DVFSP_CSPM_ACCESS_H */
