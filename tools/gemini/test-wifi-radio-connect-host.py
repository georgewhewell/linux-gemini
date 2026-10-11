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
# Use this kernel's real enums: hand-copied flags had assigned UHR's bit to
# MLO_SUPPORT, hiding rejection of wpa_supplicant's userspace capability flag.
cfg80211_header = (Path(__file__).resolve().parents[2] / 'include/net/cfg80211.h').read_text()
program += '\n#define BIT(n) (1U << (n))\n'
for name in ('ieee80211_channel_flags', 'cfg80211_assoc_req_flags'):
    program += re.search(r'enum ' + name + r'\s*\{.*?^\};',
                         cfg80211_header, re.S | re.M)[0] + '\n'
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
#define NL80211_MAX_NR_AKM_SUITES 2
#define WLAN_AKM_SUITE_SAE 0x000fac08
#define WLAN_AKM_SUITE_PSK_SHA256 0x000fac06
#define WLAN_EID_RSNX 244
#define WLAN_EID_SUPPORTED_REGULATORY_CLASSES 59
static u32 get_unaligned_be32(const void *p) { const u8 *b=p; return (u32)b[0]<<24|(u32)b[1]<<16|(u32)b[2]<<8|b[3]; }
static void put_unaligned_be32(u32 v,void *p) { u8 *b=p; b[0]=v>>24; b[1]=v>>16; b[2]=v>>8; b[3]=v; }
static void rsn_put_le16(u16 v,void *p) { u8 *b=p; b[0]=v; b[1]=v>>8; }
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
/* PMF, SAE external authentication and RSNXE admission with the real ais_rsn.c. */
static u8 ap_rsn[]={48,24,1,0,0,15,172,4,1,0,0,15,172,4,2,0,0,15,172,2,0,15,172,8,0x80,0};
static void ap_ies(bool with_rsn) {
 u8 rates[]={1,1,0x82}; peer.len=0;
 memcpy(peer.data,rates,3); peer.len=3;
 if(with_rsn) { memcpy(peer.data+3,ap_rsn,sizeof(ap_rsn)); peer.len+=sizeof(ap_rsn); }
}
static void wpa3_cases(void) {
 u8 sta[40]; memcpy(sta,rsn,22); u8 ies[48];
 /* WPA2-PSK + PMF (ieee80211w=1 against an MFPC AP): PMF negotiated. */
 setup(true); ap_ies(true); sta[20]=0x80; p.ie=sta; p.ie_len=22; p.mfp=NL80211_MFP_REQUIRED;
 assert(!connect() && r.connection->pmf_negotiated && !r.connection->sae && queued==1); release();
 setup(true); ap_ies(true); p.ie=sta; p.ie_len=22; /* ap_scan=2: MFPC but mfp NO */
 assert(!connect() && r.connection->pmf_negotiated); release();
 setup(true); ap_ies(false); p.ie=sta; p.ie_len=22;
 assert(connect()==-EOPNOTSUPP && !queued && freed==allocated && bss_puts==1);
 ap_rsn[24]=0; setup(true); ap_ies(true); p.ie=sta; p.ie_len=22;
 assert(!connect() && !r.connection->pmf_negotiated); release();
 setup(true); ap_ies(true); p.ie=sta; p.ie_len=22; p.mfp=NL80211_MFP_REQUIRED;
 assert(connect()==-EOPNOTSUPP && freed==allocated); ap_rsn[24]=0x80;
 /* The station selection, rather than allowed-list order, drives policy. */
 setup(true); ap_rsn[19]=6; ap_ies(true); sta[19]=6;
 p.ie=sta; p.ie_len=22; p.crypto.n_akm_suites=2;
 p.crypto.akm_suites[1]=WLAN_AKM_SUITE_PSK_SHA256;
 assert(!connect() && r.connection->pmf_negotiated && !r.connection->sae); release();
 ap_rsn[19]=2;
 /* WPA3-SAE: supplicant 2.10 (WPA2 version) and 2.11 (WPA3 version). */
 memcpy(sta,rsn,22); sta[19]=8; sta[20]=0xc0;
 for(unsigned version=0;version<2;version++) {
  setup(true); ap_ies(true); p.ie=sta; p.ie_len=22; p.mfp=NL80211_MFP_REQUIRED;
  p.crypto.akm_suites[0]=WLAN_AKM_SUITE_SAE; p.auth_type=NL80211_AUTHTYPE_SAE;
  p.flags=CONNECT_REQ_EXTERNAL_AUTH_SUPPORT; if(version) p.crypto.wpa_versions=NL80211_WPA_VERSION_3;
  assert(!connect() && r.connection->sae && r.connection->pmf_negotiated);
  assert(r.connection->join.negotiated.security==MT6797_WIFI_AIS_SECURITY_WPA2_PSK_CCMP); release();
 }
 setup(true); ap_ies(true); p.ie=sta; p.ie_len=22; p.mfp=NL80211_MFP_REQUIRED;
 p.crypto.wpa_versions=NL80211_WPA_VERSION_3; p.crypto.n_akm_suites=2;
 p.crypto.akm_suites[1]=WLAN_AKM_SUITE_SAE; p.auth_type=NL80211_AUTHTYPE_SAE;
 p.flags=CONNECT_REQ_EXTERNAL_AUTH_SUPPORT;
 assert(!connect() && r.connection->sae && r.connection->pmf_negotiated); release();
 setup(true); ap_ies(true); p.ie=sta; p.ie_len=22; p.mfp=NL80211_MFP_REQUIRED;
 p.crypto.akm_suites[0]=WLAN_AKM_SUITE_SAE; p.auth_type=NL80211_AUTHTYPE_SAE;
 assert(connect()==-EOPNOTSUPP && !queued && freed==allocated); /* no external auth owner */
 setup(true); ap_ies(true); p.ie=rsn; p.ie_len=22; p.crypto.wpa_versions=NL80211_WPA_VERSION_3;
 assert(connect()==-EOPNOTSUPP && !allocated); /* WPA3 version only for SAE */
 setup(false); p.auth_type=NL80211_AUTHTYPE_SAE; assert(connect()==-EOPNOTSUPP && !allocated);
 setup(false); p.mfp=NL80211_MFP_REQUIRED; assert(connect()==-EOPNOTSUPP && !allocated);
 /* H2E RSNXE: one short element on secure joins only. */
 const u8 rsnx[]={244,1,0x20};
 setup(true); memcpy(ies,rsn,22); memcpy(ies+22,rsnx,3); p.ie=ies; p.ie_len=25;
 assert(!connect() && r.connection->join.association.ies_length==9+25); release();
 setup(true); memcpy(ies+25,rsnx,3); p.ie=ies; p.ie_len=28; assert(connect()==-EOPNOTSUPP && !allocated);
 setup(true); ies[23]=3; p.ie=ies; p.ie_len=27; ies[23]=1; memcpy(ies,rsn,22);
 u8 long_rsnx[]={244,3,0x20,0,0}; memcpy(ies+22,long_rsnx,5); assert(connect()==-EOPNOTSUPP && !allocated);
 setup(false); p.ie=rsnx; p.ie_len=3; assert(connect()==-EOPNOTSUPP && !allocated);
 /* Supported Operating Classes, as wpa_supplicant 2.10 sends with ap_scan=1. */
 const u8 opclass[]={59,8,0x80,0x51,0x52,0x73,0x76,0x79,0x7c,0x7d};
 u8 sae_ies[64]; memcpy(sae_ies,sta,22); memcpy(sae_ies+22,opclass,10); memcpy(sae_ies+32,rsnx,3);
 setup(true); ap_ies(true); p.ie=sae_ies; p.ie_len=35; p.mfp=NL80211_MFP_REQUIRED;
 p.crypto.akm_suites[0]=WLAN_AKM_SUITE_SAE; p.auth_type=NL80211_AUTHTYPE_SAE;
 p.flags=CONNECT_REQ_EXTERNAL_AUTH_SUPPORT;
 assert(!connect() && r.connection->sae && r.connection->join.association.ies_length==9+35); release();
 const u8 empty_opclass[]={59,0}; memcpy(sae_ies+22,empty_opclass,2);
 setup(true); p.ie=sae_ies; p.ie_len=24; assert(connect()==-EINVAL && !allocated);
 const u8 vendor[]={221,4,0,0x50,0xf2,2}; memcpy(sae_ies+22,vendor,6);
 setup(true); p.ie=sae_ies; p.ie_len=28; assert(connect()==-EOPNOTSUPP && !allocated);
 ap_ies(false);
}
int main(void) {
 /* NetworkManager/wpa_supplicant 2.11 sends allowed PSK + PSK-SHA256
  * selectors, while the station's RSN IE selects exactly one of them.
  */
 setup(true); p.crypto.n_akm_suites=2;
 p.crypto.akm_suites[1]=WLAN_AKM_SUITE_PSK_SHA256;
 assert(!connect() && queued==1); release();
 setup(true); p.crypto.n_akm_suites=2;
 p.crypto.akm_suites[0]=WLAN_AKM_SUITE_PSK_SHA256;
 p.crypto.akm_suites[1]=WLAN_AKM_SUITE_PSK;
 assert(!connect() && queued==1); release();
 /* No implicit downgrade: the transmitted RSN selection must be allowed. */
 setup(true); p.crypto.akm_suites[0]=WLAN_AKM_SUITE_PSK_SHA256;
 assert(connect()==-EOPNOTSUPP && !allocated && !queued);
 setup(true); p.crypto.n_akm_suites=2; p.crypto.akm_suites[1]=0x000fac01;
 assert(connect()==-EOPNOTSUPP && !allocated && !queued);
 setup(true); p.crypto.n_akm_suites=0;
 assert(connect()==-EOPNOTSUPP && !allocated && !queued);
 setup(true); p.crypto.n_akm_suites=3;
 assert(connect()==-EOPNOTSUPP && !allocated && !queued);
 setup(true); p.mfp=2; assert(connect()==-EOPNOTSUPP && !queued && !allocated); /* MFP optional */
 setup(true); p.crypto.akm_suites[0]=0x000fac08; assert(connect()==-EOPNOTSUPP && !queued && freed==allocated);
 wpa3_cases();

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
  /* 2.11 fullmac connect always sends this userspace capability flag. */
  setup(secure); p.flags=CONNECT_REQ_MLO_SUPPORT;
  assert(!connect() && queued==1 && allocated==1); release();
  setup(secure); p.flags=CONNECT_REQ_MLO_SUPPORT | ASSOC_REQ_DISABLE_HT |
   ASSOC_REQ_DISABLE_VHT | ASSOC_REQ_DISABLE_HE | ASSOC_REQ_DISABLE_EHT | ASSOC_REQ_DISABLE_UHR;
  assert(!connect() && queued==1); release();
  setup(secure); p.flags=CONNECT_REQ_MLO_SUPPORT | ASSOC_REQ_USE_RRM;
  assert(connect()==-EOPNOTSUPP && !allocated);
  setup(secure); p.flags=CONNECT_REQ_MLO_SUPPORT | ASSOC_REQ_SPP_AMSDU;
  assert(connect()==-EOPNOTSUPP && !allocated);
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
 /* A cached infrastructure AP is permitted on passive/DFS channels. */
 for(unsigned secure=0;secure<2;secure++) {
  setup(secure); channel.flags=IEEE80211_CHAN_NO_IR;
  assert(!connect() && queued==1 && gets==1 && !bss_puts); release();
  setup(secure); channel.flags=IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR;
  assert(!connect() && queued==1); release();
  setup(secure); channel.flags=IEEE80211_CHAN_DISABLED;
  assert(connect()==-EOPNOTSUPP && freed==1 && bss_puts==1 && !queued);
  setup(secure); channel.flags=IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR;
  assert(connect()==-EOPNOTSUPP && freed==1 && bss_puts==1 && !queued);
 }
 setup(false); rate_error=-EINVAL; assert(connect()==-EINVAL && freed==1 && bss_puts==1);
 setup(false); bss.ies=NULL; assert(connect()==-ENOENT && freed==1 && bss_puts==1);
 setup(false); r.stopping=true; assert(connect()==-ESHUTDOWN && !queued && freed==1 && bss_puts==1);
 setup(false); r.request=(void *)1; assert(connect()==-EBUSY && !queued && freed==1 && bss_puts==1);
 printf("PASS: production connect admission: native kernel flag enums, userspace MLO capability on a single-link station, cached passive/DFS BSS with disabled-channel rejection, modern NM multi-AKM lists preserve the station RSN selection, open/WPA2 construction, WPA2+PMF, WPA3-SAE external auth (2.10/2.11 versions), RSNXE, Supported Operating Classes, control-port support, crypto/IE bounds, MFP/RSN rejection, cached-BSS errors and all allocation/reference cleanup paths\n");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-radio-connect-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
