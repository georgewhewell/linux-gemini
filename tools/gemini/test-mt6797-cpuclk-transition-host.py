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
    'static u32 mt6797_cpuclk_pll_offset(',
    'static u32 mt6797_cpuclk_fhctl_channel(',
    'static int\nmt6797_cpuclk_write_dcm(',
    'static int mt6797_cpuclk_write_divider(',
    'static int\nmt6797_cpuclk_write_mux(',
    'static int mt6797_cpuclk_postdivider_switch(',
    'static int mt6797_cpuclk_transition_transaction(',
    'static int\nmt6797_cpuclk_observer_transition(',
    'static int mt6797_cpuclk_snapshot(',
    'static void mt6797_cpuclk_release_mainpll_rate(',
    'static int mt6797_cpuclk_provider_plan(',
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
#define MT6797_FHCTL_DDS_MASK GENMASK(20,0)
#define MT6797_FHCTL_HP_EN 0
#define MT6797_CPUCLK_MP0_PLL_CON1 0x204
#define MT6797_CPUCLK_MP1_PLL_CON1 0x214
#define MT6797_CPUCLK_CCI_PLL_CON1 0x224
#define MT6797_CPUCLK_MUXSEL 0x270
#define MT6797_CPUCLK_CLKDIV 0x274
#define MT6797_CPUCLK_SYNC_DCM 0x744
#define dev_err(...) ((void)0)
'''
model = r'''
struct mt6797_cpuclk_observer {
 void *topckgen, *mainpll; bool mainpll_rate_locked;
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
static unsigned long ceiling, fallback_rate;
static unsigned int active_domain, fallback_setting, rate_refs, rate_get_error;
static unsigned int hp_en, top_read_error, top_updates, top_partial;
static unsigned int refuse_final_divider, cspm_error;
static unsigned int final_corruption;
static char trace[64];
static void check(void)
{
 struct mt6797_cpuclk_state state;
 assert(!mt6797_cpuclk_decode_state(active_domain, &live, 26000000, &state));
 unsigned long parent = state.source == MT6797_CPUCLK_MAINPLL ?
  fallback_rate : state.armpll_rate;
 if (state.source == MT6797_CPUCLK_MAINPLL) {
  assert(fallback_setting == 3 && rate_refs == 1);
 }
 unsigned long cpu = mt6797_cpuclk_apply_divider(parent, state.divider);
 assert(cpu <= ceiling);
 assert(((live.sync_dcm >> (active_domain==3 ? 1 : active_domain ? 17 : 9)) & 31) <= cpu / 70000000);
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
 if (p == mixed + MT6797_CPUCLK_MUXSEL) return live.muxsel;
 if (p == mixed + 0x204) return live.armpll_con1[0];
 if (p == mixed + 0x214) return live.armpll_con1[1];
 if (p == mixed + 0x224) return live.armpll_con1[3];
 assert(p == cfg + MT6797_CPUCLK_SYNC_DCM); return live.sync_dcm;
}
static void writel(u32 value, volatile void *p)
{
 if (p == mixed + MT6797_CPUCLK_MUXSEL) {
  if (!step('M')) live.muxsel=value;
  check(); return;
 }
 if (p == mixed + 0x204 || p == mixed + 0x214 || p == mixed + 0x224) {
  unsigned int d = p == mixed + 0x224 ? 3 : p == mixed + 0x214;
  assert(((live.muxsel >> (d==3 ? 6 : 2 + 2*d)) & 3) == MT6797_CPUCLK_MAINPLL);
  if (!step('P')) live.armpll_con1[d]=value;
  check(); return;
 }
 assert(p == mixed + MT6797_CPUCLK_CLKDIV);
 divider_attempts++;
 if (refuse_final_divider && divider_attempts>1) { step('X'); return; }
 if (divider_refusals) { divider_refusals--; step('X'); return; }
 live.clkdiv = value;
 step('V'); check();
}
static void udelay(unsigned int us) { assert(us == 2 || us == 20); }
static void ndelay(unsigned int ns) { assert(ns == 200); }
static unsigned long clk_get_rate(void *clk)
{ assert(clk); return fallback_rate; }
static int clk_rate_exclusive_get(void *clk)
{ assert(clk && !rate_refs); if (rate_get_error) return -EBUSY; rate_refs++; return 0; }
static void clk_rate_exclusive_put(void *clk)
{ assert(clk && rate_refs==1); rate_refs--; }
static u32 mt6797_cpuclk_pll_offset(enum mt6797_cpuclk_domain d);
static u32 mt6797_cpuclk_fhctl_read(struct mt6797_cpuclk_observer *o, u32 offset)
{ (void)o; assert(offset==MT6797_FHCTL_HP_EN); return hp_en; }
static int mt6797_topckgen_cpu_read(void *top, u8 *value)
{ assert(top); *value=fallback_setting; return top_read_error ? -EIO : 0; }
static int mt6797_topckgen_cpu_update(void *top, u8 expected, u8 value)
{
 assert(top && fallback_setting==expected); top_updates++;
 if (step('T')) { if (top_partial) fallback_setting=value; return -EIO; }
 fallback_setting=value; check(); return 0;
}
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
  if (final_corruption==3) s->armpll_con1[active_domain] ^= BIT(0);
  if (final_corruption==4) return -EIO;
 }
 return 0;
}
static u32 mt6797_cpuclk_fhctl_channel(enum mt6797_cpuclk_domain d);
static int mt6797_cpuclk_fhctl_init(struct mt6797_cpuclk_observer *o, u32 ch)
{ (void)o; assert(ch == (active_domain==3 ? 2 : active_domain)); return 0; }
static void mutex_lock(int *lock) { assert(!*lock); *lock=1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock=0; }
static int mt6797_dvfsp_cspm_transaction(void *handoff,
 int (*fn)(void *, void *), void *context)
{ (void)handoff; return cspm_error ? -EHOSTDOWN : fn(NULL,context); }
static int mt6797_cpuclk_fhctl_hop(struct mt6797_cpuclk_observer *o,
 enum mt6797_cpuclk_domain d, u32 dds)
{
 (void)o; assert(d == active_domain);
 /* Exercise a partial hop on failure; never repair or raise CKDIV afterward. */
 int fail = step('H');
 if (fail) dds = (dds + (live.armpll_con1[d] & GENMASK(20,0))) / 2;
 live.armpll_con1[d] = (live.armpll_con1[d] & ~GENMASK(20,0)) | dds;
 check(); return fail ? -EIO : 0;
}
'''
main = r'''
static int sample(void *context, struct mt6797_cpuclk_snapshot *s)
{ return mt6797_cpuclk_read_stable(context,s); }
static unsigned long mainpll_rate(void *context)
{ struct mt6797_cpuclk_observer *o=context; return o->mainpll ? fallback_rate : 0; }
static const struct mt6797_cpuclk_backend_ops backend_ops = {
 .mainpll_rate=mainpll_rate, .snapshot=sample, .transition=mt6797_cpuclk_observer_transition,
};
static const unsigned int rates[] = {221,338,481,624,715,806,897};
static const unsigned int divs[] = {4,2,2,1,1,1,1};
static struct mt6797_cpuclk_observer observer;
static struct mt6797_cpuclk_transition_plan plan;
static void reset(unsigned int i, unsigned int j)
{
 active_domain=0; fallback_rate=1092000000;
 refuse_final_divider=cspm_error=top_partial=0;
 fallback_setting=rate_refs=rate_get_error=hp_en=top_read_error=top_updates=0;
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
static const unsigned int l_rates[] = {325,468,650,832,962,1092,1209};
static void reset_l(unsigned int i, unsigned int j, unsigned int saved)
{
 reset(3,3); active_domain=1; fallback_setting=saved;
 observer.mainpll=observer.topckgen=&observer;
 unsigned int div = i<2 ? 2 : 1, pos = i<5 ? 1 : 0;
 unsigned long pll=l_rates[i]*div*1000000UL;
 live.armpll_con1[1]=BIT(31) | (pos<<24) |
  ((u64)pll * (1U << (14+pos)) / 26000000);
 live.clkdiv=(live.clkdiv & ~(31U<<10)) | ((div==2 ? 10 : 8)<<10);
 live.sync_dcm=(live.sync_dcm & ~(31U<<17)) | (l_rates[i]/70<<17);
 ceiling=(l_rates[i]>l_rates[j] ? l_rates[i] : l_rates[j])*1000000UL;
 assert(!mt6797_cpuclk_plan_transition_mainpll(1,&live,26000000,
  l_rates[j]*1000000UL,fallback_rate,&plan));
}
static const unsigned int cci_rates[]={169,234,325,416,481,533,611,676,
 754,819,845,884,910,936,975,988};
static void reset_cci(unsigned int i,unsigned int j)
{
 reset(3,3); active_domain=MT6797_CPUCLK_CCI;
 unsigned int div=i<2 ? 4 : i<6 ? 2 : 1;
 unsigned long pll=cci_rates[i]*div*1000000UL;
 live.armpll_con1[3]=BIT(31)|BIT(24)|((u64)pll*32768/26000000);
 live.armpll_con1[1]=0x800c4000;
 live.clkdiv=(live.clkdiv & ~(31U<<15))|((div==4 ? 11 : div==2 ? 10 : 8)<<15);
 live.sync_dcm |= (cci_rates[i]/70<<1)|BIT(6);
 ceiling=(cci_rates[i]>cci_rates[j] ? cci_rates[i] : cci_rates[j])*1000000UL;
 assert(!mt6797_cpuclk_plan_transition(3,&live,26000000,
  cci_rates[j]*1000000UL,&plan));
 assert(!plan.changes_postdiv);
 if (i>j) {
  assert(!(plan.before.value[0] & (31U<<1)));
  assert(!(plan.after.value[2] & (31U<<1)));
 }
}
static void reset_cci_post(unsigned int saved)
{
 reset_cci(15,0); observer.mainpll=observer.topckgen=&observer;
 fallback_setting=saved;
 live.armpll_con1[3]=BIT(31)|((u64)988000000*16384/26000000);
 assert(!mt6797_cpuclk_plan_transition_mainpll(3,&live,26000000,
  169000000,fallback_rate,&plan));
 assert(plan.changes_postdiv);
}
static void assert_fenced(void)
{
 assert(observer.faulted && observer.mainpll_rate_locked && rate_refs==1);
 unsigned int old_operations=operations;
 assert(run()==-EUCLEAN && operations==old_operations);
 /* Teardown releases the retained reference; no retry changes hardware. */
 mt6797_cpuclk_release_mainpll_rate(&observer);
 assert(!rate_refs);
 observer.mainpll_rate_locked=false;
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
 unsigned int l_pairs=0, post_faults=0;
 branch.domain=MT6797_CPUCLK_MP1;
 for (unsigned int saved=0;saved<4;saved++)
 for (unsigned int i=0;i<7;i++) for (unsigned int j=0;j<7;j++) {
  reset_l(i,j,saved);
  unsigned long target=l_rates[j]*1000000UL;
  struct clk_rate_request lr={.rate=target,.min_rate=target,.max_rate=target,
   .best_parent_rate=26000000};
  assert(!mt6797_cpuclk_branch_determine_rate(&branch.hw,&lr));
  assert(!mt6797_cpuclk_branch_set_rate(&branch.hw,lr.rate,lr.best_parent_rate));
  assert(!observer.faulted && !rate_refs && !observer.mainpll_rate_locked);
  assert(fallback_setting==saved && releases==1);
  assert(top_updates==(plan.changes_postdiv ? 2 : 0));
  struct mt6797_cpuclk_state state;
  assert(!mt6797_cpuclk_decode_state(1,&live,26000000,&state));
  assert(state.source==MT6797_CPUCLK_ARMPLL);
  assert(mt6797_cpuclk_apply_divider(state.armpll_rate,state.divider)==target);
  l_pairs++;
  if (!plan.changes_postdiv) continue;
  assert(!strcmp(trace,"DDDTVMPMVDDDT"));
  unsigned int length=operations;
  char success[64]; strcpy(success,trace);
  for (unsigned int n=1;n<=length;n++) {
   if (success[n-1]=='V') continue;
   reset_l(i,j,saved); fail_at=n; assert(run()==-EIO);
   assert(operations==n && releases==1);
   if (n>4) assert(fallback_setting==3);
   assert_fenced(); post_faults++;
  }
  reset_l(i,j,saved); divider_refusals=1;
  assert(!run() && divider_attempts==3 && !rate_refs);
  reset_l(i,j,saved); divider_refusals=2;
  assert(run()==-EIO && divider_attempts==2 && fallback_setting==3);
  assert_fenced(); post_faults++;
  reset_l(i,j,saved); refuse_final_divider=1;
  assert(run()==-EIO && divider_attempts==3 && fallback_setting==3);
  assert_fenced(); post_faults++;
 }
 for (unsigned int n=4;n<=13;n+=9) {
  reset_l(6,0,0); top_partial=1; fail_at=n;
  assert(run()==-EIO); assert_fenced();
 }
 reset_l(6,0,0); cspm_error=1;
 assert(run()==-EHOSTDOWN && !operations && !rate_refs && !observer.faulted);
 for (unsigned int c=1;c<=4;c++) {
  reset_l(6,0,0); final_corruption=c;
  assert(run()==-EIO && fallback_setting==3); assert_fenced();
 }
 reset_l(6,0,0); fallback_rate++;
 assert(run()==-EAGAIN && !operations && !rate_refs && !observer.faulted);
 reset_l(6,0,0); hp_en=BIT(1);
 assert(run()==-EBUSY && !operations && !rate_refs && !observer.faulted);
 reset_l(6,0,0); top_read_error=1;
 assert(run()==-EIO && !operations && !rate_refs && !observer.faulted);
 reset_l(6,0,0); rate_get_error=1;
 assert(run()==-EBUSY && !operations && !releases && !rate_refs);
 reset_l(6,0,0); acquire_error=1;
 assert(run()==-EBUSY && !operations && !releases && !rate_refs);
 reset_l(6,0,0); stale=1;
 assert(run()==-EAGAIN && !operations && !rate_refs);
 reset_l(6,0,0); release_error=1;
 assert(run()==-EUCLEAN); assert_fenced();
 for (unsigned int mux=0;mux<4;mux++) {
  if (mux==2) continue; /* The active MP1 path must start on ARMPLL. */
  reset_l(6,0,0);
  live.muxsel=(live.muxsel & ~(3U<<(2*mux))) | (2U<<(2*mux));
  plan.snapshot=live;
  assert(run()==-EUCLEAN && !operations && !rate_refs && !observer.faulted);
  reset_l(6,0,3);
  live.muxsel=(live.muxsel & ~(3U<<(2*mux))) | (2U<<(2*mux));
  plan.snapshot=live; plan.mainpll_mux=(live.muxsel & ~plan.mux_mask) |
   (MT6797_CPUCLK_MAINPLL<<4);
  assert(!run() && fallback_setting==3 && !rate_refs);
 }
 unsigned int cci_pairs=0,cci_faults=0;
 branch.domain=MT6797_CPUCLK_CCI;
 for(unsigned int i=0;i<16;i++) for(unsigned int j=0;j<16;j++) {
  reset_cci(i,j);
  struct mt6797_cpuclk_snapshot before=live;
  unsigned long target=cci_rates[j]*1000000UL;
  struct clk_rate_request cr={.rate=target,.min_rate=target,.max_rate=target,
   .best_parent_rate=26000000};
  assert(!mt6797_cpuclk_branch_determine_rate(&branch.hw,&cr));
  assert(!mt6797_cpuclk_branch_set_rate(&branch.hw,cr.rate,cr.best_parent_rate));
  assert(!observer.faulted && releases==1 && !rate_refs);
  struct mt6797_cpuclk_state state;
  assert(!mt6797_cpuclk_decode_state(3,&live,26000000,&state));
  assert(mt6797_cpuclk_apply_divider(state.armpll_rate,state.divider)==target);
  assert(live.armpll_con1[0]==before.armpll_con1[0]);
  assert(live.armpll_con1[1]==before.armpll_con1[1]);
  assert(!((live.clkdiv^before.clkdiv)&~(31U<<15)));
  assert(!((live.sync_dcm^before.sync_dcm)&~(63U<<1)));
  cci_pairs++;
  unsigned int length=operations;
  char success[64]; strcpy(success,trace);
  for(unsigned int n=1;n<=length;n++) {
   if(success[n-1]=='V') continue;
   reset_cci(i,j); fail_at=n;
   assert(run()==-EIO && observer.faulted && releases==1);
   unsigned int old_operations=operations;
   assert(run()==-EUCLEAN && operations==old_operations);
   cci_faults++;
  }
 }
 for(unsigned int saved=0;saved<4;saved++) {
  reset_cci_post(saved); assert(!run());
  assert(fallback_setting==saved && !rate_refs);
  unsigned int length=operations;
  char success[64]; strcpy(success,trace);
  for(unsigned int n=1;n<=length;n++) {
   if(success[n-1]=='V') continue;
   reset_cci_post(saved); fail_at=n;
   assert(run()==-EIO && operations==n); assert_fenced(); cci_faults++;
  }
 }
 reset_cci_post(0); hp_en=BIT(2);
 assert(run()==-EBUSY && !operations && !observer.faulted && !rate_refs);
 reset_cci(15,0); divider_refusals=2;
 assert(run()==-EIO && observer.faulted && divider_attempts==2);
 reset_cci(15,0); stale=1;
 assert(run()==-EAGAIN && !operations && !observer.faulted);
 reset_cci(0,15); release_error=1;
 assert(run()==-EUCLEAN && observer.faulted);
 printf("PASS %u CCI pairs, %u injected failures, sibling registers preserved\n",
  cci_pairs,cci_faults);
 printf("PASS %u L pairs across 4 fallback states, %u postdivider failures\n",
  l_pairs,post_faults);
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
