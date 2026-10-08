#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production observation/transaction snapshot paths with changing reads.

Mocks only register reads, the CSPM lease and mutex. Covers domain isolation,
strict transaction admission, read failures and lease-release fault fencing.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/clk/mediatek/clk-mt6797-cpu.c').read_text()
def extract(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]
core = '\n'.join(re.sub(r'^#include .*\n', '', (root / f).read_text(), flags=re.M)
                 for f in ('drivers/clk/mediatek/clk-mt6797-cpu.h',
                           'drivers/clk/mediatek/clk-mt6797-cpu-core.c'))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define __iomem
#define U32_MAX UINT32_MAX
#define min(a,b) ((a)<(b)?(a):(b))
#define BIT(n) (1UL<<(n))
#define GENMASK(h,l) ((~0UL<<(l)) & (~0UL>>(63-(h))))
#define FIELD_GET(m,v) (((v)&(m))>>__builtin_ctzl(m))
#define FIELD_PREP(m,v) (((v)<<__builtin_ctzl(m))&(m))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define div_u64(a,b) ((a)/(b))
#define mult_frac(x,n,d) (((x)/(d))*(n)+(((x)%(d))*(n))/(d))
#define EXPORT_SYMBOL_GPL(x)
'''
prefix += '\n'.join(line for line in source.splitlines() if line.startswith('#define MT6797_CPUCLK_') and any(key in line for key in ('MP0_PLL_CON1 ', 'MP1_PLL_CON1 ', 'CCI_PLL_CON1 ', 'MUXSEL ', 'CLKDIV '))) + '\n'
stubs = r'''
struct mt6797_cpuclk_observer {
 void *mcumixed; int lock; bool faulted, probe_snapshot_valid; void *handoff;
 struct mt6797_cpuclk_snapshot probe_snapshot;
};
static struct mt6797_cpuclk_observer owner;
static struct mt6797_cpuclk_snapshot reads[2];
static unsigned int calls, leases, releases, transactions, fail_read;
static int acquire_error, release_error, transaction_error;
static unsigned char registers[0x800];
static unsigned int seen, secure_calls, fail_secure;
static u32 readl_relaxed(void *address)
{
 unsigned int reg=(unsigned char *)address-registers, word;
 assert(owner.lock);
 switch(reg){
 case MT6797_CPUCLK_MUXSEL: assert(calls<2); calls++; word=6; break;
 case MT6797_CPUCLK_CLKDIV: word=7; break;
 case MT6797_CPUCLK_MP0_PLL_CON1: word=0; break;
 case MT6797_CPUCLK_MP1_PLL_CON1: word=1; break;
 case MT6797_CPUCLK_CCI_PLL_CON1: word=3; break;
 default: assert(0); return 0;
 }
 assert(calls); seen|=BIT(word);
 if(word<4)return reads[calls-1].armpll_con1[word];
 return word==6 ? reads[calls-1].muxsel : reads[calls-1].clkdiv;
}
static int mt6797_cpuclk_secure_read(unsigned long fid,u32 address,u32 *value)
{
 assert(owner.lock && calls && fid==MT6797_CPUCLK_SMC_REG_READ);
 secure_calls++;
 if(calls==fail_read || secure_calls==fail_secure)return -EIO;
 if(address==MT6797_CPUCLK_MP2_PLL_CON0){seen|=BIT(4);*value=reads[calls-1].mp2_armpll_con0;}
 else {assert(address==MT6797_CPUCLK_MP2_PLL_CON1);seen|=BIT(5);*value=reads[calls-1].mp2_armpll_con1;}
 return 0;
}
static void mutex_lock(int *lock) { assert(!*lock); *lock=1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock=0; }
static int mt6797_cpuclk_read_once(struct mt6797_cpuclk_observer *o,
 struct mt6797_cpuclk_snapshot *s)
{
 assert(o==&owner && o->lock && calls<2); *s=reads[calls++];
 return calls==fail_read ? -EIO : 0;
}
static int mt6797_cpuclk_lease_acquire(struct mt6797_cpuclk_observer *o, void *cspm)
{ assert(o==&owner && o->lock && cspm==&owner); leases++; return acquire_error; }
static int mt6797_cpuclk_lease_release(struct mt6797_cpuclk_observer *o, void *cspm)
{ assert(o==&owner && o->lock && cspm==&owner); releases++; return release_error; }
static int mt6797_dvfsp_cspm_transaction(void *handoff, int (*run)(void *,void *),void *ctx)
{ assert(handoff==&owner && owner.lock); transactions++; return transaction_error ? transaction_error : run(&owner,ctx); }
'''
functions = extract('static int mt6797_cpuclk_read_stable(') + '\n'
functions += extract('static int mt6797_cpuclk_read_domain(') + '\n'
functions += extract('static int mt6797_cpuclk_read_observation(') + '\n'
start = source.index('struct mt6797_cpuclk_snapshot_transaction {')
functions += source[start:source.index('\n};', start)+3] + '\n'
for signature in ('static int mt6797_cpuclk_snapshot_transaction(',
                  'static int\nmt6797_cpuclk_snapshot_locked(',
                  'static int\nmt6797_cpuclk_observer_sample(',
                  'static int\nmt6797_cpuclk_observer_snapshot(',
                  'static int\nmt6797_cpuclk_observer_observe('):
    functions += extract(signature) + '\n'
main = r'''
static void reset(void)
{
 owner=(struct mt6797_cpuclk_observer){.handoff=&owner,.mcumixed=registers};
 memset(reads,0,sizeof(reads));
 calls=leases=releases=transactions=fail_read=seen=secure_calls=fail_secure=0;
 acquire_error=release_error=transaction_error=0;
}
static u32 *word(struct mt6797_cpuclk_snapshot *s, unsigned int i)
{
 if(i<4)return &s->armpll_con1[i];
 switch(i){case 4:return &s->mp2_armpll_con0;case 5:return &s->mp2_armpll_con1;
 case 6:return &s->muxsel;case 7:return &s->clkdiv;default:assert(i==8);return &s->sync_dcm;}
}
int main(void)
{
 /* Independent register layout: MP0 mux[3:2]/div[9:5], MP1[5:4]/[14:10],
  * MP2[1:0]/[4:0], CCI[7:6]/[19:15]. DCM is not a rate input. */
 const unsigned int mux_shift[]={2,4,0,6},div_shift[]={5,10,0,15};
 struct mt6797_cpuclk_snapshot out;
 unsigned int cases=0;
 for(unsigned int domain=0;domain<4;domain++)for(unsigned int i=0;i<9;i++)for(unsigned int bit=0;bit<32;bit++){
  bool relevant=(domain==2 ? i==4 || i==5 : i==domain);
  if(i==6)relevant=bit>=mux_shift[domain] && bit<mux_shift[domain]+2;
  if(i==7)relevant=bit>=div_shift[domain] && bit<div_shift[domain]+5;
  reset();*word(&reads[1],i)^=1U<<bit;
  assert(mt6797_cpuclk_observer_observe(&owner,domain,&out)==(relevant ? -EAGAIN : 0));
  assert(calls==2 && leases==1 && releases==1 && transactions==1 && !owner.lock && !owner.faulted);
  assert(seen==(BIT(6)|BIT(7)|(domain==2 ? BIT(4)|BIT(5) : BIT(domain))));
  assert(secure_calls==(domain==2 ? 4U : 0U));
  reset();*word(&reads[1],i)^=1U<<bit;
  assert(mt6797_cpuclk_observer_snapshot(&owner,&out)==-EAGAIN);
  assert(calls==2 && releases==1 && !owner.lock && !owner.faulted);cases++;
 }
 for(unsigned int domain=0;domain<=4;domain++)for(unsigned int failure=1;failure<=5;failure++){
  if(failure<=2 && domain!=2 && domain!=4)continue;
  reset();int expected=-EIO;
  if(failure<=2)fail_read=failure;
  if(failure==3)acquire_error=-EIO;
  if(failure==4){release_error=-EIO;expected=-EUCLEAN;}
  if(failure==5)transaction_error=-EIO;
  int ret=domain==4 ? mt6797_cpuclk_observer_snapshot(&owner,&out) : mt6797_cpuclk_observer_observe(&owner,domain,&out);
  assert(ret==expected && !owner.lock && owner.faulted==(failure==4));
  assert(calls==(failure<=2 ? failure : failure==4 ? 2U : 0U));
  assert(releases==(failure<=2 || failure==4));
 }
 for(unsigned int failure=1;failure<=4;failure++){
  reset();fail_secure=failure;
  assert(mt6797_cpuclk_observer_observe(&owner,2,&out)==-EIO);
  assert(secure_calls==failure && releases==1 && !owner.lock && !owner.faulted);
 }
 for(unsigned int domain=0;domain<4;domain++)if(domain!=2){
  reset();fail_secure=1;
  assert(!mt6797_cpuclk_observer_observe(&owner,domain,&out));
  assert(!secure_calls && releases==1 && !owner.lock);
 }
 reset();owner.faulted=true;
 assert(mt6797_cpuclk_observer_observe(&owner,0,&out)==-EUCLEAN && !calls && !transactions && !owner.lock);
 reset();owner.probe_snapshot_valid=true;owner.probe_snapshot.muxsel=0x54;
 assert(!mt6797_cpuclk_observer_observe(&owner,1,&out) && out.muxsel==0x54 && !calls && !transactions && !owner.lock);
 reset();assert(mt6797_cpuclk_observer_observe(&owner,4,&out)==-EINVAL && !calls && !transactions);
 assert(!mt6797_cpuclk_observations_equal(0,NULL,&out));
 assert(!mt6797_cpuclk_observations_equal(4,&out,&out));
 printf("PASS %u single-bit domain isolation cases, exact domain register access, strict full snapshots,23 injected errors and ownership checks\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-clock-snapshot-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(prefix + core + stubs + functions + main)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
