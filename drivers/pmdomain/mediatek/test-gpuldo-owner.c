/* SPDX-License-Identifier: GPL-2.0-only */
/* Compile against the actual policy header; no copied implementation. */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "mt6797-gpuldo-owner.h"

struct mock {
	int off, enabled, fail_at, calls, nr;
	unsigned int reg[64], value[64];
};
static int step(struct mock *m)
{
	return ++m->calls == m->fail_at ? -EIO : 0;
}
static int write_reg(void *ctx, unsigned int reg, unsigned int value)
{
	struct mock *m = ctx;
	assert(m->nr < 64);
	m->reg[m->nr] = reg;
	m->value[m->nr++] = value;
	return step(m);
}
static int enable(void *ctx)
{
	struct mock *m = ctx;
	int ret = step(m);
	/* An error may still have changed hardware: never assume otherwise. */
	m->enabled = 1;
	m->reg[m->nr] = 0x10000;
	m->value[m->nr++] = 1;
	return ret;
}
static int disable(void *ctx)
{
	struct mock *m = ctx;
	int ret = step(m);
	if (!ret)
		m->enabled = 0;
	m->reg[m->nr] = 0x10000;
	m->value[m->nr++] = 0;
	return ret;
}
static int enabled(void *ctx) { return ((struct mock *)ctx)->enabled; }
static int off(void *ctx) { return ((struct mock *)ctx)->off; }
static const struct mt6797_gpuldo_ops ops = {
	write_reg, enable, disable, enabled, off,
};

int main(void)
{
	struct mt6797_gpuldo_owner owner;
	struct mock m;
	int i, before;
	const unsigned int regs[] = {0xfd8, 0xfe4, 0xfc0, 0xfc4, 0xfc8,
		0xfbc, 0x10000, 0x10000, 0xfbc};
	const unsigned int values[] = {0x88888888, 8, 0x0f0f0f0f,
		0x0f0f0f0f, 0xf, 0x1ff, 1, 0, 0};

	for (i = -2; i <= 1; ++i) {
		owner = (struct mt6797_gpuldo_owner){0};
		m = (struct mock){.off = i};
		assert((mt6797_gpuldo_admit(&owner, &ops, &m) == 0) == (i == 1));
		assert(m.nr == 0);
	}
	for (i = -2; i <= 2; ++i) {
		owner = (struct mt6797_gpuldo_owner){0};
		m = (struct mock){.off = 1, .enabled = i};
		assert((mt6797_gpuldo_admit(&owner, &ops, &m) == 0) == (i == 0));
		assert(m.nr == 0);
	}
	owner = (struct mt6797_gpuldo_owner){0};
	m = (struct mock){.off = 1};
	assert(mt6797_gpuldo_enable(&owner, &ops, &m) == -EPERM);
	assert(mt6797_gpuldo_admit(&owner, &ops, &m) == 0);
	assert(mt6797_gpuldo_enable(&owner, &ops, &m) == 0);
	assert(mt6797_gpuldo_enable(&owner, &ops, &m) == -EPERM);
	assert(mt6797_gpuldo_disable(&owner, &ops, &m) == 0);
	assert(m.nr == 9);
	assert(!memcmp(m.reg, regs, sizeof(regs)));
	assert(!memcmp(m.value, values, sizeof(values)));
	assert(mt6797_gpuldo_disable(&owner, &ops, &m) == -EPERM);
	assert(mt6797_gpuldo_enable(&owner, &ops, &m) == 0);
	assert(m.nr == 11 && m.reg[9] == 0xfbc && m.reg[10] == 0x10000);

	/* Every setup/enable/off failure is terminal, including effective errors. */
	for (i = 1; i <= 9; ++i) {
		owner = (struct mt6797_gpuldo_owner){0};
		m = (struct mock){.off = 1, .fail_at = i};
		assert(mt6797_gpuldo_admit(&owner, &ops, &m) == 0);
		if (i <= 7)
			assert(mt6797_gpuldo_enable(&owner, &ops, &m) < 0);
		else {
			assert(mt6797_gpuldo_enable(&owner, &ops, &m) == 0);
			assert(mt6797_gpuldo_disable(&owner, &ops, &m) < 0);
		}
		assert(owner.failed);
		before = m.nr;
		assert(mt6797_gpuldo_enable(&owner, &ops, &m) == -EPERM);
		assert(mt6797_gpuldo_disable(&owner, &ops, &m) == -EPERM);
		assert(m.nr == before);
	}
	/* A domain error latches before the parent can cut its supply. */
	owner = (struct mt6797_gpuldo_owner){0};
	m = (struct mock){.off = 1};
	assert(!mt6797_gpuldo_admit(&owner, &ops, &m));
	assert(!mt6797_gpuldo_enable(&owner, &ops, &m));
	assert(mt6797_gpuldo_fail(&owner, -ETIMEDOUT) == -ETIMEDOUT);
	before = m.nr;
	assert(mt6797_gpuldo_disable(&owner, &ops, &m) == -EPERM);
	assert(m.nr == before && m.enabled == 1);
	puts("GPULDO actual policy: admission/order/all-nine-write-failures/latch PASS");
	return 0;
}
