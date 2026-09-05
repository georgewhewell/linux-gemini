#!/usr/bin/env python3
"""Actual registration/probe publication: retain objects, reject partial graph."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parent
src=(root/"mtk-scpsys.c").read_text()
def fn(name):
    at=src.index(name+"(")
    start=src.rfind("\nstatic ",0,at)+1
    pos=src.index("{",at)+1
    depth=1
    while depth:
        depth+=(src[pos]=="{")-(src[pos]=="}")
        pos+=1
    return src[start:pos]
pre=r'''
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include "mt6797-gpuldo-owner.h"
struct device { void *of_node; };
struct platform_device { struct device dev; };
struct generic_pm_domain { int (*power_on)(struct generic_pm_domain *); };
struct scp_domain { struct generic_pm_domain genpd; } domains[6];
struct generic_pm_domain *ptrs[6];
struct genpd_onecell_data { struct generic_pm_domain **domains; };
struct scp { struct scp_domain *domains; struct genpd_onecell_data pd_data;
 bool gpuldo_mode; struct mt6797_gpuldo_owner gpuldo; } state;
struct scp_subdomain { int origin,subdomain; } edges[]={{0,1},{1,2},{1,3},{1,4},{1,5}};
struct scp_soc_data { void *domains; int num_domains; int regs; bool bus_prot_reg_update;
 struct scp_subdomain *subdomains; int num_subdomains; } soc={0,6,0,0,edges,5};
static struct scp *gpuldo_provider;
static int init_calls,init_fail,provider_calls,provider_fail,edge_calls,edge_fail,publish;
#define MTK_SCPD_CAPS(d,c) true
#define WARN_ON(c) (c)
#define IS_ENABLED(c) 1
#define IS_ERR(p) false
#define PTR_ERR(p) (-EINVAL)
#define dev_err(...) do {} while(0)
#define smp_store_release(p,v) do { *(p)=(v); publish++; } while(0)
static int pm_genpd_init(struct generic_pm_domain *p,void *g,bool off) {
 (void)p;(void)g; assert(off); return ++init_calls==init_fail?-EIO:0; }
static int of_genpd_add_provider_onecell(void *n,struct genpd_onecell_data *d) {
 (void)n;(void)d; provider_calls++; return provider_fail?-EIO:0; }
static int pm_genpd_add_subdomain(struct generic_pm_domain *a,struct generic_pm_domain *b) {
 assert(a==ptrs[edges[edge_calls].origin] && b==ptrs[edges[edge_calls].subdomain]);
 return ++edge_calls==edge_fail?-EIO:0; }
static const void *of_device_get_match_data(struct device *d) { (void)d; return &soc; }
static struct scp *init_scp(struct platform_device *p,void *d,int n,const int *r,bool b) {
 (void)p;(void)d;(void)n;(void)r;(void)b; return &state; }
'''
main=r'''
static void fresh(bool mode) {
 init_calls=init_fail=provider_calls=provider_fail=edge_calls=edge_fail=publish=0;
 gpuldo_provider=NULL;
 for(int i=0;i<6;i++)ptrs[i]=&domains[i].genpd;
 state=(struct scp){.domains=domains,.pd_data={ptrs},.gpuldo_mode=mode,.gpuldo={.admitted=true}};
}
int main(void) {
 struct platform_device p={0};
 fresh(true); assert(!scpsys_probe(&p)); assert(publish==1 && edge_calls==5 && init_calls==6);
 for(int i=1;i<=6;i++) {
  fresh(true); init_fail=i; assert(!scpsys_probe(&p));
  assert(state.gpuldo.failed && !provider_calls && !publish && !edge_calls);
 }
 fresh(true); provider_fail=1; assert(!scpsys_probe(&p));
 assert(state.gpuldo.failed && !publish && !edge_calls);
 for(int i=1;i<=5;i++) {
  fresh(true); edge_fail=i; assert(!scpsys_probe(&p));
  assert(state.gpuldo.failed && !publish && !gpuldo_provider);
 }
 fresh(false); init_fail=3; provider_fail=1; edge_fail=2;
 assert(!scpsys_probe(&p)); assert(!state.gpuldo.failed && provider_calls==1 && edge_calls==5 && !publish);
 puts("actual registration/probe: six init/provider/five hierarchy failures never publish; ordinary path PASS");
}
'''
cc=os.environ.get("HOSTCC","/nix/store/061ybr50r1m2jnvih4vvpgmgwb37df3r-gcc-wrapper-12.2.0/bin/gcc")
with tempfile.TemporaryDirectory(prefix="gemini-gpuldo-registration-") as tmp:
    p=Path(tmp)
    (p/"test.c").write_text(pre+fn("mtk_register_power_domains")+fn("scpsys_probe")+main)
    subprocess.run([cc,"-std=gnu11","-Wall","-Wextra","-Werror","-I",str(root),str(p/"test.c"),"-o",str(p/"test")],check=True)
    subprocess.run([str(p/"test")],check=True)
