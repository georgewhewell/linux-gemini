#!/usr/bin/env python3
"""Production connect admission/ownership with cached-BSS/rate-builder fakes.
Codec and HIF tests cover the real rate logic/stages. No kernel MLME or firmware.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile
base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-worker-host.py')))
program = base['program'].split('enum stage {')[0]
program = program.replace('struct cfg80211_bss { u8 bssid[6]; };',
 'struct cfg80211_bss_ies { u8 data[64]; size_t len; }; struct cfg80211_bss { u8 bssid[6]; struct ieee80211_channel *channel; struct cfg80211_bss_ies *ies; u16 capability; };')
program = program.replace('struct ieee80211_channel { int dummy; };',
 'struct ieee80211_channel { unsigned flags,band,center_freq; };')
program += r'''
#define ETH_P_PAE 0x888e
#define WLAN_CIPHER_SUITE_CCMP 0x000fac04
#define WLAN_AKM_SUITE_PSK 0x000fac02
#define WLAN_EID_RSN 48
#define WLAN_EID_EXT_CAPABILITY 127
#define WLAN_EID_SSID 0
#define WLAN_CAPABILITY_ESS 1
#define WLAN_CAPABILITY_PRIVACY 16
#define WLAN_CAPABILITY_SHORT_PREAMBLE 32
#define WLAN_CAPABILITY_SHORT_SLOT_TIME 1024
#define IEEE80211_CHAN_DISABLED 1
#define IEEE80211_CHAN_NO_IR 128
#define IEEE80211_BSS_TYPE_ESS 1
#define IEEE80211_PRIVACY_ON 1
#define IEEE80211_PRIVACY_OFF 0
#define NL80211_BAND_2GHZ 0
#define NL80211_MFP_NO 0
#define NL80211_MFP_REQUIRED 1
#define NL80211_AUTHTYPE_OPEN_SYSTEM 0
#define NL80211_AUTHTYPE_SAE 4
#define NL80211_AUTHTYPE_AUTOMATIC 8
#define NL80211_WPA_VERSION_2 2
#define NL80211_WPA_VERSION_3 4
#define WLAN_AKM_SUITE_SAE 0x000fac08
#define WLAN_AKM_SUITE_PSK_SHA256 0x000fac06
#define WLAN_EID_RSNX 244
#define CONNECT_REQ_EXTERNAL_AUTH_SUPPORT 8
static u32 get_unaligned_be32(const void *p) { const u8 *b=p; return (u32)b[0]<<24|(u32)b[1]<<16|(u32)b[2]<<8|b[3]; }
static void put_unaligned_be32(u32 v,void *p) { u8 *b=p; b[0]=v>>24; b[1]=v>>16; b[2]=v>>8; b[3]=v; }
static void rsn_put_le16(u16 v,void *p) { u8 *b=p; b[0]=v; b[1]=v>>8; }
#define ASSOC_REQ_DISABLE_HT 1
#define ASSOC_REQ_DISABLE_VHT 2
#define ASSOC_REQ_DISABLE_HE 16
#define ASSOC_REQ_DISABLE_EHT 32
#define ASSOC_REQ_DISABLE_UHR 64
#define rcu_read_lock() do {} while(0)
#define rcu_read_unlock() do {} while(0)
#define rcu_dereference(p) (p)
static u16 htons(u16 v) { return v>>8|v<<8; }
struct cfg80211_crypto_settings {
 u32 wpa_versions,cipher_group,ciphers_pairwise[4],akm_suites[4];
 unsigned n_ciphers_pairwise,n_akm_suites;
 u16 control_port_ethertype;
 bool control_port_over_nl80211,control_port_no_encrypt;
};
struct cfg80211_connect_params {
 const u8 *ssid,*ie,*bssid,*prev_bssid;
 size_t ssid_len,ie_len,key_len;
 bool privacy,pbss,want_1x;
 unsigned mfp,flags,auth_type;
 struct ieee80211_channel *channel;
 struct cfg80211_crypto_settings crypto;
};
static struct mt6797_wifi_radio r;
static struct ieee80211_channel channel={.center_freq=2437};
static struct cfg80211_bss_ies peer={.data={1,1,0x82},.len=3};
static struct cfg80211_bss bss={.bssid={2,1,2,3,4,5},.channel=&channel,.ies=&peer};
static unsigned allocated,freed,gets,bss_puts,queued;
static bool no_memory,no_bss;
static int rate_error;
static struct mt6797_wifi_radio *wiphy_priv(struct wiphy *w) { (void)w; return &r; }
static void init_completion(struct completion *c) { c->done=false; }
static void *kzalloc(size_t n,int flags) { (void)flags; if(no_memory) return NULL; allocated++; return calloc(1,n); }
static void kfree(void *p) { freed++; free(p); }
static struct cfg80211_bss *cfg80211_get_bss(struct wiphy *w,struct ieee80211_channel *ch,
 const u8 *mac,const u8 *ssid,size_t length,int type,int privacy) {
 (void)w; (void)ch; (void)mac; (void)privacy; assert(ssid && length && type==IEEE80211_BSS_TYPE_ESS);
 gets++; return no_bss?NULL:&bss;
}
static void cfg80211_put_bss(struct wiphy *w,struct cfg80211_bss *b) { (void)w; assert(b==&bss); bss_puts++; }
static const u8 *cfg80211_find_ie(u8 id,const u8 *ies,size_t length) {
 for(size_t i=0;i+2<=length && i+2+ies[i+1]<=length;i+=2+ies[i+1]) if(ies[i]==id) return ies+i;
 return NULL;
}
static unsigned ieee80211_frequency_to_channel(unsigned freq) { assert(freq==2437); return 6; }
int mt6797_wifi_ais_join_rates_parse(const u8 *ies,size_t n,u16 cap,struct mt6797_wifi_ais_join_params *p) {
 (void)ies; (void)n; (void)cap; p->operational_rates=p->basic_rates=1; return rate_error;
}
int mt6797_wifi_ais_join_rates_ies_build(const struct mt6797_wifi_ais_join_params *p,u8 *ies,size_t n,size_t *length) {
 (void)p; assert(n>=3); ies[0]=1; ies[1]=1; ies[2]=0x82; *length=3; return 0;
}
static void queue_work(int queue,struct work_struct *work) { (void)queue; (void)work; queued++; }
'''
rsn_source = (Path(__file__).resolve().parents[2] / 'drivers/net/wireless/mediatek/mt6797/ais_rsn.c').read_text()
program += re.sub(r'^#include .*\n', '', rsn_source, flags=re.M).replace('put_unaligned_le16', 'rsn_put_le16')
program += base['function']('mt6797_radio_connect(')
program += r'''
static u8 ssid[]={1,2,3,4};
static u8 rsn[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
static struct cfg80211_connect_params p;
static void setup(bool secure) {
 memset(&r,0,sizeof(r)); memset(&p,0,sizeof(p));
 p.ssid=ssid; p.ssid_len=4; p.privacy=secure;
 allocated=freed=gets=bss_puts=queued=0; no_memory=no_bss=false; rate_error=0;
 channel.flags=0; bss.ies=&peer;
 if(secure) {
  p.ie=rsn; p.ie_len=sizeof(rsn); p.crypto.wpa_versions=2;
  p.crypto.cipher_group=p.crypto.ciphers_pairwise[0]=WLAN_CIPHER_SUITE_CCMP;
  p.crypto.n_ciphers_pairwise=p.crypto.n_akm_suites=1; p.crypto.akm_suites[0]=WLAN_AKM_SUITE_PSK;
 }
}
static int connect(void) { int ret=mt6797_radio_connect(NULL,NULL,&p); assert(!r.lock); return ret; }
static void release(void) { free(r.connection); r.connection=NULL; }
/* PMF admission with the real ais_rsn.c. */
static u8 ap_rsn[]={48,24,1,0,0,15,172,4,1,0,0,15,172,4,2,0,0,15,172,2,0,15,172,8,0x80,0};
static void ap_ies(bool with_rsn) {
 u8 rates[]={1,1,0x82}; peer.len=0;
 memcpy(peer.data,rates,3); peer.len=3;
 if(with_rsn) { memcpy(peer.data+3,ap_rsn,sizeof(ap_rsn)); peer.len+=sizeof(ap_rsn); }
}
static void pmf_cases(void) {
 u8 sta[40]; memcpy(sta,rsn,22);
 /* WPA2-PSK + PMF (ieee80211w=1 against an MFPC AP): PMF negotiated. */
 setup(true); ap_ies(true); sta[20]=0x80; p.ie=sta; p.ie_len=22; p.mfp=NL80211_MFP_REQUIRED;
 assert(!connect() && r.connection->pmf_negotiated && queued==1); release();
 setup(true); ap_ies(true); p.ie=sta; p.ie_len=22; /* ap_scan=2: MFPC but mfp NO */
 assert(!connect() && r.connection->pmf_negotiated); release();
 setup(true); ap_ies(false); p.ie=sta; p.ie_len=22;
 assert(connect()==-EOPNOTSUPP && !queued && freed==allocated && bss_puts==1);
 ap_rsn[24]=0; setup(true); ap_ies(true); p.ie=sta; p.ie_len=22;
 assert(!connect() && !r.connection->pmf_negotiated); release();
 setup(true); ap_ies(true); p.ie=sta; p.ie_len=22; p.mfp=NL80211_MFP_REQUIRED;
 assert(connect()==-EOPNOTSUPP && freed==allocated); ap_rsn[24]=0x80;
 setup(false); p.mfp=NL80211_MFP_REQUIRED; assert(connect()==-EOPNOTSUPP && !allocated);
 setup(true); p.auth_type=NL80211_AUTHTYPE_SAE; assert(connect()==-EOPNOTSUPP && !allocated);
 ap_ies(false);
}
int main(void) {
 setup(true); p.mfp=2; assert(connect()==-EOPNOTSUPP && !queued && !allocated); /* MFP optional */
 setup(true); p.crypto.akm_suites[0]=0x000fac08; assert(connect()==-EOPNOTSUPP && !queued && !allocated); /* SAE not yet */
 pmf_cases();

 for(unsigned secure=0;secure<2;secure++) {
  setup(secure); p.crypto.control_port_no_encrypt=true;
  assert(!connect() && queued==1 && allocated==1 && !freed && !bss_puts);
  assert(r.tx_diag.connection_generation==1);
  assert(!r.connection->pmf_negotiated);
  assert(r.connection->control_port_no_encrypt && r.connection->join.negotiated.channel.primary==6);
  assert(r.connection->join.negotiated.security==(secure?MT6797_WIFI_AIS_SECURITY_WPA2_PSK_CCMP:MT6797_WIFI_AIS_SECURITY_OPEN));
  assert(r.connection->ies[0]==0 && r.connection->ies[1]==4 && !memcmp(r.connection->ies+2,ssid,4));
  assert(r.connection->join.association.ies_length==9+(secure?sizeof(rsn):0)); release();
 }
 for(unsigned secure=0;secure<2;secure++) {
  setup(secure); p.crypto.control_port_over_nl80211=true; assert(connect()==-EOPNOTSUPP && !allocated);
  setup(secure); p.crypto.control_port_ethertype=htons(0x0800); assert(connect()==-EOPNOTSUPP && !allocated);
 }
 for(unsigned which=0;which<16;which++) {
  setup(true);
  switch(which) {
  case 0: p.ssid=NULL; break; case 1: p.ssid_len=0; break; case 2: p.ssid_len=33; break;
  case 3: p.ie_len=1025; break; case 4: p.ie=NULL; break; case 5: p.key_len=16; break;
  case 6: p.pbss=true; break; case 7: p.prev_bssid=ssid; break; case 8: p.mfp=3; break;
  case 9: p.want_1x=true; break; case 10: p.flags=0x8000; break; case 11: p.auth_type=1; break;
  case 12: p.crypto.wpa_versions=1; break; case 13: p.crypto.n_ciphers_pairwise=0; break;
  case 14: p.crypto.cipher_group=0; break; case 15: p.crypto.akm_suites[0]=0; break;
  }
  assert(connect()==-EOPNOTSUPP && !allocated);
 }
 for(size_t n=0;n<sizeof(rsn);n++) {
  setup(true); u8 *short_ie=malloc(n?n:1); memcpy(short_ie,rsn,n); p.ie=short_ie; p.ie_len=n;
  assert(connect()<0 && !allocated); free(short_ie);
 }
 setup(true); u8 twice[44]; memcpy(twice,rsn,22); memcpy(twice+22,rsn,22); p.ie=twice; p.ie_len=44;
 assert(connect()==-EINVAL && !allocated);
 setup(false); p.ie=rsn; p.ie_len=22; assert(connect()==-EINVAL && !allocated);
 setup(true); rsn[20]=0x40; assert(connect()==-EINVAL && freed==allocated); rsn[20]=0; /* MFPR w/o MFPC */
 setup(false); no_memory=true; assert(connect()==-ENOMEM && !allocated && !gets);
 setup(false); no_bss=true; assert(connect()==-ENOENT && allocated==1 && freed==1 && !bss_puts);
 setup(false); channel.flags=IEEE80211_CHAN_NO_IR; assert(connect()==-EOPNOTSUPP && freed==1 && bss_puts==1);
 setup(false); rate_error=-EINVAL; assert(connect()==-EINVAL && freed==1 && bss_puts==1);
 setup(false); bss.ies=NULL; assert(connect()==-ENOENT && freed==1 && bss_puts==1);
 setup(false); r.stopping=true; assert(connect()==-ESHUTDOWN && !queued && freed==1 && bss_puts==1);
 setup(false); r.request=(void *)1; assert(connect()==-EBUSY && !queued && freed==1 && bss_puts==1);
 printf("PASS: production connect admission: open/WPA2 construction, WPA2+PMF (MFPC/MFPR vs AP), control-port support, crypto/IE bounds, MFP/RSN rejection, cached-BSS errors and all allocation/reference cleanup paths\n");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-radio-connect-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
