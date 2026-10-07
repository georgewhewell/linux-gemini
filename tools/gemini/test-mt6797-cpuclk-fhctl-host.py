#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production MT6797 ARMPLL hopping sequence against a register model.

The FHCTL helpers are extracted verbatim from clk-mt6797-cpu.c and run on the
host with readl()/writel() bound to a small model of the MCUMIXED ARMPLL and
MCU FHCTL registers.  The model drives the PLL from CON1 while the channel's
HP_EN bit is clear and from the controller's MON value while it is set, and
records the PCW the PLL outputs after every register access.  The tests check
the vendor register order, that the output never leaves the interval between
the old and new DDS, and that returning to register control never changes the
running DDS, including when the slew stalls or the controller refuses the
PLL.  This models the register contract only; it is not hardware evidence.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/clk/mediatek/clk-mt6797-cpu.c').read_text()


def extract(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


defines = '\n'.join(line for line in source.splitlines()
                    if line.startswith(('#define MT6797_FHCTL_',
                                        '#define MT6797_ARMPLL_CHG',
                                        '#define MT6797_CPUCLK_MP0_PLL_CON1',
                                        '#define MT6797_CPUCLK_MP1_PLL_CON1',
                                        '#define MT6797_CPUCLK_CCI_PLL_CON1')))
functions = '\n'.join(extract(signature) for signature in (
    'static u32 mt6797_cpuclk_pll_offset(',
    'static u32 mt6797_cpuclk_fhctl_channel(',
    'static u32 mt6797_cpuclk_fhctl_read(',
    'static int mt6797_cpuclk_fhctl_write(',
    'static int mt6797_cpuclk_fhctl_update(',
    'static int mt6797_cpuclk_fhctl_init(',
    'static int mt6797_cpuclk_fhctl_hop(',
))

c = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((~0U) << (l)) & (~0U >> (31 - (h))))
enum mt6797_cpuclk_domain { MT6797_CPUCLK_MP0, MT6797_CPUCLK_MP1, MT6797_CPUCLK_MP2, MT6797_CPUCLK_CCI };
struct mt6797_cpuclk_observer { unsigned char *mcumixed; u32 fhctl_ready; };
''' + defines + r'''
static u32 regs[0x1000 / 4];
static int irq_off, stall_at_half, refuse_hp_en, accesses;
static u32 lo, hi, target, trace_len, hop_ch;
static char trace[4096];
static unsigned int reg_index(const volatile void *p)
{ return (unsigned int)((const unsigned char *)p - (unsigned char *)regs) / 4; }
static u32 channel_of(unsigned int off)
{ return off == MT6797_CPUCLK_MP0_PLL_CON1 ? 0 : 1; }
static u32 con1_of(u32 ch)
{ return ch == 2 ? MT6797_CPUCLK_CCI_PLL_CON1 :
 ch ? MT6797_CPUCLK_MP1_PLL_CON1 : MT6797_CPUCLK_MP0_PLL_CON1; }
/* The PCW the PLL currently outputs. */
static u32 output(u32 ch)
{
 if (regs[MT6797_FHCTL_HP_EN / 4] & BIT(ch))
  return regs[MT6797_FHCTL_MON(ch) / 4] & MT6797_FHCTL_DDS_MASK;
 return regs[con1_of(ch) / 4] & MT6797_FHCTL_DDS_MASK;
}
static void check(void)
{
 u32 pcw = output(hop_ch);
 if (lo || hi) assert(pcw >= lo && pcw <= hi);
 accesses++;
}
static void note(const char *what)
{ trace_len += snprintf(trace + trace_len, sizeof(trace) - trace_len, "%s ", what); }
static u32 readl(const volatile void *p)
{
 unsigned int off = reg_index(p) * 4;
 for (u32 ch = 0; ch < 3; ch++) {
  /* The controller slews one step towards DVFS on every MON sample. */
  if (off == MT6797_FHCTL_MON(ch) && (regs[MT6797_FHCTL_HP_EN / 4] & BIT(ch)) &&
      (regs[MT6797_FHCTL_CFG(ch) / 4] & MT6797_FHCTL_CFG_SFSTR_EN) &&
      (regs[MT6797_FHCTL_DVFS(ch) / 4] & MT6797_FHCTL_DDS_EN)) {
   u32 *mon = &regs[MT6797_FHCTL_MON(ch) / 4];
   u32 goal = regs[MT6797_FHCTL_DVFS(ch) / 4] & MT6797_FHCTL_DDS_MASK;
   u32 stop = stall_at_half ? (lo + hi) / 2 : goal;
   u32 step = 0x4000;
   if (*mon < goal && *mon < stop) *mon = *mon + step > stop ? stop : *mon + step;
   if (*mon > goal && *mon > stop) *mon = *mon - step < stop ? stop : *mon - step;
  }
 }
 check();
 return regs[off / 4];
}
static void writel(u32 v, volatile void *p)
{
 unsigned int off = reg_index(p) * 4;
 if (off == MT6797_FHCTL_HP_EN && refuse_hp_en) v = regs[off / 4];
 for (u32 ch = 0; ch < 3; ch++) {
  if (off == MT6797_FHCTL_CFG(ch) && v == 0) note("cfg0");
  if (off == MT6797_FHCTL_CFG(ch) && v) note("cfg-dvfs");
  if (off == MT6797_FHCTL_DDS(ch) && v) {
   note("dds");
   /* Loading the DDS register seeds the controller output. */
   regs[MT6797_FHCTL_MON(ch) / 4] = v & MT6797_FHCTL_DDS_MASK;
  }
  if (off == MT6797_FHCTL_DVFS(ch)) note("dvfs");
  if (off == con1_of(ch)) note("con1");
 }
 if (off == MT6797_FHCTL_SLOPE0) note("slope");
 if (off == MT6797_FHCTL_HP_EN) note(v ? "hp-on" : "hp-off");
 regs[off / 4] = v;
 check();
}
static void ndelay(unsigned long ns) { (void)ns; }
static void udelay(unsigned long us) { (void)us; }
static void mb(void) {}
#define local_irq_save(f) do { (f) = 1; assert(!irq_off); irq_off = 1; } while (0)
#define local_irq_restore(f) do { (void)(f); assert(irq_off); irq_off = 0; } while (0)
''' + functions + r'''
static struct mt6797_cpuclk_observer observer = { (unsigned char *)regs, 0 };
static void reset(u32 con1_ll, u32 con1_l)
{
 memset(regs, 0, sizeof(regs));
 regs[MT6797_CPUCLK_MP0_PLL_CON1 / 4] = con1_ll;
 regs[MT6797_CPUCLK_MP1_PLL_CON1 / 4] = con1_l;
 observer.fhctl_ready = 0;
 stall_at_half = refuse_hp_en = 0;
 trace_len = 0; trace[0] = 0; lo = hi = 0;
}
static void bounds(u32 ch, u32 a, u32 b)
{ hop_ch = ch; lo = a < b ? a : b; hi = a < b ? b : a; target = b; }
int main(void)
{
 const u32 boot_ll = 0xc1114000, boot_l = 0x400c4000;
 u32 con1;

 /* LL 897 -> 624 MHz within post divider /2. */
 reset(boot_ll, boot_l);
 assert(mt6797_cpuclk_fhctl_init(&observer, 0) == 0);
 assert(regs[MT6797_FHCTL_CLK_CON / 4] & BIT(0));
 assert(regs[MT6797_FHCTL_RST_CON / 4] & BIT(0));
 assert(observer.fhctl_ready == BIT(0));
 trace_len = 0; trace[0] = 0;
 bounds(0, 0x114000, 0xc0000);
 assert(mt6797_cpuclk_fhctl_hop(&observer, MT6797_CPUCLK_MP0, 0xc0000) == 0);
 assert(!strcmp(trace, "cfg0 dds cfg-dvfs slope hp-on dvfs con1 hp-off cfg0 "));
 con1 = regs[MT6797_CPUCLK_MP0_PLL_CON1 / 4];
 assert((con1 & MT6797_FHCTL_DDS_MASK) == 0xc0000);
 assert((con1 & GENMASK(26, 24)) == (boot_ll & GENMASK(26, 24)));
 assert(con1 & MT6797_ARMPLL_CHG);
 assert(!(regs[MT6797_FHCTL_HP_EN / 4] & BIT(0)));
 assert(!regs[MT6797_FHCTL_CFG(0) / 4]);
 assert(regs[MT6797_FHCTL_SLOPE0 / 4] == MT6797_FHCTL_SLOPE0_VALUE);
 /* The other cluster is untouched. */
 assert(regs[MT6797_CPUCLK_MP1_PLL_CON1 / 4] == boot_l);
 assert(!irq_off);

 /* L 1274 -> 1209 MHz, then back up to 1274 MHz. */
 reset(boot_ll, boot_l);
 assert(mt6797_cpuclk_fhctl_init(&observer, 1) == 0);
 bounds(1, 0xc4000, 0xba000);
 assert(mt6797_cpuclk_fhctl_hop(&observer, MT6797_CPUCLK_MP1, 0xba000) == 0);
 assert((regs[MT6797_CPUCLK_MP1_PLL_CON1 / 4] & MT6797_FHCTL_DDS_MASK) == 0xba000);
 bounds(1, 0xba000, 0xc4000);
 assert(mt6797_cpuclk_fhctl_hop(&observer, MT6797_CPUCLK_MP1, 0xc4000) == 0);
 assert((regs[MT6797_CPUCLK_MP1_PLL_CON1 / 4] & MT6797_FHCTL_DDS_MASK) == 0xc4000);
 assert(regs[MT6797_CPUCLK_MP0_PLL_CON1 / 4] == boot_ll);

 /* A stalled slew is handed back at the reached DDS and reported. */
 reset(boot_ll, boot_l);
 assert(mt6797_cpuclk_fhctl_init(&observer, 0) == 0);
 stall_at_half = 1;
 bounds(0, 0x114000, 0xc0000);
 assert(mt6797_cpuclk_fhctl_hop(&observer, MT6797_CPUCLK_MP0, 0xc0000) == -ETIMEDOUT);
 con1 = regs[MT6797_CPUCLK_MP0_PLL_CON1 / 4] & MT6797_FHCTL_DDS_MASK;
 assert(con1 == (regs[MT6797_FHCTL_MON(0) / 4] & MT6797_FHCTL_DDS_MASK));
 assert(con1 > 0xc0000 && con1 < 0x114000);
 assert(!(regs[MT6797_FHCTL_HP_EN / 4] & BIT(0)));

 /* A controller that never takes the PLL leaves CON1 alone. */
 reset(boot_ll, boot_l);
 assert(mt6797_cpuclk_fhctl_init(&observer, 0) == 0);
 refuse_hp_en = 1;
 bounds(0, 0x114000, 0x114000);
 assert(mt6797_cpuclk_fhctl_hop(&observer, MT6797_CPUCLK_MP0, 0xc0000) == -EIO);
 assert(regs[MT6797_CPUCLK_MP0_PLL_CON1 / 4] == boot_ll);
 assert(!regs[MT6797_FHCTL_CFG(0) / 4]);

 /* A channel already driving its PLL is not reset. */
 reset(boot_ll, boot_l);
 regs[MT6797_FHCTL_HP_EN / 4] = BIT(1);
 regs[MT6797_FHCTL_MON(1) / 4] = boot_l & MT6797_FHCTL_DDS_MASK;
 assert(mt6797_cpuclk_fhctl_init(&observer, 1) == -EBUSY);
 assert(!regs[MT6797_FHCTL_CLK_CON / 4] && !observer.fhctl_ready);

 /* CCI channel 2 must leave both CPU PLLs and their channel bits alone. */
 for (unsigned int fail=0; fail<2; fail++) {
  reset(boot_ll,boot_l);
  regs[MT6797_CPUCLK_CCI_PLL_CON1/4]=0x81114000;
  assert(!mt6797_cpuclk_fhctl_init(&observer,2));
  bounds(2,0x114000,0xf0000); stall_at_half=fail;
  assert(mt6797_cpuclk_fhctl_hop(&observer,MT6797_CPUCLK_CCI,0xf0000)==
   (fail ? -ETIMEDOUT : 0));
  assert(regs[MT6797_CPUCLK_MP0_PLL_CON1/4]==boot_ll);
  assert(regs[MT6797_CPUCLK_MP1_PLL_CON1/4]==boot_l);
  assert(!(regs[MT6797_FHCTL_HP_EN/4]&BIT(2)));
 }
 printf("mt6797-cpuclk-fhctl: pass (%d modelled register accesses)\n", accesses);
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='gemini-cpuclk-fhctl-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(c)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', '-Wno-unused-function',
                    '-o', str(exe), str(src)], check=True)
    subprocess.run([str(exe)], check=True)
