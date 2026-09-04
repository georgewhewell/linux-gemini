// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 CSPM owner-side access arbitration.
 */

#include <linux/errno.h>

#include "mt6797-dvfsp-cspm-access.h"

void mt6797_dvfsp_cspm_access_init(struct mt6797_dvfsp_cspm_access *access)
{
	mutex_init(&access->lock);
	atomic_set(&access->permission, MT6797_DVFSP_PERMISSION_BLOCKED);
}

int mt6797_dvfsp_cspm_access_read(const struct mt6797_dvfsp_cspm_access *access)
{
	return atomic_read(&access->permission);
}

int mt6797_dvfsp_cspm_access_set(struct mt6797_dvfsp_cspm_access *access,
					 int permission)
{
	int permission_current;

	if (permission < MT6797_DVFSP_PERMISSION_FAULTED ||
	    permission > MT6797_DVFSP_PERMISSION_READY)
		return -EINVAL;

	for (;;) {
		permission_current = mt6797_dvfsp_cspm_access_read(access);
		if (permission_current == MT6797_DVFSP_PERMISSION_FAULTED &&
		    permission != MT6797_DVFSP_PERMISSION_FAULTED)
			return -EHOSTDOWN;
		if (atomic_cmpxchg(&access->permission, permission_current,
				   permission) == permission_current)
			return 0;
	}
}

int mt6797_dvfsp_cspm_access_cmpxchg(struct mt6797_dvfsp_cspm_access *access,
					     int old, int new)
{
	if (old == MT6797_DVFSP_PERMISSION_FAULTED &&
	    new != MT6797_DVFSP_PERMISSION_FAULTED)
		return MT6797_DVFSP_PERMISSION_FAULTED;

	return atomic_cmpxchg(&access->permission, old, new);
}

void mt6797_dvfsp_cspm_access_lock(struct mt6797_dvfsp_cspm_access *access)
{
	mutex_lock(&access->lock);
}

void mt6797_dvfsp_cspm_access_unlock(struct mt6797_dvfsp_cspm_access *access)
{
	mutex_unlock(&access->lock);
}

void mt6797_dvfsp_cspm_access_assert_held(
	struct mt6797_dvfsp_cspm_access *access)
{
	lockdep_assert_held(&access->lock);
}

int mt6797_dvfsp_cspm_access_transaction(
	struct mt6797_dvfsp_cspm_access *access, void __iomem *base,
	mt6797_dvfsp_cspm_access_fn operation, void *context,
	mt6797_dvfsp_cspm_terminal_fn terminal, void *terminal_context)
{
	int ret;

	if (!access || !base || !operation)
		return -EINVAL;

	mt6797_dvfsp_cspm_access_lock(access);
	if (mt6797_dvfsp_cspm_access_read(access) !=
	    MT6797_DVFSP_PERMISSION_READY) {
		ret = -EHOSTDOWN;
	} else {
		ret = operation(base, context);
		if (ret == -EUCLEAN) {
			/* Fence every later admission before publishing the fault. */
			mt6797_dvfsp_cspm_access_set(
				access, MT6797_DVFSP_PERMISSION_FAULTED);
			if (terminal)
				terminal(terminal_context);
		}
	}
	mt6797_dvfsp_cspm_access_unlock(access);

	return ret;
}
