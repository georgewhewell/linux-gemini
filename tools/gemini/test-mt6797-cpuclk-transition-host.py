#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production CPU transition transaction against a faultable model.

Checks register order, intermediate CPU/DCM limits, retries, stale plans and
fencing. FHCTL slew/register details have their own model test. Neither model
is evidence of hardware acceptance.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/clk/mediatek/clk-mt6797-cpu.c').read_text()

def extract(signature):
    start = source.rindex(signature)
    return source[start:source.index('\n}\n', start) + 3]

core = '\n'.join(re.sub(r'^#include .*\n', '', (root / f).read_text(),
                        flags=re.MULTILINE) for f in (
    'drivers/clk/mediatek/clk-mt6797-cpu.h',
    'drivers/clk/mediatek/clk-mt6797-cpu-core.c'))
functions = '\n'.join(extract(s) for s in (
    'static int\nmt6797_cpuclk_write_dcm(',
    'static int mt6797_cpuclk_write_divider(',
    'static int mt6797_cpuclk_transition_transaction(',
    'static int\nmt6797_cpuclk_observer_transition(',
    'static int mt6797_cpuclk_snapshot(',
    'static int mt6797_cpuclk_branch_determine_rate(',
    'static int mt6797_cpuclk_branch_set_rate(',
))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define min(a,b) ((a)<(b)?(a):(b))
#define BIT(n) (1UL << (n))
#define GENMASK(h,l) ((~0UL << (l)) & (~0UL >> (63-(h))))
#define FIELD_GET(mask,v) (((v) & (mask)) >> __builtin_ctzl(mask))
#define FIELD_PREP(mask,v) (((v) << __builtin_ctzl(mask)) & (mask))
#define mult_frac(x,n,d) (((x)/(d))*(n)+(((x)%(d))*(n))/(d))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define div_u64(a,b) ((a)/(b))
#define __iomem
#define MT6797_CPUCLK_CLKDIV 0x274
#define MT6797_CPUCLK_SYNC_DCM 0x744
#define dev_err(...) ((void)0)
'''
model = r'''
struct mt6797_cpuclk_observer {
 void *dev, *handoff; int lock; unsigned char *mcumixed, *mcucfg; bool faulted;
};
struct mt6797_cpuclk_transition_transaction {
 struct mt6797_cpuclk_observer *observer;
 const struct mt6797_cpuclk_transition_plan *plan;
};
struct clk_hw { unsigned long rate; };
struct clk_rate_request { unsigned long rate, min_rate, max_rate, best_parent_rate; };
struct mt6797_cpuclk { struct mt6797_cpuclk_backend backend; };
struct mt6797_cpuclk_pll_hw {
 struct clk_hw hw; struct mt6797_cpuclk *provider; enum mt6797_cpuclk_domain domain;
};
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
static struct mt6797_cpuclk_snapshot live;
static unsigned char mixed[0x800], cfg[0x800];
static unsigned int operations, fail_at, release_error, acquire_error;
static unsigned int divider_refusals, divider_attempts, releases, stale, checks;
static unsigned long ceiling;
static unsigned int final_corruption;
static char trace[64];
static void check(void)
{
 struct mt6797_cpuclk_state state;
 assert(!mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP0, &live, 26000000, &state));
 unsigned long cpu = mt6797_cpuclk_apply_divider(state.armpll_rate, state.divider);
 assert(cpu <= ceiling);
 assert(((live.sync_dcm >> 9) & 31) <= cpu / 70000000);
 checks++;
}
static int step(char c)
{
 assert(operations + 1 < sizeof(trace));
 trace[operations++] = c;
 trace[operations] = 0;
 return operations == fail_at;
}
static u32 readl(const volatile void *p)
{
 if (p == mixed + MT6797_CPUCLK_CLKDIV) return live.clkdiv;
 assert(p == cfg + MT6797_CPUCLK_SYNC_DCM); return live.sync_dcm;
}
static void writel(u32 value, volatile void *p)
{
 assert(p == mixed + MT6797_CPUCLK_CLKDIV);
 divider_attempts++;
 if (divider_refusals) { divider_refusals--; step('X'); return; }
 live.clkdiv = value;
 step('V'); check();
}
static void udelay(unsigned int us) { assert(us == 2); }
static int mt6797_cpuclk_secure_write_dcm(u32 value)
{
 live.sync_dcm = value;
 int fail = step('D'); check(); return fail ? -EIO : 0;
}
static int mt6797_cpuclk_lease_acquire(struct mt6797_cpuclk_observer *o, void *c)
{ (void)o; (void)c; return acquire_error ? -EBUSY : 0; }
static int mt6797_cpuclk_lease_release(struct mt6797_cpuclk_observer *o, void *c)
{ (void)o; (void)c; releases++; return release_error ? -EIO : 0; }
static int mt6797_cpuclk_read_stable(struct mt6797_cpuclk_observer *o,
 struct mt6797_cpuclk_snapshot *s)
{
 (void)o; *s = live; if (stale) s->muxsel ^= BIT(6);
 if (operations) {
  if (final_corruption==1) s->clkdiv ^= BIT(5);
  if (final_corruption==2) s->sync_dcm ^= BIT(9);
  if (final_corruption==3) s->armpll_con1[0] ^= BIT(0);
  if (final_corruption==4) return -EIO;
 }
 return 0;
}
static u32 mt6797_cpuclk_fhctl_channel(enum mt6797_cpuclk_domain d)
{ return d; }
static int mt6797_cpuclk_fhctl_init(struct mt6797_cpuclk_observer *o, u32 ch)
{ (void)o; assert(ch == 0); return 0; }
static void mutex_lock(int *lock) { assert(!*lock); *lock=1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock=0; }
static int mt6797_dvfsp_cspm_transaction(void *handoff,
 int (*fn)(void *, void *), void *context)
{ (void)handoff; return fn(NULL,context); }
static int mt6797_cpuclk_fhctl_hop(struct mt6797_cpuclk_observer *o,
 enum mt6797_cpuclk_domain d, u32 dds)
{
 (void)o; assert(d == 0);
 /* Exercise a partial hop on failure; never repair or raise CKDIV afterward. */
 int fail = step('H');
 if (fail) dds = (dds + (live.armpll_con1[0] & GENMASK(20,0))) / 2;
 live.armpll_con1[0] = (live.armpll_con1[0] & ~GENMASK(20,0)) | dds;
 check(); return fail ? -EIO : 0;
}
'''
main = r'''
static int sample(void *context, struct mt6797_cpuclk_snapshot *s)
{ return mt6797_cpuclk_read_stable(context,s); }
static const struct mt6797_cpuclk_backend_ops backend_ops = {
 .snapshot=sample, .transition=mt6797_cpuclk_observer_transition,
};
static const unsigned int rates[] = {221,338,481,624,715,806,897};
static const unsigned int divs[] = {4,2,2,1,1,1,1};
static struct mt6797_cpuclk_observer observer;
static struct mt6797_cpuclk_transition_plan plan;
static void reset(unsigned int i, unsigned int j)
{
 memset(&live,0,sizeof(live));
 unsigned long pll = rates[i] * divs[i] * 1000000UL;
 live.armpll_con1[0] = BIT(31) | BIT(24) | ((u64)pll * 32768 / 26000000);
 live.muxsel = 0x54;
 live.clkdiv = BIT(31) | (8 << 15) | (8 << 10) |
  ((divs[i] == 4 ? 11 : divs[i] == 2 ? 10 : 8) << 5) | 8;
 live.sync_dcm = (rates[i] / 70 << 9) | BIT(14) | (18 << 17) | BIT(22);
 observer = (struct mt6797_cpuclk_observer){.mcumixed=mixed,.mcucfg=cfg};
 operations=fail_at=release_error=acquire_error=divider_refusals=0;
 divider_attempts=releases=stale=final_corruption=0; trace[0]=0;
 ceiling = (rates[i] > rates[j] ? rates[i] : rates[j]) * 1000000UL;
 assert(!mt6797_cpuclk_plan_transition(0,&live,26000000,rates[j]*1000000UL,&plan));
}
static int run(void)
{
 int ret=mt6797_cpuclk_observer_transition(&observer,&plan);
 assert(!observer.lock); return ret;
}
int main(void)
{
 unsigned int pairs=0, faults=0;
 struct mt6797_cpuclk provider={.backend={.ops=&backend_ops,.context=&observer}};
 struct mt6797_cpuclk_pll_hw branch={.provider=&provider,.domain=MT6797_CPUCLK_MP0};
 for (unsigned int i=0;i<7;i++) for (unsigned int j=0;j<7;j++) {
  reset(i,j);
  unsigned long target=rates[j]*1000000UL;
  struct clk_rate_request req={.rate=target,.min_rate=target,.max_rate=target,
   .best_parent_rate=26000000};
  assert(!mt6797_cpuclk_branch_determine_rate(&branch.hw,&req));
  assert(!operations && req.rate==target && req.best_parent_rate==26000000);
  assert(!mt6797_cpuclk_branch_set_rate(&branch.hw,req.rate,req.best_parent_rate));
  assert(!observer.faulted && releases==1);
  const char *expected = divs[j]>divs[i] ? "DDDVHDDD" :
   divs[j]<divs[i] ? "DDDHVDDD" : "DDDHDDD";
  assert(!strcmp(trace,expected)); check(); pairs++;
  unsigned int length=operations;
  for (unsigned int n=1;n<=length;n++) {
   if (expected[n-1]=='V') continue; /* Divider refusal has its own checks. */
   reset(i,j); fail_at=n; assert(run()==-EIO);
   assert(observer.faulted && operations==n && releases==1); faults++;
  }
  if (divs[i]!=divs[j]) {
   reset(i,j); divider_refusals=1; assert(!run());
   assert(divider_attempts==2 && !observer.faulted);
   reset(i,j); divider_refusals=2; assert(run()==-EIO);
   assert(divider_attempts==2 && observer.faulted && releases==1);
   assert(trace[operations-1]=='X'); faults++;
  }
 }
 for (unsigned int c=1;c<=4;c++) {
  reset(3,2); final_corruption=c; assert(run()==-EIO && observer.faulted);
 }
 reset(3,2);
 struct clk_rate_request req={.rate=481000000,.min_rate=624000000,
  .max_rate=897000000,.best_parent_rate=26000000};
 assert(mt6797_cpuclk_branch_determine_rate(&branch.hw,&req)==-ERANGE);
 assert(!operations);
 req.rate=1118000000; req.min_rate=0; req.max_rate=2000000000;
 assert(mt6797_cpuclk_branch_determine_rate(&branch.hw,&req)==-EOPNOTSUPP);
 assert(!operations);
 reset(3,2); plan.changes_postdiv=true; assert(run()==-EOPNOTSUPP);
 assert(!operations && !releases);
 reset(3,2); observer.faulted=true; assert(run()==-EUCLEAN);
 assert(!operations && !releases);
 reset(3,2); stale=1; assert(run()==-EAGAIN);
 assert(!operations && !observer.faulted && releases==1);
 reset(3,2); acquire_error=1; assert(run()==-EBUSY);
 assert(!operations && !observer.faulted && !releases);
 reset(3,2); release_error=1; assert(run()==-EUCLEAN && observer.faulted);
 printf("PASS %u pairs, %u injected write failures, %u intermediate checks\n",pairs,faults,checks);
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-cpu-transition-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(prefix + core + model + functions + main)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', '-Wno-sign-compare', str(src),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
