#!/usr/bin/env python3
"""Production GEN3 credit arithmetic under synthetic IRQ release ordering.
Does not exercise HIF integration, firmware page capacity, or queue scheduling.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
driver = root / 'drivers/net/wireless/mediatek/mt6797'
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint16_t u16; typedef uint64_t u64;
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define check_add_overflow(a,b,p) __builtin_add_overflow(a,b,p)
'''
for name in ['tx_credit.h', 'tx_credit.c']:
    program += re.sub(r'^#include .*\n', '', (driver / name).read_text(), flags=re.M)
program += r'''
int main(void) {
 struct mt6797_wifi_tx_credit c, before;
 u16 r[16]={0};
 for(unsigned queue=0;queue<16;queue++) {
  mt6797_wifi_tx_credit_init(&c);
  assert((mt6797_wifi_tx_credit_acquire(&c,queue,1)==0)==(queue==4||queue==15));
 }
 for(size_t length=1;length<=1664;length++) {
  mt6797_wifi_tx_credit_init(&c);
  assert(!mt6797_wifi_tx_credit_acquire(&c,4,length));
  assert(c.free[4]==13-(length+127)/128);
 }
 mt6797_wifi_tx_credit_init(&c); before=c;
 assert(mt6797_wifi_tx_credit_acquire(&c,4,SIZE_MAX)==-EMSGSIZE && !memcmp(&c,&before,sizeof(c)));
 assert(mt6797_wifi_tx_credit_acquire(&c,4,0)==-EINVAL);
 assert(mt6797_wifi_tx_credit_acquire(&c,16,1)==-EINVAL);
 for(unsigned order=0;order<2;order++) {
  mt6797_wifi_tx_credit_init(&c);
  assert(!mt6797_wifi_tx_credit_acquire(&c,4,1664) && c.free[4]==0);
  assert(mt6797_wifi_tx_credit_acquire(&c,4,1)==-EAGAIN);
  memset(r,0,sizeof(r)); r[order?14:4]=13;
  assert(!mt6797_wifi_tx_credit_release(&c,r) && c.free[4]==(order?13:0));
  assert(c.preused[4]==(order?13:0));
  memset(r,0,sizeof(r)); r[order?4:14]=13;
  assert(!mt6797_wifi_tx_credit_release(&c,r) && c.free[4]==13 && !c.done[4] && !c.available && !c.preused[4]);
 }
 mt6797_wifi_tx_credit_init(&c);
 memset(r,0,sizeof(r)); r[4]=65535; r[15]=65535;
 assert(!mt6797_wifi_tx_credit_release(&c,r) && !c.done[4] && !c.done[15]);
 assert(!mt6797_wifi_tx_credit_acquire(&c,4,1664));
 memset(r,0,sizeof(r)); r[14]=13;
 assert(!mt6797_wifi_tx_credit_release(&c,r) && c.free[4]==13 && c.preused[4]==13); /* Reuse FFA; old unknown queue reports were discarded. */
 r[14]=0; r[4]=13; assert(!mt6797_wifi_tx_credit_release(&c,r) && c.free[4]==13 && !c.preused[4]);
 mt6797_wifi_tx_credit_init(&c);
 assert(!mt6797_wifi_tx_credit_acquire(&c,4,1664));
 assert(!mt6797_wifi_tx_credit_acquire(&c,15,1664));
 memset(r,0,sizeof(r)); r[4]=r[15]=13; r[14]=7;
 assert(!mt6797_wifi_tx_credit_release(&c,r) && c.free[4]+c.free[15]==7);
 memset(r,0,sizeof(r)); r[14]=19;
 assert(!mt6797_wifi_tx_credit_release(&c,r) && c.free[4]==13 && c.free[15]==13 && !c.available);
 c.available=UINT64_MAX; before=c; r[14]=1;
 assert(mt6797_wifi_tx_credit_release(&c,r)==-EOVERFLOW && !memcmp(&c,&before,sizeof(c)));
 /* Reuse an FFA return, transmit again, then consume the old queue report.
  * The delayed report must not give the new packet a second refund. */
 mt6797_wifi_tx_credit_init(&c);
 assert(!mt6797_wifi_tx_credit_acquire(&c,4,1664));
 memset(r,0,sizeof(r)); r[14]=13;
 assert(!mt6797_wifi_tx_credit_release(&c,r) && c.free[4]==13 && c.preused[4]==13);
 assert(!mt6797_wifi_tx_credit_acquire(&c,4,1664));
 memset(r,0,sizeof(r)); r[4]=13;
 assert(!mt6797_wifi_tx_credit_release(&c,r) && !c.free[4] && !c.preused[4] && !c.done[4]);
 assert(mt6797_wifi_tx_credit_acquire(&c,4,1)==-EAGAIN);
 /* Exact M18 terminal ledger: returned memory must unblock DATA without
  * pretending the delayed queue report has already arrived. */
 mt6797_wifi_tx_credit_init(&c); c.free[4]=0; c.available=14;
 memset(r,0,sizeof(r));
 assert(!mt6797_wifi_tx_credit_release(&c,r) && c.free[4]==13 && c.preused[4]==13 && c.available==1);
 r[4]=13;
 assert(!mt6797_wifi_tx_credit_release(&c,r) && c.free[4]==13 && !c.preused[4] && c.available==1);
 /* FFA cached while idle may be consumed on a later admission shortage. */
 mt6797_wifi_tx_credit_init(&c); c.available=13;
 assert(!mt6797_wifi_tx_credit_acquire(&c,4,1664));
 assert(!mt6797_wifi_tx_credit_acquire(&c,4,1664) && !c.free[4] && c.preused[4]==13 && !c.available);
 mt6797_wifi_tx_credit_init(&c); c.free[4]=0; c.preused[4]=UINT64_MAX;
 before=c; memset(r,0,sizeof(r)); r[14]=1;
 assert(mt6797_wifi_tx_credit_release(&c,r)==-EOVERFLOW && !memcmp(&c,&before,sizeof(c)));
 /* Arbitrary split release batches must never inflate either queue's budget. */
 uint32_t random=0x91827364;
 uint64_t memory=26;
 mt6797_wifi_tx_credit_init(&c);
 for(unsigned step=0;step<100000;step++) {
  random=random*1664525+1013904223;
  unsigned q=(random&1)?4:15;
  if(random&2) {
   size_t length=1+(random>>16)%1664;
   if(!mt6797_wifi_tx_credit_acquire(&c,q,length)) memory-=(length+127)/128;
  }
  else {
   memset(r,0,sizeof(r)); r[q]=(random>>8)%17; r[14]=(random>>16)%17; memory+=r[14];
   assert(!mt6797_wifi_tx_credit_release(&c,r));
  }
  assert(c.free[4]<=13 && c.free[15]<=13);
  assert(c.available+c.free[4]+c.free[15]==memory);
  assert(c.done[4]<=13-c.free[4] && c.done[15]<=13-c.free[15]);
 }
 puts("PASS: credit admission 128-byte boundaries, exhausted queues, split done/FFA in both orders, delayed-report debt, exact M18 starvation, cached FFA admission, partial dual-queue refunds, atomic overflow and 100000 randomized memory conservation checks");
}
'''
if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='gemini-tx-credit-') as temporary:
        path = Path(temporary)
        (path / 'test.c').write_text(program)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
