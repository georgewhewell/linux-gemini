#!/usr/bin/env python3
"""Exercise production RX group configuration with transport/event faults.
No hardware register semantics or IRQ concurrency are emulated.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

driver = Path(__file__).resolve().parents[2] / 'drivers/net/wireless/mediatek/mt6797'
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define MT6797_WIFI_MCR_WTDR1 0x34
#define BIT(n) (1U << (n))
#define lockdep_assert_held(m) assert((m)->held)
#define msecs_to_jiffies(n) (n)
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=false; }
struct completion { int unused; };
struct mt6797_wifi_hif_wait_event { struct completion completion; bool armed; };
struct mt6797_wifi_hif_irq { struct mutex lifecycle_lock; struct mt6797_wifi_hif_wait_event wait_event; };
struct mt6797_wifi_hif_event_copy {
 u8 payload[16]; size_t payload_length,packet_length; int error; u8 id,sequence;
};
static struct mt6797_wifi_hif_irq irq;
static struct mt6797_wifi_hif_event_copy response;
static int status_error,arm_error,send_error,take_error,failure;
static unsigned sent,disarmed,waited;
static bool timeout, no_write_effect, stale_readback;
static unsigned timeout_at, fail_send_at, set_count;
static u32 get_unaligned_le32(const void *p) {
 const u8 *b=p; return (u32)b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24;
}
static void put_unaligned_le32(u32 v,void *p) { u8 *b=p; for(unsigned i=0;i<4;i++) b[i]=v>>(i*8); }
static int mt6797_wifi_hif_runtime_status(struct mt6797_wifi_hif_irq *i) {
 assert(i==&irq && i->lifecycle_lock.held); return status_error;
}
static int mt6797_wifi_command_init(u8 *p,size_t cap,size_t payload,u8 id,bool set,u8 seq) {
 assert(cap==16 && payload==8 && id==0xc0 && set==(seq==0xf1));
 assert(seq==0xf0 || seq==0xf1 || seq==0xf2);
 memset(p,0,16); p[0]=16; p[3]=0x80; p[4]=id; p[5]=0xa0; p[6]=set; p[7]=seq; return 0;
}
static int mt6797_wifi_hif_wait_event_arm(struct mt6797_wifi_hif_wait_event *w,u8 id,const u8 *m,size_t n) {
 assert(irq.lifecycle_lock.held && !w->armed && id==5 && !m && !n);
 if(arm_error) return arm_error;
 w->armed=true; return 0;
}
static int mt6797_wifi_hif_runtime_send(struct mt6797_wifi_hif_irq *i,u32 addr,const u8 *p,size_t n) {
 assert(i==&irq && i->lifecycle_lock.held && addr==0x34 && n==16);
 assert(p[0]==16 && !p[1] && !p[2] && p[3]==0x80 && p[4]==0xc0 && p[5]==0xa0);
 assert(get_unaligned_le32(p+8)==0x800c0068);
 sent++;
 if(p[6]) {
  assert(p[6]==1 && p[7]==0xf1 && !i->wait_event.armed);
  assert(get_unaligned_le32(p+12)==(get_unaligned_le32(response.payload+4)|1U));
  set_count++;
  if(!no_write_effect) memcpy(response.payload+4,p+12,4);
 } else {
  assert(i->wait_event.armed && !get_unaligned_le32(p+12));
  if(p[7]==0xf2 && !stale_readback) response.sequence=0xf2;
 }
 if(fail_send_at==sent) return -EIO;
 return send_error;
}
static unsigned long wait_for_completion_timeout(struct completion *c,unsigned long ticks) {
 assert(c==&irq.wait_event.completion && irq.wait_event.armed && ticks==1000);
 waited++; return !timeout && waited!=timeout_at;
}
static int mt6797_wifi_hif_wait_event_take(struct mt6797_wifi_hif_wait_event *w,struct mt6797_wifi_hif_event_copy *e) {
 assert(w->armed); *e=response; w->armed=false; return take_error;
}
static void mt6797_wifi_hif_runtime_fail(struct mt6797_wifi_hif_irq *i,int reason) {
 assert(i==&irq && i->lifecycle_lock.held && reason<0); failure=reason;
}
static void mt6797_wifi_hif_wait_event_disarm(struct mt6797_wifi_hif_wait_event *w) { w->armed=false; disarmed++; }
'''
src = (driver / 'hif_runtime.c').read_text()
program += src[src.index('int mt6797_wifi_hif_runtime_read_rxinf('):src.index('int mt6797_wifi_hif_runtime_start(')]
program += r'''
static void setup(void) {
 memset(&irq,0,sizeof(irq)); memset(&response,0,sizeof(response));
 status_error=arm_error=send_error=take_error=failure=0; sent=disarmed=waited=set_count=0; timeout=no_write_effect=stale_readback=false; timeout_at=fail_send_at=0;
 response.id=5; response.sequence=0xf0; response.packet_length=16; response.payload_length=8;
 put_unaligned_le32(0x800c0068,response.payload); put_unaligned_le32(0x12345678,response.payload+4);
}
static void run(int error) {
 u32 value=0xdeadbeef;
 mutex_lock(&irq.lifecycle_lock);
 assert(mt6797_wifi_hif_runtime_read_rxinf(&irq,0xf0,&value)==error);
 mutex_unlock(&irq.lifecycle_lock);
 assert(value==(error?0:0x12345678)); assert(!irq.lifecycle_lock.held && !irq.wait_event.armed);
}
int main(void) {
 setup(); run(0); assert(sent==1 && waited==1 && disarmed==1 && !failure);
 setup(); status_error=-EPIPE; run(-EPIPE); assert(!sent && !disarmed && !failure);
 setup(); arm_error=-EBUSY; run(-EBUSY); assert(!sent && !disarmed && !failure);
 setup(); send_error=-EAGAIN; run(-EAGAIN); assert(sent==1 && !waited && disarmed==1 && !failure);
 setup(); send_error=-EIO; run(-EIO); assert(!waited && disarmed==1);
 setup(); timeout=true; run(-ETIMEDOUT); assert(failure==-ETIMEDOUT && disarmed==1);
 setup(); take_error=-EIO; run(-EIO); assert(failure==-EIO && disarmed==1);
 setup(); response.error=-EMSGSIZE; run(-EMSGSIZE); assert(failure==-EMSGSIZE);
 for(unsigned bad=0;bad<5;bad++) {
  setup();
  switch(bad) {
   case 0: response.id++; break;
   case 1: response.sequence++; break;
   case 2: response.packet_length++; break;
   case 3: response.payload_length--; break;
   case 4: response.payload[0]++; break;
  }
  run(-EPROTO); assert(failure==-EPROTO && disarmed==1);
 }
 u32 before,after;
 setup();
 assert(!mt6797_wifi_hif_runtime_configure_rxinf(&irq,&before,&after));
 assert(before==0x12345678 && after==0x12345679 && sent==3 && set_count==1 && waited==2 && !failure);
 assert(!irq.lifecycle_lock.held && !irq.wait_event.armed);
 setup(); put_unaligned_le32(0x12345679,response.payload+4);
 assert(!mt6797_wifi_hif_runtime_configure_rxinf(&irq,&before,&after));
 assert(before==0x12345679 && after==before && sent==1 && !set_count);
 for(unsigned bad=0;bad<6;bad++) {
  setup();
  switch(bad) {
   case 0: timeout_at=1; break;
   case 1: fail_send_at=2; break;
   case 2: fail_send_at=3; break;
   case 3: timeout_at=2; break;
   case 4: no_write_effect=true; break;
   case 5: stale_readback=true; break;
  }
  assert(mt6797_wifi_hif_runtime_configure_rxinf(&irq,&before,&after)<0);
  assert(failure<0 && !irq.lifecycle_lock.held && !irq.wait_event.armed);
  assert(set_count==(bad!=0));
 }
 setup(); response.payload[0]++;
 assert(mt6797_wifi_hif_runtime_configure_rxinf(&irq,&before,&after)==-EPROTO && !set_count);
 assert(mt6797_wifi_hif_runtime_configure_rxinf(NULL,&before,&after)==-EINVAL);
 assert(mt6797_wifi_hif_runtime_configure_rxinf(&irq,&before,&before)==-EINVAL);
 puts("PASS: production RXINF read/modify/readback: group1 only, other bits preserved, distinct sequences, existing enable, malformed/stale/timeout/error fencing, no consumer after ambiguous write");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-rxinf-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
