#!/usr/bin/env python3
"""Execute the OPP core, Panfrost callback and sole-owner voltage path on host."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent
kernel = root.parents[2]
scp = (root / "mtk-scpsys.c").read_text()
gpu = (kernel / "drivers/gpu/drm/panfrost/panfrost_devfreq.c").read_text()
opp = (kernel / "drivers/opp/core.c").read_text()


def function(source, name):
    match = re.search(r"^(?:static )?int " + re.escape(name) + r"\(", source, re.M)
    assert match, name
    start = match.start()
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos] + "\n"


pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "mt6797-gpuldo-owner.h"
typedef uint32_t u32;
#define EPROBE_DEFER 517
#define MT6797_POWER_DOMAIN_MFG_ASYNC 0
#define MT6797_GPULDO_DOMAIN_MASK 0x3f00
#define unlikely(x) (x)
#define smp_load_acquire(p) (*(p))
#define IS_ERR_OR_NULL(p) (!(p))
#define dev_err(...) do {} while (0)
#define dev_dbg(...) do {} while (0)
#define dev_dbg_ratelimited(...) do {} while (0)
#define pr_err(...) do {} while (0)
struct device_node { int id; } node, wrong;
struct device { struct device_node *of_node; } dev={&node};
struct regulator { int uv, enabled; } rail;
struct scp_domain { struct regulator *supply; } domain={&rail};
struct scp { struct scp_domain *domains; struct device_node *gpuldo_gpu;
 void *base; struct { int pwr_sta_offs,pwr_sta2nd_offs; } ctrl_reg;
 bool gpuldo_mode; int gpuldo_lock; struct mt6797_gpuldo_owner gpuldo; } state;
static struct scp *gpuldo_provider;
static u32 bank[2];
static int calls, fail_at, cuts, enables, reports, sleeps, bad_readback, clock_fail;
static unsigned long clock_rate;
static char events[32]; static unsigned n_events;
static void record(char e) { assert(n_events<sizeof(events)); events[n_events++]=e; }
static int event(char e) { record(e); return ++calls==fail_at?-EIO:0; }
static u32 readl(void *p) { return *(u32 *)p; }
static void mutex_lock(int *p) { assert(!*p); *p=1; }
static void mutex_unlock(int *p) { assert(*p); *p=0; }
static int regulator_get_voltage(struct regulator *r) {
 int ret=event('R'); return ret?ret:(bad_readback&&calls>=3?600000:r->uv); }
static int regulator_set_voltage(struct regulator *r,unsigned lo,unsigned hi) {
 int ret=event('V'); r->uv=((lo-600000+6249)/6250)*6250+600000;
 assert(r->uv>=lo && r->uv<=hi); return ret; /* failure may follow effective write */ }
static int regulator_enable(struct regulator *r) { enables++; r->enabled=1; return 0; }
static void usleep_range(unsigned lo,unsigned hi) {
 assert(lo==350 && hi==400); sleeps++; record('S'); }
static void scpsys_gpuldo_report(struct scp *s,const char *stage,int ret) {
 assert(s==&state && !strcmp(stage,"OPP voltage") && ret<0); reports++; }
struct dev_pm_opp_supply { unsigned long u_volt,u_volt_min,u_volt_max,u_amp,u_watt; };
struct opp_table;
struct dev_pm_opp { struct dev_pm_opp_supply *supplies; unsigned long rates[1];
 unsigned level; struct { unsigned peak; } *bandwidth; struct opp_table *opp_table; };
struct opp_table { int regulator_count; struct regulator **regulators;
 struct dev_pm_opp *current_opp; bool enabled;
 int (*config_regulators)(struct device *,struct dev_pm_opp *,struct dev_pm_opp *,
                          struct regulator **,unsigned);
 int (*config_clks)(struct device *,struct opp_table *,struct dev_pm_opp *,void *,bool); } table;
static void _find_current_opp(struct device *d,struct opp_table *t) { assert(0); }
static int _disable_opp_table(struct device *d,struct opp_table *t) { assert(0);return 0; }
static int _opp_compare_rate(struct opp_table *t,struct dev_pm_opp *a,struct dev_pm_opp *b) {
 return a->rates[0]==b->rates[0]?0:(a->rates[0]<b->rates[0]?-1:1); }
static int _opp_compare_bw(struct opp_table *t,struct dev_pm_opp *a,struct dev_pm_opp *b) {return 0;}
static int _set_required_opps(struct device *d,struct opp_table *t,struct dev_pm_opp *p,bool up) {return 0;}
static int _set_opp_level(struct device *d,struct dev_pm_opp *p) {return 0;}
static int _set_opp_bw(struct opp_table *t,struct dev_pm_opp *p,struct device *d) {return 0;}
static void dev_pm_opp_put(struct dev_pm_opp *p) {}
static struct dev_pm_opp *dev_pm_opp_get(struct dev_pm_opp *p) {return p;}
static int clock_set(struct device *d,struct opp_table *t,struct dev_pm_opp *p,void *data,bool down) {
 record('C'); if(clock_fail)return -EIO;
 assert(rail.uv>=(int)p->supplies[0].u_volt_min);
 clock_rate=p->rates[0]; return 0;
}
'''
code = "".join(function(scp, name) for name in [
    "scpsys_gpuldo_all_off", "scpsys_gpuldo_voltage",
    "mtk_scpsys_gpuldo_gpu_set_voltage", "scpsys_gpuldo_supply_on"])
code += function(opp, "dev_pm_opp_get_supplies")
code += function(gpu, "panfrost_mt6797_config_regulators")
code += function(opp, "_opp_compare_key") + function(opp, "_set_opp")
main = r'''
static struct dev_pm_opp_supply supplies[]={
 {846700,846700,850000,0,0}, {910800,910800,912500,0,0},
 {936500,936500,937500,0,0}, {975000,975000,975000,0,0},
 {1013400,1013400,1018750,0,0}, {1103200,1103200,1106250,0,0},
 {1154500,1154500,1156250,0,0}};
static unsigned long rates[]={238000000,365000000,442500000,520000000,
 610000000,780000000,900000000};
static struct dev_pm_opp points[7];
static void fresh(unsigned i) {
 calls=fail_at=cuts=enables=reports=sleeps=bad_readback=clock_fail=n_events=0;
 memset(events,0,sizeof(events));
 rail=(struct regulator){((supplies[i].u_volt_min-600000+6249)/6250)*6250+600000,1};
 state=(struct scp){.domains=&domain,.gpuldo_gpu=&node,.base=bank,
  .ctrl_reg={0,4},.gpuldo_mode=true,.gpuldo={.admitted=true,.active=true,
   .supply_vote=true,.min_uv=supplies[i].u_volt_min,.max_uv=supplies[i].u_volt_max}};
 bank[0]=bank[1]=0x3f00; gpuldo_provider=&state; clock_rate=rates[i];
 table=(struct opp_table){.regulator_count=1,.current_opp=&points[i],.enabled=true,
  .config_regulators=panfrost_mt6797_config_regulators,.config_clks=clock_set};
 for(unsigned j=0;j<7;j++) points[j]=(struct dev_pm_opp){.supplies=&supplies[j],
  .rates={rates[j]},.opp_table=&table};
}
int main(void) {
 /* All pairs exercise actual OPP core ordering against physical voltage. */
 for(unsigned a=0;a<7;a++) for(unsigned b=0;b<7;b++) {
  fresh(a); assert(!_set_opp(&dev,&table,&points[b],NULL,false));
  assert(clock_rate==rates[b] && table.current_opp==&points[b]);
  assert(!cuts && !enables && !reports && !state.gpuldo_lock);
  if(a==b) { assert(!n_events); continue; }
  assert(!strcmp(events,a<b?"RVRSC":"CRVR"));
  assert(sleeps==(a<b));
  /* Off/on restores the last voltage vote, including the highest OPP. */
  state.gpuldo.active=false; state.gpuldo.supply_vote=false;
  bank[0]=bank[1]=0; rail.enabled=0; rail.uv=600000;
  assert(!scpsys_gpuldo_supply_on(&state));
  assert(rail.uv>=(int)supplies[b].u_volt_min &&
         rail.uv<=(int)supplies[b].u_volt_max && enables==1);
 }
 /* A regulator failure cannot advance the OPP or clock-up, nor be retried. */
 for(unsigned a=0;a<7;a++) for(unsigned b=0;b<7;b++) if(a!=b)
  for(int f=1;f<=3;f++) {
   fresh(a); fail_at=f;
   assert(_set_opp(&dev,&table,&points[b],NULL,false)==-EIO);
   assert(state.gpuldo.failed && reports==1 && !enables && !cuts);
   assert(table.current_opp==&points[a]);
   assert(clock_rate==rates[a<b?a:b]);
   int before=calls;
   assert(mtk_scpsys_gpuldo_gpu_set_voltage(&node,975000,975000)==-EPERM);
   assert(calls==before && !state.gpuldo_lock);
  }
 fresh(0); bad_readback=1;
 assert(_set_opp(&dev,&table,&points[6],NULL,false)==-ERANGE);
 assert(clock_rate==rates[0] && state.gpuldo.failed);
 /* A failed clock-up leaves the higher vote for a later resume. */
 fresh(0); clock_fail=1;
 assert(_set_opp(&dev,&table,&points[6],NULL,false)==-EIO);
 assert(clock_rate==rates[0] && state.gpuldo.min_uv==1154500 && !state.gpuldo.failed);
 /* Votes while fully off don't write a disabled regulator. */
 fresh(0); state.gpuldo.active=false; state.gpuldo.supply_vote=false; bank[0]=bank[1]=0;
 assert(!mtk_scpsys_gpuldo_gpu_set_voltage(&node,1154500,1156250) && !calls);
 rail.enabled=0; assert(!scpsys_gpuldo_supply_on(&state));
 assert(rail.uv==1156250 && enables==1);
 fresh(0); state.gpuldo.active=false;
 assert(mtk_scpsys_gpuldo_gpu_set_voltage(&node,975000,975000)==-EBUSY);
 assert(!calls && state.gpuldo.failed);
 fresh(0); state.gpuldo.supply_vote=false;
 assert(mtk_scpsys_gpuldo_gpu_set_voltage(&node,975000,975000)==-EPERM && !calls);
 fresh(0); assert(mtk_scpsys_gpuldo_gpu_set_voltage(&wrong,975000,975000)==-EINVAL);
 assert(mtk_scpsys_gpuldo_gpu_set_voltage(&node,600000,600000)==-EINVAL);
 assert(mtk_scpsys_gpuldo_gpu_set_voltage(&node,1154500,1200000)==-EINVAL);
 assert(mtk_scpsys_gpuldo_gpu_set_voltage(&node,975000,970000)==-EINVAL);
 assert(!calls); gpuldo_provider=NULL;
 assert(mtk_scpsys_gpuldo_gpu_set_voltage(&node,975000,975000)==-EPROBE_DEFER);
 puts("PASS actual OPP/Panfrost/owner: 49 transitions, 126 regulator failures, voltage-before-clock-up, clock-before-voltage-down, saved resume vote, sole handle");
}
'''
cc = os.environ.get("HOSTCC", "/nix/store/061ybr50r1m2jnvih4vvpgmgwb37df3r-gcc-wrapper-12.2.0/bin/gcc")
with tempfile.TemporaryDirectory(prefix="gemini-gpuldo-dvfs-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(pre + code + main)
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-sign-compare",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "-I", str(root), str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
