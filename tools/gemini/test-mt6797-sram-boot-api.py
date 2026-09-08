#!/usr/bin/env python3
"""Run actual SRAM provider API functions against ownership/SMC boundary doubles."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / "drivers/regulator/mt6797-bigidvfs-sram.c").read_text()
def function(name):
    at = source.index(name + "(")
    start = source.rfind("\n", 0, at) + 1
    end = source.index("\n}\n", at) + 3
    return source[start:end]
functions = "\n".join(function(n) for n in [
    "mt6797_bigidvfs_sram_secure_read_word", "mt6797_bigidvfs_sram_secure_set",
    "mt6797_bigidvfs_sram_io_set", "mt6797_sram_first_start",
    "mt6797_sram_read_sample", "mt6797_sram_settle",
    "mt6797_sram_lock_consumer", "mt6797_sram_unlock_consumer",
    "mt6797_sram_read_boot_sample", "mt6797_sram_prepare_cpu8",
    "mt6797_sram_set_voltage_sel",
])
c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32; typedef int32_t s32;
#define U32_MAX UINT32_MAX
#define BIT(n) (1UL<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define IS_ERR(p) ((intptr_t)(p)<0 && (intptr_t)(p)>-4096)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define SMCCC_RET_NOT_SUPPORTED (-1)
#define PSCI_0_2_AFFINITY_LEVEL_OFF 1
#define MT6797_BIGIDVFS_FID_READ 0xc200035fUL
#define MT6797_BIGIDVFS_FID_SRAM_SET 0xc20003bfUL
#define MT6797_BIGIDVFS_SRAM_SELECTOR 0x102222b0U
#define MT6797_BIGIDVFS_SRAM_CALIBRATION 0x102222b4U
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held);m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held);m->held=false; }
struct device_node { int id; };
struct device { struct device_node *of_node; void *driver,*data; struct device *parent; struct mutex lock; };
struct regulator_desc { int id; };
struct regulator_dev { struct device dev; const struct regulator_desc *desc; int exclusive,open_count; void *data; };
struct regulator { struct device *dev; struct regulator_dev *rdev; };
struct mt6797_sram_sample {u32 control,calibration;};
struct mt6797_sram_first_state {bool attempted,ready,faulted;int error;struct mt6797_sram_sample samples[2];};
struct mt6797_sram_first_io { int (*set_voltage)(void *,unsigned);void (*settle)(void *,unsigned);int (*read_sample)(void *,struct mt6797_sram_sample *); };
struct mt6797_sram_io { int dummy; };
struct mt6797_bigidvfs_sram {struct device *dev;struct mutex lock;bool faulted;struct mt6797_sram_first_state first;};
static struct { int driver; } mt6797_bigidvfs_sram_driver;
static const struct regulator_desc mt6797_bigidvfs_sram_desc={1};
static struct mutex mt6797_sram_boot_lock;
static int mt6797_sram_boot_attempted;
static int atomic_cmpxchg(int *p,int old,int value) {int v=*p;if(v==old)*p=value;return v;}
static int atomic_read(int *p) {return *p;}
static void device_lock(struct device *d) {mutex_lock(&d->lock);}
static void device_unlock(struct device *d) {mutex_unlock(&d->lock);}
static void *dev_get_drvdata(struct device *d) {return d->data;}
static void *rdev_get_drvdata(struct regulator_dev *d) {return d->data;}
static struct device_node nodes[3];
static struct device provider,consumer;
static struct regulator_dev rdev;
static struct regulator supply;
static struct mt6797_bigidvfs_sram sram;
static int compatible,refs,missing,wrong_node,spm_error,reg_error,affinity_error;
static unsigned nr_cpu_ids=10;
static bool online[10],cpus_held;
static u32 isolation;
static struct regmap {int unused;} spm;
static int of_device_is_compatible(struct device_node *n,const char *s) {assert(n==consumer.of_node);assert(!strcmp(s,"mediatek,mt6797-a72-power"));return compatible;}
static struct device_node *of_parse_phandle(struct device_node *n,const char *s,int index) {assert(n==consumer.of_node && index==0);assert(!strcmp(s,"sram-big-supply"));if(missing)return NULL;refs++;return &nodes[wrong_node?2:1];}
static void of_node_put(struct device_node *n) {assert(n && refs>0);refs--;}
static struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *n,const char *s) {assert(n==consumer.of_node);assert(!strcmp(s,"mediatek,spm"));return spm_error?(void *)(intptr_t)-ENODEV:&spm;}
static int regmap_read(struct regmap *m,u32 reg,u32 *value) {assert(m==&spm && reg==0x290);*value=isolation;return reg_error;}
static void lockdep_assert_cpus_held(void) {assert(cpus_held);}
static bool cpu_online(unsigned cpu) {assert(cpu<10);return online[cpu];}
static unsigned cpu_logical_map(unsigned cpu) {return cpu;}
static int affinity(unsigned cpu,unsigned level) {assert(cpu>=8 && cpu<=9 && level==0);return affinity_error?:1;}
static struct {int (*affinity_info)(unsigned,unsigned);} psci_ops={affinity};
static unsigned reads,sets,waits,ordinary_sets;
static int fail_read_at,set_error;
static u32 words[4];
struct arm_smccc_res {unsigned long a0;};
static void arm_smccc_smc(unsigned long fid,unsigned long arg,unsigned long a2,unsigned long a3,unsigned long a4,unsigned long a5,unsigned long a6,unsigned long a7,struct arm_smccc_res *res)
{
 assert(mt6797_sram_boot_lock.held && sram.lock.held);
 if(fid==MT6797_BIGIDVFS_FID_SRAM_SET) {
  assert(arg==110000 && sram.first.attempted && mt6797_sram_boot_attempted);
  sets++;res->a0=set_error;return;
 }
 assert(fid==MT6797_BIGIDVFS_FID_READ && reads<4);
 assert(arg==(reads%2?MT6797_BIGIDVFS_SRAM_CALIBRATION:MT6797_BIGIDVFS_SRAM_SELECTOR));
 res->a0=(int)reads==fail_read_at?~0UL:words[reads];reads++;
}
static void usleep_range(unsigned min,unsigned max) {assert(sets==1 && min==240 && max==290);waits++;}
static int mt6797_bigidvfs_sram_io_set(void *,unsigned);
static void mt6797_sram_settle(void *,unsigned);
static int mt6797_sram_read_sample(void *,struct mt6797_sram_sample *);
static const struct mt6797_sram_first_io mt6797_sram_boot_io={mt6797_bigidvfs_sram_io_set,mt6797_sram_settle,mt6797_sram_read_sample};
static const struct mt6797_sram_io mt6797_bigidvfs_sram_io={0};
static const unsigned mt6797_bigidvfs_sram_voltages[16]={0};
static int mt6797_sram_set_transaction(const struct mt6797_sram_io *io,void *ctx,const unsigned *v,unsigned count,unsigned selector)
{assert(io && ctx==&sram && v && count==16 && selector==11);ordinary_sets++;return 0;}
''' + functions + r'''
static void reset(void)
{
 memset(&sram,0,sizeof(sram));memset(&provider,0,sizeof(provider));
 provider.driver=&mt6797_bigidvfs_sram_driver.driver;provider.data=&sram;
 consumer=(struct device){.of_node=&nodes[0]};sram.dev=&provider;
 rdev=(struct regulator_dev){.dev={.parent=&provider,.of_node=&nodes[1]},.desc=&mt6797_bigidvfs_sram_desc,.exclusive=1,.open_count=1,.data=&sram};
 supply=(struct regulator){&consumer,&rdev};
 refs=missing=wrong_node=spm_error=reg_error=affinity_error=0;compatible=1;
 mt6797_sram_boot_attempted=0;mt6797_sram_boot_lock.held=false;
 reads=sets=waits=ordinary_sets=0;fail_read_at=-1;set_error=0;
 words[0]=words[2]=0x4008fb;words[1]=words[3]=0x8967;
 memset(online,0,sizeof(online));online[4]=true;nr_cpu_ids=10;cpus_held=true;
 psci_ops.affinity_info=affinity;
}
static void released(void) {assert(!refs && !provider.lock.held && !sram.lock.held && !mt6797_sram_boot_lock.held);}
int main(void)
{
 struct mt6797_sram_sample sample;
 reset();assert(mt6797_sram_read_boot_sample(&consumer,&supply,&sample)==0);assert(reads==4 && sets==0 && sample.control==0x4008fb);released();
 for(int i=0;i<4;i++){reset();fail_read_at=i;sample.control=123;assert(mt6797_sram_read_boot_sample(&consumer,&supply,&sample)==-EOPNOTSUPP);assert(sample.control==123 && sets==0);released();}
 reset();words[2]^=BIT(22);assert(mt6797_sram_read_boot_sample(&consumer,&supply,&sample)==-EAGAIN);released();
 for(int i=0;i<17;i++) {
  reset();unsigned cpu=8;
  switch(i){
  case 0: compatible=0;break;case 1:supply.dev=&provider;break;
  case 2:rdev.desc=NULL;break;case 3:rdev.exclusive=0;break;case 4:rdev.open_count=2;break;
  case 5:missing=1;break;case 6:wrong_node=1;break;case 7:provider.driver=NULL;break;
  case 8:provider.data=NULL;break;case 9:rdev.data=NULL;break;case 10:sram.faulted=true;break;
  case 11:online[8]=true;break;case 12:online[9]=true;break;case 13:online[4]=false;break;
  case 14:isolation=BIT(1);break;case 15:spm_error=1;break;case 16:cpu=9;break;
  }
  assert(mt6797_sram_prepare_cpu8(&consumer,&supply,cpu,&sample)<0);
  assert(sets==0 && reads==0 && !mt6797_sram_boot_attempted);released();isolation=0;
 }
 reset();reg_error=-EIO;assert(mt6797_sram_prepare_cpu8(&consumer,&supply,8,&sample)==-EIO);assert(sets==0);released();
 reset();affinity_error=2;assert(mt6797_sram_prepare_cpu8(&consumer,&supply,8,&sample)==-EBUSY);assert(sets==0);released();
 reset();psci_ops.affinity_info=NULL;assert(mt6797_sram_prepare_cpu8(&consumer,&supply,8,&sample)==-EOPNOTSUPP);assert(sets==0);released();
 reset();assert(mt6797_sram_prepare_cpu8(&consumer,&supply,8,&sample)==0);assert(sets==1 && reads==4 && waits==1);released();
 assert(mt6797_sram_prepare_cpu8(&consumer,&supply,8,&sample)==-EUCLEAN);assert(sets==1);released();
 assert(mt6797_sram_set_voltage_sel(&rdev,11)==-EUCLEAN);assert(ordinary_sets==0);released();
 /* Simulate replacement driver data: the boot latch must still refuse effects. */
 memset(&sram.first,0,sizeof(sram.first));assert(mt6797_sram_prepare_cpu8(&consumer,&supply,8,&sample)==-EUCLEAN);assert(sets==1);released();
 for(int i=0;i<5;i++){reset();if(i==0)set_error=-1;else fail_read_at=i-1;assert(mt6797_sram_prepare_cpu8(&consumer,&supply,8,&sample)==-EUCLEAN);assert(sram.faulted && sets==1);released();assert(mt6797_sram_set_voltage_sel(&rdev,11)==-EUCLEAN);assert(ordinary_sets==0);released();}
 reset();assert(mt6797_sram_set_voltage_sel(&rdev,11)==0);assert(ordinary_sets==1);released();
 puts("PASS SRAM provider ownership, full-word reads, CPU/PSCI/isolation gates, reprobe latch, and ordinary-setter exclusion");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-sram-api-") as tmp:
    src,exe=Path(tmp,"test.c"),Path(tmp,"test")
    src.write_text(c)
    subprocess.run([os.environ.get("HOSTCC","cc"),"-std=gnu11","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-Wno-sign-compare",str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
