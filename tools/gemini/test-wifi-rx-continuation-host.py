#!/usr/bin/env python3
"""Actual bounded pump and runtime gates, with synthetic FIFOs/pthread IRQ races."""
from pathlib import Path
import os, re, subprocess, tempfile

root = Path(os.environ.get('GEMINI_KERNEL_SOURCE', Path(__file__).resolve().parents[2]))
driver = root / 'drivers/net/wireless/mediatek/mt6797'

def stripped(name):
    return re.sub(r'^#include .*\n', '', (driver / name).read_text(), flags=re.M)

def function(name):
    src = (driver / 'hif_runtime.c').read_text()
    match = re.search(r'^static [^;{}]*\b' + name + r'\([^;{}]*\)\n\{', src, re.M)
    assert match, name
    end = match.end(); depth = 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}'); end += 1
    return src[match.start():end] + '\n'

program = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t s64;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) (((~0U) << (l)) & (~0U >> (31-(h))))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define check_add_overflow(a,b,d) __builtin_add_overflow(a,b,d)
#define MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH 2352U
#define MT6797_WIFI_EVENT_HEADER_LENGTH 8U
struct mt6797_wifi_event_view { const u8 *payload; size_t payload_length, packet_length; u8 id, sequence; };
enum { MT6797_WIFI_RX_EVENT, MT6797_WIFI_RX_IEEE80211, MT6797_WIFI_RX_NATIVE_DATA, MT6797_WIFI_RX_ETHERNET };
struct mt6797_wifi_rx_view { int kind; const u8 *packet, *payload; size_t packet_length, payload_length; u8 event_id, sequence; };
static int parse_error;
static int mt6797_wifi_rx_parse(const u8 *p, size_t n, struct mt6797_wifi_rx_view *v)
{ memset(v,0,sizeof(*v)); v->kind=MT6797_WIFI_RX_EVENT; v->packet=p; v->packet_length=n; v->event_id=1; return parse_error; }
'''
program += stripped('hif_regs.h') + stripped('hif_pump.h') + stripped('hif_pump.c')
program += r'''
struct fifo { unsigned left[2], read[2], delivered, length_reads, per_pass, max_pass, tx_calls; u32 status; int fail, callback_result; bool odd_pair; };
static struct fifo fifo;
static bool runtime_fixture,wrapper_masked;
static pthread_mutex_t block_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t block_cond = PTHREAD_COND_INITIALIZER;
static bool block_packet, packet_entered, release_packet;
static int st(void *c,u32 *v) { (void)c;assert(!runtime_fixture || wrapper_masked); *v=fifo.status; fifo.status=0; fifo.per_pass=0; return fifo.fail==1?-EIO:0; }
static int tx(void *c,u16 *v) { (void)c; fifo.tx_calls++; memset(v,0,32);v[3]=2; return fifo.fail==2?-EIO:0; }
static int len(void *c,u16 *v) {
 (void)c; fifo.length_reads++;
 v[0]=fifo.left[0]?8:0; v[1]=fifo.left[1]?8:0;
 if(fifo.odd_pair && !fifo.read[0] && !fifo.read[1]) v[1]=0;
 if(fifo.fail==4) v[0]=MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH+1;
 return fifo.fail==3?-EIO:0;
}
static int pkt(void *c,unsigned port,u16 n,const u8 **p) {
 static u8 packet[8]; (void)c; assert(port<2 && n==8 && fifo.left[port]);
 pthread_mutex_lock(&block_lock);
 if(block_packet) { packet_entered=true;pthread_cond_broadcast(&block_cond);while(!release_packet)pthread_cond_wait(&block_cond,&block_lock); }
 pthread_mutex_unlock(&block_lock);
 if(fifo.fail==5) return -EIO;
 fifo.left[port]--; fifo.read[port]++; fifo.per_pass++;
 if(fifo.per_pass>fifo.max_pass) fifo.max_pass=fifo.per_pass;
 assert(fifo.per_pass<=32); *p=packet;return 0;
}
static int deliver(void *c,const struct mt6797_wifi_event_view *v) { (void)c;(void)v;fifo.delivered++;return fifo.callback_result; }
static struct mt6797_wifi_hif_pump_ops ops={.read_status=st,.read_tx_released=tx,.read_lengths=len,.read_packet=pkt,.deliver_event=deliver};
static void fill(unsigned a,unsigned b) { memset(&fifo,0,sizeof(fifo));fifo.left[0]=a;fifo.left[1]=b;fifo.status=MT6797_WIFI_WHISR_RX_DONE;parse_error=0; }
static void pump_cases(void) {
 struct mt6797_wifi_hif_pump_stats s={0}; u32 ack;int ret;unsigned passes;
 for(unsigned a=0;a<=100;a++) for(unsigned b=0;b<=100;b++) {
  fill(a,b);memset(&s,0,sizeof(s));passes=0;
  do { ret=mt6797_wifi_hif_pump_run(&ops,&s,&ack);passes++;assert(passes<9); } while(ret==MT6797_WIFI_HIF_PUMP_MORE);
  assert(ret==(a+b?0:-ENODATA)); assert(fifo.read[0]==a && fifo.read[1]==b && fifo.delivered==a+b);
  assert(!s.rx_pending && s.rx_packets==a+b && s.rx_budget_yields==passes-1 && fifo.max_pass<=32);
 }
 /* Odd first length pair exhausts budget between ports; port1 survives. */
 fill(50,50);fifo.odd_pair=true;memset(&s,0,sizeof(s));
 assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==MT6797_WIFI_HIF_PUMP_MORE);
 assert(fifo.read[0]==17 && fifo.read[1]==15 && s.rx_pending);
 /* A fresh TX read-clear bit coexists with pending RX whose bit is gone. */
 fifo.status=MT6797_WIFI_WHISR_TX_DONE;
 assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==MT6797_WIFI_HIF_PUMP_MORE);
 assert(s.tx_released[3]==2 && fifo.tx_calls==1);
 while(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==MT6797_WIFI_HIF_PUMP_MORE) {}
 assert(s.rx_packets==100 && !s.rx_pending && fifo.tx_calls==1);
 fill(4097,1031);memset(&s,0,sizeof(s));passes=0;
 do { ret=mt6797_wifi_hif_pump_run(&ops,&s,&ack);passes++;assert(passes<162); } while(ret==MT6797_WIFI_HIF_PUMP_MORE);
 assert(!ret && s.rx_packets==5128 && !s.rx_pending);
 /* No stale read-clear credit can fabricate another transaction. */
 assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==-ENODATA);
 ops.allow_empty_status=true;assert(!mt6797_wifi_hif_pump_run(&ops,&s,&ack));ops.allow_empty_status=false;
 /* Terminal transport/firmware/length/consumer/counter errors stay real. */
 for(unsigned error=1;error<=5;error++) {fill(1,0);fifo.status|=MT6797_WIFI_WHISR_TX_DONE;fifo.fail=error;memset(&s,0,sizeof(s));assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==(error==4?-EMSGSIZE:-EIO));}
 fill(1,0);memset(&s,0,sizeof(s));fifo.callback_result=1;assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==-EPROTO);
 fill(1,0);memset(&s,0,sizeof(s));fifo.callback_result=-ENOSPC;assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==-ENOSPC);
 fill(1,0);memset(&s,0,sizeof(s));parse_error=-EMSGSIZE;assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==-EMSGSIZE);
 fill(1,0);memset(&s,0,sizeof(s));fifo.status=MT6797_WIFI_WHISR_ABNORMAL;assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==-EIO);
 fill(1,0);memset(&s,0,sizeof(s));fifo.status=BIT(3);assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==-EPROTO);
 fill(33,0);memset(&s,0,sizeof(s));s.rx_budget_yields=UINT64_MAX;assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==-EOVERFLOW && s.rx_budget_yields==UINT64_MAX);
 fill(1,0);memset(&s,0,sizeof(s));s.rx_packets=UINT64_MAX;assert(mt6797_wifi_hif_pump_run(&ops,&s,&ack)==-EOVERFLOW);
 fill(0,0);memset(&s,0,sizeof(s));s.rx_pending=true;assert(!mt6797_wifi_hif_pump_run(&ops,&s,&ack) && !s.rx_pending);
 puts("pump: 10,201 FIFO pairs, odd split, read-clear continuation/TX, 5128 packet backlog and real errors PASS");
}

/* Compile the actual runtime owners against pthread locks and a queued-work
 * shim. The packet callback can block a real concurrent stop/terminal owner.
 */
struct mutex { pthread_mutex_t lock; };
typedef pthread_mutex_t spinlock_t;
static _Thread_local unsigned terminal_depth;
#define mutex_lock(m) assert(!pthread_mutex_lock(&(m)->lock))
#define mutex_unlock(m) assert(!pthread_mutex_unlock(&(m)->lock))
#define spin_lock_irqsave(m,f) do { (f)=0;assert(!pthread_mutex_lock(m));terminal_depth++; } while(0)
#define spin_unlock_irqrestore(m,f) do { (void)(f);terminal_depth--;assert(!pthread_mutex_unlock(m)); } while(0)
#define READ_ONCE(x) __atomic_load_n(&(x),__ATOMIC_ACQUIRE)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x),(v),__ATOMIC_RELEASE)
#define smp_store_release(p,v) do {assert(terminal_depth);__atomic_store_n((p),(v),__ATOMIC_RELEASE);} while(0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct work_struct { bool queued; unsigned schedules,cancels; pthread_t thread; bool running; };
enum mt6797_wifi_hif_irq_mode { MT6797_WIFI_HIF_IRQ_QUIESCENT, MT6797_WIFI_HIF_IRQ_RUNTIME, MT6797_WIFI_HIF_IRQ_QUIESCING, MT6797_WIFI_HIF_IRQ_TERMINAL };
struct mt6797_wifi_hif_irq { struct mutex rx_lock,lifecycle_lock;spinlock_t terminal_lock;int mode,terminal_reason;struct work_struct rx_work;struct mt6797_wifi_hif_pump_stats pump_stats;u32 acknowledged_status;u8 *rx_buffer[2];void *hif,*dev;int irq;unsigned unmasks,faults,disabled; };
static struct mt6797_wifi_hif_irq irq;
static bool schedule_work(struct work_struct *w) { assert(terminal_depth);w->schedules++;bool changed=!w->queued;w->queued=true;return changed; }
static void disable_irq(int n) { (void)n;irq.disabled++;assert(READ_ONCE(irq.mode)!=MT6797_WIFI_HIF_IRQ_RUNTIME); }
static void cancel_work_sync(struct work_struct *w) { assert(irq.disabled);if(w->running){assert(!pthread_join(w->thread,NULL));w->running=false;}w->queued=false;w->cancels++; }
static void mt6797_wifi_ahb_irq_mask(void *hif) { (void)hif;wrapper_masked=true; }
static void mt6797_wifi_ahb_irq_unmask(void *hif) { (void)hif;assert(terminal_depth && irq.mode==MT6797_WIFI_HIF_IRQ_RUNTIME && !irq.pump_stats.rx_pending);irq.unmasks++;wrapper_masked=false; }
static int mt6797_wifi_hif_irq_mode(struct mt6797_wifi_hif_irq *i) { return READ_ONCE(i->mode); }
static void mt6797_wifi_hif_runtime_terminal(struct mt6797_wifi_hif_irq *i,int error,const void *caller) { (void)caller;unsigned long f;spin_lock_irqsave(&i->terminal_lock,f);i->faults++;i->terminal_reason=error;smp_store_release(&i->mode,MT6797_WIFI_HIF_IRQ_TERMINAL);spin_unlock_irqrestore(&i->terminal_lock,f); }
typedef s64 ktime_t;
#define ktime_get() 0
#define ktime_sub(a,b) ((a)-(b))
#define ktime_to_ns(a) (a)
#define NSEC_PER_USEC 1000
#define NSEC_PER_MSEC 1000000
#define MT6797_WIFI_HIF_PUMP_SLOW_NS 100000000
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define mt6797_wifi_hif_runtime_pump_status st
#define mt6797_wifi_hif_runtime_pump_tx tx
#define mt6797_wifi_hif_runtime_pump_lengths len
#define mt6797_wifi_hif_runtime_pump_packet pkt
#define mt6797_wifi_hif_runtime_pump_event deliver
#define mt6797_wifi_hif_runtime_pump_frame NULL
#define mt6797_wifi_hif_runtime_reject_packet NULL
'''
for name in ['mt6797_wifi_hif_runtime_publish','mt6797_wifi_hif_runtime_rearm','mt6797_wifi_hif_runtime_unmask','mt6797_wifi_hif_runtime_disable_linux','mt6797_wifi_hif_runtime_clear_status','mt6797_wifi_hif_power_off_drain','mt6797_wifi_hif_runtime_service','mt6797_wifi_hif_rx_work']:
    program += function(name)
program += r'''
static void setup_irq(void) {
 static u8 buffers[2][8];memset(&irq,0,sizeof(irq));irq.rx_buffer[0]=buffers[0];irq.rx_buffer[1]=buffers[1];
 assert(!pthread_mutex_init(&irq.rx_lock.lock,NULL));assert(!pthread_mutex_init(&irq.lifecycle_lock.lock,NULL));assert(!pthread_mutex_init(&irq.terminal_lock,NULL));
 block_packet=packet_entered=release_packet=false;runtime_fixture=true;wrapper_masked=true;
}
static void destroy_irq(void) {assert(!pthread_mutex_destroy(&irq.rx_lock.lock));assert(!pthread_mutex_destroy(&irq.lifecycle_lock.lock));assert(!pthread_mutex_destroy(&irq.terminal_lock));}
static void run_work(void) { assert(irq.rx_work.queued);irq.rx_work.queued=false;mt6797_wifi_hif_rx_work(&irq.rx_work); }
static void *thread_work(void *unused) { (void)unused;mt6797_wifi_hif_rx_work(&irq.rx_work);return NULL; }
static void wait_packet(void) {pthread_mutex_lock(&block_lock);while(!packet_entered)pthread_cond_wait(&block_cond,&block_lock);pthread_mutex_unlock(&block_lock);}
static void unblock_packet(void) {pthread_mutex_lock(&block_lock);release_packet=true;pthread_cond_broadcast(&block_cond);pthread_mutex_unlock(&block_lock);}
static void runtime_cases(void) {
 unsigned schedules,unmasks;bool pending;
 setup_irq();fill(601,31);
 assert(!mt6797_wifi_hif_runtime_clear_status(&irq));assert(irq.pump_stats.rx_pending && fifo.delivered==32 && !irq.rx_work.queued);
 mt6797_wifi_hif_runtime_publish(&irq,MT6797_WIFI_HIF_IRQ_RUNTIME);
 mt6797_wifi_hif_runtime_unmask(&irq);assert(irq.rx_work.queued && !irq.unmasks);
 /* Command thread owns lifecycle_lock while worker supplies its completion. */
 mutex_lock(&irq.lifecycle_lock);
 while(irq.rx_work.queued)run_work();
 mutex_unlock(&irq.lifecycle_lock);
 assert(fifo.delivered==632 && !irq.pump_stats.rx_pending && irq.unmasks==1 && !irq.faults);
 /* An already queued IRQ whose WHISR was consumed by worker is harmless. */
 mt6797_wifi_hif_runtime_service(&irq,runtime_cases,true);
 assert(!irq.faults && irq.pump_stats.interrupts==1);
 /* IRQ drains pending FIFO before its queued worker gets scheduled. */
 fill(33,0);mt6797_wifi_hif_runtime_service(&irq,runtime_cases,true);assert(irq.rx_work.queued);
 mt6797_wifi_hif_runtime_service(&irq,runtime_cases,true);assert(!irq.pump_stats.rx_pending);run_work();assert(!irq.faults);
 /* Normal stop closes the rearm gate before waiting for FIFO ownership. */
 fill(96,0);mt6797_wifi_hif_runtime_service(&irq,runtime_cases,true);schedules=irq.rx_work.schedules;unmasks=irq.unmasks;
 mt6797_wifi_hif_runtime_publish(&irq,MT6797_WIFI_HIF_IRQ_QUIESCING);run_work();
 assert(irq.rx_work.schedules==schedules && irq.unmasks==unmasks && fifo.delivered==32);
 mt6797_wifi_hif_runtime_disable_linux(&irq);assert(!irq.rx_work.queued && irq.rx_work.cancels==1);
 /* Power-off owns pending read-clear FIFO after both asynchronous owners end. */
 assert(!mt6797_wifi_hif_power_off_drain(&irq,&pending));assert(pending && fifo.read[0]==64 && irq.pump_stats.rx_pending);
 assert(!mt6797_wifi_hif_power_off_drain(&irq,&pending));assert(!pending && fifo.delivered==32 && fifo.read[0]==96 && !irq.pump_stats.rx_pending);
 destroy_irq();
 for(unsigned terminal=0;terminal<2;terminal++) {
  setup_irq();fill(64,0);mt6797_wifi_hif_runtime_publish(&irq,MT6797_WIFI_HIF_IRQ_RUNTIME);
  irq.pump_stats.rx_pending=true;block_packet=true;irq.rx_work.running=true;
  assert(!pthread_create(&irq.rx_work.thread,NULL,thread_work,NULL));wait_packet();
  assert(pthread_mutex_trylock(&irq.rx_lock.lock)==EBUSY);
  if(terminal)mt6797_wifi_hif_runtime_terminal(&irq,-EIO,runtime_cases);
  else mt6797_wifi_hif_runtime_publish(&irq,MT6797_WIFI_HIF_IRQ_QUIESCING);
  unblock_packet();mt6797_wifi_hif_runtime_disable_linux(&irq);
  assert(fifo.delivered==32 && irq.pump_stats.rx_pending && !irq.rx_work.queued && !irq.rx_work.schedules && !irq.unmasks);
  mt6797_wifi_hif_runtime_unmask(&irq);assert(!irq.rx_work.schedules && !irq.unmasks);
  destroy_irq();
 }
 setup_irq();fill(1,0);fifo.fail=5;mt6797_wifi_hif_runtime_publish(&irq,MT6797_WIFI_HIF_IRQ_RUNTIME);mt6797_wifi_hif_runtime_service(&irq,runtime_cases,true);assert(irq.faults==1 && irq.mode==MT6797_WIFI_HIF_IRQ_TERMINAL && !irq.unmasks && !irq.rx_work.queued);destroy_irq();
 setup_irq();fill(0,0);mt6797_wifi_hif_runtime_publish(&irq,MT6797_WIFI_HIF_IRQ_RUNTIME);mt6797_wifi_hif_runtime_service(&irq,runtime_cases,true);assert(irq.faults==1 && irq.terminal_reason==-ENODATA);destroy_irq();
 setup_irq();fill(33,0);fifo.fail=5;assert(mt6797_wifi_hif_runtime_clear_status(&irq)==-EIO && !irq.rx_work.queued && !irq.unmasks && irq.mode==MT6797_WIFI_HIF_IRQ_QUIESCENT);destroy_irq();
 setup_irq();fill(1,0);irq.pump_stats.interrupts=UINT64_MAX;mt6797_wifi_hif_runtime_publish(&irq,MT6797_WIFI_HIF_IRQ_RUNTIME);mt6797_wifi_hif_runtime_service(&irq,runtime_cases,true);assert(irq.faults==1 && irq.terminal_reason==-EOVERFLOW && !fifo.delivered);destroy_irq();
 puts("runtime: startup continuation, coalesced IRQ, command waiter, stop/terminal races, work drain, poweroff and real faults PASS");
}
int main(void) {pump_cases();runtime_cases();return 0;}
'''
with tempfile.TemporaryDirectory(prefix='gemini-rx-continuation-') as td:
    td=Path(td);(td/'test.c').write_text(program)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(td/'test.c'),'-o',str(td/'test')],check=True)
    subprocess.run([str(td/'test')],check=True,timeout=20)
