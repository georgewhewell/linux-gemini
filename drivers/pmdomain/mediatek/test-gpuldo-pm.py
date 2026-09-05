#!/usr/bin/env python3
"""Execute actual Panfrost runtime/system PM: clocks precede parent rail off."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parent
src=(root.parents[2]/"drivers/gpu/drm/panfrost/panfrost_device.c").read_text()
def fn(name):
    at=src.index(name+"("); start=src.rfind("\nstatic ",0,at)+1
    pos=src.index("{",at)+1; depth=1
    while depth:
        depth+=(src[pos]=="{")-(src[pos]=="}"); pos+=1
    return src[start:pos]
pre=r'''
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#define BIT(n) (1U<<(n))
#define GPU_PM_RT 0
#define GPU_PM_CLK_DIS 1
#define GPU_PM_VREG_OFF 2
struct device { void *data; };
struct panfrost_device;
struct comp { unsigned pm_features; int (*vendor_power_off)(struct panfrost_device *); };
struct clk { int votes; } clock_gpu,clock_bus;
struct panfrost_device { struct comp *comp; struct clk *clock,*bus_clock;
 bool mt6797_gpuldo_domain_supply; void *rstc; struct { unsigned long fast_rate; } pfdevfreq; } gpu;
struct dev_pm_opp { int x; } opp;
static int calls,fail_at,resets,rail_on,cuts,idle,opp_calls;
static bool suspended;
static int event(void) { return ++calls==fail_at?-EIO:0; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static int reset_control_deassert(void *r) { (void)r; resets++; return event(); }
static void reset_control_assert(void *r) { (void)r; resets++; }
static int clk_enable(struct clk *c) { assert(rail_on); int ret=event(); if(!ret)c->votes++; return ret; }
static void clk_disable(struct clk *c) { assert(c->votes==1); c->votes--; }
static int panfrost_device_reset(struct panfrost_device *p,bool e) { (void)p;(void)e; return event(); }
static int panfrost_devfreq_resume(struct panfrost_device *p) { (void)p; return event(); }
static int panfrost_devfreq_suspend(struct panfrost_device *p) { (void)p; return event(); }
static bool panfrost_jm_is_idle(struct panfrost_device *p) { (void)p; return idle; }
static int vendor_off(struct panfrost_device *p) { (void)p; return event(); }
static void panfrost_gpu_power_off(struct panfrost_device *p) { (void)p; }
static void panfrost_jm_suspend_irq(struct panfrost_device *p) { (void)p; }
static void panfrost_mmu_suspend_irq(struct panfrost_device *p) { (void)p; }
static void panfrost_gpu_suspend_irq(struct panfrost_device *p) { (void)p; }
#define dev_err(...) do {} while(0)
#define IS_ERR(p) false
#define PTR_ERR(p) (-EINVAL)
static struct dev_pm_opp *dev_pm_opp_find_freq_ceil(struct device *d,unsigned long *f) {
 (void)d;(void)f; opp_calls++; return &opp; }
static int dev_pm_opp_set_opp(struct device *d,struct dev_pm_opp *o) { (void)d;(void)o; opp_calls++; return 0; }
static void dev_pm_opp_put(struct dev_pm_opp *o) { (void)o; }
static int panfrost_device_runtime_suspend(struct device *d);
static int panfrost_device_runtime_resume(struct device *d);
static int pm_runtime_force_suspend(struct device *d) {
 if(suspended)return 0;
 int ret=panfrost_device_runtime_suspend(d);
 if(!ret) { suspended=true; if(gpu.mt6797_gpuldo_domain_supply) {
  assert(!clock_gpu.votes && !clock_bus.votes); rail_on=0; cuts++;
 }} return ret;
}
static int pm_runtime_force_resume(struct device *d) {
 if(!suspended)return 0; rail_on=1;
 int ret=panfrost_device_runtime_resume(d); if(!ret)suspended=false; return ret;
}
'''
main=r'''
static struct comp comp;
static struct device dev={&gpu};
static void fresh(bool mode,bool active) {
 calls=fail_at=resets=cuts=opp_calls=0; idle=1; rail_on=1; suspended=!active;
 clock_gpu.votes=clock_bus.votes=active;
 comp=(struct comp){.pm_features=mode?0:BIT(GPU_PM_CLK_DIS)|BIT(GPU_PM_VREG_OFF),.vendor_power_off=vendor_off};
 gpu=(struct panfrost_device){.comp=&comp,.clock=&clock_gpu,.bus_clock=&clock_bus,.mt6797_gpuldo_domain_supply=mode};
}
int main(void) {
 fresh(true,true);
 assert(!panfrost_device_suspend(&dev)); assert(cuts==1 && !rail_on && !resets && !opp_calls);
 assert(!panfrost_device_suspend(&dev)); assert(cuts==1);
 assert(!panfrost_device_resume(&dev)); assert(clock_gpu.votes==1 && clock_bus.votes==1 && !resets && !opp_calls);
 assert(!panfrost_device_resume(&dev)); assert(clock_gpu.votes==1 && clock_bus.votes==1);
 for(int n=1;n<=4;n++) {
  fresh(true,false); fail_at=n;
  assert(panfrost_device_resume(&dev)<0);
  assert(!clock_gpu.votes && !clock_bus.votes && !resets && !opp_calls);
 }
 for(int n=1;n<=2;n++) {
  fresh(true,true); fail_at=n;
  assert(panfrost_device_suspend(&dev)<0);
  assert(clock_gpu.votes==1 && clock_bus.votes==1 && !cuts && rail_on && !resets && !opp_calls);
 }
 fresh(true,true); idle=0;
 assert(panfrost_device_suspend(&dev)==-EBUSY && clock_gpu.votes==1 && !cuts);
 /* Ordinary T7 mode keeps runtime clock votes and uses its old system gate. */
 fresh(false,true); assert(!panfrost_device_runtime_suspend(&dev));
 assert(clock_gpu.votes==1 && clock_bus.votes==1 && !resets);
 fresh(false,true); assert(!panfrost_device_suspend(&dev));
 assert(!clock_gpu.votes && !clock_bus.votes && opp_calls==1);
 puts("actual Panfrost PM: runtime/system order, 4 resume+2 suspend failures, busy, no duplicate/reset/OPP calls PASS");
}
'''
cc=os.environ.get("HOSTCC","/nix/store/061ybr50r1m2jnvih4vvpgmgwb37df3r-gcc-wrapper-12.2.0/bin/gcc")
with tempfile.TemporaryDirectory(prefix="gemini-gpuldo-pm-") as tmp:
    p=Path(tmp)
    code="".join(fn(n) for n in ["panfrost_device_runtime_resume","panfrost_device_runtime_suspend","panfrost_device_resume","panfrost_device_suspend"])
    (p/"test.c").write_text(pre+code+main)
    subprocess.run([cc,"-std=gnu11","-Wall","-Wextra","-Werror","-Wno-misleading-indentation",str(p/"test.c"),"-o",str(p/"test")],check=True)
    subprocess.run([str(p/"test")],check=True)
