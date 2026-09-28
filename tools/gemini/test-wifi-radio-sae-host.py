#!/usr/bin/env python3
"""Production SAE worker loop and cfg80211 mgmt_tx/external_auth callbacks.

Runs the real mt6797_radio_sae(), mt6797_radio_mgmt_tx() and
mt6797_radio_external_auth() against a scripted wpa_supplicant (invoked from
the worker's wait point, as cfg80211 would call the callbacks concurrently)
and fake AIS bridge/cfg80211 functions. The AIS bridge itself is covered by
test-wifi-sae-hif-host.py. No kernel, firmware or real SAE cryptography.
"""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile

base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-worker-host.py')))
program = base['program'].split('enum stage {')[0]
program = program.replace('struct cfg80211_bss { u8 bssid[6]; };',
 'struct ieee80211_channel; struct cfg80211_bss { u8 bssid[6]; struct ieee80211_channel *channel; };')
program = program.replace('struct ieee80211_channel { int dummy; };', 'struct ieee80211_channel { unsigned center_freq; };')
program = program.replace('static ktime_t ktime_get(void) { return 100; }',
 'static ktime_t now_ms=100; static ktime_t ktime_get(void) { return now_ms; }')
program += r'''
#define WLAN_STATUS_SUCCESS 0
#define WLAN_STATUS_UNSPECIFIED_FAILURE 1
#define WLAN_AKM_SUITE_SAE 0x000fac08
static ktime_t ktime_sub_ms(ktime_t t,unsigned ms) { return t-ms; }
static void *wiphy_dev(struct wiphy *w) { (void)w; return NULL; }
static struct mt6797_wifi_radio *wiphy_priv(struct wiphy *w) { (void)w; extern struct mt6797_wifi_radio r; return &r; }
enum nl80211_external_auth_action { NL80211_EXTERNAL_AUTH_START, NL80211_EXTERNAL_AUTH_ABORT };
struct cfg80211_ssid { u8 ssid[32]; u8 ssid_len; };
struct cfg80211_external_auth_params {
 enum nl80211_external_auth_action action; u8 bssid[6]; struct cfg80211_ssid ssid;
 unsigned key_mgmt_suite; u16 status; const u8 *pmkid; u8 mld_addr[6];
};
struct cfg80211_mgmt_tx_params {
 struct ieee80211_channel *chan; bool offchan; unsigned wait; const u8 *buf; size_t len;
 bool no_cck, dont_wait_for_ack; int n_csa_offsets; const u16 *csa_offsets; int link_id;
};
enum mode { NORMAL, TIMEOUT, REJECT, CANCEL, NO_OWNER, HARD_TX, NO_ACK, BEGIN_FAIL };
struct mt6797_wifi_radio r;
static struct net_device netdev;
static struct ieee80211_channel chan={.center_freq=2437}, other={.center_freq=2412};
static struct cfg80211_bss bss={.bssid={2,6,7,8,9,10},.channel=&chan};
static struct {
 enum mode mode; unsigned step, waits, begins, ends, sends, starts, aborts, rx, tx_status, acks, pmkid_sets;
 int owner_error, end_status, begin_error; u16 sequences[8]; u8 rxq[4][64]; unsigned rx_count;
 u64 cookies[8]; bool resent;
} t;
/* Fake AIS bridge. */
int mt6797_wifi_ais_hif_ext_auth_begin(struct mt6797_wifi_ais_hif *ais) {
 assert(ais==&r.connection->ais && r.connection_lock.held && !r.lock); t.begins++;
 return t.begin_error;
}
int mt6797_wifi_ais_sae_auth_tx_check(const struct mt6797_wifi_ais_auth_pending *p,const u8 *f,size_t n) {
 assert(p==&r.connection->ais.auth.pending); return n>=30 && f[24]==3 ? 0 : -EINVAL;
}
int mt6797_wifi_ais_hif_ext_auth_send(struct mt6797_wifi_ais_hif *ais,
 const struct mt6797_wifi_ais_hif_ext_auth_request *q,struct mt6797_wifi_ais_mgmt_tx_status *s) {
 assert(ais==&r.connection->ais && !r.lock && q->frame==r.connection->sae_scratch);
 assert(q->fixed_rate==r.connection->join.negotiated.default_rate && q->timeout_ms==500 && q->frame[24]==3);
 t.sequences[t.sends++]=q->sequence_number; memset(s,0,sizeof(*s));
 if(t.mode==HARD_TX) return -EIO;
 if(t.mode==NO_ACK && t.sends==1) { s->status=1; return -EREMOTEIO; }
 return 0;
}
int mt6797_wifi_ais_hif_ext_auth_take(struct mt6797_wifi_ais_hif *ais,u8 *f,size_t cap,size_t *n) {
 assert(ais==&r.connection->ais && !r.lock && f==r.connection->sae_scratch && cap==sizeof(r.connection->sae_scratch));
 if(!t.rx_count) return -EAGAIN;
 memcpy(f,t.rxq[0],64); *n=64; memmove(t.rxq[0],t.rxq[1],sizeof(t.rxq[0])*3); t.rx_count--; return 0;
}
void mt6797_wifi_ais_hif_ext_auth_kick(struct mt6797_wifi_ais_hif *ais) { assert(ais==&r.connection->ais && r.lock); }
int mt6797_wifi_ais_hif_ext_auth_end(struct mt6797_wifi_ais_hif *ais,u16 status) {
 assert(ais==&r.connection->ais && !r.lock && !r.connection->sae_active); t.ends++; t.end_status=status;
 return status ? -EACCES : 0;
}
int mt6797_wifi_ais_rsn_set_pmkid(u8 *ies,size_t *len,size_t cap,const u8 *pmkid) {
 assert(ies==r.connection->ies && cap==sizeof(r.connection->ies) && pmkid[0]==0x55); t.pmkid_sets++; *len+=18; return 0;
}
/* Fake cfg80211. */
static int cfg80211_external_auth_request(struct net_device *d,struct cfg80211_external_auth_params *p,int gfp) {
 (void)gfp; assert(d==&netdev && !r.lock && p->key_mgmt_suite==WLAN_AKM_SUITE_SAE);
 assert(!memcmp(p->bssid,bss.bssid,6) && p->ssid.ssid_len==4 && !memcmp(p->ssid.ssid,"test",4));
 if(p->action==NL80211_EXTERNAL_AUTH_START) { t.starts++; return t.owner_error; }
 t.aborts++; return 0;
}
static bool cfg80211_rx_mgmt(struct wireless_dev *w,int freq,int sig,const u8 *buf,size_t n,u32 flags) {
 assert(w==&r.wdev && freq==2437 && !sig && !flags && n==64 && buf[24]==3 && !r.lock); t.rx++; return true;
}
static void cfg80211_mgmt_tx_status(struct wireless_dev *w,u64 cookie,const u8 *buf,size_t n,bool ack,int gfp) {
 (void)gfp; assert(w==&r.wdev && buf && n && !r.lock && cookie==t.cookies[t.tx_status]); t.tx_status++; t.acks+=ack;
}
'''
fn = base['function']
import re
radio = base['radio']
for name in ['MT6797_RADIO_SAE_ASSOC_RESERVE_MS', 'MT6797_RADIO_SAE_MIN_BUDGET_MS', 'MT6797_RADIO_SAE_TX_TIMEOUT_MS']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', radio, re.M)[0] + '\n'
program += 'static void memzero_explicit(void *p,size_t n) { memset(p,0,n); }\n'
for name in ['mt6797_radio_connection_cancelled', 'mt6797_radio_sae_request', 'mt6797_radio_sae',
             'mt6797_radio_mgmt_tx', 'mt6797_radio_external_auth']:
    program += fn(name)
program += r'''
static u8 frame[64];
static int user_tx(u16 transaction,bool noack) {
 struct cfg80211_mgmt_tx_params p={.chan=&chan,.buf=frame,.len=sizeof(frame),.dont_wait_for_ack=noack,.link_id=-1};
 u64 cookie=0; memset(frame,0,sizeof(frame)); frame[24]=3; frame[26]=transaction;
 int ret=mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie);
 if(!ret && !noack) t.cookies[t.tx_status]=cookie;
 if(!ret) { assert(cookie && r.connection->sae_tx_pending);
  assert(mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-EBUSY); }
 return ret;
}
static void peer_rx(u16 transaction) { memset(t.rxq[t.rx_count],0,64); t.rxq[t.rx_count][24]=3; t.rxq[t.rx_count][26]=transaction; t.rx_count++; }
static int user_status(u16 status,bool pmkid) {
 static const u8 id[16]={0x55};
 struct cfg80211_external_auth_params p={.status=status,.pmkid=pmkid?id:NULL};
 memcpy(p.bssid,bss.bssid,6); memcpy(p.ssid.ssid,"test",4); p.ssid.ssid_len=4;
 return mt6797_radio_external_auth(NULL,&netdev,&p);
}
/* wpa_supplicant runs while the worker sleeps at its single wait point. */
void mt6797_wifi_ais_hif_ext_auth_wait(struct mt6797_wifi_ais_hif *ais,ktime_t deadline) {
 assert(ais==&r.connection->ais && !r.lock && r.connection_lock.held && r.connection->sae_active);
 assert(deadline==r.connection->ais.channel_deadline-600);
 t.waits++; if(getenv("TRACE")) fprintf(stderr,"wait mode=%d step=%u sends=%u tx_status=%u\n",t.mode,t.step,t.sends,t.tx_status);
 switch(t.mode==NO_ACK && t.step==1 && !t.resent ? 99 : t.step++) {
 case 0:
  if(t.mode==TIMEOUT) { now_ms=deadline; t.step--; break; }
  if(t.mode==CANCEL) { r.connection->cancel=true; break; }
  assert(!user_tx(1,false)); break;        /* SAE commit (ack requested) */
 case 1: peer_rx(1); peer_rx(1); break;     /* AP commit + retransmission */
 case 2: assert(!user_tx(2,true)); break;   /* SAE confirm, no-ack as wpa_supplicant 2.10 */
 case 3: peer_rx(2);
  if(t.mode==REJECT) assert(!user_status(WLAN_STATUS_UNSPECIFIED_FAILURE,false));
  else assert(!user_status(WLAN_STATUS_SUCCESS,true));
  assert(user_status(0,false)==-ENOTCONN); /* exactly one result */
  assert(user_tx(1,false)==-ENOTCONN);
  break;
 case 99: t.resent=true; assert(!user_tx(1,false)); break; /* resend after no-ack */
 default: assert(0);
 }
}
static void setup(enum mode mode) {
 memset(&r,0,sizeof(r)); memset(&t,0,sizeof(t)); now_ms=100; t.mode=mode;
 r.netdev=&netdev; r.connection=calloc(1,sizeof(*r.connection));
 r.connection->bss=&bss; r.connection->sae=true; r.connection->pmf_negotiated=true;
 r.connection->join.negotiated.default_rate=7;
 memcpy(r.connection->join.negotiated.ssid,"test",4); r.connection->join.negotiated.ssid_length=4;
 r.connection->join.association.ies=r.connection->ies; r.connection->join.association.ies_length=20;
 r.connection->ais.channel_deadline=100+3990;
 if(mode==NO_OWNER) t.owner_error=-EINVAL;
 if(mode==BEGIN_FAIL) t.begin_error=-EPIPE;
 r.connection_lock.held=1;
}
static int run(void) { int ret=mt6797_radio_sae(&r); assert(!r.lock && !r.connection->sae_active && !r.connection->sae_tx_pending); return ret; }
static void scratch_clean(void) {
 for(size_t i=0;i<sizeof(r.connection->sae_frame);i++) assert(!r.connection->sae_frame[i] && !r.connection->sae_scratch[i]);
}
int main(void) {
 setup(NORMAL); assert(!run());
 assert(t.begins==1 && t.starts==1 && !t.aborts && t.ends==1 && !t.end_status);
 assert(t.sends==2 && t.sequences[0]==1 && t.sequences[1]==2 && r.connection->auth_sequence==2);
 assert(t.rx==3 && t.tx_status==1 && t.acks==1 && t.pmkid_sets==1);
 assert(r.connection->join.association.ies_length==38); scratch_clean(); free(r.connection);
 setup(NO_ACK); assert(!run()); assert(t.sends==3 && t.tx_status==2 && t.acks==1 && t.sequences[2]==3); free(r.connection);
 setup(REJECT); assert(run()==-EACCES && t.end_status==1 && !t.aborts && !t.pmkid_sets); free(r.connection);
 setup(TIMEOUT); assert(run()==-ETIMEDOUT && t.aborts==1 && t.end_status==1 && !t.sends); free(r.connection);
 setup(CANCEL); assert(run()==-ECANCELED && t.aborts==1 && t.ends==1); free(r.connection);
 setup(HARD_TX); assert(run()==-EIO && t.sends==1 && t.aborts==1 && t.tx_status==1 && !t.acks); scratch_clean(); free(r.connection);
 setup(NO_OWNER); assert(run()==-ENOTCONN && t.starts==1 && !t.aborts && t.ends==1 && !t.waits); free(r.connection);
 setup(BEGIN_FAIL); assert(run()==-EPIPE && !t.starts && !t.ends); free(r.connection);
 /* Not enough of the join channel grant left for SAE plus association. */
 setup(NORMAL); r.connection->ais.channel_deadline=100+600+199; assert(run()==-ETIMEDOUT && !t.begins); free(r.connection);
 setup(NORMAL); r.connection->cancel=true; assert(run()==-ECANCELED && t.begins==1 && !t.starts && t.ends==1); free(r.connection);
 /* Callback admission outside an SAE exchange and malformed requests. */
 setup(NORMAL); r.connection_lock.held=0;
 struct cfg80211_mgmt_tx_params p={.chan=&chan,.buf=frame,.len=64,.link_id=-1}; u64 cookie;
 memset(frame,0,64); frame[24]=3;
 assert(mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-ENOTCONN);
 r.connection->sae_active=true;
 p.chan=&other; assert(mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-EINVAL); p.chan=NULL;
 frame[24]=0; assert(mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-EINVAL); frame[24]=3;
 p.len=29; assert(mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-EINVAL); p.len=1025;
 assert(mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-EINVAL); p.len=64;
 p.offchan=true; assert(mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-EOPNOTSUPP); p.offchan=false;
 p.wait=10; assert(mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-EOPNOTSUPP); p.wait=0;
 p.link_id=0; assert(mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-EOPNOTSUPP); p.link_id=-1;
 struct wireless_dev stranger; assert(mt6797_radio_mgmt_tx(NULL,&stranger,&p,&cookie)==-EOPNOTSUPP);
 r.stopping=true; assert(mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-ENOTCONN); r.stopping=false;
 assert(!r.connection->sae_tx_pending);
 struct cfg80211_external_auth_params e={.status=0}; memcpy(e.bssid,bss.bssid,6);
 e.bssid[5]^=1; assert(mt6797_radio_external_auth(NULL,&netdev,&e)==-EINVAL); e.bssid[5]^=1;
 memcpy(e.ssid.ssid,"tesx",4); e.ssid.ssid_len=4; assert(mt6797_radio_external_auth(NULL,&netdev,&e)==-EINVAL);
 e.ssid.ssid_len=3; memcpy(e.ssid.ssid,"tes",3); assert(mt6797_radio_external_auth(NULL,&netdev,&e)==-EINVAL);
 struct net_device stranger_dev; assert(mt6797_radio_external_auth(NULL,&stranger_dev,&e)==-EINVAL);
 assert(!r.connection->sae_done);
 e.ssid.ssid_len=0; assert(!mt6797_radio_external_auth(NULL,&netdev,&e) && r.connection->sae_done && !r.connection->sae_pmkid_set);
 free(r.connection);
 r.connection=NULL; assert(mt6797_radio_external_auth(NULL,&netdev,&e)==-ENOTCONN && mt6797_radio_mgmt_tx(NULL,&r.wdev,&p,&cookie)==-ENOTCONN);
 puts("PASS: production SAE worker loop: START/ABORT requests, commit/confirm mailbox with EBUSY/cookies/ack policy, peer RX delivery, success with PMKID, rejection, timeout, cancel, hard TX failure, missing socket owner, grant budget; mgmt_tx/external_auth admission");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-radio-sae-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
