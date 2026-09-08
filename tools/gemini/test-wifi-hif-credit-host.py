#!/usr/bin/env python3
"""Production HIF send/release integration with register and transport fakes.
No IRQ concurrency or actual firmware capacity is verified.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile
base = runpy.run_path(str(Path(__file__).with_name('test-wifi-tx-credit-host.py')))
driver = base['driver']
program = base['program'].split('int main(void) {')[0]
program += r'''
typedef uint8_t u8; typedef uint32_t u32;
#define MT6797_WIFI_HIF_TX_QUEUES 16U
#define MT6797_WIFI_COMMAND_HEADER_LENGTH 8U
#define MT6797_WIFI_TX_DESCRIPTOR_LENGTH 28U
#define MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH 2352U
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=1; (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(*(l)); *(l)=0; } while(0)
#define lockdep_assert_held(l) assert(*(l))
struct completion { bool done; };
static void reinit_completion(struct completion *c) { c->done=false; }
static void complete(struct completion *c) { c->done=true; }
struct mt6797_wifi_hif_irq {
 int lifecycle_lock,tx_credit_lock,mode;
 void *hif;
 struct mt6797_wifi_tx_credit tx_credit;
 struct completion tx_credit_completion;
};
static struct mt6797_wifi_hif_irq irq;
static unsigned writes,failures,reads;
static int write_error,read_error;
static u16 registers[16];
static u16 get_unaligned_le16(const void *p) { const u8 *b=p; return b[0]|(u16)b[1]<<8; }
static int mt6797_wifi_hif_runtime_status(struct mt6797_wifi_hif_irq *i) { return i->mode; }
static void mt6797_wifi_hif_runtime_fail(struct mt6797_wifi_hif_irq *i,int error) { i->mode=error; failures++; }
static int mt6797_wifi_ahb_cmd53_write(void *hif,u32 address,const u8 *packet,size_t length) {
 (void)hif; (void)address; (void)packet; (void)length;
 assert(irq.lifecycle_lock && !irq.tx_credit_lock); writes++; return write_error;
}
static bool mt6797_wifi_hif_runtime_pump_expired(struct mt6797_wifi_hif_irq *i) { (void)i; return false; }
'''
regs = (driver / 'hif_regs.h').read_text()
for name in ['MT6797_WIFI_MCR_WTDR1', 'MT6797_WIFI_MCR_WTQCR0']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', regs, re.M)[0] + '\n'
program += r'''
static int mt6797_wifi_hif_reg_read(struct mt6797_wifi_hif_irq *i,u32 address,u32 *value) {
 (void)i; unsigned index=(address-MT6797_WIFI_MCR_WTQCR0)/4; assert(index<8);
 reads++; if(read_error && reads==(unsigned)read_error) return -EIO;
 *value=registers[2*index]|(u32)registers[2*index+1]<<16; return 0;
}
'''
src = (driver / 'hif_runtime.c').read_text()
program += src[src.index('int mt6797_wifi_hif_runtime_send('):src.index('static int mt6797_wifi_hif_runtime_pump_lengths(')]
program += r'''
static void setup(void) {
 memset(&irq,0,sizeof(irq)); mt6797_wifi_tx_credit_init(&irq.tx_credit); irq.lifecycle_lock=1;
 writes=failures=reads=0; write_error=read_error=0; memset(registers,0,sizeof(registers));
}
static int pump(void) { u16 released[16]; reads=0; return mt6797_wifi_hif_runtime_pump_tx(&irq,released); }
int main(void) {
 u8 command[16]={16,0,0,0x80}, data[44]={43,0,20,0x20};
 setup();
 for(unsigned n=0;n<13;n++) assert(!mt6797_wifi_hif_runtime_send(&irq,MT6797_WIFI_MCR_WTDR1,command,16));
 assert(writes==13 && !irq.tx_credit.free[15]);
 irq.tx_credit_completion.done=true;
 assert(mt6797_wifi_hif_runtime_send(&irq,MT6797_WIFI_MCR_WTDR1,command,16)==-EAGAIN);
 assert(writes==13 && !irq.tx_credit_completion.done && !failures);
 registers[14]=13; assert(!pump() && irq.tx_credit_completion.done && !irq.tx_credit.free[15]);
 memset(registers,0,sizeof(registers)); registers[15]=13;
 assert(!pump() && irq.tx_credit.free[15]==13);
 assert(!mt6797_wifi_hif_runtime_send(&irq,MT6797_WIFI_MCR_WTDR1,data,44));
 assert(irq.tx_credit.free[4]==12 && irq.tx_credit.free[15]==13);
 data[3]=0x88;
 assert(!mt6797_wifi_hif_runtime_send(&irq,MT6797_WIFI_MCR_WTDR1,data,44));
 assert(irq.tx_credit.free[15]==12);
 assert(mt6797_wifi_hif_runtime_send(&irq,0x30,data,44)==-EINVAL);
 setup(); command[0]=17;
 assert(mt6797_wifi_hif_runtime_send(&irq,MT6797_WIFI_MCR_WTDR1,command,16)==-EINVAL && !writes);
 command[0]=12;
 assert(mt6797_wifi_hif_runtime_send(&irq,MT6797_WIFI_MCR_WTDR1,command,16)==-EINVAL && !writes);
 command[0]=16; write_error=-EIO;
 assert(mt6797_wifi_hif_runtime_send(&irq,MT6797_WIFI_MCR_WTDR1,command,16)==-EIO);
 assert(writes==1 && failures==1 && irq.tx_credit.free[15]==12); /* No ambiguous refund. */
 assert(mt6797_wifi_hif_runtime_send(&irq,MT6797_WIFI_MCR_WTDR1,command,16)==-EIO && writes==1);
 for(unsigned fail=1;fail<=8;fail++) {
  setup(); assert(!mt6797_wifi_hif_runtime_send(&irq,MT6797_WIFI_MCR_WTDR1,command,16));
  registers[14]=registers[15]=1; read_error=fail;
  assert(pump()==-EIO && irq.tx_credit.free[15]==12 && !irq.tx_credit_completion.done);
 }
 puts("PASS: real HIF credit send/release: exhaustion before write, loss-free wake arming, split FFA/CPU refund, FIFO classification, packet bounds, ambiguous-write fencing and all partial register-read failures");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-hif-credit-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
