#!/usr/bin/env python3
"""Exercise production association orchestration with a synchronous fake HIF.

Uses actual codecs/management ownership. IRQ sink, clock and locking are shims;
this tests ordering and failure policy, not kernel concurrency or hardware.
Accepts the same optional vendor-root argument as test-wifi-join-host.py.
"""
from pathlib import Path
import runpy

base = runpy.run_path(str(Path(__file__).with_name('test-wifi-join-host.py')))
program = base['program'].replace('int main(void) {', 'static int codecs_main(void) {')
source = base['source']
program += r'''
typedef int64_t ktime_t;
typedef int spinlock_t;
struct completion { bool done; };
#define spin_lock_init(l) (*(l)=0)
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=1; (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(*(l)); *(l)=0; } while(0)
static void mutex_lock(int *l) { assert(!*l); *l=1; }
static void mutex_unlock(int *l) { assert(*l); *l=0; }
static void init_completion(struct completion *c) { c->done=false; }
static void reinit_completion(struct completion *c) { c->done=false; }
static void complete(struct completion *c) { c->done=true; }
static ktime_t ktime_add_ms(ktime_t t,unsigned ms) { return t+ms; }
static bool ktime_before(ktime_t a,ktime_t b) { return a<b; }
#define GFP_KERNEL 0
#define kmalloc(n,f) malloc(n)
#define kfree(p) free(p)
#define MT6797_WIFI_MCR_WTDR1 0x34
struct mt6797_wifi_hif_runtime_sink {
 int (*event)(void *,const struct mt6797_wifi_event_view *);
 int (*frame)(void *,const struct mt6797_wifi_rx_view *);
 void (*abort)(void *,int);
 void *context;
};
struct mt6797_wifi_hif_irq {
 int lifecycle_lock;
 struct mt6797_wifi_hif_runtime_sink sink;
 bool attached, terminal, abort_on_detach;
};
'''
program += source('ais_auth.h') + source('ais_runtime.h') + source('ais_hif.h')
program += r'''
static unsigned find_first_zero_bit(const unsigned long *bits,unsigned count) {
 for(unsigned i=0;i<count;i++) if(!(*bits&(1UL<<i))) return i;
 return count;
}
static bool test_bit(unsigned i,const unsigned long *bits) { return !!(*bits&(1UL<<i)); }
static void __set_bit(unsigned i,unsigned long *bits) { *bits|=1UL<<i; }
static void __clear_bit(unsigned i,unsigned long *bits) { *bits&=~(1UL<<i); }
static bool bitmap_empty(const unsigned long *bits,unsigned count) {
 return !(*bits & ((1UL<<count)-1));
}
'''
program += source('ais_resources.c') + source('ais_runtime.c')
program += r'''
static int mt6797_wifi_ais_hif_status(const struct mt6797_wifi_ais_hif *ais) {
 return ais->irq->terminal ? -EIO : 0;
}
static unsigned long mt6797_wifi_ais_hif_remaining(struct mt6797_wifi_ais_hif *ais,
                                                 ktime_t deadline) {
 ktime_t now=ais->backend.now(ais->backend.context);
 return deadline>now ? deadline-now : 0;
}
static int mt6797_wifi_hif_runtime_sink_attach(struct mt6797_wifi_hif_irq *irq,
                           const struct mt6797_wifi_hif_runtime_sink *sink) {
 if(irq->attached) return -EBUSY;
 irq->attached=true; irq->sink=*sink; return 0;
}
static int mt6797_wifi_hif_runtime_sink_detach(struct mt6797_wifi_hif_irq *irq,
                                            void *context) {
 assert(irq->attached && irq->sink.context==context);
 if(irq->abort_on_detach) irq->sink.abort(context,-EIO);
 irq->attached=false; memset(&irq->sink,0,sizeof(irq->sink)); return 0;
}
'''
hif = (base['driver'] / 'ais_hif.c').read_text()
program += hif[hif.index('static bool mt6797_ais_join_finished('):hif.index('int mt6797_wifi_ais_hif_release_channel(')]
program += r'''
enum mode { NORMAL, RX_FIRST, ASYNC, AP_REJECT, TX_FAIL, NO_TX, NO_RX,
            NO_ACTIVATION, EXPIRE_AFTER_BSS, ABORT_AFTER_BSS, LATE_ABORT };
struct fixture {
 struct mt6797_wifi_ais_hif ais;
 struct mt6797_wifi_hif_irq irq;
 struct mt6797_wifi_ais_hif_join_request request;
 unsigned sends, terminals, fail_send, waits;
 enum mode mode;
 ktime_t now;
 u8 command_ids[3];
};
static void tx_reply(struct fixture *f) {
 u8 payload[16]={0};
 struct mt6797_wifi_event_view event={.payload=payload,.payload_length=16,
  .packet_length=24,.id=0x0f};
 payload[0]=f->ais.join.lease.pid;
 payload[1]=f->mode==TX_FAIL;
 put_unaligned_le16(8,payload+2);
 payload[4]=f->ais.join.lease.wlan_index; payload[5]=1; payload[8]=1;
 assert(!f->irq.sink.event(f->irq.sink.context,&event));
}
static void association_reply(struct fixture *f) {
 u8 data[33]; response(data,f->mode==AP_REJECT?17:0,0xc123);
 data[30]=1; data[31]=1; data[32]=0x82;
 struct mt6797_wifi_rx_view frame={.payload=data,.payload_length=sizeof(data),
  .kind=MT6797_WIFI_RX_IEEE80211,.header_length=24,
  .wlan_index=f->ais.join.lease.wlan_index};
 assert(!f->irq.sink.frame(f->irq.sink.context,&frame));
 /* RX storage is borrowed; overwriting it must not corrupt the owned copy. */
 memset(data,0xa5,sizeof(data));
}
static void finish_association(struct fixture *f) {
 if(f->mode==RX_FIRST) { association_reply(f); tx_reply(f); }
 else {
  if(f->mode!=NO_TX) tx_reply(f);
  if(f->mode!=NO_RX && f->mode!=TX_FAIL) association_reply(f);
 }
}
static int send_packet(void *context,u32 address,const u8 *packet,size_t length) {
 struct fixture *f=context;
 assert(address==MT6797_WIFI_MCR_WTDR1 && f->irq.attached);
 assert(f->sends<3);
 unsigned step=++f->sends;
 if(step==1) {
  assert(length>=58 && packet[28]==0 && packet[20]==2);
 } else {
  assert(packet[4]==(step==2?0x12:0x13));
  assert(length==(step==2?96:132));
  f->command_ids[step-1]=packet[4];
 }
 if(f->fail_send==step) return -EIO;
 if(step==1 && f->mode!=ASYNC) finish_association(f);
 if(step==2 && f->mode==EXPIRE_AFTER_BSS) f->now=1001;
 if(step==2 && f->mode==ABORT_AFTER_BSS) f->irq.sink.abort(f->irq.sink.context,-EIO);
 if(step==3 && f->mode!=NO_ACTIVATION) {
  u8 payload[8]; memcpy(payload,sta.peer_addr,6);
  payload[6]=sta.sta_index; payload[7]=sta.bss_index;
  struct mt6797_wifi_event_view event={.payload=payload,.payload_length=8,
   .packet_length=16,.id=0x0c,.sequence=packet[7]};
  assert(!f->irq.sink.event(f->irq.sink.context,&event));
 }
 return 0;
}
static ktime_t now(void *context) {
 struct fixture *f=context;
 assert(!f->ais.join.lock); /* Backend may synchronously deliver IRQ callbacks. */
 return f->now;
}
static unsigned long wait_reply(void *context,struct completion *c,unsigned long timeout) {
 struct fixture *f=context; f->waits++;
 if(f->mode==ASYNC && f->sends==1) { finish_association(f); return 1; }
 f->now+=timeout;
 (void)c; return 0;
}
static void terminal(void *context,int reason) {
 struct fixture *f=context; assert(reason<0); f->terminals++; f->irq.terminal=true;
}
static void setup(struct fixture *f,enum mode mode,unsigned fail_send) {
 memset(f,0,sizeof(*f)); f->mode=mode; f->fail_send=fail_send;
 f->ais.irq=&f->irq; f->ais.initialized=true; f->ais.channel_deadline=1000;
 f->ais.runtime.state=MT6797_WIFI_AIS_RUNTIME_STA_STATE1;
 f->ais.runtime.bss=bss; f->ais.runtime.sta=sta;
 f->ais.runtime.channel_pending.band=1; f->ais.runtime.channel_pending.primary=6;
 f->ais.runtime.next_command_sequence=11;
 f->ais.auth.tx_done=true; f->ais.auth.response_received=true;
 f->ais.backend=(struct mt6797_wifi_ais_hif_backend){send_packet,now,wait_reply,terminal,f};
 f->request.association=params;
 f->request.negotiated=join;
 f->request.timeout_ms=900; f->request.sequence_number=8;
 /* Simulate the completed auth lease with the real ownership functions. */
 struct mt6797_wifi_ais_mgmt_pid_lease lease;
 mt6797_wifi_ais_mgmt_pid_init(&f->ais.auth.pid_owner);
 assert(!mt6797_wifi_ais_mgmt_pid_open(&f->ais.auth.pid_owner,&bss,&sta));
 assert(!mt6797_wifi_ais_mgmt_pid_acquire(&f->ais.auth.pid_owner,&lease));
 assert(lease.pid==1);
 assert(!mt6797_wifi_ais_mgmt_pid_release(&f->ais.auth.pid_owner,&lease));
 assert(!mt6797_wifi_ais_mgmt_pid_close(&f->ais.auth.pid_owner));
 f->irq.abort_on_detach=mode==LATE_ABORT;
}
static void run(enum mode mode,unsigned fail,int expected,unsigned sends,bool fenced) {
 struct fixture *f=calloc(1,sizeof(*f));
 struct mt6797_wifi_ais_join_params out={0};
 setup(f,mode,fail);
 int ret=mt6797_wifi_ais_hif_associate(&f->ais,&f->request,&out);
 if(ret!=expected) fprintf(stderr,"mode=%d fail=%u result=%d expected=%d\n",mode,fail,ret,expected);
 assert(ret==expected && f->sends==sends && !!f->terminals==fenced);
 assert(!f->irq.attached && !f->irq.lifecycle_lock);
 if(!ret) {
  assert(f->ais.runtime.state==MT6797_WIFI_AIS_RUNTIME_STA_STATE3);
  assert(out.association_id==0x123 && out.basic_rates==1);
  assert(f->ais.join.response[30]==1 && f->ais.join.response[32]==0x82);
  assert(f->ais.runtime.next_command_sequence==13);
 }
 if(!fenced) {
  assert(!f->ais.auth.pid_owner.held);
  assert(f->ais.auth.pid_owner.state==MT6797_WIFI_AIS_MGMT_PID_CLOSED);
 }
 free(f);
}
struct cleanup_fixture {
 struct mt6797_wifi_ais_runtime runtime;
 struct mt6797_wifi_ais_runtime_ops ops;
 unsigned sends, fail_send, terminals;
 bool release_early;
};
static int cleanup_precheck(void *context) { (void)context; return 0; }
static void cleanup_fence(void *context,int error) {
 struct cleanup_fixture *f=context; assert(error<0); f->terminals++;
}
static int cleanup_send(void *context,const struct mt6797_wifi_ais_runtime *runtime,
 enum mt6797_wifi_ais_runtime_command command,const u8 *packet,size_t length) {
 struct cleanup_fixture *f=context;
 enum mt6797_wifi_ais_runtime_command order[]={
  MT6797_WIFI_AIS_RUNTIME_BSS_ACTIVATE,MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST,
  MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE,MT6797_WIFI_AIS_RUNTIME_STA_REMOVE,
  MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT,MT6797_WIFI_AIS_RUNTIME_BSS_DEACTIVATE};
 if(f->release_early) { order[3]=MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT; order[4]=MT6797_WIFI_AIS_RUNTIME_STA_REMOVE; }
 assert(runtime==&f->runtime && f->sends<6 && command==order[f->sends]);
 assert(packet && length); f->sends++;
 return f->sends==f->fail_send?-EIO:0;
}
static int cleanup_channel(void *context,unsigned timeout) {
 struct cleanup_fixture *f=context; u8 payload[16]={0};
 struct mt6797_wifi_ais_channel_pending *p=&f->runtime.channel_pending;
 struct mt6797_wifi_event_view e={.payload=payload,.payload_length=16,.packet_length=24,.id=0x10};
 payload[0]=p->bss_index; payload[1]=p->token;
 payload[3]=p->primary; payload[5]=p->band; put_unaligned_le32(1000,payload+12);
 enum mt6797_wifi_ais_event_disposition d;
 assert(timeout);
 int ret=mt6797_wifi_ais_runtime_event(&f->runtime,&f->ops,&e,&d);
 assert(!ret && d==MT6797_WIFI_AIS_EVENT_COMPLETE); return ret;
}
static void cleanup_test(void) {
 for(unsigned early=0;early<2;early++) {
 for(unsigned fail=0;fail<=6;fail++) {
  if(fail && fail<4) continue;
  struct cleanup_fixture f={.fail_send=fail,.release_early=early};
  f.ops=(struct mt6797_wifi_ais_runtime_ops){cleanup_precheck,cleanup_send,cleanup_channel,cleanup_fence,&f};
  struct mt6797_wifi_ais_runtime_params p={.channel={1,6},.bss_index=3,.own_mac_index=1,.response_timeout_ms=1000};
  memcpy(p.own_addr,bss.own_addr,6); memcpy(p.peer_addr,sta.peer_addr,6);
  mt6797_wifi_ais_runtime_init(&f.runtime,3,1);
  assert(!mt6797_wifi_ais_runtime_start(&f.runtime,&f.ops,&p));
  assert(f.runtime.state==MT6797_WIFI_AIS_RUNTIME_STA_STATE1);
  assert(mt6797_wifi_ais_runtime_release_channel(&f.runtime,&f.ops)==-EALREADY);
  assert(f.sends==3);
  f.runtime.state=MT6797_WIFI_AIS_RUNTIME_STA_STATE3;
  if(early) {
   int release=mt6797_wifi_ais_runtime_release_channel(&f.runtime,&f.ops);
   assert(release==(fail==4?-EIO:0));
   assert(f.sends==4);
   /* A duplicate success or a terminal retry must never emit a second abort. */
   assert(mt6797_wifi_ais_runtime_release_channel(&f.runtime,&f.ops)==release);
   assert(f.sends==4);
  }
  int ret=mt6797_wifi_ais_runtime_stop(&f.runtime,&f.ops);
  assert(ret==(fail?-EIO:0) && f.sends==(fail?fail:6));
  if(fail) {
   assert(f.terminals==1 && f.runtime.state==MT6797_WIFI_AIS_RUNTIME_TERMINAL);
  } else {
   assert(!f.terminals && f.runtime.state==MT6797_WIFI_AIS_RUNTIME_STOPPED);
   assert(f.runtime.resources.state==MT6797_WIFI_AIS_RESOURCES_CLOSED);
   assert(!f.runtime.resources.bss_used);
   assert(bitmap_empty(f.runtime.resources.sta_used,MT6797_WIFI_AIS_STA_COUNT));
   assert(bitmap_empty(f.runtime.resources.wtbl_used,MT6797_WIFI_AIS_WTBL_COUNT));
  }
 }
 }
 puts("PASS: real AIS resource ledger and runtime cleanup from state3, exact remove/channel-abort/deactivate order, released slots and no subsequent writes after each cleanup failure");
}
int main(void) {
 codecs_main();
 cleanup_test();
 run(NORMAL,0,0,3,false); run(RX_FIRST,0,0,3,false); run(ASYNC,0,0,3,false);
 run(AP_REJECT,0,-EACCES,1,false); run(TX_FAIL,0,-EREMOTEIO,1,false);
 run(NO_TX,0,-ETIMEDOUT,1,true); run(NO_RX,0,-ETIMEDOUT,1,true);
 run(NO_ACTIVATION,0,-ETIMEDOUT,3,true);
 for(unsigned step=1;step<=3;step++) run(NORMAL,step,-EIO,step,true);
 run(EXPIRE_AFTER_BSS,0,-ETIMEDOUT,2,true);
 run(ABORT_AFTER_BSS,0,-EIO,2,true); run(LATE_ABORT,0,-EIO,3,true);
 struct fixture *f=calloc(1,sizeof(*f)); struct mt6797_wifi_ais_join_params out;
 setup(f,NORMAL,0); f->now=1001;
 assert(mt6797_wifi_ais_hif_associate(&f->ais,&f->request,&out)==-ETIMEDOUT);
 assert(!f->sends && !f->terminals);
 setup(f,NORMAL,0); f->request.negotiated.channel.primary=11;
 assert(mt6797_wifi_ais_hif_associate(&f->ais,&f->request,&out)==-EINVAL);
 assert(!f->sends);
 setup(f,NORMAL,0); f->request.negotiated.ssid[0]='x';
 assert(mt6797_wifi_ais_hif_associate(&f->ais,&f->request,&out)==-EINVAL);
 assert(!f->sends);
 setup(f,NORMAL,0); f->request.negotiated.listen_interval++;
 assert(mt6797_wifi_ais_hif_associate(&f->ais,&f->request,&out)==-EINVAL);
 assert(!f->sends);
 free(f);
 puts("PASS: production HIF association flow, synchronous and delayed TX/RX, owned RX copy, PID continuity, BSS-before-activation, rejection, missing completions, each failed write, grant expiry, late abort and identity admission");
}
'''
base['common']['run_program'](program)
