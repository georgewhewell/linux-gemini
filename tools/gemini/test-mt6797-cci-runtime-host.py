#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute native CCI runtime callbacks and probe unwind with boundary doubles."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
def source(path):
    return re.sub(r'^#include .*\n','',(root/path).read_text(),flags=re.M)
policy=source('include/linux/soc/mediatek/mt6797-cci-policy.h')
driver=source('drivers/devfreq/mt6797-cci-devfreq.c')
dts=(root/'arch/arm64/boot/dts/mediatek/mt6797-gemini-pda.dts').read_text()
start=dts.index('cci_opp: opp-table-cci')
block=dts[start:dts.index('\n\t};',start)]
points=re.findall(r'opp-hz = /bits/ 64 <(\d+)>;\s+opp-microvolt = <(\d+) (\d+) (\d+)>;',block)
expected=re.findall(r'\{ (\d+), (\d+) \}',policy)
assert len(points)==len(expected)==16
assert [(hz,uv) for hz,uv,_,_ in points]==expected
assert all(uv==lo and hi=='1200000' for _,uv,lo,hi in points)
prelude=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define EPROBE_DEFER 517
#define current get_current()
#define GFP_KERNEL 0
#define __init
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define device_initcall(x)
#define BIT(n) (1U<<(n))
#define CPUFREQ_POSTCHANGE 1
#define CPUFREQ_CREATE_POLICY 0
#define CPUFREQ_REMOVE_POLICY 1
#define CPUFREQ_TRANSITION_NOTIFIER 0
#define CPUFREQ_POLICY_NOTIFIER 1
#define CPUHP_AP_ONLINE_DYN 100
#define DEVFREQ_GOV_SUSPEND 4
#define DEVFREQ_GOV_RESUME 5
#define DEVFREQ_GOV_FLAG_IMMUTABLE 1
#define DEVFREQ_GOV_FLAG_IRQ_DRIVEN 2
#define NOTIFY_DONE 0
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define hlist_entry(p,t,m) container_of(p,t,m)
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define dev_err_probe(dev,ret,...) (ret)
#define dev_info(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
#define pm_sleep_ptr(p) (p)
#define DEFINE_SIMPLE_DEV_PM_OPS(n,s,r) int n
#define for_each_possible_cpu(c) for((c)=0;(c)<10;(c)++)
struct mutex { int held; };
static int cpu_read,cpu_write;
static void cpus_read_lock(void) { assert(!cpu_write);cpu_read++; }
static void cpus_read_unlock(void) { assert(cpu_read>0);cpu_read--; }
static void mutex_lock(struct mutex *m) { assert(!m->held && !cpu_write);m->held=1; }
static void mutex_unlock(struct mutex *m) { assert(m->held);m->held=0; }
typedef struct { unsigned int bits; } cpumask_t;
static cpumask_t online={1023};
#define cpu_online_mask (&online)
static bool cpumask_intersects(const cpumask_t *a,const cpumask_t *b) { return a->bits&b->bits; }
static void cpumask_set_cpu(unsigned int c,cpumask_t *m) { m->bits|=1U<<c; }
struct cpufreq_policy { cpumask_t *related_cpus; };
struct clk { unsigned long rate; int id; };
struct regulator { unsigned int vote,enabled; };
struct device { void *data; int id; };
struct platform_device { struct device dev; };
struct dev_pm_opp { unsigned long hz,uv; };
struct mt6797_dvfsp_handoff { bool faulted,claimed; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; bool pending; void (*fn)(struct work_struct*); };
#define to_delayed_work(w) container_of(w,struct delayed_work,work)
#define INIT_DELAYED_WORK(w,f) ((w)->fn=(f))
struct notifier_block { int (*notifier_call)(struct notifier_block*,unsigned long,void*); };
struct hlist_node { int unused; };
struct devfreq_dev_status { int unused; };
struct devfreq_dev_profile {
 unsigned long initial_freq;
 int (*target)(struct device*,unsigned long*,u32);
 int (*get_cur_freq)(struct device*,unsigned long*);
};
struct devfreq { struct mutex lock; void *data; struct device *parent; struct devfreq_dev_profile *profile; int suspend_count; };
struct devfreq_governor { const char *name; unsigned int flags;
 int (*get_target_freq)(struct devfreq*,unsigned long*);
 int (*event_handler)(struct devfreq*,unsigned int,void*); };
struct of_device_id { const char *compatible; };
struct platform_driver { int (*probe)(struct platform_device*);
 struct { const char *name; const struct of_device_id *of_match_table; int *pm; bool suppress_bind_attrs; } driver; };
static struct clk clocks[4];
static struct regulator supply;
static struct device cpudev[10];
static struct mt6797_dvfsp_handoff owner;
static struct devfreq df;
static unsigned long minimum_hz,maximum_hz;
static unsigned int votes,rate_writes,fences,queues,cancels,opp_refs,clk_refs,peer_uv,supported_uv;
static int regfail,resourcefail,opp_ready=1,voltage_fail,clock_fail,bad_opp;
static bool freq_registered,policy_registered,hp_registered,hp_instance;
static struct { void (*fn)(void*);void *data; } actions[8];
static unsigned int nactions;
static void *allocation;
static void *devm_kzalloc(struct device *d,size_t n,int flags)
{ (void)d;(void)flags;allocation=calloc(1,n);return allocation; }
static void platform_set_drvdata(struct platform_device *p,void *d) { p->dev.data=d; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static struct mt6797_dvfsp_handoff *mt6797_dvfsp_handoff_get(struct device *d)
{ (void)d;return resourcefail==1?ERR_PTR(-EPROBE_DEFER):&owner; }
static int mt6797_dvfsp_handoff_require_ready(struct mt6797_dvfsp_handoff *o) { return o->faulted?-EHOSTDOWN:0; }
static void mt6797_dvfsp_handoff_rail_fault(struct mt6797_dvfsp_handoff *o) { o->faulted=true;fences++; }
static int mt6797_dvfsp_handoff_cci_claim(struct mt6797_dvfsp_handoff *o)
{ assert(o==&owner && supply.enabled && supply.vote==1000000 && clocks[0].rate==611000000);
 assert(freq_registered && policy_registered && hp_instance && nactions==2);
 if(regfail==9)return -EHOSTDOWN;assert(!o->claimed);o->claimed=true;return 0; }
static struct clk *devm_clk_get(struct device *d,const char *name)
{ (void)d;const char *names[]={"cci","ll","l","a72"};
 if(resourcefail==2)return ERR_PTR(-EPROBE_DEFER);
 for(int i=0;i<4;i++)if(!strcmp(name,names[i]))return &clocks[i];assert(0);return NULL; }
static unsigned long clk_get_rate(struct clk *c) { return c->rate; }
static int clk_set_rate(struct clk *c,unsigned long rate)
{ assert(!owner.faulted && c==&clocks[0]);rate_writes++;if(clock_fail)return -EIO;c->rate=rate;return 0; }
static struct device *get_cpu_device(unsigned int cpu) { return &cpudev[cpu]; }
static struct clk *clk_get(struct device *d,const char *name)
{ assert(!name);clk_refs++;return &clocks[d->id<4?1:d->id<8?2:3]; }
static void clk_put(struct clk *c) { assert(c && clk_refs);clk_refs--; }
static bool clk_is_match(struct clk *a,struct clk *b) { return a==b; }
static struct regulator *devm_regulator_get(struct device *d,const char *name)
{ (void)d;assert(!strcmp(name,"proc"));return resourcefail==3?ERR_PTR(-EPROBE_DEFER):&supply; }
static int regulator_get_voltage(struct regulator *r)
{ assert(r==&supply);return supply.vote>peer_uv?supply.vote:peer_uv; }
static int regulator_set_voltage(struct regulator *r,int lo,int hi)
{ assert(!owner.faulted && r==&supply && hi==1200000);votes++;if(voltage_fail)return -EIO;r->vote=lo;return 0; }
static int regulator_is_supported_voltage(struct regulator *r,int lo,int hi)
{ assert(r==&supply && hi==1200000);return lo<=(int)supported_uv; }
static int regulator_enable(struct regulator *r) { if(regfail==7)return -EIO;r->enabled++;return 0; }
static int regulator_disable(struct regulator *r) { assert(r->enabled && owner.faulted);r->enabled--;return 0; }
static int devm_pm_opp_of_add_table(struct device *d) { (void)d;return resourcefail==4?-EINVAL:0; }
static int dev_pm_opp_get_opp_count(struct device *d)
{ for(int i=0;i<10;i++)if(d==&cpudev[i])return opp_ready?7:0;return bad_opp?15:16; }
static struct dev_pm_opp opp;
static struct dev_pm_opp *dev_pm_opp_find_freq_exact(struct device*,unsigned long,bool);
static struct dev_pm_opp *dev_pm_opp_find_freq_ceil(struct device*,unsigned long*);
static unsigned long dev_pm_opp_get_voltage(struct dev_pm_opp *o) { return o->uv; }
static void dev_pm_opp_put(struct dev_pm_opp *o) { assert(o==&opp && opp_refs);opp_refs--; }
static struct devfreq *devm_devfreq_add_device(struct device *d,struct devfreq_dev_profile *p,const char *gov,void *data)
{ assert(!strcmp(gov,"mt6797-cci"));if(regfail==1)return ERR_PTR(-ENOMEM);df.parent=d;df.data=data;df.profile=p;return &df; }
static int devfreq_update_target(struct devfreq*,unsigned long);
static void devfreq_get_freq_range(struct devfreq *d,unsigned long *lo,unsigned long *hi)
{ assert(d==&df);*lo=minimum_hz;*hi=maximum_hz; }
static int cpufreq_register_notifier(struct notifier_block *n,unsigned int type)
{ assert(n->notifier_call);if(regfail==(type?3:2))return -EIO;
 if(type)policy_registered=true;else freq_registered=true;return 0; }
static int cpufreq_unregister_notifier(struct notifier_block *n,unsigned int type)
{ assert(n->notifier_call);if(type){assert(policy_registered);policy_registered=false;}
 else{assert(freq_registered);freq_registered=false;}return 0; }
static int cpuhp_setup_state_multi(int state,const char *name,int (*up)(unsigned int,struct hlist_node*),int (*down)(unsigned int,struct hlist_node*))
{ assert(state==100 && name && up==down);if(regfail==4)return -ENOMEM;hp_registered=true;return 123; }
static int cpuhp_state_add_instance_nocalls(int state,struct hlist_node *n)
{ assert(state==123 && n && hp_registered);if(regfail==5)return -EIO;hp_instance=true;return 0; }
static void cpuhp_state_remove_instance_nocalls(int state,struct hlist_node *n)
{ assert(state==123 && n && hp_instance);hp_instance=false; }
static void cpuhp_remove_multi_state(int state) { assert(state==123 && hp_registered && !hp_instance);hp_registered=false; }
static void mt6797_cci_stop(void*);
static int devm_add_action_or_reset(struct device *d,void (*fn)(void*),void *context)
{ (void)d;if(regfail==(fn==mt6797_cci_stop?6:8)){fn(context);return -ENOMEM;}
 actions[nactions].fn=fn;actions[nactions++].data=context;return 0; }
static int system_unbound_wq;
static unsigned long msecs_to_jiffies(unsigned int n) { return n; }
static void mod_delayed_work(int wq,struct delayed_work *w,unsigned long delay)
{ (void)wq;assert(delay==0 || delay==20);w->pending=true;queues++; }
static void cancel_delayed_work_sync(struct delayed_work *w)
{ assert(!cpu_write && !df.lock.held);w->pending=false;cancels++; }
static int devfreq_suspend_device(struct devfreq*);
static int devfreq_resume_device(struct devfreq*);
static int devfreq_add_governor(struct devfreq_governor *g) { assert(g);return 0; }
static void devfreq_remove_governor(struct devfreq_governor *g) { assert(g); }
static int platform_driver_register(struct platform_driver *p) { assert(p->driver.suppress_bind_attrs);return 0; }
'''
main=r'''
static struct dev_pm_opp *dev_pm_opp_find_freq_exact(struct device *d,unsigned long hz,bool enabled)
{
 (void)d;assert(enabled);
 for(unsigned int i=0;i<16;i++)if(mt6797_cci_opps[i].hz==hz){opp=(struct dev_pm_opp){hz,mt6797_cci_opps[i].uv};opp_refs++;return &opp;}
 return ERR_PTR(-ERANGE);
}
static struct dev_pm_opp *dev_pm_opp_find_freq_ceil(struct device *d,unsigned long *hz)
{
 assert(cpu_read && opp_ready);
 /* Full vendor SB CPU tables, including upper points not yet enabled in DT. */
 static const unsigned long cpu_hz[2][16]={
  {221000000,338000000,481000000,624000000,715000000,806000000,897000000,1014000000,
   1118000000,1222000000,1274000000,1339000000,1391000000,1443000000,1495000000,1547000000},
  {325000000,468000000,650000000,832000000,962000000,1092000000,1209000000,1352000000,
   1495000000,1625000000,1703000000,1755000000,1820000000,1885000000,1950000000,2002000000}};
 unsigned int cluster=d->id<4?0:1, index=0;
 while(index<16 && cpu_hz[cluster][index]<*hz)index++;
 if(index==16)return ERR_PTR(-ERANGE);
 *hz=cpu_hz[cluster][index];
 unsigned long uv=mt6797_cci_opps[index].uv;
 opp=(struct dev_pm_opp){*hz,uv};opp_refs++;return &opp;
}
static int devfreq_update_target(struct devfreq *d,unsigned long ignored)
{
 (void)ignored;assert(d->lock.held);unsigned long hz;
 int ret=mt6797_cci_governor_target(d,&hz);
 return ret?ret:d->profile->target(d->parent,&hz,0);
}
static int devfreq_suspend_device(struct devfreq *d)
{ if(++d->suspend_count==1)return mt6797_cci_governor_event(d,DEVFREQ_GOV_SUSPEND,NULL);return 0; }
static int devfreq_resume_device(struct devfreq *d)
{ assert(d->suspend_count>0);if(--d->suspend_count==0)return mt6797_cci_governor_event(d,DEVFREQ_GOV_RESUME,NULL);return 0; }
static struct platform_device pdev;
static void reset(void)
{
 assert(!allocation && !nactions && !cpu_read && !cpu_write && !opp_refs && !clk_refs);
 clocks[0]=(struct clk){629999328,0};clocks[1]=(struct clk){897000000,1};
 clocks[2]=(struct clk){1209000000,2};clocks[3]=(struct clk){750000000,3};
 for(int i=0;i<10;i++)cpudev[i].id=i;
 supply=(struct regulator){1000000,0};owner=(struct mt6797_dvfsp_handoff){0};peer_uv=1000000;supported_uv=1000000;
 memset(&df,0,sizeof(df));memset(&pdev,0,sizeof(pdev));online.bits=1023;
 votes=rate_writes=fences=queues=cancels=0;minimum_hz=169000000;maximum_hz=988000000;regfail=resourcefail=voltage_fail=clock_fail=bad_opp=0;opp_ready=1;
 assert(!freq_registered && !policy_registered && !hp_registered && !hp_instance);
}
static void cleanup(void)
{
 while(nactions){nactions--;actions[nactions].fn(actions[nactions].data);}
 assert(!freq_registered && !policy_registered && !hp_registered && !hp_instance);
 assert(!supply.enabled && !opp_refs && !clk_refs && !cpu_read && !df.lock.held);
 free(allocation);allocation=NULL;
}
static void work_once(struct mt6797_cci *c)
{ assert(c->work.pending);c->work.pending=false;c->work.fn(&c->work.work); }
int main(void)
{
 assert(!mt6797_cci_init());
 reset();assert(!mt6797_cci_probe(&pdev));struct mt6797_cci *c=pdev.dev.data;
 assert(owner.claimed && c->state.ready && clocks[0].rate==611000000 && supply.vote==1000000 && supply.enabled==1);
 assert(c->cpus[0].bits==15 && c->cpus[1].bits==240 && c->cpus[2].bits==768);
 assert(freq_registered && policy_registered && hp_instance && c->work.pending);
 work_once(c);assert(clocks[0].rate==611000000);
 clocks[1].rate=221000000;clocks[2].rate=325000000;
 assert(!mt6797_cci_frequency_event(&c->frequency_nb,CPUFREQ_POSTCHANGE,NULL));
 work_once(c);assert(clocks[0].rate==416000000 && supply.vote==900000); /* A72, no CPUFreq policy. */
 cpu_write=1;online.bits=255;mt6797_cci_hotplug(8,&c->hotplug_node);assert(c->work.pending);cpu_write=0;
 work_once(c);assert(clocks[0].rate==169000000 && supply.vote==770000);
 cpu_write=1;online.bits=1023;mt6797_cci_hotplug(8,&c->hotplug_node);cpu_write=0;
 work_once(c);assert(clocks[0].rate==416000000 && supply.vote==900000);
 opp_ready=0;mt6797_cci_queue(c,0);unsigned int writes=rate_writes;work_once(c);
 assert(c->work.pending && rate_writes==writes && !fences);opp_ready=1;work_once(c);
 unsigned long requested=169000000;
 assert(mt6797_cci_target(&pdev.dev,&requested,0)==-EAGAIN && rate_writes==writes);
 maximum_hz=325000000;mt6797_cci_queue(c,0);work_once(c);
 assert(!c->work.pending && !fences && rate_writes==writes);maximum_hz=988000000;
 minimum_hz=676000000;mt6797_cci_queue(c,0);work_once(c);
 assert(!c->work.pending && !fences && rate_writes==writes);minimum_hz=169000000;
 /* With the board voltage range enabled, a higher valid QoS vote works. */
 supported_uv=1200000;minimum_hz=676000000;mt6797_cci_queue(c,0);work_once(c);
 assert(clocks[0].rate==676000000 && supply.vote==1040000 && regulator_get_voltage(&supply)==1040000);
 minimum_hz=169000000;peer_uv=770000;mt6797_cci_queue(c,0);work_once(c);
 assert(clocks[0].rate==416000000 && supply.vote==900000 && regulator_get_voltage(&supply)==900000);
 /* Each newly enabled CPU OPP must raise CCI to its shared-rail budget. */
 static const unsigned long upper_cpu[2][9]={{1014000000,1118000000,1222000000,1274000000,1339000000,1391000000,1443000000,1495000000,1547000000},
                                            {1352000000,1495000000,1625000000,1703000000,1755000000,1820000000,1885000000,1950000000,2002000000}};
 /* L1703/L1885 require the next CCI OPP to satisfy the half-rate floor. */
 static const unsigned long upper_cci[2][9]={
  {676000000,754000000,819000000,845000000,884000000,910000000,936000000,975000000,988000000},
  {676000000,754000000,819000000,884000000,884000000,910000000,975000000,975000000,988000000}};
 static const unsigned int cci_uv[2][9]={
  {1040000,1070000,1100000,1130000,1140000,1150000,1170000,1190000,1200000},
  {1040000,1070000,1100000,1140000,1140000,1150000,1190000,1190000,1200000}};
 static const unsigned int upper_uv[]={1040000,1070000,1100000,1130000,1140000,1150000,1170000,1190000,1200000};
 for(unsigned int cluster=1;cluster<=2;cluster++) for(unsigned int point=0;point<sizeof(upper_uv)/sizeof(upper_uv[0]);point++) {
  unsigned long saved=clocks[cluster].rate;
  clocks[cluster].rate=upper_cpu[cluster-1][point];peer_uv=upper_uv[point];
  mt6797_cci_frequency_event(&c->frequency_nb,CPUFREQ_POSTCHANGE,NULL);work_once(c);
  assert(clocks[0].rate==upper_cci[cluster-1][point] && supply.vote==cci_uv[cluster-1][point] && (unsigned int)regulator_get_voltage(&supply)==cci_uv[cluster-1][point] && !owner.faulted);
  clocks[cluster].rate=saved;peer_uv=770000;
  mt6797_cci_frequency_event(&c->frequency_nb,CPUFREQ_POSTCHANGE,NULL);work_once(c);
  assert(clocks[0].rate==416000000 && supply.vote==900000 && !owner.faulted);
 }
 devfreq_suspend_device(&df);assert(c->suspended);assert(!mt6797_cci_suspend(&pdev.dev));
 assert(df.suspend_count==2 && !c->work.pending);
 mt6797_cci_frequency_event(&c->frequency_nb,CPUFREQ_POSTCHANGE,NULL);assert(!c->work.pending);
 assert(!mt6797_cci_resume(&pdev.dev) && c->suspended && df.suspend_count==1);
 devfreq_resume_device(&df);assert(!c->suspended && c->work.pending);work_once(c);
 cpumask_t managed={15};struct cpufreq_policy policy={&managed};
 cpu_write=1;mt6797_cci_policy_event(&c->policy_nb,CPUFREQ_REMOVE_POLICY,&policy);cpu_write=0;
 assert(c->stopping && owner.faulted);queues=0;mt6797_cci_queue(c,0);assert(!queues);
 cleanup();
 for(int i=1;i<=9;i++) { reset();regfail=i;assert(mt6797_cci_probe(&pdev)<0);assert(owner.faulted && !owner.claimed);cleanup(); }
 for(int i=1;i<=4;i++) { reset();resourcefail=i;assert(mt6797_cci_probe(&pdev)<0);assert(!votes && !rate_writes && !fences);cleanup(); }
 reset();bad_opp=1;assert(mt6797_cci_probe(&pdev)==-EINVAL && !votes && !rate_writes);cleanup();
 for(int fault=0;fault<2;fault++) {
  reset();assert(!mt6797_cci_probe(&pdev));c=pdev.dev.data;work_once(c);
  clocks[1].rate=221000000;clocks[2].rate=325000000;
  if(fault)voltage_fail=1;else clock_fail=1;
  mt6797_cci_queue(c,0);work_once(c);assert(c->state.faulted && owner.faulted && !c->work.pending);
  queues=0;mt6797_cci_queue(c,0);assert(!queues);cleanup();
 }
 puts("PASS CCI runtime: native clock/vote callbacks, A72 and hotplug, OPP readiness, stale targets, nested PM, policy removal, 9 post-adoption unwind failures, ownership after vote/cleanup and 5 preflight failures");
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-cci-runtime-') as tmp:
    c,exe=Path(tmp,'test.c'),Path(tmp,'test')
    c.write_text(prelude+policy+driver+main)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-unused-parameter','-Wno-misleading-indentation',
                    '-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
