#!/usr/bin/env python3
"""Exercise the actual online commit without a PSCI/SMC observer interface."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'drivers/soc/mediatek/mt6797-a72-power.c').read_text()
a=s.index('mt6797_a72_cpu_online_commit(')
b=s.index('\nstatic int mt6797_a72_install_cpu_on_gate',a)
function=s[a:b]
c=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define MT6797_A72_FIRST_CPU 8
struct hlist_node { int unused; };
struct transaction { bool active; };
struct mt6797_a72_preflight {
 struct hlist_node online_node; unsigned cpu_mask; bool faulted,cpu8_armed;
 struct transaction transaction; void *dev;
};
#define hlist_entry(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define dev_info(...) ((void)0)
static unsigned running, online, midr, commits, identity_checks, boot8_checks, boot9_checks;
static int commit_error,mt6797_a72_owner_lock;
static bool cpumask_test_cpu(unsigned c,const unsigned *mask){return !!(*mask & (1u<<c));}
static unsigned raw_smp_processor_id(void){return running;}
static bool cpu_online(unsigned c){return !!(online & (1u<<c));}
static unsigned read_cpuid_id(void){return midr;}
static void mutex_lock(int *p){assert(p==&mt6797_a72_owner_lock);}
static void mutex_unlock(int *p){assert(p==&mt6797_a72_owner_lock);}
static int mt6797_a72_validate_cpu8_identity(unsigned value){identity_checks++;return value==0x410fd081?0:-ENODEV;}
static int mt6797_a72_validate_cpu8_boot(unsigned cpu,bool cpu4,bool active,bool faulted){
 boot8_checks++;assert(cpu==8);return cpu4 && active && !faulted?0:-EPERM;
}
static int mt6797_a72_validate_cpu9_boot(unsigned cpu,bool cpu8,bool faulted){
 boot9_checks++;assert(cpu==9);return cpu8 && !faulted?0:-EPERM;
}
static int mt6797_a72_transaction_commit(struct transaction *t){
 commits++;assert(t->active);if(commit_error)return commit_error;t->active=false;return 0;
}
'''
c+='static int '+function+'\n'
c+=r'''
static struct mt6797_a72_preflight fresh(unsigned cpu){
 running=cpu;online=(1u<<4)|(1u<<8)|(1u<<9);midr=0x410fd081;
 commits=identity_checks=boot8_checks=boot9_checks=0;commit_error=0;
 return (struct mt6797_a72_preflight){.cpu_mask=(1u<<8)|(1u<<9),.cpu8_armed=true,.transaction={.active=true}};
}
static int run(unsigned cpu,struct mt6797_a72_preflight *p){return mt6797_a72_cpu_online_commit(cpu,&p->online_node);}
int main(void){
 struct mt6797_a72_preflight p=fresh(8);
 assert(run(8,&p)==0 && commits==1 && identity_checks==1 && boot8_checks==1);
 assert(!p.transaction.active && !p.cpu8_armed && !p.faulted);
 p=fresh(9);assert(run(9,&p)==0 && commits==0 && identity_checks==1 && boot9_checks==1);
 for(unsigned cpu=8;cpu<=9;cpu++){
  p=fresh(cpu);running=4;assert(run(cpu,&p)==-EPERM && p.faulted && !commits && !identity_checks);
  p=fresh(cpu);online &= ~(1u<<cpu);assert(run(cpu,&p)==-EPERM && p.faulted && !commits);
  p=fresh(cpu);midr=0x410fd080;assert(run(cpu,&p)==-ENODEV && p.faulted && !commits);
  p=fresh(cpu);p.faulted=true;assert(run(cpu,&p)==-EPERM && !commits);
 }
 p=fresh(8);online &= ~(1u<<4);assert(run(8,&p)==-EPERM && !commits && !p.cpu8_armed);
 p=fresh(8);p.transaction.active=false;assert(run(8,&p)==-EPERM && !commits);
 p=fresh(9);online &= ~(1u<<8);assert(run(9,&p)==-EPERM && !commits);
 p=fresh(8);commit_error=-EIO;assert(run(8,&p)==-EIO && p.faulted && !p.cpu8_armed && p.transaction.active);
 p=fresh(4);assert(run(4,&p)==0 && !commits && !identity_checks && !p.faulted);
 puts("PASS actual online commit: CPU8/9 identity/context/prerequisites/failure handling; no PSCI/SMC interface");
}
'''
with tempfile.TemporaryDirectory() as d:
 src,exe=Path(d,'t.c'),Path(d,'t');src.write_text(c)
 subprocess.run([os.environ.get('HOSTCC','cc'),'-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
