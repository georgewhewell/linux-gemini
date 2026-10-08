#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production MP2 access/SMC code with device and hardware doubles."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
s = (root / 'drivers/clk/mediatek/clk-mt6797-cpu.c').read_text()
a = s.index('enum mt6797_cpuclk_mp2_operation {')
api = s[a:s.index('\nmodule_platform_driver(', a)]
a = s.index('static int mt6797_cpuclk_secure_read(')
secure_read = s[a:s.index('\n}\n', a) + 3]
core = '\n'.join(re.sub(r'^#include .*\n', '', (root / f).read_text(),
                        flags=re.MULTILINE) for f in (
    'drivers/clk/mediatek/clk-mt6797-cpu.h',
    'drivers/clk/mediatek/clk-mt6797-cpu-core.c'))
c = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#define BIT(n) (1UL << (n))
#define GENMASK(h,l) ((~0UL << (l)) & (~0UL >> (63-(h))))
#define FIELD_GET(mask,v) (((v) & (mask)) >> __builtin_ctzl(mask))
#define FIELD_PREP(mask,v) (((v) << __builtin_ctzl(mask)) & (mask))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define div_u64(a,b) ((a)/(b))
#define mult_frac(x,n,d) (((x)/(d))*(n)+(((x)%(d))*(n))/(d))
#define U32_MAX UINT32_MAX
#define min(a,b) ((a)<(b)?(a):(b))
#define EPROBE_DEFER 517
#define CLK_MCU_MP2 2
#define MT6797_CPUCLK_SMC_MCUSYS_WRITE 0x82000201UL
#define IS_ERR_OR_NULL(p) (!(p))
#define __iomem
#define EXPORT_SYMBOL_GPL(x)
''' + core + r'''
struct device { void *of_node, *driver; };
struct platform_device { struct device dev; };
struct clk_hw { int tag; };
struct clk { struct clk_hw *hw; };
struct mt6797_cpuclk_observer { struct clk *i2c_clk, *mainpll; void *topckgen; bool mainpll_rate_locked; int lock; void *handoff; bool faulted;
 struct mt6797_cpuclk_snapshot mp2_running; bool mp2_restart_valid; };
struct mt6797_cpuclk {
 struct mt6797_cpuclk_observer observer;
 struct { struct clk_hw hw; } paths[4];
};
struct of_phandle_args { void *np; int args_count; unsigned args[1]; };
struct arm_smccc_res { unsigned long a0; };
static struct { int driver; } mt6797_cpuclk_driver;
static struct mt6797_cpuclk provider;
static struct platform_device pdev;
static struct clk_hw ref, wrong;
static struct clk bus, mainpll;
static int pins;
static struct mt6797_cpuclk_snapshot snap;
static int fail, locked, node_puts, device_puts, prepared, software, lease;
static int step, fail_step, corrupt_step, writes, global_fault, effect, high_status, corrupted;
static u32 dcm, written[4];
static bool boundary(void) { step++; return step == fail_step; }
static void ccf(void) { assert(locked && !provider.observer.lock && !software && !lease); }
static int of_property_match_string(void *n,const char *p,const char *v)
{ assert(n && !strcmp(p,"clock-names") && !strcmp(v,"mp2")); return fail==1 ? -ENOENT : 3; }
static bool of_device_is_compatible(void *n,const char *s)
{ assert(n && !strcmp(s,"mediatek,mt6797-a72-power")); return fail!=14; }
static int of_parse_phandle_with_args(void *n,const char *p,const char *cells,int i,struct of_phandle_args *a)
{ assert(n && !strcmp(p,"clocks") && !strcmp(cells,"#clock-cells") && i==3);
 a->np=n; a->args_count=fail==6?0:1; a->args[0]=fail==7?1:2; return fail==2?-EINVAL:0; }
static struct platform_device *of_find_device_by_node(void *n)
{ assert(n); return fail==3?NULL:&pdev; }
static void of_node_put(void *n) { assert(n); node_puts++; }
static void device_lock(struct device *d) { assert(d==&pdev.dev && !locked); locked=1; }
static void device_unlock(struct device *d)
{ assert(d==&pdev.dev && locked && !prepared && !provider.observer.lock); locked=0; }
static struct mt6797_cpuclk *platform_get_drvdata(struct platform_device *p)
{ assert(p==&pdev && locked); return fail==5?NULL:&provider; }
static struct clk_hw *__clk_get_hw(struct clk *c) { ccf(); return c->hw; }
static struct clk_hw *clk_hw_get_parent_by_index(struct clk_hw *h,int i)
{ ccf(); assert(h==&provider.paths[2].hw && i==0); return fail==9?NULL:&ref; }
static unsigned long clk_hw_get_rate(struct clk_hw *h)
{ ccf(); assert(h==&ref); return fail==10?0:26000000; }
static int clk_rate_exclusive_get(struct clk *c)
{ ccf(); assert(c==&mainpll); if(fail==15) return -EBUSY; pins++; return 0; }
static void clk_rate_exclusive_put(struct clk *c)
{ ccf(); assert(c==&mainpll && pins); pins--; }
static unsigned long clk_get_rate(struct clk *c)
{ ccf(); assert(c==&mainpll && pins); return 1092000000; }
static int clk_prepare_enable(struct clk *c)
{ ccf(); assert(c==&bus && !prepared); if(fail==11) return -EIO; prepared=1; return 0; }
static void clk_disable_unprepare(struct clk *c)
{ ccf(); assert(c==&bus && prepared); prepared=0; }
static void mutex_lock(int *m) { assert(prepared && !*m); *m=1; }
static void mutex_unlock(int *m) { assert(prepared && *m && !software && !lease); *m=0; }
static int mt6797_dvfsp_cspm_transaction(void *h,int (*fn)(void *,void *),void *ctx)
{ assert(h==&provider && prepared && provider.observer.lock && !software);
 if(fail==13) return -EHOSTDOWN;
 software=1; int r=fn(h,ctx); software=0; if(r==-EUCLEAN) global_fault=1; return r; }
static int mt6797_dvfsp_cspm_acquire(void *h)
{ assert(h==&provider && software && prepared && !lease);
 if(boundary()) return -EBUSY; lease=1; return 0; }
static int mt6797_dvfsp_cspm_release(void *h)
{ assert(h==&provider && software && prepared && lease); lease=0;
 return boundary() ? -EIO : 0; }
static int mt6797_cpuclk_read_stable(struct mt6797_cpuclk_observer *o,
 struct mt6797_cpuclk_snapshot *s)
{ assert(o==&provider.observer && lease && software && o->lock);
 if(boundary()) return -EIO; *s=snap; return 0; }
static int mt6797_cpuclk_write_divider(struct mt6797_cpuclk_observer *o,u32 v,u32 mask)
{ assert(o==&provider.observer && lease && mask==31); bool bad=boundary();
 if(!bad || effect) snap.clkdiv=v; return bad?-EIO:0; }
static int mt6797_cpuclk_write_mux(struct mt6797_cpuclk_observer *o,u32 v,u32 mask)
{ assert(o==&provider.observer && lease && mask==3); bool bad=boundary();
 if(!bad || effect) snap.muxsel=v; return bad?-EIO:0; }
static void arm_smccc_smc(unsigned long fid,u32 addr,u32 v,int a,int b,int c,int d,int e,
 struct arm_smccc_res *r)
{ assert(lease && software && provider.observer.lock && prepared);
 assert(!a && !b && !c && !d && !e);
 bool bad=boundary();
 if(fid==0xc20003b8UL) {
  assert(addr>=250 && addr<=3000 && !v && pins);
  if(!bad || effect) {
   unsigned pd=addr<=500?2:addr<=1000?1:0;
   snap.mp2_armpll_con0=(snap.mp2_armpll_con0 & ~0x7000U) | (pd<<12) | 1;
   snap.mp2_armpll_con1=((uint64_t)addr << (24+pd))/26;
  }
  r->a0=bad ? (high_status ? (1UL<<32) : (unsigned long)-EIO) : 0;
  return;
 }
 assert(addr==MT6797_CPUCLK_MP2_SYNC_DCM);
 if(fid==MT6797_CPUCLK_SMC_MCUSYS_WRITE) {
  assert(writes<4); written[writes++]=v; if(!bad || effect) dcm=v;
  r->a0=bad ? (high_status ? (1UL<<32) : (unsigned long)-EIO) : 0;
 } else {
  assert(fid==MT6797_CPUCLK_SMC_REG_READ);
  if(step==corrupt_step) corrupted=1;
  r->a0=bad ? (1UL<<32) : dcm ^ (step==corrupt_step ? BIT(8) : 0);
 }
}
static void put_device(struct device *d) { assert(d==&pdev.dev && !locked); device_puts++; }
''' + secure_read + api + r'''
static struct device consumer;
static struct clk clk;
static void reset(bool running)
{
 memset(&provider,0,sizeof(provider)); memset(&snap,0,sizeof(snap));
 provider.observer.mainpll=&mainpll; provider.observer.topckgen=&provider; pins=0;
 provider.observer.i2c_clk=&bus; provider.observer.handoff=&provider;
 consumer.of_node=&consumer; clk.hw=&provider.paths[2].hw;
 pdev.dev.driver=&mt6797_cpuclk_driver.driver;
 locked=node_puts=device_puts=prepared=software=lease=step=writes=global_fault=0;
 fail=fail_step=corrupt_step=effect=high_status=corrupted=0; dcm=0xabcd008d;
 if(running) { snap.mp2_armpll_con0=0x00ff1101; snap.mp2_armpll_con1=0xb9b13b14;
  snap.muxsel=0x55; snap.clkdiv=8; }
}
static void clean(void)
{ assert(!locked && !prepared && !software && !lease && !provider.observer.lock); }
int main(void)
{
 /* Actual rate API: endpoint, DCM restoration, lock/pin lifetime and every
  * hardware error boundary, both before and after a write takes effect. */
 reset(true);
 assert(!mt6797_cpuclk_set_mp2_rate(&consumer,&clk,1001000000,1)); clean();
 int rate_steps=step;
 assert(writes==4 && dcm==0xabcd008d && !pins && !provider.observer.faulted);
 assert(!provider.observer.mp2_restart_valid && (snap.clkdiv & 31)==8);
 for(int n=1;n<=rate_steps;n++) for(int applied=0;applied<2;applied++) {
  reset(true); fail_step=n; effect=applied; high_status=applied;
  int r=mt6797_cpuclk_set_mp2_rate(&consumer,&clk,1001000000,1); clean();
  assert(r<0);
  bool fault=writes || n==rate_steps;
  assert(provider.observer.faulted==fault && global_fault==fault);
  assert(pins==fault && provider.observer.mainpll_rate_locked==fault);
  assert(!provider.observer.mp2_restart_valid);
  if(fault) {
   assert(r==-EUCLEAN); int before=step;
   assert(mt6797_cpuclk_set_mp2_rate(&consumer,&clk,1001000000,1)==-EUCLEAN);
   assert(step==before && pins==1); clean();
  }
 }
 for(int n=1;n<=rate_steps;n++) {
  reset(true); corrupt_step=n;
  int r=mt6797_cpuclk_set_mp2_rate(&consumer,&clk,1001000000,1); clean();
  if(corrupted) {
   assert(r<0 && provider.observer.faulted==!!writes);
   assert(pins==!!writes);
  } else assert(!r && !pins);
 }
 reset(true); fail=15;
 assert(mt6797_cpuclk_set_mp2_rate(&consumer,&clk,1001000000,1)==-EBUSY);
 assert(!pins && !step); clean();
 reset(true); provider.observer.mp2_restart_valid=true;
 assert(mt6797_cpuclk_set_mp2_rate(&consumer,&clk,1001000000,1)==-EBUSY);
 assert(!pins && !writes); clean();
 reset(true); dcm |= 2;
 assert(mt6797_cpuclk_set_mp2_rate(&consumer,&clk,1001000000,1)==-EBUSY);
 assert(!pins && !writes); clean();
 reset(true);
 assert(mt6797_cpuclk_set_mp2_rate(&consumer,&clk,1001000001,1)==-EINVAL);
 assert(!pins && !writes); clean();
 reset(true);
 assert(!mt6797_cpuclk_set_mp2_rate(&consumer,&clk,338000000,2)); clean();
 assert((snap.clkdiv & 31)==10 && !pins && dcm==0xabcd008d);
 printf("MP2 hardware rate API: %d error boundaries, retained fallback pin, DCM restore passed\n",rate_steps);
 const int results[]={0,-ENOENT,-EINVAL,-EPROBE_DEFER,-EPROBE_DEFER,
  -EPROBE_DEFER,-EINVAL,-EINVAL,-EINVAL,-ENOENT,-ERANGE,-EIO,-EUCLEAN,-EHOSTDOWN};
 reset(false);
 assert(mt6797_cpuclk_check_mp2_boot(NULL,&clk)==-EINVAL);
 assert(mt6797_cpuclk_check_mp2_boot(&consumer,NULL)==-EINVAL);
 for(int n=0;n<14;n++) {
  reset(false); fail=n;
  if(n==4) pdev.dev.driver=NULL;
  if(n==8) clk.hw=&wrong;
  if(n==12) provider.observer.faulted=true;
  assert(mt6797_cpuclk_check_mp2_boot(&consumer,&clk)==results[n]); clean();
  assert(node_puts==(n!=1 && n!=2));
  assert(device_puts==(n!=1 && n!=2 && n!=3)); assert(!writes);
 }
 for(int n=1;n<=3;n++) {
  reset(false); fail_step=n;
  assert(mt6797_cpuclk_check_mp2_boot(&consumer,&clk)==
   (n==1?-EBUSY:n==2?-EIO:-EUCLEAN)); clean();
  assert(!writes && global_fault==(n==3));
 }
 reset(true);
 assert(mt6797_cpuclk_check_mp2_boot(&consumer,&clk)==-EBUSY); clean();
 reset(true); fail=14;
 assert(mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,true)==-EINVAL);
 assert(!node_puts && !step);
 for(int on=0;on<2;on++) {
  reset(true);
  assert(!mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,on)); clean();
  assert(step==9 && writes==2 && !global_fault);
  assert(written[0]==(0xabcd0080U | (on ? 0xf : 2)));
  assert(written[1]==(0xabcd0080U | (on ? 0xd : 0)));
  for(int n=1;n<=9;n++) for(int applied=0;applied<2;applied++) {
   reset(true); fail_step=n; effect=applied;
   int expected=n==1?-EBUSY:n<5?-EIO:-EUCLEAN;
   assert(mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,on)==expected); clean();
   assert(provider.observer.faulted==(n>=5)); assert(global_fault==(n>=5));
   assert(!provider.observer.mp2_restart_valid);
   if(n>=5) { int before=step;
    assert(mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,on)==-EUCLEAN);
    assert(step==before); clean(); }
  }
  for(int n=3;n<=8;n++) {
   if(n==5 || n==7) continue;
   reset(true); corrupt_step=n;
   assert(mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,on)==(n<5?-EAGAIN:-EUCLEAN));
   assert(provider.observer.faulted==(n>=5)); clean();
  }
  for(int n=5;n<=7;n+=2) {
   reset(true); fail_step=n; high_status=1;
   assert(mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,on)==-EUCLEAN); clean();
  }
  for(int bit=7;bit<32;bit++) {
   reset(true); dcm=BIT(bit) | 0x7f;
   assert(!mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,on)); clean();
   assert(dcm==(BIT(bit) | (on?0xd:0)));
  }
  reset(true); snap.mp2_armpll_con1=0;
  assert(mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,on)==-ERANGE);
  assert(!writes); clean();
  reset(false);
  assert(mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,on)==-EBUSY);
  assert(!writes); clean();
  reset(true); snap.mp2_armpll_con0 &= ~BIT(0);
  assert(mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,on)==-EBUSY);
  assert(!writes); clean();
 }
 reset(true);
 assert(mt6797_cpuclk_check_mp2_restart(&consumer,&clk)==-EAGAIN); clean();
 assert(!mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,false)); clean();
 assert(provider.observer.mp2_restart_valid);
 assert(mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,false)==-EALREADY); clean();
 assert(writes==2 && !global_fault);
 assert(mt6797_cpuclk_check_mp2_restart(&consumer,&clk)==-EBUSY); clean();
 snap.mp2_armpll_con0 &= ~BIT(0); snap.muxsel &= ~BIT(0);
 assert(!mt6797_cpuclk_check_mp2_restart(&consumer,&clk)); clean();
 snap.mp2_armpll_con1 ^= BIT(4);
 assert(mt6797_cpuclk_check_mp2_restart(&consumer,&clk)==-EBUSY); clean();
 snap.mp2_armpll_con1 ^= BIT(4);
 /* The rail-off reset tuple needs the same saved shutdown receipt. */
 snap.mp2_armpll_con0=snap.mp2_armpll_con1=0;
 assert(!mt6797_cpuclk_check_mp2_restart(&consumer,&clk)); clean();
 assert(mt6797_cpuclk_check_mp2_boot(&consumer,&clk)==-EBUSY); clean();
 snap.mp2_armpll_con0=BIT(4);
 assert(mt6797_cpuclk_check_mp2_restart(&consumer,&clk)==-EBUSY); clean();
 snap=provider.observer.mp2_running; writes=0;
 assert(!mt6797_cpuclk_set_mp2_sync_dcm(&consumer,&clk,true)); clean();
 assert(!provider.observer.mp2_restart_valid);
 assert(mt6797_cpuclk_check_mp2_restart(&consumer,&clk)==-EAGAIN); clean();
 puts("PASS MP2 saved running state, duplicate-stop refusal, stopped-state validation and invalidation after DCM enable");
 puts("mt6797-cpuclk-boot-api: pass (lookup, lock order, DCM toggle, 36 I/O failures, 8 readback corruptions, full-width SMC errors, fencing)");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(c)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', '-Wno-sign-compare',
                    '-Wno-misleading-indentation', '-fsanitize=undefined',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
