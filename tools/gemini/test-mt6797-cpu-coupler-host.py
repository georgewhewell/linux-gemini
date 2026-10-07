#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run production CPU coupler callbacks with regulator/OF boundaries modeled."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
def stripped(path):
    return re.sub(r'^#include .*\n', '', (root/path).read_text(), flags=re.M)
def function(path, signature):
    s=(root/path).read_text(); a=s.index(signature)
    return s[a:s.index('\n}\n',a)+3]
headers='\n'.join(stripped('include/linux/soc/mediatek/'+n) for n in
 ('mt6797-cpu-rails.h','mt6797-cpu-rail-plan.h'))
core='\n'.join(function('drivers/regulator/core.c',sig) for sig in
 ('int regulator_check_consumers(', 'int regulator_check_voltage('))
fault=function('drivers/soc/mediatek/mt6797-dvfsp-cspm-access.c',
 'int mt6797_dvfsp_cspm_access_set(')
fault+='\n'+'\n'.join(function('drivers/soc/mediatek/mt6797-dvfsp-handoff.c',sig) for sig in (
 'static int mt6797_dvfsp_set_permission(',
 'static void __mt6797_dvfsp_fault_locked(',
 'static void mt6797_dvfsp_fault_locked(',
 'void mt6797_dvfsp_handoff_rail_fault('))
driver=stripped('drivers/soc/mediatek/mt6797-cpu-regulator-coupler.c')
prelude=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
typedef uint32_t u32;
#define current get_current()
typedef int suspend_state_t;
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define clamp(a,l,h) min(max(a,l),h)
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define PM_SUSPEND_ON 0
#define REGULATOR_CHANGE_VOLTAGE 1
#define BUG_ON(a) assert(!(a))
#define rdev_err(...) ((void)0)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define arch_initcall(x)
#define __init
#define EPROBE_DEFER 517
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
struct device_node { const char *name,*compat; struct device_node *parent,*peer; int refs; };
struct device { struct device_node *of_node; struct device *parent; };
struct regulation_constraints { int min_uV,max_uV,max_spread[1]; bool always_on,change; };
struct regulator_voltage { int min_uV,max_uV; };
struct regulator { struct regulator_voltage voltage[2]; };
struct regulator_dev { struct device dev; struct regulation_constraints *constraints;
 struct { int n_coupled; } coupling_desc; struct regulator consumer_list[3]; int id; };
#define list_for_each_entry(v,list,member) for((v)=*(list); (v)<*(list)+3; (v)++)
struct regulator_coupler {
 int (*attach_regulator)(struct regulator_coupler*,struct regulator_dev*);
 int (*detach_regulator)(struct regulator_coupler*,struct regulator_dev*);
 int (*balance_voltage)(struct regulator_coupler*,struct regulator_dev*,suspend_state_t);
};
struct mutex { int held; };
struct mt6797_dvfsp_cspm_access { int held,permission; };
struct mt6797_dvfsp_handoff { struct mutex lock; struct mt6797_dvfsp_cspm_access cspm_access;
 int state,fault_count,terminal; const char *reason; struct device *dev; };
static struct mt6797_dvfsp_handoff owner;
static void mutex_lock(struct mutex *m) { assert(!m->held && owner.cspm_access.held);m->held=1; }
static void mutex_unlock(struct mutex *m) { assert(m->held);m->held=0; }
static void mt6797_dvfsp_cspm_access_lock(struct mt6797_dvfsp_cspm_access *a)
{ assert(!a->held && !owner.lock.held);a->held=1; }
static void mt6797_dvfsp_cspm_access_unlock(struct mt6797_dvfsp_cspm_access *a)
{ assert(a->held && !owner.lock.held);a->held=0; }
#define MT6797_DVFSP_PERMISSION_FAULTED (-1)
#define MT6797_DVFSP_PERMISSION_READY 1
#define MT6797_DVFSP_FAULTED 3
#define dev_err(...) ((void)0)
static void complete_all(int *done) { *done=1; }
static int mt6797_dvfsp_cspm_access_read(const struct mt6797_dvfsp_cspm_access *a)
{ return a->permission; }
static int atomic_cmpxchg(int *p,int old,int new)
{ int before=*p; if(before==old)*p=new;return before; }
static bool regulator_ops_is_valid(struct regulator_dev *r,int op)
{ assert(op==REGULATOR_CHANGE_VOLTAGE);return r->constraints->change; }
static int get_error,registered; static bool machine=true;
static struct mt6797_dvfsp_handoff *mt6797_dvfsp_handoff_get(struct device *d)
{ assert(d);return get_error ? (void*)(intptr_t)get_error : &owner; }
static bool of_device_is_compatible(struct device_node *n,const char *c)
{ return n && n->compat && !strcmp(n->compat,c); }
static bool of_node_name_eq(struct device_node *n,const char *s)
{ return n && n->name && !strcmp(n->name,s); }
static struct device_node *of_get_parent(struct device_node *n)
{ n=n?n->parent:NULL;if(n)n->refs++;return n; }
static void of_node_put(struct device_node *n) { if(n)assert(n->refs-->0); }
static struct device_node *of_parse_phandle(struct device_node *n,const char *s,int i)
{ assert(!strcmp(s,"regulator-coupled-with") && !i);n=n?n->peer:NULL;if(n)n->refs++;return n; }
static bool of_machine_is_compatible(const char *s) { assert(!strcmp(s,"mediatek,mt6797"));return machine; }
static int regulator_coupler_register(struct regulator_coupler *c) { assert(c);registered++;return 0; }
static unsigned int reads,writes,fail_read,fail_write,ignore_write;
static bool effective,locked=true;
static u32 volts[2];
static int regulator_get_voltage_rdev(struct regulator_dev *r)
{ assert(locked && !owner.cspm_access.held);reads++;return reads==fail_read ? -EIO : (int)volts[r->id]; }
static int regulator_set_voltage_rdev(struct regulator_dev *r,int lo,int hi,int state)
{
 assert(locked && !owner.cspm_access.held && owner.cspm_access.permission==1 && !state && lo==hi);
 assert(lo>=r->constraints->min_uV && lo<=r->constraints->max_uV);
 writes++;
 if(writes!=ignore_write && (writes!=fail_write || effective)) volts[r->id]=lo;
 assert(volts[1]>=volts[0] && volts[1]-volts[0]<=300000);
 return writes==fail_write ? -EIO : 0;
}
'''
main=r'''
static struct regulation_constraints pc={770000,1200000,{300000},true,true};
static struct regulation_constraints sc={1000000,1200000,{300000},true,true};
static struct device parent;
static struct regulator_dev p={.constraints=&pc,.id=0,.dev.parent=&parent};
static struct regulator_dev s={.constraints=&sc,.id=1,.dev.parent=&parent};
static struct mt6797_cpu_coupler cpu;
static void reset(unsigned int vp,unsigned int vs,unsigned int target)
{
 cpu=(struct mt6797_cpu_coupler){.vproc=&p,.vsram=&s,.handoff=&owner};
 memset(p.consumer_list,0,sizeof(p.consumer_list));memset(s.consumer_list,0,sizeof(s.consumer_list));
 p.consumer_list[0].voltage[0]=(struct regulator_voltage){target,1200000};
 pc.min_uV=770000;pc.max_uV=1200000;pc.change=true;
 sc.min_uV=1000000;sc.max_uV=1200000;sc.change=true;
 owner=(struct mt6797_dvfsp_handoff){.cspm_access.permission=1};
 volts[0]=vp;volts[1]=vs;reads=writes=fail_read=fail_write=ignore_write=0;effective=false;
}
static int run(void) { return mt6797_cpu_coupler_balance(&cpu.coupler,&p,0); }
static void fenced(void)
{
 assert(cpu.faulted && owner.cspm_access.permission==-1 && owner.fault_count==1);
 assert(owner.state==MT6797_DVFSP_FAULTED && owner.terminal);
 assert(!strcmp(owner.reason,"cpu-rail-transition-failed"));
 assert(mt6797_dvfsp_cspm_access_set(&owner.cspm_access,1)==-EHOSTDOWN);
 unsigned int old_reads=reads,old_writes=writes;
 assert(run()==-EHOSTDOWN && reads==old_reads && writes==old_writes);
}
int main(void)
{
 unsigned int paths=0,faults=0;
 for(unsigned int vp=770000;vp<=1200000;vp+=10000)
 for(unsigned int vs=1000000;vs<=1200000;vs+=25000) {
  if(vs<vp || vs-vp>300000) continue;
  for(unsigned int target=770000;target<=1200000;target+=10000) {
   reset(vp,vs,target);assert(!run());
   struct mt6797_cpu_rail_pair final;
   assert(!mt6797_cpu_rail_target(target,&final));
   assert(volts[0]==final.vproc_uv && volts[1]==final.vsram_uv && !cpu.faulted);
   unsigned int nr=reads,nw=writes;paths++;
   for(unsigned int i=1;i<=nr;i++) {
    reset(vp,vs,target);fail_read=i;
    assert(run()==(i<=2 ? -EIO : -EUCLEAN));
    if(i<=2) assert(!writes && !cpu.faulted);else fenced();
    faults++;
   }
   for(unsigned int i=1;i<=nw;i++) for(unsigned int partial=0;partial<2;partial++) {
    reset(vp,vs,target);fail_write=i;effective=partial;
    assert(run()==-EUCLEAN && writes==i);fenced();faults++;
   }
   for(unsigned int i=1;i<=nw;i++) {
    reset(vp,vs,target);ignore_write=i;
    assert(run()==-EUCLEAN && writes==i);fenced();faults++;
   }
  }
 }
 /* Three A53/CCI requests merge through the actual regulator core helper. */
 reset(1000000,1100000,770000);
 p.consumer_list[1].voltage[0]=(struct regulator_voltage){900000,1200000};
 p.consumer_list[2].voltage[0]=(struct regulator_voltage){1040000,1200000};
 assert(!run() && volts[0]==1040000 && volts[1]==1150000);
 reset(1000000,1100000,900000);
 p.consumer_list[1].voltage[0]=(struct regulator_voltage){1040000,1200000};
 p.consumer_list[0].voltage[0].max_uV=1000000;
 assert(run()==-EINVAL && !writes && !cpu.faulted);
 reset(1000000,1100000,0);assert(!run() && !writes);
 reset(1000000,1100000,1000000);pc.change=false;pc.min_uV=pc.max_uV=1000000;
 assert(!run() && !writes); /* Adoption of a fixed rail is allowed. */
 reset(1000000,1100000,1200000);sc.change=false;
 assert(run()==-EINVAL && !writes && !cpu.faulted);
 reset(1000000,1100000,900000);s.consumer_list[0].voltage[0]=(struct regulator_voltage){1150000,1200000};
 assert(!run() && volts[0]==1030000 && volts[1]==1150000);
 reset(1000000,1100000,1200000);sc.max_uV=1150000;
 assert(run()==-EINVAL && !writes && !cpu.faulted);
 reset(1000000,1100000,1200000);
 assert(mt6797_cpu_coupler_balance(&cpu.coupler,&p,1)==-EOPNOTSUPP && !reads && !writes);
 reset(1000000,975000,1000000);assert(run()==-EUCLEAN && !writes);fenced();
 /* Explicit fault entry must also fence an owner blocked during PM. */
 reset(1000000,1100000,1000000);owner.cspm_access.permission=0;
 assert(mt6797_cpu_coupler_fault(&cpu)==-EUCLEAN);fenced();
 /* Production attachment, reciprocal OF identity, detach and registration. */
 struct device_node chip={.compat="dlg,da9214-legacy"};
 struct device_node regs={.name="regulators",.parent=&chip};
 struct device_node pn={.name="BUCKA",.parent=&regs};
 struct device_node sn={.compat="mediatek,mt6797-cpu-sram-regulator"};
 pn.peer=&sn;sn.peer=&pn;p.dev.of_node=&pn;s.dev.of_node=&sn;
 p.coupling_desc.n_coupled=s.coupling_desc.n_coupled=2;
 reset(1000000,1100000,1000000);cpu.vproc=cpu.vsram=NULL;cpu.handoff=NULL;
 assert(!mt6797_cpu_coupler_attach(&cpu.coupler,&p));
 get_error=-EPROBE_DEFER;
 assert(mt6797_cpu_coupler_attach(&cpu.coupler,&s)==-EPROBE_DEFER && !cpu.vsram);
 get_error=0;assert(!mt6797_cpu_coupler_attach(&cpu.coupler,&s));
 assert(mt6797_cpu_coupler_attach(&cpu.coupler,&p)==-EBUSY);
 assert(!mt6797_cpu_coupler_detach(&cpu.coupler,&s) && !cpu.vsram && !cpu.handoff);
 assert(!mt6797_cpu_coupler_detach(&cpu.coupler,&p) && !cpu.vproc);
 pn.name="BUCKB";assert(mt6797_cpu_coupler_attach(&cpu.coupler,&p)==1);pn.name="BUCKA";
 pn.peer=NULL;assert(mt6797_cpu_coupler_attach(&cpu.coupler,&p)==-EINVAL);pn.peer=&sn;
 sn.peer=NULL;assert(mt6797_cpu_coupler_attach(&cpu.coupler,&p)==-EINVAL);sn.peer=&pn;
 pc.max_spread[0]=100000;assert(mt6797_cpu_coupler_attach(&cpu.coupler,&p)==-EINVAL);pc.max_spread[0]=300000;
 pc.always_on=false;assert(mt6797_cpu_coupler_attach(&cpu.coupler,&p)==-EINVAL);pc.always_on=true;
 p.coupling_desc.n_coupled=3;assert(mt6797_cpu_coupler_attach(&cpu.coupler,&p)==-EINVAL);
 assert(!pn.refs && !sn.refs && !regs.refs && !chip.refs);
 machine=false;assert(!mt6797_cpu_coupler_init() && !registered);
 machine=true;assert(!mt6797_cpu_coupler_init() && registered==1);
 printf("PASS CPU coupler: %u paths, %u injected failures, shared votes, constraints, OF attachment and PM-blocked fencing\n",paths,faults);
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-coupler-') as tmp:
    src,exe=Path(tmp,'test.c'),Path(tmp,'test')
    src.write_text(prelude+headers+core+fault+driver+main)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O2','-Wall','-Wextra',
                    '-Werror','-Wno-sign-compare','-fsanitize=undefined',
                    '-fno-sanitize-recover=all',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
