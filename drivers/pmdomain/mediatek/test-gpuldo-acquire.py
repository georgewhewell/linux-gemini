#!/usr/bin/env python3
"""Actual init_scp ordering, post-acquisition retention, first-event capture."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parent
src=(root/'mtk-scpsys.c').read_text()
def fn(name):
    at=src.index(name+'(');start=src.rfind('\nstatic ',0,at)+1
    pos=src.index('{',at)+1;depth=1
    while depth:
        depth+=(src[pos]=='{')-(src[pos]=='}');pos+=1
    return src[start:pos]+'\n'
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include "mt6797-gpuldo-owner.h"
typedef uint32_t u32;
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_CAST(p) ((void *)(p))
#define GFP_KERNEL 0
#define CLK_MAX 2
#define MAX_CLKS 2
#define MT6797_POWER_DOMAIN_MFG_ASYNC 0
#define MT6797_GPULDO_DOMAIN_MASK 0x3f00
#define MTK_SCPD_CAPS(d,c) false
#define GENPD_FLAG_ACTIVE_WAKEUP 1
struct device_node {int refs;} node;
struct device {struct device_node *of_node;};
struct platform_device {struct device dev;};
struct clk {int x;} clock;
struct regulator {int x;} regulator;
struct generic_pm_domain {const char *name;int (*power_on)(struct generic_pm_domain *);
 int (*power_off)(struct generic_pm_domain *);int flags;};
struct scp_domain_data {const char *name;int clk_id[MAX_CLKS];};
struct scp_ctrl_reg {int pwr_sta_offs,pwr_sta2nd_offs;};
struct scp;
struct scp_domain {struct regulator *supply;struct scp *scp;
 const struct scp_domain_data *data;struct clk *clk[MAX_CLKS];struct generic_pm_domain genpd;};
struct genpd_onecell_data {struct generic_pm_domain **domains;int num_domains;};
struct scp {struct device *dev;struct scp_ctrl_reg ctrl_reg;
 bool bus_prot_reg_update,gpuldo_mode,gpuldo_acquired,gpuldo_reported;
 int bus_prot_lock,gpuldo_lock;void *base,*infracfg;
 struct scp_domain *domains;struct genpd_onecell_data pd_data;
 struct mt6797_gpuldo_owner gpuldo;struct device_node *gpuldo_gpu;};
static int steps,fail_at,acquires,on,post_failure,logs,enabled;
static unsigned a_log,b_log;static const char *stage_log;
static u32 bank[2];static void *allocated[3];static unsigned allocations;
static int step(void){return ++steps==fail_at?-EIO:0;}
static void *devm_kzalloc(struct device *d,size_t n,int f){
 (void)d;(void)f;if(step())return NULL;void *p=calloc(1,n);assert(p);allocated[allocations++]=p;return p;}
static void *devm_kcalloc(struct device *d,size_t a,size_t b,int f){return devm_kzalloc(d,a*b,f);}
static void mutex_init(int *p){*p=0;}
static int scpsys_gpuldo_validate_dt(struct device *d){(void)d;return step()?-EIO:1;}
static void *devm_platform_ioremap_resource(struct platform_device *p,int i){(void)p;assert(i==0);return step()?ERR_PTR(-EIO):bank;}
static void *syscon_regmap_lookup_by_phandle(struct device_node *n,const char *p){(void)n;(void)p;return step()?ERR_PTR(-EIO):bank;}
static struct regulator *devm_regulator_get_optional(struct device *d,const char *name){
 (void)d;assert(name[0]=='o');return step()?ERR_PTR(-EIO):ERR_PTR(-ENODEV);}
static void init_clks(struct platform_device *p,struct clk **c){(void)p;c[1]=step()?ERR_PTR(-EIO):&clock;}
static struct device_node *of_find_compatible_node(void *p,void *q,const char *s){
 (void)p;(void)q;(void)s;if(step())return NULL;node.refs++;return &node;}
static void of_node_put(struct device_node *n){assert(n->refs==1);n->refs--;}
static struct regulator *devm_regulator_get_exclusive_state(struct device *d,const char *name,bool *vote){
 (void)d;assert(name[0]=='m');acquires++;if(step())return ERR_PTR(-EIO);
 *vote=on;enabled=on;if(post_failure==1)bank[1]=0x2000;
 if(post_failure==2)enabled=-EIO;
 if(post_failure==3)enabled=!on;
 return &regulator;}
static u32 readl(void *p){return *(u32 *)p;}
static void log_error(struct device *d,const char *format,...){
 (void)d;if(format[0]!='G')return;
 va_list ap;va_start(ap,format);stage_log=va_arg(ap,const char *);
 assert(va_arg(ap,int)<0);a_log=va_arg(ap,unsigned);b_log=va_arg(ap,unsigned);
 (void)va_arg(ap,int);(void)va_arg(ap,int);va_end(ap);logs++;}
#define dev_err log_error
static int scpsys_power_on(struct generic_pm_domain *p){(void)p;abort();}
static int scpsys_power_off(struct generic_pm_domain *p){(void)p;abort();}
static int rail_enabled(void *p){(void)p;return enabled;}
'''
code=fn('scpsys_gpuldo_all_off')+fn('scpsys_gpuldo_report')
code+='static const struct mt6797_gpuldo_ops scpsys_gpuldo_ops={.all_domains_off=scpsys_gpuldo_all_off,.supply_is_enabled=rail_enabled};\n'
code+=fn('init_scp')
main=r'''
static struct scp_domain_data data[]={{"mfg_async",{1,0}},{"other",{1,0}}};
static struct scp_ctrl_reg ctrl={0,4};static struct platform_device p;
static void fresh(void){
 for(unsigned i=0;i<allocations;i++)free(allocated[i]);allocations=0;
 steps=fail_at=acquires=post_failure=logs=enabled=0;node.refs=0;bank[0]=bank[1]=0;
}
int main(void){
 int count;
 fresh();struct scp *s=init_scp(&p,data,2,&ctrl,false);assert(!IS_ERR(s));count=steps;
 assert(acquires==1&&s->gpuldo.admitted&&!s->gpuldo.failed&&node.refs==1);
 for(int i=1;i<=count;i++){
  fresh();fail_at=i;s=init_scp(&p,data,2,&ctrl,false);
  assert(IS_ERR(s)&&!node.refs);assert(acquires==(i==count));
 }
 for(on=0;on<=1;on++){
  fresh();s=init_scp(&p,data,2,&ctrl,false);assert(!IS_ERR(s));
  assert(s->gpuldo.supply_vote==on&&s->gpuldo.admitted&&s->gpuldo_acquired);
  for(int f=1;f<=3;f++){
   fresh();post_failure=f;s=init_scp(&p,data,2,&ctrl,false);
   assert(!IS_ERR(s)&&s->gpuldo.failed&&!s->gpuldo.admitted);
   assert(s->gpuldo_acquired&&s->gpuldo.supply_vote==on&&node.refs==1&&logs==1);
   assert(a_log==0&&b_log==(f==1?0x2000:0));
   scpsys_gpuldo_report(s,"second",-EIO);assert(logs==1);
  }
 }
 fresh();bank[0]=0x100; s=init_scp(&p,data,2,&ctrl,false);
 assert(IS_ERR(s)&&!acquires&&!node.refs&&logs==1&&a_log==0x100&&!b_log);
 fresh();puts("PASS actual init_scp: all fallible prerequisites before acquisition,0/1 adoption, postget error retention, no transition, paired first-failure snapshots");
}
'''
with tempfile.TemporaryDirectory(prefix='gpuldo-acquire-') as d:
    p=Path(d);(p/'test.c').write_text(pre+code+main)
    cc=os.environ.get('HOSTCC','/nix/store/061ybr50r1m2jnvih4vvpgmgwb37df3r-gcc-wrapper-12.2.0/bin/gcc')
    subprocess.run([cc,'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-I',str(root),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
