#!/usr/bin/env python3
"""Production key callbacks and event matcher with synchronous transport fakes.

Exercises lifecycle/error policy, not real IRQ concurrency or firmware crypto.
The separate key codec test checks vendor command layout with synthetic keys.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
driver = root / 'drivers/net/wireless/mediatek/mt6797'
base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-worker-host.py')))
function = base['function']
program = base['program'].split('enum stage {')[0]
program = program.replace('struct mt6797_wifi_hif_irq { int lifecycle_lock; bool attached; };',
    'struct mt6797_wifi_hif_irq { int lifecycle_lock; void *hif; int mode; };')
program += r'''
#define WLAN_CIPHER_SUITE_CCMP 0x000fac04
#define NL80211_KEY_RX_TX 0
#define BIT(n) (1U<<(n))
#define NL80211_STA_FLAG_AUTHORIZED 1
#define CFG80211_STA_AP_STA 3
struct station_parameters { unsigned sta_flags_mask,sta_flags_set; };
static int cfg80211_check_station_change(struct wiphy *w,struct station_parameters *p,int type) { (void)w; assert(type==CFG80211_STA_AP_STA); return p->sta_flags_mask & ~BIT(NL80211_STA_FLAG_AUTHORIZED) ? -EINVAL : 0; }
#define MT6797_WIFI_HIF_IRQ_RUNTIME 2
#define MT6797_WIFI_MCR_WTDR1 0x34
struct key_params { const u8 *key,*seq; int key_len,seq_len; u16 vlan_id; u32 cipher; int mode; };
static struct mt6797_wifi_radio r;
static struct mt6797_wifi_hif_irq irq;
static struct mt6797_radio_connection c;
static struct cfg80211_bss bss={.bssid={2,6,7,8,9,10}};
static unsigned writes,terminals,queued,wipes;
static int action,terminal_reason;
static struct mt6797_wifi_radio *wiphy_priv(struct wiphy *w) { (void)w; return &r; }
static int mt6797_wifi_hif_irq_mode(struct mt6797_wifi_hif_irq *i) { return i->mode; }
static bool is_valid_ether_addr(const u8 *a) {
 const u8 zero[6]={0}; return !(a[0]&1) && memcmp(a,zero,6);
}
static void reinit_completion(struct completion *c) { c->done=false; }
static unsigned long msecs_to_jiffies(unsigned n) { return n; }
static void memzero_explicit(void *p,size_t n) { memset(p,0,n); if(n==72) wipes++; }
static void queue_work(int queue,struct work_struct *work) { (void)queue; (void)work; queued++; }
static int mt6797_radio_link_event(void *,const struct mt6797_wifi_event_view *);
static void mt6797_radio_link_abort(void *,int);
int mt6797_wifi_ais_sta_pending_init(const struct mt6797_wifi_ais_bss_handle *b,
 const struct mt6797_wifi_ais_sta_handle *s,u8 sequence,struct mt6797_wifi_ais_sta_pending *p) {
 (void)b; (void)s; (void)sequence; memset(p,0,sizeof(*p));
 p->generation=p->bss_cookie=p->sta_cookie=1; p->bss_index=0;
 memcpy(p->peer_addr,bss.bssid,6); return 0;
}
int mt6797_wifi_ais_ccmp_key_build(const struct mt6797_wifi_ais_bss_handle *b,
 const struct mt6797_wifi_ais_sta_handle *s,const struct mt6797_wifi_ais_key_params *p,
 u8 seq,u8 *command,size_t capacity) {
 (void)b; (void)s; assert(seq && capacity==72 && p->index<=3);
 if(!p->remove) { assert(p->material && p->material_length==16); memcpy(command+24,p->material,16); }
 return 0;
}
static int offer(int kind) {
 u8 bytes[8]={0}; memcpy(bytes+2,bss.bssid,6);
 struct mt6797_wifi_event_view e={.id=0x24,.payload=bytes,.payload_length=8,.packet_length=16};
 if(kind==2) bytes[2]^=4;
 if(kind==3) e.payload_length=7;
 return mt6797_radio_link_event(&r,&e);
}
static int mt6797_wifi_ahb_cmd53_write(void *hif,u32 address,const u8 *command,size_t length) {
 (void)hif; (void)command;
 assert(r.connection_lock.held && irq.lifecycle_lock && address==MT6797_WIFI_MCR_WTDR1 && length==72);
 writes++;
 if(action==1) assert(!offer(0)); /* Completion before send returns. */
 return action==4?-EIO:0;
}
static unsigned long wait_for_completion_timeout(struct completion *completion,unsigned long timeout) {
 assert(timeout==1000 && wipes && r.connection_lock.held && irq.lifecycle_lock);
 if(action==0) assert(!offer(0));
 if(action==2) assert(!offer(2));
 if(action==3) assert(offer(3)==-EPROTO);
 if(action==6) mt6797_radio_link_abort(&r,-ESHUTDOWN);
 return completion->done;
}
static void mt6797_wifi_hif_runtime_fail(struct mt6797_wifi_hif_irq *i,int reason) {
 assert(i==&irq && i->lifecycle_lock); terminals++; terminal_reason=reason;
 i->mode=0; mt6797_radio_link_abort(&r,reason);
}
'''
key_source = (driver / 'ais_key.c').read_text()
program += key_source[key_source.index('int mt6797_wifi_ais_ccmp_ready_match('):]
for name in ['mt6797_radio_connection_cancelled', 'mt6797_radio_link_abort',
             'mt6797_radio_link_event', 'mt6797_radio_key_connection',
             'mt6797_radio_key_send', 'mt6797_radio_add_key', 'mt6797_radio_del_key',
             'mt6797_radio_set_default_key', 'mt6797_radio_change_station']:
    program += function(name)
program += r'''
static u8 material[16]={1,2,3}, sequence[6]={7,0,0,0,0,0};
static struct key_params params;
static void setup(void) {
 memset(&r,0,sizeof(r)); memset(&irq,0,sizeof(irq)); memset(&c,0,sizeof(c));
 r.irq=&irq; r.connection=&c; r.command_sequence=255;
 c.bss=&bss; c.connected=c.link_attached=true;
 c.join.negotiated.security=MT6797_WIFI_AIS_SECURITY_WPA2_PSK_CCMP;
 irq.mode=MT6797_WIFI_HIF_IRQ_RUNTIME;
 params=(struct key_params){.key=material,.key_len=16,.seq=sequence,.seq_len=6,.cipher=WLAN_CIPHER_SUITE_CCMP};
 writes=terminals=queued=wipes=0; action=terminal_reason=0;
}
static int add(u8 index,bool pairwise) {
 int ret=mt6797_radio_add_key(NULL,&r.wdev,-1,index,pairwise,pairwise?bss.bssid:NULL,&params);
 assert(!r.connection_lock.held && !irq.lifecycle_lock && !r.lock); return ret;
}
static int del(u8 index,bool pairwise) {
 int ret=mt6797_radio_del_key(NULL,&r.wdev,-1,index,pairwise,pairwise?bss.bssid:NULL);
 assert(!r.connection_lock.held && !irq.lifecycle_lock && !r.lock); return ret;
}
static int authorize(bool set) {
 struct station_parameters p={.sta_flags_mask=BIT(NL80211_STA_FLAG_AUTHORIZED),
 .sta_flags_set=set?BIT(NL80211_STA_FLAG_AUTHORIZED):0};
 int ret=mt6797_radio_change_station(NULL,&r.wdev,bss.bssid,&p);
 assert(!r.connection_lock.held && !r.lock); return ret;
}
int main(void) {
 setup(); r.connection=NULL; assert(add(0,true)==-ENOTCONN && !writes);
 setup(); c.cancel=true; assert(add(0,true)==-ENOTCONN && !writes);
 setup(); irq.mode=0; assert(add(0,true)==-ENOTCONN && !writes);
 setup(); params.key_len=15; assert(add(0,true)==-EINVAL && !writes);
 setup(); params.seq_len=5; assert(add(0,true)==-EINVAL && !writes);
 setup(); assert(add(1,true)==-EINVAL && !writes);
 setup(); params.cipher=0; assert(add(0,true)==-EINVAL && !writes);
 for(action=0;action<2;) {
  int mode=action; setup(); action=mode;
  assert(!add(0,true) && writes==1 && c.pairwise_key.installed && c.pairwise_ready && !c.key_waiting);
  assert(r.command_sequence==3 && !terminals);
  assert(!offer(3)); /* Late malformed/duplicate events do not rewrite a completed result. */
  assert(!add(0,true) && writes==1); /* Identical installation is not sent again. */
  u8 other[16]={9}; params.key=other;
  assert(add(0,true)==-EOPNOTSUPP && writes==1);
  assert(!del(0,true) && writes==2 && !c.pairwise_ready && !c.pairwise_key.installed);
  assert(add(0,true)==-EOPNOTSUPP && writes==2); /* Delete does not re-arm unsolicited PTK matching. */
  action=mode+1;
 }
 setup(); assert(!add(1,false) && writes==1 && c.group_key.index==1);
 assert(!add(1,false) && writes==1);
 assert(add(2,false)==-EOPNOTSUPP && writes==1); /* Same key under a new ID would reset counters. */
 u8 replacement[16]={8}; params.key=replacement;
 assert(!add(2,false) && writes==2 && c.group_key.index==2);
 assert(del(1,false)==-ENOENT && writes==2 && c.group_key.installed);
 assert(!del(2,false) && writes==3 && !c.group_key.installed);
 for(int mode=2;mode<=6;mode++) {
  setup(); action=mode;
  int expected=mode==3?-EPROTO:mode==4?-EIO:mode==6?-ESHUTDOWN:-ETIMEDOUT;
  assert(add(0,true)==expected && writes==1 && terminals==1 && terminal_reason==expected);
  assert(c.cancel && !c.pairwise_key.installed && !c.key_waiting && queued);
  assert(add(0,true)==-ENOTCONN && writes==1);
 }
 setup(); assert(authorize(true)==-ENOKEY && !c.authorized);
 assert(!add(0,true)); assert(authorize(true)==-ENOKEY && !c.authorized);
 assert(!add(1,false)); assert(!authorize(true) && c.authorized);
 assert(!mt6797_radio_set_default_key(NULL,r.netdev,-1,1,false,true));
 assert(mt6797_radio_set_default_key(NULL,r.netdev,-1,2,false,true)==-ENOENT);
 assert(mt6797_radio_set_default_key(NULL,r.netdev,-1,1,true,true)==-EOPNOTSUPP);
 assert(!authorize(false) && !c.authorized);
 assert(!authorize(true) && c.authorized);
 assert(!del(1,false) && !c.authorized);
 setup(); c.join.negotiated.security=MT6797_WIFI_AIS_SECURITY_OPEN;
 assert(!authorize(true) && c.authorized);
 c.cancel=true; assert(authorize(false)==-ENOTCONN);
 puts("PASS: production key callbacks and authorization: immediate/deferred readiness, duplicate suppression, PTK rekey refusal, GTK slot ownership, malformed/foreign events, timeout/transport/abort fencing and lock release");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-radio-key-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
