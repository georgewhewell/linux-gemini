#!/usr/bin/env python3
"""Compile the actual MD1 SRCCLKENA writers against the vendor MD3 mask.

Vendor eccci/mt6797/cldma_platform.c md_cd_power_on() with
CONFIG_MTK_MD3_SUPPORT=2 (research/vendor-kernel.config) computes
(value & ~0x92) | 0x29 on INFRA_AO 0xF0C.  Both mainline writers must match it
for every low byte and must leave bits 8-31 untouched.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
cold = (root / 'drivers/soc/mediatek/mt6797-md1-cold-owner.c').read_text()
start = cold.index('static int mt6797_md1_cold_write_value(')
body = cold[start:cold.index('static int mt6797_md1_cold_pll_start(')]
backend = (root / 'drivers/soc/mediatek/mt6797-md1-bringup-backend-core.c').read_text()
bstart = backend.index('int mt6797_md1_bringup_backend_enable_srcclk(')
bbody = backend[bstart:backend.index('int mt6797_md1_bringup_backend_pll_poll(')]
program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define __iomem
#define MT6797_MD1_INFRA_SRC_CLKENA 0xf0c
enum { MT6797_MD1_RES_INFRA, MT6797_MD1_RES_MAX };
static u32 regs[0x1000 / 4]; static int writes;
static u32 readl(const void __iomem *a) { return *(const volatile u32 *)a; }
static void writel(u32 v, void __iomem *a) { writes++; *(volatile u32 *)a = v; }
struct device;
#define dev_err(...) ((void)0)
struct mt6797_md1_cold_owner_backend { void __iomem *base[MT6797_MD1_RES_MAX]; };
struct mt6797_md1_bringup_backend { struct device *dev; void __iomem *infracfg;
	u32 srcclk_original; bool srcclk_valid; };
static int mt6797_md1_bringup_write_verify(void __iomem *base, u32 offset, u32 value)
{ writel(value, (char *)base + offset); return readl((char *)base + offset) == value ? 0 : -EIO; }
''' + body.replace('static int mt6797_md1_cold_write_value', 'static int __attribute__((unused)) mt6797_md1_cold_write_value', 1) + bbody + r'''
int main(void)
{
	struct mt6797_md1_cold_owner_backend c = { .base = { regs } };
	struct mt6797_md1_bringup_backend b = { .infracfg = regs };
	for (u32 low = 0; low < 256; low++) {
		const u32 hi[] = { 0, 0xff00, 0x55aa0000, 0xffffff00 };
		for (unsigned i = 0; i < 4; i++) {
			u32 before = hi[i] | low, want = (before & ~0x92u) | 0x29u;
			regs[0xf0c / 4] = before; writes = 0;
			assert(!mt6797_md1_cold_enable_srcclk(&c));
			assert(regs[0xf0c / 4] == want && writes == 1);
			assert((regs[0xf0c / 4] & 0x44u) == (before & 0x44u));
			regs[0xf0c / 4] = before;
			assert(!mt6797_md1_bringup_backend_enable_srcclk(&b));
			assert(regs[0xf0c / 4] == want && b.srcclk_original == before && b.srcclk_valid);
		}
	}
	/* Boot-status observer recorded srcclkena=0x00ffff on CI9 before power-on. */
	regs[0xf0c / 4] = 0xffff; assert(!mt6797_md1_cold_enable_srcclk(&c));
	assert(regs[0xf0c / 4] == 0xff6d);
	printf("PASS MD1 SRCCLKENA: cold owner and bringup backend match vendor MD3=2 mask for all low bytes; C2K bits 2/6 kept; 0xffff->0xff6d\n");
	return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-srcclkena-') as d:
    p = Path(d)
    (p / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True, timeout=5)
