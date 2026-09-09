#!/usr/bin/env python3
"""Production key callbacks and event matcher with synchronous transport fakes.

Exercises lifecycle/error policy, not real IRQ concurrency or firmware crypto.
The production codecs and ownership validation are included below; all keys are synthetic.
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
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
static u64 mt6797_wifi_data_rx_pn(const u8 p[6]) { u64 n=0; for(unsigned i=0;i<6;i++) n|=(u64)p[i]<<(8*i); return n; }
#define WLAN_CIPHER_SUITE_CCMP 0x000fac04
#define WLAN_CIPHER_SUITE_AES_CMAC 0x000fac06
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
static u8 last_command[72];
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
static int offer(int kind) {
 u8 bytes[8]={0}; memcpy(bytes+2,bss.bssid,6);
 struct mt6797_wifi_event_view e={.id=0x24,.payload=bytes,.payload_length=8,.packet_length=16};
 if(kind==2) bytes[2]^=4;
 if(kind==3) e.payload_length=7;
 return mt6797_radio_link_event(&r,&e);
}
static int mt6797_wifi_hif_runtime_send(struct mt6797_wifi_hif_irq *hif,u32 address,const u8 *command,size_t length) {
 (void)hif; (void)command;
 assert(r.connection_lock.held && irq.lifecycle_lock && address==MT6797_WIFI_MCR_WTDR1 && length==72);
 writes++; memcpy(last_command,command,length);
 if(action==7) mt6797_radio_link_abort(&r,-ECANCELED);
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
# Include the real ownership validators and key/command encoders. Transport,
# locks and cfg80211 are the only fakes around these production callbacks.
def extract(text, name):
    match = re.search(r'(?:static )?(?:int|bool)\s+' + re.escape(name) + r'\([^;{}]*\)\n\{', text)
    assert match, name
    end = match.end()
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end] + '\n'
protocol = (driver / 'protocol.c').read_text()
for name in ['MT6797_WIFI_COMMAND_PQ_ID', 'MT6797_WIFI_COMMAND_PACKET_TYPE']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', protocol, re.M)[0] + '\n'
program += r'''
#define check_add_overflow(a,b,p) __builtin_add_overflow(a,b,p)
static void put_unaligned_le16(u16 v,void *p) { u8 *b=p; b[0]=v; b[1]=v>>8; }
'''
program += extract(protocol, 'mt6797_wifi_command_init')
protocol = (driver / 'ais_protocol.c').read_text()
for name in ['mt6797_wifi_ais_wtbl_valid', 'mt6797_wifi_ais_bss_valid',
             'mt6797_wifi_ais_sta_valid', 'mt6797_wifi_ais_sta_pending_init']:
    program += extract(protocol, name)
program += re.sub(r'^#include .*\n', '', (driver / 'ais_key.c').read_text(), flags=re.M)
management = (driver / 'ais_mgmt_tx.c').read_text()
program += '#define MT6797_WIFI_AIS_MGMT_EVENT_TX_DONE 0x0fU\n'
for name in ['mt6797_wifi_ais_mgmt_require_open', 'mt6797_wifi_ais_mgmt_lease_matches',
             'mt6797_wifi_ais_mgmt_tx_event_match']:
    program += extract(management, name)

for name in ['mt6797_radio_connection_cancelled', 'mt6797_radio_link_abort',
             'mt6797_radio_link_event', 'mt6797_radio_key_connection',
             'mt6797_radio_key_send', 'mt6797_radio_sta_owner_same',
             'mt6797_radio_wtbl_same', 'mt6797_radio_igtk_owner',
             'mt6797_radio_igtk_connection', 'mt6797_radio_igtk_forget',
             'mt6797_radio_igtk_send', 'mt6797_radio_add_igtk',
             'mt6797_radio_del_igtk', 'mt6797_radio_igtk_cleanup',
             'mt6797_radio_set_default_mgmt_key', 'mt6797_radio_add_key', 'mt6797_radio_del_key',
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
 c.ais.runtime.bss=(struct mt6797_wifi_ais_bss_handle){
  .generation=1,.cookie=1,.own_addr={2,1,2,3,4,5},.bss_index=0,
  .bmc={.generation=1,.cookie=2,.owner_cookie=1,.index=0,.role=MT6797_WIFI_AIS_WTBL_ROLE_BSS_BMC}};
 c.ais.runtime.sta=(struct mt6797_wifi_ais_sta_handle){
  .generation=1,.cookie=3,.bss_cookie=1,.bss_index=0,.sta_index=0,
  .pairwise={.generation=1,.cookie=4,.owner_cookie=3,.index=1,.role=MT6797_WIFI_AIS_WTBL_ROLE_STA_PAIRWISE},
  .ap_bmc_group={.generation=1,.cookie=5,.owner_cookie=3,.index=2,.role=MT6797_WIFI_AIS_WTBL_ROLE_AP_STA_BMC_GROUP}};
 memcpy(c.ais.runtime.sta.peer_addr,bss.bssid,6);
 irq.mode=MT6797_WIFI_HIF_IRQ_RUNTIME;
 params=(struct key_params){.key=material,.key_len=16,.seq=sequence,.seq_len=6,.cipher=WLAN_CIPHER_SUITE_CCMP};
 writes=terminals=queued=wipes=0; action=terminal_reason=0; memset(last_command,0,sizeof(last_command));
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
static void pmf_setup(void) {
 setup(); c.pmf_negotiated=true; params.cipher=WLAN_CIPHER_SUITE_AES_CMAC;
}
static int default_igtk(u8 index) {
 int ret=mt6797_radio_set_default_mgmt_key(NULL,&r.wdev,-1,index);
 assert(!r.connection_lock.held && !irq.lifecycle_lock && !r.lock); return ret;
}
static int cleanup_igtk(void) {
 mutex_lock(&r.connection_lock);
 int ret=mt6797_radio_igtk_cleanup(&r);
 mutex_unlock(&r.connection_lock);
 assert(!r.connection_lock.held && !irq.lifecycle_lock && !r.lock); return ret;
}
static void assert_forgotten(void) {
 struct mt6797_radio_key zero={0}; struct mt6797_wifi_ais_sta_pending owner={0};
 assert(!memcmp(&c.igtk,&zero,sizeof(zero)) && !memcmp(&c.igtk_owner,&owner,sizeof(owner)));
 struct mt6797_wifi_ais_wtbl_handle wtbl={0};
 assert(!memcmp(&c.igtk_wtbl,&wtbl,sizeof(wtbl)) && !c.igtk_default && !c.authorized);
}
static void igtk_tests(void) {
 u8 next[16]={8}, seq[6]={0};
 /* Non-PMF association cannot install/delete/select IGTK, even through a raw callback. */
 setup(); params.cipher=WLAN_CIPHER_SUITE_AES_CMAC;
 assert(add(4,false)==-EOPNOTSUPP && del(4,false)==-EOPNOTSUPP && default_igtk(4)==-EOPNOTSUPP && !writes);
 for(unsigned id=0;id<256;id++) if(id!=4 && id!=5) {
  pmf_setup(); assert(add(id,false)==-EINVAL && !writes);
 }
 pmf_setup(); assert(add(4,true)==-EINVAL && !writes);
 pmf_setup(); params.key_len=15; assert(add(4,false)==-EINVAL && !writes);
 pmf_setup(); params.seq_len=5; assert(add(4,false)==-EINVAL && !writes);
 pmf_setup(); params.seq=NULL; assert(add(4,false)==-EINVAL && !writes);
 pmf_setup(); params.key=NULL; assert(add(4,false)==-EINVAL && !writes);
 pmf_setup(); params.mode=1; assert(add(4,false)==-EINVAL && !writes);
 pmf_setup(); params.vlan_id=1; assert(add(4,false)==-EINVAL && !writes);
 pmf_setup(); assert(mt6797_radio_add_key(NULL,&r.wdev,0,4,false,NULL,&params)==-EINVAL && !writes);
 pmf_setup(); assert(mt6797_radio_add_key(NULL,&r.wdev,-1,4,false,bss.bssid,&params)==-EINVAL && !writes);
 for(unsigned id=4;id<=5;id++) {
  pmf_setup(); assert(!add(id,false) && writes==1 && c.igtk.index==id && c.igtk.installed);
  assert(last_command[19]==6 && last_command[20]==id && last_command[22]==2);
  assert(!last_command[9] && !last_command[10]); /* RX group BIP, not PTK TX ownership. */
  assert(c.igtk.rx_pn[0]==7 && !c.group_key.installed && !c.pairwise_key.installed);
  c.igtk.rx_pn[0]=35; params.seq=seq;
  assert(!add(id,false) && writes==1 && c.igtk.rx_pn[0]==35);
  assert(add(9-id,false)==-EOPNOTSUPP && writes==1); /* Same material/new ID cannot reset IPN. */
  assert(!default_igtk(id) && c.igtk_default && writes==1);
  assert(default_igtk(9-id)==-ENOENT && c.igtk_default);
  params.key=next; assert(!add(id,false) && writes==2 && c.igtk_default && c.igtk.rx_pn[0]==0);
  params.key=material; assert(!add(9-id,false) && writes==3 && !c.igtk_default);
  assert(del(id,false)==-ENOENT && writes==3 && c.igtk.installed);
  assert(default_igtk(id)==-ENOENT && !c.igtk_default);
  assert(!default_igtk(9-id)); c.authorized=true;
  assert(!del(9-id,false) && writes==4); assert_forgotten();
  assert(last_command[8]==0 && last_command[20]==9-id && last_command[22]==2);
  for(unsigned n=24;n<72;n++) assert(!last_command[n]);
  assert(del(9-id,false)==-ENOENT && writes==4);
 }
 /* Real owner validation rejects invalid and coherently replaced generations. */
 pmf_setup(); c.ais.runtime.sta.ap_bmc_group.owner_cookie++;
 assert(add(4,false)==-EINVAL && !writes);
 pmf_setup(); assert(!add(4,false)); c.ais.runtime.sta.ap_bmc_group.cookie++;
 assert(del(4,false)==-ESTALE && default_igtk(4)==-ESTALE && writes==1);
 pmf_setup(); assert(!add(4,false)); c.ais.runtime.sta.ap_bmc_group.index=3;
 assert(add(4,false)==-ESTALE && del(4,false)==-ESTALE && default_igtk(4)==-ESTALE && writes==1);
 pmf_setup(); assert(!add(4,false));
 c.ais.runtime.bss.generation=c.ais.runtime.bss.bmc.generation=2;
 c.ais.runtime.sta.generation=c.ais.runtime.sta.pairwise.generation=c.ais.runtime.sta.ap_bmc_group.generation=2;
 assert(add(4,false)==-ESTALE && del(4,false)==-ESTALE && default_igtk(4)==-ESTALE && writes==1);
 pmf_setup(); assert(!add(4,false)); c.ais.runtime.sta.peer_addr[1]^=2;
 assert(del(4,false)==-ESTALE && writes==1);
 /* A failed install, rekey or delete clears host secrets only after transport fencing. */
 for(int mode=4;mode<=7;mode+=3) for(unsigned stage=0;stage<3;stage++) {
  pmf_setup(); if(stage) { assert(!add(4,false)); assert(!default_igtk(4)); c.authorized=true; }
  action=mode; params.key=next;
  int ret=stage==2 ? del(4,false) : add(stage?5:4,false);
  assert(ret==(mode==4?-EIO:-ECANCELED));
  assert(terminals && c.cancel && irq.mode!=MT6797_WIFI_HIF_IRQ_RUNTIME); assert_forgotten();
  assert(add(4,false)==-ENOTCONN);
 }
 /* Teardown wipes partial host state even when no firmware install completed. */
 pmf_setup(); c.igtk.updating=true; memset(c.igtk.material,0x5a,sizeof(c.igtk.material));
 c.igtk_default=true; c.authorized=true;
 assert(!cleanup_igtk() && !writes && !terminals); assert_forgotten();
 /* Teardown deletes before handles are released, despite an intentional disconnect. */
 pmf_setup(); assert(!add(4,false)); c.cancel=true; c.authorized=true;
 assert(!cleanup_igtk() && writes==2 && !terminals); assert_forgotten();
 assert(!cleanup_igtk() && writes==2);
 pmf_setup(); assert(!add(5,false)); c.cancel=true; action=4;
 assert(cleanup_igtk()==-EIO && terminals && c.cancel); assert_forgotten();
 pmf_setup(); assert(!add(4,false)); c.ais.runtime.sta.ap_bmc_group.index=3;
 assert(cleanup_igtk()==-ESTALE && writes==1 && terminals); assert_forgotten();
 pmf_setup(); assert(!add(4,false)); irq.mode=0;
 assert(cleanup_igtk()==-ENOTCONN && writes==1 && terminals); assert_forgotten();
 /* GTK and IGTK bookkeeping are independent even though firmware chooses AP BMC for both. */
 pmf_setup(); params.cipher=WLAN_CIPHER_SUITE_CCMP; assert(!add(1,false));
 params.cipher=WLAN_CIPHER_SUITE_AES_CMAC; assert(!add(4,false));
 assert(c.group_key.installed && c.group_key.index==1);
 assert(!del(4,false) && c.group_key.installed && c.group_key.index==1);
 pmf_setup(); params.cipher=WLAN_CIPHER_SUITE_CCMP; assert(!add(0,true)); assert(!add(1,false));
 assert(authorize(true)==-ENOKEY && !c.authorized);
 params.cipher=WLAN_CIPHER_SUITE_AES_CMAC; assert(!add(4,false)); assert(!authorize(true));
 assert(!del(4,false) && !c.authorized);
 puts("PASS: production IGTK callbacks/codecs: PMF gate, IDs4/5, BMC ownership, duplicate IPN preservation, replacement/stale deletion, default key, real generation validation, transport fencing and teardown secret cleanup");
}
int main(void) {
 igtk_tests();
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
  for(unsigned tid=0;tid<16;tid++) assert(c.pairwise_key.rx_pn[tid]==7);
  assert(c.pairwise_key.mgmt_rx_pn==7); c.pairwise_key.mgmt_rx_pn=29;
  c.pairwise_key.rx_pn[0]=19;
  assert(!offer(3)); /* Late malformed/duplicate events do not rewrite a completed result. */
  assert(!add(0,true) && writes==1 && c.pairwise_key.rx_pn[0]==19 && c.pairwise_key.mgmt_rx_pn==29); /* Identical installation is not sent again. */
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
