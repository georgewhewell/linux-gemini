/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT6797_GPULDO_OWNER_H
#define MT6797_GPULDO_OWNER_H

/*
 * Fixed-profile sequencing policy. Caller holds the owner mutex, establishes
 * exact DT/resource/exclusive-supply admission, and supplies checked callbacks.
 * Any failure consumes this boot's owner: never restore unknown register state
 * or cut a rail after a possibly effective write. Recovery is a separate boot.
 */
struct mt6797_gpuldo_owner {
	bool admitted;
	bool configured;
	bool active;
	bool failed;
};

struct mt6797_gpuldo_ops {
	int (*write)(void *ctx, unsigned int reg, unsigned int value);
	int (*supply_enable)(void *ctx);
	int (*supply_disable)(void *ctx);
	int (*supply_is_enabled)(void *ctx);
	int (*all_domains_off)(void *ctx);
};

static inline int mt6797_gpuldo_fail(struct mt6797_gpuldo_owner *owner, int ret)
{
	owner->failed = true;
	return ret < 0 ? ret : -EIO;
}

/* Snapshot checks only: admission never initializes or disables hardware. */
static inline int mt6797_gpuldo_admit(struct mt6797_gpuldo_owner *owner,
		const struct mt6797_gpuldo_ops *ops, void *ctx)
{
	int ret;

	if (owner->admitted || owner->failed)
		return -EPERM;
	ret = ops->all_domains_off(ctx);
	if (ret != 1)
		return mt6797_gpuldo_fail(owner, ret < 0 ? ret : -EBUSY);
	ret = ops->supply_is_enabled(ctx);
	if (ret != 0)
		return mt6797_gpuldo_fail(owner, ret < 0 ? ret : -EBUSY);
	owner->admitted = true;
	return 0;
}

/* Called only for the outermost MFG_ASYNC domain, before its first clock. */
static inline int mt6797_gpuldo_enable(struct mt6797_gpuldo_owner *owner,
		const struct mt6797_gpuldo_ops *ops, void *ctx)
{
	static const unsigned int setup[][2] = {
		{ 0xfd8, 0x88888888 }, { 0xfe4, 0x8 },
		{ 0xfc0, 0x0f0f0f0f }, { 0xfc4, 0x0f0f0f0f }, { 0xfc8, 0xf },
	};
	unsigned int i;
	int ret;

	if (!owner->admitted || owner->failed || owner->active)
		return -EPERM;
	/* Recheck physical state before each new supply activation. */
	ret = ops->all_domains_off(ctx);
	if (ret != 1)
		return mt6797_gpuldo_fail(owner, ret < 0 ? ret : -EBUSY);
	ret = ops->supply_is_enabled(ctx);
	if (ret != 0)
		return mt6797_gpuldo_fail(owner, ret < 0 ? ret : -EBUSY);
	if (!owner->configured) {
		for (i = 0; i < sizeof(setup) / sizeof(setup[0]); ++i) {
			ret = ops->write(ctx, setup[i][0], setup[i][1]);
			if (ret)
				return mt6797_gpuldo_fail(owner, ret);
		}
		owner->configured = true;
	}
	ret = ops->write(ctx, 0xfbc, 0x1ff);
	if (ret)
		return mt6797_gpuldo_fail(owner, ret);
	ret = ops->supply_enable(ctx);
	if (ret)
		return mt6797_gpuldo_fail(owner, ret);
	owner->active = true;
	return 0;
}

/* Caller proves clocks off; every MFG domain must be physically off as well. */
static inline int mt6797_gpuldo_disable(struct mt6797_gpuldo_owner *owner,
		const struct mt6797_gpuldo_ops *ops, void *ctx)
{
	int ret;

	if (!owner->admitted || owner->failed || !owner->active)
		return -EPERM;
	ret = ops->all_domains_off(ctx);
	if (ret != 1)
		return mt6797_gpuldo_fail(owner, ret < 0 ? ret : -EBUSY);
	ret = ops->supply_disable(ctx);
	if (ret)
		return mt6797_gpuldo_fail(owner, ret);
	ret = ops->supply_is_enabled(ctx);
	if (ret != 0)
		return mt6797_gpuldo_fail(owner, ret < 0 ? ret : -EBUSY);
	ret = ops->write(ctx, 0xfbc, 0);
	if (ret)
		return mt6797_gpuldo_fail(owner, ret);
	owner->active = false;
	return 0;
}

#endif
