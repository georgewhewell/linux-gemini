#!/usr/bin/env python3
"""Execute actual scpsys regulator, transition and error-unwind C on host."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
src = (root / "mtk-scpsys.c").read_text()

def function(name):
    start = src.rfind("\nstatic ", 0, src.index(name + "(")) + 1
    opening = src.index("{", start)
    depth = 1
    pos = opening + 1
    while depth:
        depth += (src[pos] == "{") - (src[pos] == "}")
        pos += 1
    return src[start:pos] + "\n"

pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "mt6797-gpuldo-owner.h"
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define MT6797_GPULDO_DOMAIN_MASK 0x3f00
#define MT6797_POWER_DOMAIN_MFG_ASYNC 0
#define MAX_CLKS 3
#define __iomem
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define PWR_ON_BIT BIT(2)
#define PWR_ON_2ND_BIT BIT(3)
#define PWR_CLK_DIS_BIT BIT(4)
#define PWR_ISO_BIT BIT(1)
#define PWR_RST_B_BIT BIT(0)
struct clk { int x; };
struct regulator { int enabled; };
struct generic_pm_domain { const char *name; };
struct data { u32 sta_mask, bus_prot_group_mask; int ctl_offs;
 int sram_pdn_ctl_offs, sram_pdn_ack_offs; };
struct scp;
struct scp_domain { struct generic_pm_domain genpd; struct scp *scp;
 struct data *data; struct regulator *supply; struct clk *clk[MAX_CLKS]; };
struct scp { struct scp_domain *domains; void *base, *dev, *infracfg;
 struct { int pwr_sta_offs, pwr_sta2nd_offs; } ctrl_reg;
 bool gpuldo_mode; int gpuldo_lock, bus_prot_lock;
 struct mt6797_gpuldo_owner gpuldo; };
static int calls, fail_at, supply_cuts, clocks_off, writes, ldo_on;
static u32 bank[256];
static int event(void) { return ++calls == fail_at ? -EIO : 0; }
static void mutex_lock(int *p) { assert(!*p); *p=1; }
static void mutex_unlock(int *p) { assert(*p); *p=0; }
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 v, void *p) { *(u32 *)p=v; }
static int regmap_write(void *m, unsigned r, unsigned v) {
 (void)m; writes++; if(r==0xfbc) ldo_on=v!=0; return event(); }
static int regulator_set_voltage(struct regulator *r,int lo,int hi) {
 assert(!r->enabled && lo==846700 && hi==850000 && ldo_on); return event(); }
static int regulator_enable(struct regulator *r) {
 int ret=event(); r->enabled=1; return ret; }
static int regulator_disable(struct regulator *r) {
 int ret=event(); supply_cuts++; if(!ret) r->enabled=0; return ret; }
static int regulator_is_enabled(struct regulator *r) { return r->enabled; }
static int scpsys_clk_enable(struct clk **c,int n) { (void)c;(void)n; return event(); }
static void scpsys_clk_disable(struct clk **c,int n) { (void)c;(void)n; clocks_off++; }
static int scpsys_bus_protect_enable(struct scp_domain *d) { (void)d; return event(); }
static int scpsys_bus_protect_disable(struct scp_domain *d) { (void)d; return event(); }
static int scpsys_sram_enable(struct scp_domain *d,void *p,void *a) {
 (void)d;(void)p;(void)a; return event(); }
static int scpsys_sram_disable(struct scp_domain *d,void *p,void *a) {
 (void)d;(void)p;(void)a; return event(); }
static int poll(struct scp_domain *d) {
 int ret=event(); if(!ret) {
  u32 ctl=readl(d->scp->base+d->data->ctl_offs);
  u32 on=(ctl&PWR_ON_BIT)?d->data->sta_mask:0;
  bank[0]=bank[1]=on;
 } return ret; }
#define readx_poll_timeout(fn,d,tmp,cond,delay,timeout) poll(d)
#define dev_err(...) do {} while(0)
'''
names = ["scpsys_gpuldo_domain", "scpsys_gpuldo_all_off", "scpsys_gpuldo_write",
         "scpsys_gpuldo_supply_on", "scpsys_gpuldo_supply_off",
         "scpsys_gpuldo_supply_enabled"]
code = "".join(function(name) for name in names)
ops_start = src.index("static const struct mt6797_gpuldo_ops scpsys_gpuldo_ops")
code += src[ops_start:src.index("};", ops_start) + 2] + "\n"
code += "".join(function(name) for name in ["scpsys_regulator_enable",
    "scpsys_regulator_disable", "scpsys_power_on_raw", "scpsys_power_off_raw",
    "scpsys_gpuldo_transition", "scpsys_power_on", "scpsys_power_off"])
main = r'''
static struct regulator rail;
static struct data data;
static struct scp scp;
static struct scp_domain domain;
static void fresh(bool mode) {
 memset(bank,0,sizeof(bank)); calls=fail_at=supply_cuts=clocks_off=writes=ldo_on=0;
 rail=(struct regulator){0}; data=(struct data){.sta_mask=BIT(13),.ctl_offs=16};
 scp=(struct scp){.domains=&domain,.base=bank,.gpuldo_mode=mode,
  .ctrl_reg={0,4},.gpuldo={.admitted=true}};
 domain=(struct scp_domain){.genpd={"mfg_async"},.scp=&scp,.data=&data,.supply=&rail};
}
int main(void) {
 int n,on_events,off_events,before,a,b;
 /* Actual paired status reader: every one of the six domains must be off. */
 for(a=0;a<64;a++) for(b=0;b<64;b++) {
  fresh(true); bank[0]=a<<8; bank[1]=b<<8;
  scp.gpuldo=(struct mt6797_gpuldo_owner){0};
  assert((mt6797_gpuldo_admit(&scp.gpuldo,&scpsys_gpuldo_ops,&scp)==0)==(!a&&!b));
  assert(!writes && !supply_cuts);
 }
 fresh(true); assert(!scpsys_power_on(&domain.genpd)); on_events=calls;
 assert(rail.enabled && ldo_on && !supply_cuts);
 calls=0; assert(!scpsys_power_off(&domain.genpd)); off_events=calls;
 assert(!rail.enabled && !ldo_on && supply_cuts==1 && clocks_off==1);
 for(n=1;n<=on_events;n++) {
  fresh(true); fail_at=n; assert(scpsys_power_on(&domain.genpd)<0);
  assert(scp.gpuldo.failed && !supply_cuts);
  before=calls; assert(scpsys_power_off(&domain.genpd)==-EPERM);
  assert(scpsys_power_on(&domain.genpd)==-EPERM && calls==before);
  assert(!scp.gpuldo_lock && !scp.bus_prot_lock);
 }
 for(n=1;n<=off_events;n++) {
  fresh(true); assert(!scpsys_power_on(&domain.genpd)); calls=0; fail_at=n;
  assert(scpsys_power_off(&domain.genpd)<0 && scp.gpuldo.failed);
  before=calls; assert(scpsys_power_off(&domain.genpd)==-EPERM && calls==before);
  /* Only final fbc-write failure may have made the LDO disable effective. */
  if(n<off_events) assert(ldo_on);
 }
 /* Generic mode still performs the original regulator unwind on clock error. */
 fresh(false); fail_at=2; assert(scpsys_power_on(&domain.genpd)<0);
 assert(supply_cuts==1 && !writes && !scp.gpuldo.failed);
 /* Shared-protection child failures cannot evade latch or drop parent rail. */
 for(n=1;n<=5;n++) {
  fresh(true); assert(!scpsys_power_on(&domain.genpd));
  data.sta_mask=BIT(11); data.bus_prot_group_mask=1; domain.supply=NULL;
  calls=0; fail_at=n;
  assert(scpsys_power_on(&domain.genpd)<0 && scp.gpuldo.failed);
  assert(!supply_cuts && rail.enabled && !scp.bus_prot_lock);
  data.sta_mask=BIT(13); domain.supply=&rail; before=calls;
  assert(scpsys_power_off(&domain.genpd)==-EPERM && calls==before);
 }
 printf("actual scpsys callbacks: %d ON + %d OFF failure sites PASS\n",on_events,off_events);
 return 0;
}
'''
cc = os.environ.get("HOSTCC", "/nix/store/061ybr50r1m2jnvih4vvpgmgwb37df3r-gcc-wrapper-12.2.0/bin/gcc")
with tempfile.TemporaryDirectory(prefix="gemini-gpuldo-integration-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(pre + code + main)
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                    "-I", str(root), str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
