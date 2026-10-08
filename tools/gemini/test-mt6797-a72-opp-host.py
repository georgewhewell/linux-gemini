#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise A72 OPP configuration/callbacks and MP2 rate determination."""
import os
import subprocess
import tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
s=(root/'drivers/soc/mediatek/mt6797-a72-power.c').read_text()
a=s.index('static int mt6797_a72_opp_clocks(')
api=s[a:s.index('static int mt6797_a72_preflight_probe(',a)]
s=(root/'drivers/clk/mediatek/clk-mt6797-cpu.c').read_text()
a=s.index('static int mt6797_cpuclk_path_determine_rate(')
clock=s[a:s.index('\n}\n',a)+3]
c=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define EPROBE_DEFER 517
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
struct device_node {int id;};
struct device {int id;struct device_node *of_node;};
struct clk {int id;};
struct opp_table {int unused;};
struct dev_pm_opp {unsigned long hz,uv;};
struct dev_pm_opp_config {
 const char * const *clk_names;
 int (*config_clks)(struct device*,struct opp_table*,struct dev_pm_opp*,void*,bool);
};
struct mt6797_a72_preflight {struct device *dev,*opp_cpu;struct clk *mp2_clk;};
static struct device_node nodes[4];
static struct device cpus[2],ownerdev;
static struct clk mp2,wrong;
static struct mt6797_a72_preflight p;
static int failure,refs,clock_refs,configs,clears,actions,calls,set_error;
static unsigned cpu_seen,div_seen,uv_seen;
static unsigned long rate_seen;
static struct device *get_cpu_device(unsigned n) {return n<8||n>9||failure==1?NULL:&cpus[n-8];}
static unsigned long dev_pm_opp_get_freq(struct dev_pm_opp *o) {return o->hz;}
static unsigned long dev_pm_opp_get_voltage(struct dev_pm_opp *o) {return o->uv;}
static int mt6797_a72_power_set_opp(unsigned cpu,unsigned long rate,unsigned div,unsigned uv) {
 calls++;cpu_seen=cpu;rate_seen=rate;div_seen=div;uv_seen=uv;return set_error;
}
static struct device_node *of_parse_phandle(struct device_node *n,const char *s,int i) {
 assert(!strcmp(s,"operating-points-v2")&&!i);
 if(failure==2 || (failure==3 && n==&nodes[1]))return NULL;
 refs++;return failure==4 && n==&nodes[1]?&nodes[3]:&nodes[2];
}
static bool of_property_present(struct device_node *n,const char *s) {
 if(!strcmp(s,"opp-shared"))return failure!=5;
 assert(!strcmp(s,"cpu-supply"));return failure==6;
}
static void of_node_put(struct device_node *n) {if(n){assert(refs);refs--;}}
static struct clk *clk_get(struct device *d,const char *name) {
 assert(!name);if(failure==7)return (void *)(intptr_t)-EIO;clock_refs++;return failure==8?&wrong:&mp2;
}
static bool clk_is_match(struct clk *a,struct clk *b) {return a==b;}
static void clk_put(struct clk *c) {assert(clock_refs);clock_refs--;}
static int dev_pm_opp_set_config(struct device *d,struct dev_pm_opp_config *cfg) {
 assert(d==&cpus[0] && cfg->clk_names && !cfg->clk_names[0] && !cfg->clk_names[1] && cfg->config_clks);
 configs++;return failure==9?-EBUSY:7;
}
static void dev_pm_opp_clear_config(int token) {assert(token==7);clears++;}
static int devm_add_action_or_reset(struct device *d,void (*fn)(void*),void *ctx) {
 assert(d==&ownerdev && (uintptr_t)ctx==7);actions++;
 if(failure==10){fn(ctx);return -ENOMEM;}return 0;
}
struct clk_hw {int tag;};
struct clk_rate_request {unsigned long rate;};
struct mt6797_cpuclk_path_hw {int domain;struct clk_hw hw;};
#define MT6797_CPUCLK_MP2 2
static int fallback_calls;
static int clk_hw_determine_rate_no_reparent(struct clk_hw *h,struct clk_rate_request *r) {fallback_calls++;return 42;}
''' + api + clock + r'''
int main(void) {
 cpus[0]=(struct device){8,&nodes[0]};cpus[1]=(struct device){9,&nodes[1]};
 for(int n=0;n<=10;n++) {
  failure=n;refs=clock_refs=configs=clears=actions=0;p=(struct mt6797_a72_preflight){.dev=&ownerdev,.mp2_clk=&mp2};
  int r=mt6797_a72_configure_opps(&p);
  assert((r==0)==(n==0||n==2));assert(!refs&&!clock_refs);
  assert(configs==(n==0||n>=9));assert(clears==(n==10));
  assert(actions==(n==0||n==10));
  if(n==0)assert(p.opp_cpu==&cpus[0]);
 }
 failure=0;
 struct dev_pm_opp o={338000000,800000};
 assert(!mt6797_a72_opp_clocks(&cpus[0],NULL,&o,NULL,false));
 assert(cpu_seen==8&&rate_seen==338000000&&div_seen==2&&uv_seen==800000);
 o=(struct dev_pm_opp){2587000000UL,1200000};set_error=-EUCLEAN;
 assert(mt6797_a72_opp_clocks(&cpus[1],NULL,&o,NULL,true)==-EUCLEAN);
 assert(cpu_seen==9&&div_seen==1&&uv_seen==1200000);
 int before=calls;struct device impostor=cpus[0];
 assert(mt6797_a72_opp_clocks(&impostor,NULL,&o,NULL,false)==-EINVAL);
 assert(mt6797_a72_opp_clocks(NULL,NULL,&o,NULL,false)==-EINVAL);
 assert(mt6797_a72_opp_clocks(&cpus[0],NULL,NULL,NULL,false)==-EINVAL);
 o.uv=UINT64_MAX;assert(mt6797_a72_opp_clocks(&cpus[0],NULL,&o,NULL,false)==-EINVAL);
 assert(calls==before);
 for(int domain=0;domain<4;domain++) {
  struct mt6797_cpuclk_path_hw path={.domain=domain};struct clk_rate_request request={845000000};
  fallback_calls=0;
  assert(mt6797_cpuclk_path_determine_rate(&path.hw,&request)==(domain==2?-EOPNOTSUPP:42));
  assert(fallback_calls==(domain!=2)&&request.rate==845000000);
 }
 puts("PASS A72 OPP: shared-table/clock admission, exclusive rails, registration unwind, selected OPP routing, errors, MP2 rounding refusal");
}
'''
with tempfile.TemporaryDirectory(prefix='a72-opp-') as temp:
    src,exe=Path(temp,'test.c'),Path(temp,'test')
    src.write_text(c)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',
                    '-Wno-unused-parameter','-Wno-misleading-indentation','-fsanitize=undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
