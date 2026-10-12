#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual optional C2K power core with fault/lifetime and vendor oracles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'drivers/soc/mediatek/mt6797-md3-power.c'
HEADER = ROOT / 'include/linux/soc/mediatek/mt6797-md3-power.h'
VENDOR_SHA = '4a661ed078bddaba15b7a7a2db27ca59ced8883a1b71c545bbbe24cddd64d58f'


def function(source, name):
    match = re.search(r'^(?:static )?int ' + name + r'\([^;]*?\)\n\{', source, re.M)
    assert match, name
    visible = re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: ' ' * len(m.group()), source, flags=re.S)
    start = source.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (visible[end] == '{') - (visible[end] == '}')
        end += 1
    return source[match.start():end]


STUBS = {
    'types.h': '#include <stdbool.h>\n#include <stdint.h>\ntypedef uint32_t u32;\n',
    'mutex.h': '''#pragma once
#include <assert.h>
#include <pthread.h>
struct mutex { pthread_mutex_t value; };
static inline void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->value, 0)); }
static inline void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->value)); }
static inline void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->value)); }
''',
    'bitops.h': '#define BIT(n) (1U << (n))\n',
    'delay.h': 'void fake_sleep(unsigned int min, unsigned int max);\n#define usleep_range fake_sleep\n',
    'errno.h': '#include <asm-generic/errno.h>\n',
    'export.h': '#define EXPORT_SYMBOL_GPL(x)\n',
}

TEST = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "production.c"

struct event { unsigned reg; u32 value; };
struct fixture {
 struct mt6797_md3_power power;
 u32 regs[5]; struct event events[16]; unsigned nevents;
 unsigned ops, fail, begin_count, end_count, key_attempts, reads, updates;
 unsigned live_calls, dead_at, dead_after_read, reads_after_on, ack_delay, bus_reads, bus_delay;
 unsigned sleeps, cancel_sleep, stall_ack, stall_bus, corrupt_final, bad_write;
 int begin_error; bool leased, fail_after, positive_error, ignore_write;
 bool block_begin; pthread_barrier_t *entered, *resume;
 struct mt6797_md3_power_result end_result;
};
static _Thread_local struct fixture *current;
static unsigned cases;

static int tick(struct fixture *f) {
 f->ops++;
 return f->ops == f->fail ? (f->positive_error ? 1 : -EIO) : 0;
}
static int begin(void *arg) {
 struct fixture *f = arg; int ret;
 current=f; f->begin_count++; ret=tick(f);
 assert(!f->leased);
 if(ret) return ret;
 if(f->begin_error) return f->begin_error;
 f->leased=true;
 if(f->block_begin) {
  pthread_barrier_wait(f->entered); pthread_barrier_wait(f->resume);
 }
 return 0;
}
static void end(void *arg, const struct mt6797_md3_power_result *r) {
 struct fixture *f=arg;
 assert(f->leased); f->end_count++; f->end_result=*r; f->leased=false;
}
static int check_live(void *arg) {
 struct fixture *f=arg; int ret;
 assert(f->leased); f->live_calls++; ret=tick(f);
 if(ret) return ret;
 return f->dead_at && f->live_calls >= f->dead_at ? -ESHUTDOWN : 0;
}
static u32 read_value(struct fixture *f, unsigned reg) {
 assert(reg<5);
 if(reg==MT6797_MD3_SPM_STATUS && (f->regs[0] & 12)==12) {
  f->reads_after_on++;
  if(f->reads_after_on > f->ack_delay) {
   if(!(f->stall_ack & 1)) f->regs[1] |= C2K_ACK;
   if(!(f->stall_ack & 2)) f->regs[2] |= C2K_ACK;
  }
 }
 if(reg==MT6797_MD3_INFRA_PROTECT_STA && !(f->regs[3] & C2K_PROTECT)) {
  f->bus_reads++;
  if(f->bus_reads>f->bus_delay && !f->stall_bus) f->regs[4] &= ~C2K_PROTECT;
 }
 if(f->corrupt_final && f->nevents==7 && reg==MT6797_MD3_SPM_CONTROL)
  return f->regs[reg] ^ (1U << (f->corrupt_final-1));
 return f->regs[reg];
}
static int read_reg_mock(void *arg, enum mt6797_md3_power_reg reg, u32 *v) {
 struct fixture *f=arg; int ret;
 assert(f->leased); f->reads++; ret=tick(f);
 if(ret) return ret;
 *v=read_value(f,reg);
 if(f->dead_after_read==f->reads) f->dead_at=f->live_calls+1;
 return 0;
}
static void write_value(struct fixture *f, unsigned reg, u32 v) {
 assert(f->nevents < 16);
 f->events[f->nevents++] = (struct event){reg,v};
 if(reg<5) f->regs[reg]=v;
}
static int update_mock(void *arg, enum mt6797_md3_power_reg reg,u32 mask,u32 v) {
 struct fixture *f=arg; int ret;
 assert(f->leased && !(v & ~mask));
 assert((reg==0 && (mask==1 || mask==2 || mask==4 || mask==8 || mask==16)) ||
        (reg==3 && mask==C2K_PROTECT));
 f->updates++; ret=tick(f);
 if(ret && !f->fail_after) return ret;
 if(!f->ignore_write || f->bad_write != f->updates)
  write_value(f,reg,(f->regs[reg] & ~mask)|v);
 else
  write_value(f,reg,f->regs[reg]);
 return ret;
}
static int enable_spm(void *arg) {
 struct fixture *f=arg; int ret;
 assert(f->leased); f->key_attempts++; ret=tick(f);
 if(ret && !f->fail_after) return ret;
 write_value(f,5,0x0b160001); return ret;
}
void fake_sleep(unsigned min, unsigned max) {
 assert(min==10 && max==20 && current && current->leased);
 current->sleeps++;
 if(current->cancel_sleep==current->sleeps) current->dead_at=current->live_calls+1;
}
static const struct mt6797_md3_power_ops ops={
 .begin=begin,.end=end,.check_live=check_live,.read=read_reg_mock,
 .update_bits=update_mock,.enable_spm=enable_spm,
};
static void setup(struct fixture *f) {
 memset(f,0,sizeof(*f));
 f->regs[0]=0xaa000012; f->regs[1]=0x04000800; f->regs[2]=0x00800004;
 f->regs[3]=0xaaeee111; f->regs[4]=0x77ffe222;
 assert(!mt6797_md3_power_init(&f->power,&ops,f)); current=f;
}
static void finish(struct fixture *f) {
 assert(!f->leased); pthread_mutex_destroy(&f->power.lock.value); cases++;
}
static void failed(struct fixture *f,int expected) {
 struct mt6797_md3_power_result r; unsigned n;
 assert(mt6797_md3_power_on(&f->power)==expected);
 assert(!mt6797_md3_power_result(&f->power,&r));
 assert(r.state==MT6797_MD3_POWER_FAILED && r.error==expected);
 assert(r.may_have_written == (f->key_attempts != 0));
 assert(f->end_count==(f->begin_count && f->ops>1 && !f->begin_error));
 n=f->ops;
 assert(mt6797_md3_power_on(&f->power)==-ESHUTDOWN && f->ops==n);
 assert(!mt6797_md3_power_result(&f->power,&r) && r.error==expected);
}
static unsigned success(void) {
 struct fixture f; struct mt6797_md3_power_result r; unsigned n;
 static const unsigned regs[]={5,0,0,0,0,0,3};
 static const u32 values[]={0x0b160001,0xaa000016,0xaa00001e,0xaa00000e,
                           0xaa00000c,0xaa00000d,0xaaee0111};
 setup(&f); assert(!mt6797_md3_power_on(&f.power));
 assert(f.begin_count==1 && f.end_count==1 && f.nevents==7);
 for(unsigned i=0;i<7;i++) assert(f.events[i].reg==regs[i] && f.events[i].value==values[i]);
 assert(!mt6797_md3_power_result(&f.power,&r));
 assert(r.state==MT6797_MD3_POWER_DOMAIN_ON && !r.error && r.may_have_written && r.polls==2);
 n=f.ops; assert(mt6797_md3_power_on(&f.power)==-EALREADY && f.ops==n);
 assert((f.regs[1]&~C2K_ACK)==0x04000800 && (f.regs[2]&~C2K_ACK)==0x00800004);
 assert((f.regs[4]&~C2K_PROTECT)==(0x77ffe222&~C2K_PROTECT));
 finish(&f); return n;
}
static void failures(unsigned count) {
 struct fixture f;
 for(unsigned i=1;i<=count;i++) for(unsigned after=0;after<2;after++) {
  setup(&f); f.fail=i; f.fail_after=after; failed(&f,-EIO); finish(&f);
 }
 for(unsigned i=1;i<=count;i++) {
  setup(&f); f.fail=i; f.positive_error=true; failed(&f,-EPROTO); finish(&f);
 }
 setup(&f); f.begin_error=-EPROBE_DEFER; failed(&f,-EPROBE_DEFER);
 assert(!f.key_attempts && !f.end_count); finish(&f);
 for(unsigned bit=0;bit<5;bit++) {
  setup(&f); f.regs[0]^=1U<<bit; failed(&f,-EBUSY); assert(!f.key_attempts); finish(&f);
 }
 for(unsigned i=1;i<=2;i++) {
  setup(&f); f.regs[i]|=C2K_ACK; failed(&f,-EBUSY); assert(!f.key_attempts); finish(&f);
 }
 for(unsigned i=1;i<=5;i++) {
  setup(&f); f.corrupt_final=i; failed(&f,-EBUSY); assert(f.nevents==7); finish(&f);
 }
 for(unsigned i=1;i<=6;i++) {
  setup(&f); f.ignore_write=true; f.bad_write=i;
  failed(&f,i<=2 || i==6 ? -ETIMEDOUT : -EBUSY); finish(&f);
 }
}
static void waits(void) {
 struct fixture f;
 for(unsigned stall=1;stall<=3;stall++) {
  setup(&f); f.stall_ack=stall; failed(&f,-ETIMEDOUT);
  assert(f.power.result.polls==MT6797_MD3_POWER_MAX_POLLS && f.updates==2);
  assert(f.sleeps==MT6797_MD3_POWER_MAX_POLLS-1); finish(&f);
 }
 setup(&f); f.stall_bus=1; failed(&f,-ETIMEDOUT);
 assert(f.power.result.polls==MT6797_MD3_POWER_MAX_POLLS && f.updates==6); finish(&f);
 setup(&f); f.ack_delay=MT6797_MD3_POWER_MAX_POLLS-2;
 assert(!mt6797_md3_power_on(&f.power));
 assert(f.power.result.polls==MT6797_MD3_POWER_MAX_POLLS); finish(&f);
 setup(&f); f.ack_delay=MT6797_MD3_POWER_MAX_POLLS-1;
 failed(&f,-ETIMEDOUT); assert(f.bus_reads==0 && f.updates==6); finish(&f);
 setup(&f); f.ack_delay=3000; f.bus_delay=7000; failed(&f,-ETIMEDOUT);
 assert(f.power.result.polls==10000); finish(&f);
 for(unsigned bus=0;bus<2;bus++) {
  setup(&f); f.stall_bus=bus; f.stall_ack=bus?0:3; f.cancel_sleep=3;
  failed(&f,-ESHUTDOWN); assert(f.sleeps==3); finish(&f);
 }
 setup(&f); assert(!mt6797_md3_power_on(&f.power)); unsigned n=f.live_calls, nr=f.reads; finish(&f);
 for(unsigned i=1;i<=n;i++) {
  setup(&f); f.dead_at=i; failed(&f,-ESHUTDOWN); finish(&f);
 }
 for(unsigned i=1;i<=nr;i++) {
  setup(&f); f.dead_after_read=i; failed(&f,-ESHUTDOWN); assert(f.reads==i); finish(&f);
 }
}
struct thread_arg { struct fixture *f; int ret; };
static void *run_thread(void *arg) {
 struct thread_arg *a=arg; a->ret=mt6797_md3_power_on(&a->f->power); return NULL;
}
static void races(void) {
 for(unsigned n=0;n<32;n++) {
  struct fixture f; pthread_t a,b; pthread_barrier_t entered,resume;
  struct thread_arg x={&f,99},y={&f,99};
  setup(&f); pthread_barrier_init(&entered,NULL,2); pthread_barrier_init(&resume,NULL,2);
  f.block_begin=true; f.entered=&entered; f.resume=&resume;
  assert(!pthread_create(&a,NULL,run_thread,&x)); pthread_barrier_wait(&entered);
  assert(!pthread_create(&b,NULL,run_thread,&y));
  if(n&1) f.dead_at=1;
  pthread_barrier_wait(&resume); pthread_join(a,NULL); pthread_join(b,NULL);
  assert(x.ret==((n&1)?-ESHUTDOWN:0));
  assert(y.ret==((n&1)?-ESHUTDOWN:-EALREADY));
  assert(f.begin_count==1 && f.end_count==1);
  pthread_barrier_destroy(&entered); pthread_barrier_destroy(&resume); finish(&f);
 }
}
/*VENDOR_ORACLE*/
int main(void) {
 struct fixture f; struct mt6797_md3_power_ops incomplete=ops;
 incomplete.begin=NULL; assert(mt6797_md3_power_init(&f.power,&incomplete,&f)==-EINVAL);
 incomplete=ops; incomplete.end=NULL; assert(mt6797_md3_power_init(&f.power,&incomplete,&f)==-EINVAL);
 incomplete=ops; incomplete.check_live=NULL; assert(mt6797_md3_power_init(&f.power,&incomplete,&f)==-EINVAL);
 assert(mt6797_md3_power_on(NULL)==-EINVAL); assert(mt6797_md3_power_result(NULL,NULL)==-EINVAL);
 failures(success()); waits(); races(); vendor_oracle();
 printf("PASS actual C2K power cases=%u\n",cases); return 0;
}
'''

ORACLE_PREFIX = r'''
#define TOPAXI_PROTECT_LOCK
#define _TOPAXI_TIMEOUT_CNT_ 20000
#define SPM_PROJECT_CODE 0xB16
#define POWERON_CONFIG_EN 5
#define PWR_STATUS 1
#define PWR_STATUS_2ND 2
#define C2K_PWR_CON 0
#define INFRA_TOPAXI_PROTECTEN 3
#define INFRA_TOPAXI_PROTECTSTA1 4
#define INFRA_TOPAXI_PROTECTSTA0 4
#define INFRA_BUS_IDLE_STA5 4
#define PWR_RST_B C2K_RST_B
#define PWR_ISO C2K_ISO
#define PWR_ON C2K_ON
#define PWR_ON_2ND C2K_ON_2ND
#define PWR_CLK_DIS C2K_CLK_DIS
#define C2K_PROT_MASK C2K_PROTECT
#define C2K_PWR_STA_MASK C2K_ACK
#define STA_POWER_DOWN 0
#define spm_read(r) read_value(current,(r))
#define spm_write(r,v) write_value(current,(r),(v))
#define clk_readl(r) spm_read(r)
#define do_gettimeofday(t) ((void)gettimeofday((t),NULL))
#define pr_err(...) do { if(0) fprintf(stderr,__VA_ARGS__); } while(0)
#define BUG() abort()
static bool bus_locked;
#define spm_mtcmos_noncpu_lock(f) do { (f)=0; assert(!bus_locked); bus_locked=true; } while(0)
#define spm_mtcmos_noncpu_unlock(f) do { (void)(f); assert(bus_locked); bus_locked=false; } while(0)
int spm_topaxi_protect(unsigned int mask_value, int en);
'''
ORACLE_SUFFIX = r'''
static void vendor_oracle(void) {
 for(unsigned i=0;i<512;i++) {
  struct fixture native,vendor; setup(&native); setup(&vendor);
  u32 noise=0x9e3779b9U*i;
  native.regs[0]=vendor.regs[0]=(noise & ~31U)|18;
  native.regs[1]=vendor.regs[1]=(noise & ~C2K_ACK);
  native.regs[2]=vendor.regs[2]=(~noise & ~C2K_ACK);
  native.regs[3]=vendor.regs[3]=noise|C2K_PROTECT;
  native.regs[4]=vendor.regs[4]=(~noise)|C2K_PROTECT;
  assert(!mt6797_md3_power_on(&native.power)); current=&vendor;
  assert(!spm_mtcmos_ctrl_c2k(1)); assert(!bus_locked);
  assert(native.nevents==7 && vendor.nevents==native.nevents);
  assert(!memcmp(native.events,vendor.events,native.nevents*sizeof(struct event)));
  assert(!memcmp(native.regs,vendor.regs,sizeof(native.regs)));
  finish(&native); finish(&vendor);
 }
}
'''


def build_run(tmp, source, test, label, cc):
    (tmp/'production.c').write_text(source)
    (tmp/'test.c').write_text(test)
    binary = tmp/label
    cmd = [cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
           '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
           '-fsanitize=address,undefined', '-pthread', '-DEPROBE_DEFER=517',
           '-I'+str(tmp), '-I'+str(ROOT/'include'), str(tmp/'test.c'), '-o', str(binary)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode:
        print(build.stderr)
        build.check_returncode()
    return subprocess.run([str(binary)], capture_output=True, text=True, timeout=30,
                          env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1'})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--vendor', type=Path, help='Exact retained clk-mt6797-pg.c')
    parser.add_argument('--result', type=Path)
    args = parser.parse_args()
    source = SOURCE.read_text()
    oracle = 'static void vendor_oracle(void) {}\n'
    if args.vendor:
        data = args.vendor.read_bytes()
        assert hashlib.sha256(data).hexdigest() == VENDOR_SHA
        vendor = data.decode()
        oracle = (ORACLE_PREFIX+function(vendor,'spm_topaxi_protect')+'\n'+
                  function(vendor,'spm_mtcmos_ctrl_c2k')+'\n'+ORACLE_SUFFIX)
    test = TEST.replace('/*VENDOR_ORACLE*/',oracle)
    cc = os.environ.get('CC', 'cc')
    negatives = {
        'single-ack': ('(value & C2K_ACK) && (second & C2K_ACK)',
                       '(value & C2K_ACK) || (second & C2K_ACK)'),
        'retry-failed': ('p->result.state != MT6797_MD3_POWER_NEW',
                         'p->result.state == MT6797_MD3_POWER_DOMAIN_ON'),
        'swapped-iso-clock': ('MT6797_MD3_SPM_CONTROL, C2K_CLK_DIS, 0);\n\tif (!ret)\n\t\tret = update(p, MT6797_MD3_SPM_CONTROL, C2K_ISO, 0)',
                             'MT6797_MD3_SPM_CONTROL, C2K_ISO, 0);\n\tif (!ret)\n\t\tret = update(p, MT6797_MD3_SPM_CONTROL, C2K_CLK_DIS, 0)'),
        'lost-unrelated-protect': ('MT6797_MD3_INFRA_PROTECT_EN, C2K_PROTECT, 0)',
                                  'MT6797_MD3_INFRA_PROTECT_EN, ~0U, 0)'),
        'missing-post-live': ('return ret ?: live(p);','return ret;'),
        'false-off-after-error': ('p->result.may_have_written = true;',
                                 'p->result.may_have_written = false;'),
        'missing-final-readback': ('ret = verify(p, true);','ret = 0;'),
    }
    result = {'hardware_qualified':False,'vendor_oracle':bool(args.vendor),
              'source_sha256':hashlib.sha256(source.encode()).hexdigest(),
              'header_sha256':hashlib.sha256(HEADER.read_bytes()).hexdigest(),
              'negative_controls':{}}
    with tempfile.TemporaryDirectory(prefix='md3-power-') as directory:
        tmp=Path(directory); (tmp/'linux').mkdir()
        for name,text in STUBS.items(): (tmp/'linux'/name).write_text(text)
        run=build_run(tmp,source,test,'positive',cc)
        if run.returncode:
            print(run.stdout,run.stderr); raise SystemExit(run.returncode)
        print(run.stdout,end=''); result['stdout']=run.stdout
        for name,(old,new) in negatives.items():
            assert old in source, name
            run=build_run(tmp,source.replace(old,new),test,'negative-'+name,cc)
            assert run.returncode != 0, name+' escaped'
            assert 'Assertion' in run.stderr, (name,run.stderr)
            result['negative_controls'][name]='REJECTED'
            print('REJECTED '+name)
    result['result']='PASS'
    if args.result: args.result.write_text(json.dumps(result,indent=2)+'\n')


if __name__ == '__main__': main()
