#!/usr/bin/env python3
"""Exercise the production SAE external-authentication HIF bridge.

Runs the real ais_hif.c begin/send/take/wait/end functions, the SAE frame
codecs, management PID ownership and the real association that follows, with
a synchronous fake HIF under ASan/UBSan. The SAE cryptography itself belongs to
wpa_supplicant and is not modelled; frames carry opaque bodies.
Accepts the same optional vendor-root argument as test-wifi-join-host.py.
"""
from pathlib import Path
import re
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
static unsigned completions;
static void complete(struct completion *c) { c->done=true; completions++; }
static ktime_t ktime_add_ms(ktime_t t,unsigned ms) { return t+ms; }
static bool ktime_before(ktime_t a,ktime_t b) { return a<b; }
static void memzero_explicit(void *p,size_t n) { memset(p,0,n); }
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
 bool attached, terminal, refuse_attach;
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
program += source('ais_resources.c') + source('ais_runtime.c') + source('ais_auth.c')
program += r'''
#define MT6797_WIFI_AIS_AUTH_HEADER_LENGTH 24U
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
 assert(irq->lifecycle_lock);
 if(irq->refuse_attach) return -EPIPE;
 if(irq->attached) return -EBUSY;
 irq->attached=true; irq->sink=*sink; return 0;
}
static int mt6797_wifi_hif_runtime_sink_detach(struct mt6797_wifi_hif_irq *irq,
                                            void *context) {
 assert(irq->lifecycle_lock && irq->attached && irq->sink.context==context);
 irq->attached=false; memset(&irq->sink,0,sizeof(irq->sink)); return 0;
}
'''
hif = (base['driver'] / 'ais_hif.c').read_text()

def function(name):
    match = re.search(r'^(?:static )?\w[\w\s\*]*\b' + re.escape(name) + r'\(', hif, re.M)
    assert match, name
    start = hif.rindex('\n\n', 0, match.start()) + 2
    offset = hif.index('{', match.end())
    depth = 1
    offset += 1
    while depth:
        depth += (hif[offset] == '{') - (hif[offset] == '}')
        offset += 1
    return hif[start:offset] + '\n'

program += function('mt6797_wifi_ais_hif_auth_claim_terminal')
program += function('mt6797_wifi_ais_hif_auth_abort')
program += hif[hif.index('static bool mt6797_ais_join_finished('):hif.index('int mt6797_wifi_ais_hif_release_channel(')]
program += hif[hif.index('static int mt6797_wifi_ais_hif_ext_auth_event('):]
program += r'''
enum mode { NORMAL, NO_ACK, NO_TX_DONE, SEND_FAIL, MALFORMED_EVENT, ABORT_DURING_TX };
struct fixture {
 struct mt6797_wifi_ais_hif ais;
 struct mt6797_wifi_hif_irq irq;
 enum mode mode;
 ktime_t now;
 unsigned sends, terminals, waits, auth_sends;
 u8 last_frame[1100]; size_t last_length; u16 last_sequence; u16 last_rate;
};
static void tx_done(struct fixture *f,u8 pid,u8 status) {
 u8 payload[16]={0};
 struct mt6797_wifi_event_view event={.payload=payload,.payload_length=16,.packet_length=24,.id=0x0f};
 payload[0]=pid; payload[1]=status; payload[4]=sta.pairwise.index; payload[8]=1;
 f->irq.sink.event(f->irq.sink.context,&event);
}
static int peer_frame(struct fixture *f,const u8 *data,size_t n,unsigned wlan,bool translated,unsigned kind) {
 struct mt6797_wifi_rx_view view={.payload=data,.payload_length=n,.kind=kind,
  .header_length=24,.header_translated=translated,.wlan_index=wlan};
 return f->irq.sink.frame(f->irq.sink.context,&view);
}
static size_t sae_frame(u8 *b,const u8 *da,const u8 *sa,u16 transaction,u16 status,size_t body) {
 memset(b,0,30+body); put_unaligned_le16(IEEE80211_STYPE_AUTH,b);
 memcpy(b+4,da,6); memcpy(b+10,sa,6); memcpy(b+16,da==bss.own_addr?sa:da,6);
 put_unaligned_le16(3,b+24); put_unaligned_le16(transaction,b+26); put_unaligned_le16(status,b+28);
 for(size_t i=0;i<body;i++) b[30+i]=(u8)(i*7+transaction);
 return 30+body;
}
static void association_reply(struct fixture *f) {
 u8 data[33]; response(data,0,0xc123);
 data[30]=1; data[31]=1; data[32]=0x82;
 struct mt6797_wifi_rx_view frame={.payload=data,.payload_length=sizeof(data),
  .kind=MT6797_WIFI_RX_IEEE80211,.header_length=24,.wlan_index=f->ais.join.lease.wlan_index};
 assert(!f->irq.sink.frame(f->irq.sink.context,&frame));
}
static int send_packet(void *context,u32 address,const u8 *packet,size_t length) {
 struct fixture *f=context;
 assert(address==MT6797_WIFI_MCR_WTDR1 && f->irq.attached && f->irq.lifecycle_lock);
 f->sends++;
 if(f->ais.ext_auth.active) {
  /* Management TX descriptor: auth subtype, owned PID, SN valid, fixed rate. */
  size_t frame=get_unaligned_le16(packet)-28;
  assert(packet[8]==(IEEE80211_STYPE_AUTH>>4) && packet[20]==f->ais.auth.pid_lease.pid);
  assert(packet[4]==sta.pairwise.index && !(packet[6]&0x80));
  f->auth_sends++; memcpy(f->last_frame,packet+28,frame); f->last_length=frame;
  f->last_sequence=get_unaligned_le16(packet+14)&0xfff; f->last_rate=get_unaligned_le16(packet+26)>>2;
  if(f->mode==SEND_FAIL) return -EIO;
  if(f->mode==ABORT_DURING_TX) { f->irq.sink.abort(f->irq.sink.context,-EIO); return 0; }
  if(f->mode==MALFORMED_EVENT) {
   u8 payload[8]={0}; struct mt6797_wifi_event_view e={.payload=payload,.payload_length=8,.packet_length=16,.id=0x0f};
   assert(f->irq.sink.event(f->irq.sink.context,&e)<0); return 0;
  }
  if(f->mode!=NO_TX_DONE) {
   tx_done(f,f->ais.auth.pid_lease.pid+1,0); /* foreign PID is ignored */
   tx_done(f,f->ais.auth.pid_lease.pid,f->mode==NO_ACK);
  }
  return 0;
 }
 /* Association request, then BSS info and STA state3 commands. */
 if(packet[3]!=0x80) { /* management TX descriptor, not a command header */
  assert(packet[8]==(IEEE80211_STYPE_ASSOC_REQ>>4) && length>40);
  assert(get_unaligned_le16(packet+14)&0xfff);
  f->last_sequence=get_unaligned_le16(packet+14)&0xfff;
  u8 payload[16]={0};
  struct mt6797_wifi_event_view event={.payload=payload,.payload_length=16,.packet_length=24,.id=0x0f};
  payload[0]=f->ais.join.lease.pid; payload[4]=f->ais.join.lease.wlan_index; payload[8]=1;
  assert(!f->irq.sink.event(f->irq.sink.context,&event));
  association_reply(f);
 } else if(packet[4]==0x13) {
  u8 payload[8]; memcpy(payload,sta.peer_addr,6); payload[6]=sta.sta_index; payload[7]=sta.bss_index;
  struct mt6797_wifi_event_view event={.payload=payload,.payload_length=8,.packet_length=16,.id=0x0c,.sequence=packet[7]};
  assert(!f->irq.sink.event(f->irq.sink.context,&event));
 }
 return 0;
}
static ktime_t now(void *context) { struct fixture *f=context; assert(!f->ais.auth.lock); return f->now; }
static unsigned long wait_reply(void *context,struct completion *c,unsigned long timeout) {
 struct fixture *f=context; f->waits++; (void)c; f->now+=timeout; return 0;
}
static void terminal(void *context,int reason) {
 struct fixture *f=context; assert(reason<0); f->terminals++; f->irq.terminal=true;
}
static struct fixture *setup(enum mode mode) {
 struct fixture *f=calloc(1,sizeof(*f));
 f->mode=mode; f->ais.irq=&f->irq; f->ais.initialized=true; f->ais.channel_deadline=3000;
 f->ais.runtime.state=MT6797_WIFI_AIS_RUNTIME_STA_STATE1;
 f->ais.runtime.bss=bss; f->ais.runtime.sta=sta;
 f->ais.runtime.channel_pending.band=1; f->ais.runtime.channel_pending.primary=6;
 f->ais.runtime.next_command_sequence=11;
 spin_lock_init(&f->ais.auth.lock); init_completion(&f->ais.auth.completion);
 mt6797_wifi_ais_mgmt_pid_init(&f->ais.auth.pid_owner);
 f->ais.backend=(struct mt6797_wifi_ais_hif_backend){send_packet,now,wait_reply,terminal,f};
 return f;
}
static int send(struct fixture *f,const u8 *frame,size_t n,u16 sequence,struct mt6797_wifi_ais_mgmt_tx_status *s) {
 struct mt6797_wifi_ais_hif_ext_auth_request r={.frame=frame,.length=n,.timeout_ms=500,.sequence_number=sequence,.fixed_rate=3};
 return mt6797_wifi_ais_hif_ext_auth_send(&f->ais,&r,s);
}
static void closed(struct fixture *f) {
 assert(!f->irq.attached && !f->irq.lifecycle_lock && !f->ais.auth.lock);
 assert(!f->ais.auth.pid_owner.held && f->ais.auth.pid_owner.state==MT6797_WIFI_AIS_MGMT_PID_CLOSED);
 assert(!f->ais.ext_auth.active && !f->ais.auth.armed && !f->ais.ext_auth.rx_count);
 for(size_t i=0;i<sizeof(f->ais.ext_auth.rx);i++) assert(!((u8 *)f->ais.ext_auth.rx)[i]);
}
static void full_exchange(void) {
 struct fixture *f=setup(NORMAL);
 struct mt6797_wifi_ais_mgmt_tx_status s;
 u8 out[1100], commit[300], confirm[80], peer[1100]; size_t n, length;
 assert(!mt6797_wifi_ais_hif_ext_auth_begin(&f->ais));
 assert(f->irq.attached && f->ais.ext_auth.active && f->ais.auth.pid_owner.state==MT6797_WIFI_AIS_MGMT_PID_OPEN);
 assert(mt6797_wifi_ais_hif_ext_auth_begin(&f->ais)==-EALREADY);
 /* Commit (group 19: 2+32+64 byte body) with anti-clogging retry. */
 n=sae_frame(commit,sta.peer_addr,bss.own_addr,1,0,98);
 assert(!send(f,commit,n,1,&s) && !s.status && f->auth_sends==1);
 assert(f->last_length==n && !memcmp(f->last_frame,commit,n) && f->last_sequence==1 && f->last_rate==3);
 assert(!f->ais.auth.pid_owner.held && !f->ais.ext_auth.tx_outstanding);
 assert(mt6797_wifi_ais_hif_ext_auth_take(&f->ais,out,sizeof(out),&length)==-EAGAIN);
 /* Peer commit: filtered, queued as an owned copy, delivered once. */
 size_t pn=sae_frame(peer,bss.own_addr,sta.peer_addr,1,76,40);
 unsigned before=completions;
 assert(!peer_frame(f,peer,pn,sta.pairwise.index,false,MT6797_WIFI_RX_IEEE80211));
 assert(completions==before+1 && f->ais.ext_auth.rx_count==1);
 memset(peer,0xa5,sizeof(peer));
 assert(!mt6797_wifi_ais_hif_ext_auth_take(&f->ais,out,sizeof(out),&length) && length==pn);
 assert(get_unaligned_le16(out+28)==76 && out[30]==(u8)(0*7+1));
 assert(f->ais.auth.response_received);
 n=sae_frame(commit,sta.peer_addr,bss.own_addr,1,0,98+34);
 assert(!send(f,commit,n,2,&s) && f->last_sequence==2);
 pn=sae_frame(peer,bss.own_addr,sta.peer_addr,1,0,98);
 assert(!peer_frame(f,peer,pn,sta.pairwise.index,false,MT6797_WIFI_RX_IEEE80211));
 n=sae_frame(confirm,sta.peer_addr,bss.own_addr,2,0,34);
 assert(!send(f,confirm,n,3,&s));
 pn=sae_frame(peer,bss.own_addr,sta.peer_addr,2,0,34);
 assert(!peer_frame(f,peer,pn,sta.pairwise.index,false,MT6797_WIFI_RX_IEEE80211));
 unsigned taken=0;
 while(!mt6797_wifi_ais_hif_ext_auth_take(&f->ais,out,sizeof(out),&length)) taken++;
 assert(taken==2 && get_unaligned_le16(out+26)==2);
 assert(f->ais.ext_auth.tx_frames==3 && f->ais.ext_auth.tx_acked==3 && f->ais.ext_auth.rx_frames==3);
 assert(!mt6797_wifi_ais_hif_ext_auth_end(&f->ais,0));
 closed(f);
 assert(f->ais.auth.tx_done && !f->ais.auth.tx.status && f->ais.auth.response_received && !f->ais.auth.response.status_code);
 assert(mt6797_wifi_ais_hif_ext_auth_end(&f->ais,0)==-ENOTCONN);
 assert(mt6797_wifi_ais_hif_ext_auth_send(&f->ais,&(struct mt6797_wifi_ais_hif_ext_auth_request){.frame=confirm,.length=n,.timeout_ms=5},&s)==-ENOTCONN);
 /* The unchanged association path accepts the SAE-authenticated station. */
 struct mt6797_wifi_ais_hif_join_request request={.association=params,.negotiated=join,.timeout_ms=900,.sequence_number=4};
 struct mt6797_wifi_ais_join_params negotiated={0};
 int assoc=mt6797_wifi_ais_hif_associate(&f->ais,&request,&negotiated);
 if(assoc) fprintf(stderr,"associate=%d sends=%u terminals=%u\n",assoc,f->sends,f->terminals);
 assert(!assoc);
 assert(f->ais.runtime.state==MT6797_WIFI_AIS_RUNTIME_STA_STATE3 && f->last_sequence==4);
 assert(!f->terminals);
 free(f);
}
static void filters(void) {
 struct fixture *f=setup(NORMAL);
 u8 peer[1100], out[1100]; size_t pn, length;
 assert(!mt6797_wifi_ais_hif_ext_auth_begin(&f->ais));
 pn=sae_frame(peer,bss.own_addr,sta.peer_addr,1,0,32);
 /* Wrong WTBL, translated header, non-802.11 kind: ignored, never terminal. */
 assert(!peer_frame(f,peer,pn,sta.pairwise.index+1,false,MT6797_WIFI_RX_IEEE80211));
 assert(!peer_frame(f,peer,pn,sta.pairwise.index,true,MT6797_WIFI_RX_IEEE80211));
 assert(!peer_frame(f,peer,pn,sta.pairwise.index,false,MT6797_WIFI_RX_ETHERNET));
 u8 bad[1100];
 for(unsigned which=0;which<9;which++) {
  memcpy(bad,peer,pn); size_t bn=pn;
  switch(which) {
  case 0: bad[4]^=1; break;                          /* other DA */
  case 1: bad[10]^=1; break;                         /* other SA */
  case 2: bad[16]^=1; break;                         /* other BSSID */
  case 3: put_unaligned_le16(0,bad+24); break;       /* open system */
  case 4: put_unaligned_le16(3,bad+26); break;       /* transaction 3 */
  case 5: bad[1]|=0x40; break;                       /* protected */
  case 6: bad[1]|=0x02; break;                       /* FromDS */
  case 7: bn=29; break;                              /* truncated */
  case 8: bn=1025; break;                            /* oversized */
  }
  assert(!peer_frame(f,bad,bn,sta.pairwise.index,false,MT6797_WIFI_RX_IEEE80211));
 }
 assert(!f->ais.ext_auth.rx_count && !f->ais.ext_auth.rx_frames && !f->ais.auth.terminal_reason);
 /* A retried (retry bit) peer frame is still SAE traffic. */
 peer[1]|=0x08; assert(!peer_frame(f,peer,pn,sta.pairwise.index,false,MT6797_WIFI_RX_IEEE80211)); peer[1]&=~0x08;
 /* Ring overflow drops without desynchronizing; take respects capacity. */
 for(unsigned i=0;i<6;i++) assert(!peer_frame(f,peer,pn,sta.pairwise.index,false,MT6797_WIFI_RX_IEEE80211));
 assert(f->ais.ext_auth.rx_count==4 && f->ais.ext_auth.rx_dropped==3 && f->ais.ext_auth.rx_frames==7);
 assert(mt6797_wifi_ais_hif_ext_auth_take(&f->ais,out,pn-1,&length)==-EMSGSIZE && !length);
 assert(f->ais.ext_auth.rx_count==3);
 /* Local frames must be SAE from us to the AIS peer. */
 struct mt6797_wifi_ais_mgmt_tx_status s; u8 tx[1100]; size_t n=sae_frame(tx,sta.peer_addr,bss.own_addr,1,0,32);
 for(unsigned which=0;which<7;which++) {
  u8 t[1100]; memcpy(t,tx,n); size_t tn=n;
  switch(which) {
  case 0: t[4]^=1; break; case 1: t[10]^=1; break; case 2: t[16]^=1; break;
  case 3: put_unaligned_le16(0,t+24); break; case 4: t[1]=0x40; break;
  case 5: tn=29; break; case 6: tn=1025; break;
  }
  assert(send(f,t,tn,1,&s)==-EINVAL);
 }
 assert(!f->sends && !f->ais.auth.pid_owner.held);
 /* Supplicant failure status still tears down cleanly and blocks association. */
 assert(!send(f,tx,n,1,&s));
 assert(mt6797_wifi_ais_hif_ext_auth_end(&f->ais,15)==-EACCES);
 closed(f);
 assert(!f->ais.auth.tx_done && f->ais.auth.response.status_code==15 && !f->terminals);
 struct mt6797_wifi_ais_hif_join_request request={.association=params,.negotiated=join,.timeout_ms=900,.sequence_number=4};
 struct mt6797_wifi_ais_join_params negotiated;
 assert(mt6797_wifi_ais_hif_associate(&f->ais,&request,&negotiated)==-ENOTCONN);
 free(f);
 /* Success status without an acknowledged local frame or peer frame is refused. */
 for(unsigned variant=0;variant<2;variant++) {
  f=setup(variant?NO_ACK:NORMAL);
  assert(!mt6797_wifi_ais_hif_ext_auth_begin(&f->ais));
  if(variant) assert(send(f,tx,n,1,&s)==-EREMOTEIO && s.status==1 && !f->ais.auth.pid_owner.held);
  else assert(!send(f,tx,n,1,&s));
  assert(mt6797_wifi_ais_hif_ext_auth_end(&f->ais,0)==-EPROTO && !f->ais.auth.tx_done);
  closed(f); assert(!f->terminals); free(f);
 }
}
static void failures(void) {
 struct mt6797_wifi_ais_mgmt_tx_status s; u8 tx[100];
 size_t n=sae_frame(tx,sta.peer_addr,bss.own_addr,1,0,32);
 enum mode modes[]={NO_TX_DONE,SEND_FAIL,MALFORMED_EVENT,ABORT_DURING_TX};
 int expected[]={-ETIMEDOUT,-EIO,-EPROTO,-EIO};
 for(unsigned i=0;i<4;i++) {
  struct fixture *f=setup(modes[i]);
  assert(!mt6797_wifi_ais_hif_ext_auth_begin(&f->ais));
  int ret=send(f,tx,n,1,&s);
  if(ret!=expected[i]) fprintf(stderr,"mode=%d ret=%d\n",modes[i],ret);
  assert(ret==expected[i] && f->terminals==1 && f->irq.terminal);
  assert(!f->irq.lifecycle_lock && f->ais.auth.terminal_reason);
  assert(send(f,tx,n,2,&s)==-EIO && f->auth_sends==1); /* fenced: no more writes */
  int end=mt6797_wifi_ais_hif_ext_auth_end(&f->ais,0);
  assert(end<0 && !f->irq.attached && !f->ais.auth.tx_done && !f->ais.ext_auth.active);
  free(f);
 }
 /* Admission: wrong state, dead HIF, attach refusal. */
 struct fixture *f=setup(NORMAL); f->ais.runtime.state=MT6797_WIFI_AIS_RUNTIME_STA_STATE3;
 assert(mt6797_wifi_ais_hif_ext_auth_begin(&f->ais)==-ENOTCONN && !f->irq.attached); free(f);
 f=setup(NORMAL); f->irq.terminal=true;
 assert(mt6797_wifi_ais_hif_ext_auth_begin(&f->ais)==-EIO && !f->ais.auth.attempted); free(f);
 f=setup(NORMAL); f->irq.refuse_attach=true;
 assert(mt6797_wifi_ais_hif_ext_auth_begin(&f->ais)==-EPIPE && !f->ais.ext_auth.active);
 assert(f->ais.auth.pid_owner.state==MT6797_WIFI_AIS_MGMT_PID_CLOSED); free(f);
 f=setup(NORMAL); assert(!mt6797_wifi_ais_hif_ext_auth_begin(&f->ais));
 f->ais.runtime.state=MT6797_WIFI_AIS_RUNTIME_STA_STATE3;
 assert(send(f,tx,n,1,&s)==-ENOTCONN && !f->sends); f->ais.runtime.state=MT6797_WIFI_AIS_RUNTIME_STA_STATE1;
 /* Deadline wait sleeps on the auth completion and never past the deadline. */
 f->now=100; mt6797_wifi_ais_hif_ext_auth_wait(&f->ais,150); assert(f->waits==1 && f->now==150);
 mt6797_wifi_ais_hif_ext_auth_wait(&f->ais,150); assert(f->waits==1);
 unsigned before=completions; mt6797_wifi_ais_hif_ext_auth_kick(&f->ais); assert(completions==before+1);
 assert(mt6797_wifi_ais_hif_ext_auth_end(&f->ais,0)==-EPROTO); closed(f); free(f);
}
int main(void) {
 codecs_main();
 full_exchange(); filters(); failures();
 puts("PASS: production SAE external-auth bridge: commit/anti-clogging/confirm TX with owned PIDs and sequence numbers, filtered and bounded peer RX queue, status gating of the real association, supplicant failure, unacknowledged/missing TX-done/send failure/malformed event/abort fencing and admission");
}
'''
base['common']['run_program'](program)
