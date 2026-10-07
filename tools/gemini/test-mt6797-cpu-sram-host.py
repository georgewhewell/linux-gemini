#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run production SRAM callbacks, semaphore and handoff fencing with faultable IO.

This checks software sequencing, not physical regulator settling or calibration.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/regulator/mt6797-cpu-sram-regulator.c').read_text()
handoff = (root / 'drivers/soc/mediatek/mt6797-dvfsp-handoff.c').read_text()
access = (root / 'drivers/soc/mediatek/mt6797-dvfsp-cspm-access.c').read_text()

def extract(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]

defines = '\n'.join(line for text in (source, handoff) for line in text.splitlines()
                    if line.startswith(('#define MT6797_CPULDO_', '#define MT6797_CSPM_')))
types = source[source.index('struct mt6797_cpu_sram {'):source.index('/* Code zero')]
functions = '\n'.join((
    extract(access, 'int mt6797_dvfsp_cspm_access_transaction('),
    extract(handoff, 'int mt6797_dvfsp_cspm_transaction('),
    extract(handoff, 'int mt6797_dvfsp_cspm_acquire('),
    extract(handoff, 'int mt6797_dvfsp_cspm_release('),
    *(extract(source, 'static int ' + name + '(') for name in (
        'mt6797_cpu_sram_decode', 'mt6797_cpu_sram_read',
        'mt6797_cpu_sram_transaction', 'mt6797_cpu_sram_run',
        'mt6797_cpu_sram_get_voltage_sel',
        'mt6797_cpu_sram_set_voltage_sel', 'mt6797_cpu_sram_is_enabled')),
))
prelude = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U<<(n))
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define MT6797_CPU_VSRAM_MIN_UV 1000000U
struct clk { int unused; };
struct regmap { int unused; };
struct mt6797_dvfsp_cspm_access { int permission, locked; };
struct mt6797_dvfsp_handoff {
 struct mt6797_dvfsp_cspm_access cspm_access; void *cspm;
};
typedef int (*mt6797_dvfsp_cspm_access_fn)(void *, void *);
typedef void (*mt6797_dvfsp_cspm_terminal_fn)(void *);
typedef int (*mt6797_dvfsp_cspm_transaction_fn)(void *, void *);
#define MT6797_DVFSP_PERMISSION_READY 1
#define MT6797_DVFSP_PERMISSION_FAULTED -1
static unsigned char cspm[0x800];
static struct clk clock;
static struct regmap map;
static struct mt6797_dvfsp_handoff owner;
static unsigned int refs, sem_held, sem_ops, clk_fail, acquire_fail, release_fail;
static unsigned int io, writes, fail_at, refuse_at, unstable, terminal_calls;
static unsigned int delay_calls, expected_delay, bad_enable, delay_total;
static u32 regs[3], originals[3];
static int clk_prepare_enable(struct clk *c)
{ assert(c==&clock && !refs && !owner.cspm_access.locked); if(clk_fail) return -EIO; refs=1; return 0; }
static void clk_disable_unprepare(struct clk *c)
{ assert(c==&clock && refs==1 && !owner.cspm_access.locked); refs=0; }
static void writel(u32 value, void *p)
{
 assert(refs && owner.cspm_access.locked); sem_ops++;
 if(p==cspm) { assert(value==0x0b160001); return; }
 assert(p==cspm+0x440 && value==1);
 if(sem_held) { if(!release_fail) sem_held=0; }
 else if(!acquire_fail) sem_held=1;
}
static u32 readl(void *p)
{
 assert(refs && owner.cspm_access.locked); sem_ops++;
 if(p==cspm) return 0x0b160001;
 assert(p==cspm+0x440); return sem_held;
}
static void udelay(unsigned int us)
{
 if(us==10) { delay_total+=us; return; }
 assert(us==expected_delay && refs && sem_held); delay_calls++;
}
static void mt6797_dvfsp_cspm_access_lock(struct mt6797_dvfsp_cspm_access *a)
{ assert(!a->locked); a->locked=1; }
static void mt6797_dvfsp_cspm_access_unlock(struct mt6797_dvfsp_cspm_access *a)
{ assert(a->locked); a->locked=0; }
static int mt6797_dvfsp_cspm_access_read(struct mt6797_dvfsp_cspm_access *a)
{ assert(a->locked); return a->permission; }
static void mt6797_dvfsp_cspm_access_set(struct mt6797_dvfsp_cspm_access *a,int p)
{ assert(a->locked); a->permission=p; }
static void mt6797_dvfsp_cspm_transaction_terminal(void *p)
{ assert(p==&owner && owner.cspm_access.locked && owner.cspm_access.permission==-1);
 terminal_calls++; }
static int regmap_read(struct regmap *m,unsigned int addr,u32 *value)
{
 assert(m==&map && refs && sem_held && owner.cspm_access.locked);
 assert(addr>=0xf98 && addr<=0xfa0 && !(addr&3));
 io++; if(io==fail_at) return -EIO;
 *value=regs[(addr-0xf98)/4];
 if(io==unstable) *value^=BIT(16);
 return 0;
}
static int regmap_write(struct regmap *m,unsigned int addr,u32 value)
{
 assert(m==&map && refs && sem_held && owner.cspm_access.locked);
 assert(addr==0xf9c || addr==0xfa0); /* Never write enable or unrelated registers. */
 unsigned int i=(addr-0xf98)/4;
 assert((value&0xf0f0f0f0)==(originals[i]&0xf0f0f0f0));
 io++; writes++;
 if(io!=refuse_at) regs[i]=value;
 if(bad_enable) regs[0]&=~BIT(0);
 return io==fail_at ? -EIO : 0; /* Error may follow an effective write. */
}
struct regulator_dev { void *data; };
static void *rdev_get_drvdata(struct regulator_dev *r) { return r->data; }
'''
main = r'''
static struct mt6797_cpu_sram sram={.infracfg=&map,.handoff=&owner,.i2c_clk=&clock};
static struct regulator_dev rdev={.data=&sram};
static void reset(unsigned int old,unsigned int target)
{
 owner=(struct mt6797_dvfsp_handoff){.cspm_access={.permission=1},.cspm=cspm};
 refs=sem_held=sem_ops=clk_fail=acquire_fail=release_fail=0;
 io=writes=fail_at=refuse_at=unstable=terminal_calls=0;
 delay_calls=bad_enable=delay_total=0;
 regs[0]=0x123400ff;
 regs[1]=0xa0b0c0d0|((old+7)*0x01010101U);
 regs[2]=0xe0f01020|((old+7)*0x01010101U);
 memcpy(originals,regs,sizeof(regs));
 unsigned int delta=abs((int)target-(int)old)*25000;
 expected_delay=target>old ? (delta+12499)/12500+5 : (2*delta+6249)/6250+5;
 if(expected_delay<25) expected_delay=25;
}
static void fenced(void)
{
 assert(owner.cspm_access.permission==-1 && terminal_calls==1);
 assert(!refs && !owner.cspm_access.locked);
 unsigned int count=io, sem_count=sem_ops;
 assert(mt6797_cpu_sram_set_voltage_sel(&rdev,0)==-EHOSTDOWN);
 assert(mt6797_cpu_sram_get_voltage_sel(&rdev)==-EHOSTDOWN);
 assert(io==count && sem_ops==sem_count && terminal_calls==1);
}
int main(void)
{
 unsigned int pairs=0,faults=0;
 for(unsigned int a=0;a<9;a++) for(unsigned int b=0;b<9;b++) {
  reset(a,b);
  assert(!mt6797_cpu_sram_set_voltage_sel(&rdev,b));
  assert(mt6797_cpu_sram_decode(regs)==(int)b);
  assert(regs[0]==originals[0] && !refs && !sem_held);
  assert(writes==(a==b ? 0U : 2U) && delay_calls==writes);
  unsigned int length=io;
  assert(mt6797_cpu_sram_get_voltage_sel(&rdev)==(int)b);
  assert(mt6797_cpu_sram_is_enabled(&rdev)==1);
  pairs++;
  for(unsigned int n=1;n<=length;n++) {
   reset(a,b); fail_at=n;
   int ret=mt6797_cpu_sram_set_voltage_sel(&rdev,b);
   assert(!refs && !sem_held && !owner.cspm_access.locked);
   if(n<=6) assert(ret==-EIO && !writes && !terminal_calls);
   else { assert(ret==-EUCLEAN); fenced(); }
   faults++;
  }
 }
 /* Reject missing enable bits and every illegal or mismatched lane code. */
 for(unsigned int lane=0;lane<8;lane++) for(unsigned int code=0;code<16;code++) {
  reset(2,3);
  unsigned int bank=1+lane/4,shift=(lane%4)*8;
  regs[bank]=(regs[bank]&~(15U<<shift))|(code<<shift);
  int ret=mt6797_cpu_sram_get_voltage_sel(&rdev);
  assert(ret==((code==0 || code==9) ? 2 : -EPROTO));
  assert(!writes && !refs && !sem_held);
 }
 for(unsigned int mask=0;mask<256;mask++) {
  reset(2,2); regs[0]=(regs[0]&~255U)|mask;
  assert(mt6797_cpu_sram_get_voltage_sel(&rdev)==(mask==255 ? 2 : -EPROTO));
  reset(2,2);
  for(unsigned int lane=0;lane<8;lane++) if(mask&BIT(lane))
   regs[1+lane/4]&=~(15U<<((lane%4)*8));
  assert(mt6797_cpu_sram_get_voltage_sel(&rdev)==2);
  assert(!mt6797_cpu_sram_set_voltage_sel(&rdev,2) && !writes);
 }
 for(unsigned int n=7;n<=9;n+=2) {
  reset(0,8); refuse_at=n;
  assert(mt6797_cpu_sram_set_voltage_sel(&rdev,8)==-EUCLEAN); fenced();
 }
 reset(0,8); bad_enable=1;
 assert(mt6797_cpu_sram_set_voltage_sel(&rdev,8)==-EUCLEAN); fenced();
 reset(0,8); unstable=4;
 assert(mt6797_cpu_sram_set_voltage_sel(&rdev,8)==-EAGAIN && !writes && !refs);
 reset(0,8); unstable=14;
 assert(mt6797_cpu_sram_set_voltage_sel(&rdev,8)==-EUCLEAN); fenced();
 reset(0,8); clk_fail=1;
 assert(mt6797_cpu_sram_set_voltage_sel(&rdev,8)==-EIO && !io && !sem_ops && !refs);
 reset(0,8); acquire_fail=1;
 assert(mt6797_cpu_sram_set_voltage_sel(&rdev,8)==-ETIMEDOUT && !io && !refs);
 assert(delay_total==2000);
 reset(0,8); release_fail=1;
 assert(mt6797_cpu_sram_set_voltage_sel(&rdev,8)==-EUCLEAN); fenced();
 assert(delay_total==2000);
 reset(0,8); owner.cspm_access.permission=0;
 assert(mt6797_cpu_sram_set_voltage_sel(&rdev,8)==-EHOSTDOWN && !io && !sem_ops);
 reset(0,8);
 assert(mt6797_cpu_sram_set_voltage_sel(&rdev,9)==-EINVAL);
 assert(mt6797_cpu_sram_set_voltage_sel(&rdev,UINT32_MAX)==-EINVAL);
 assert(!io && !sem_ops);
 printf("PASS %u SRAM pairs, %u IO failures, 8-lane decode, aliases, semaphore and global fencing\n",
  pairs,faults);
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-cpu-sram-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(prelude + defines + '\n' + types + functions + main)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-sign-compare', '-fsanitize=undefined',
                    '-fno-sanitize-recover=all', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
