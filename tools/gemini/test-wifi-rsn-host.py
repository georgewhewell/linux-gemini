#!/usr/bin/env python3
"""Run the production RSN/PMF/SAE admission codec under ASan/UBSan.

Covers wpa_supplicant 2.10/2.11-shaped RSN elements, AP capability matching,
malformed/truncated elements and PMKID insertion. Kernel primitive shims only;
no cfg80211, firmware or radio is exercised.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
driver = root / 'drivers/net/wireless/mediatek/mt6797'

def source(name):
    return re.sub(r'^#include .*\n', '', (driver / name).read_text(), flags=re.M)

program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
static u16 get_unaligned_le16(const void *p) { const u8 *b=p; return b[0]|(u16)b[1]<<8; }
static void put_unaligned_le16(u16 v,void *p) { u8 *b=p; b[0]=v; b[1]=v>>8; }
static u32 get_unaligned_be32(const void *p) { const u8 *b=p; return (u32)b[0]<<24|(u32)b[1]<<16|(u32)b[2]<<8|b[3]; }
static void put_unaligned_be32(u32 v,void *p) { u8 *b=p; b[0]=v>>24; b[1]=v>>16; b[2]=v>>8; b[3]=v; }
'''
program += source('ais_rsn.h') + source('ais_rsn.c')
program += r'''
#define PSK 0x000fac02u
#define PSK256 0x000fac06u
#define SAE 0x000fac08u
#define MFPC 0x80
#define MFPR 0x40
/* Build an RSN element: version 1, CCMP group, one CCMP pairwise, AKM list,
 * optional capabilities / PMKID list / group management cipher. */
static size_t rsn(u8 *e,const u32 *akms,unsigned n_akm,int caps,int n_pmkid,u32 mgmt) {
 size_t o=2; e[0]=48; put_unaligned_le16(1,e+o); o+=2;
 put_unaligned_be32(0x000fac04,e+o); o+=4;
 put_unaligned_le16(1,e+o); o+=2; put_unaligned_be32(0x000fac04,e+o); o+=4;
 put_unaligned_le16(n_akm,e+o); o+=2;
 for(unsigned i=0;i<n_akm;i++) { put_unaligned_be32(akms[i],e+o); o+=4; }
 if(caps>=0) { put_unaligned_le16(caps,e+o); o+=2; }
 if(n_pmkid>=0) {
  put_unaligned_le16(n_pmkid,e+o); o+=2;
  for(int i=0;i<n_pmkid;i++) { memset(e+o,0x70+i,16); o+=16; }
 }
 if(mgmt) { assert(n_pmkid>=0); put_unaligned_be32(mgmt,e+o); o+=4; }
 e[1]=o-2; return o;
}
static int policy(u32 akm,const u8 *sta,const u8 *ap,bool mfp,bool sae,bool ext,struct mt6797_wifi_ais_rsn_result *r) {
 struct mt6797_wifi_ais_rsn_request q={.sta_ie=sta,.ap_ie=ap,.akm=akm,.mfp_required=mfp,.sae_auth=sae,.external_auth=ext};
 r->pmf=r->sae=true; /* must be cleared on every path */
 return mt6797_wifi_ais_rsn_policy(&q,r);
}
static void parse_bounds(void) {
 u8 e[300]; const u32 a[]={PSK};
 size_t n=rsn(e,a,1,MFPC,1,0x000fac06);
 struct mt6797_wifi_ais_rsn p;
 assert(!mt6797_wifi_ais_rsn_parse(e,&p) && p.n_pmkid==1 && p.has_group_mgmt_cipher && p.capabilities==MFPC);
 assert(p.pmkid_offset==22 && p.pmkid==e+24);
 /* Every truncation that splits a field is rejected; field boundaries parse. */
 for(size_t len=0;len+2<n;len++) {
  u8 t[300]; memcpy(t,e,n); t[1]=len;
  int ret=mt6797_wifi_ais_rsn_parse(t,&p);
  bool boundary=len==2||len==6||len==12||len==18||len==20||len==22+16;
  if(len<2) assert(ret==-EINVAL);
  else assert(boundary ? !ret : ret==-EINVAL);
 }
 u8 t[300]; memcpy(t,e,n); t[1]++; t[n]=0; assert(mt6797_wifi_ais_rsn_parse(t,&p)==-EINVAL); /* trailing byte */
 memcpy(t,e,n); t[2]=2; assert(mt6797_wifi_ais_rsn_parse(t,&p)==-EINVAL); /* version */
 memcpy(t,e,n); t[0]=221; assert(mt6797_wifi_ais_rsn_parse(t,&p)==-EINVAL);
 memcpy(t,e,n); put_unaligned_le16(0,t+8); assert(mt6797_wifi_ais_rsn_parse(t,&p)==-EINVAL); /* zero pairwise */
 memcpy(t,e,n); put_unaligned_le16(17,t+8); assert(mt6797_wifi_ais_rsn_parse(t,&p)==-EINVAL);
 memcpy(t,e,n); put_unaligned_le16(9,t+22); assert(mt6797_wifi_ais_rsn_parse(t,&p)==-EINVAL); /* PMKIDs */
 assert(mt6797_wifi_ais_rsn_parse(NULL,&p)==-EINVAL && mt6797_wifi_ais_rsn_parse(e,NULL)==-EINVAL);
}
static void policy_cases(void) {
 u8 sta[300], ap[300], other[300]; struct mt6797_wifi_ais_rsn_result r;
 const u32 psk[]={PSK}, psk256[]={PSK256}, sae[]={SAE}, mixed[]={PSK,SAE}, saeonly[]={SAE};
 /* Accepted WPA2-PSK without PMF: supplicant 2.10 element, AP ignored. */
 rsn(sta,psk,1,0,-1,0);
 assert(!policy(PSK,sta,NULL,false,false,false,&r) && !r.pmf && !r.sae);
 rsn(sta,psk,1,-1,-1,0); assert(!policy(PSK,sta,NULL,false,false,false,&r) && !r.pmf);
 rsn(sta,psk,1,0,0,0); assert(!policy(PSK,sta,NULL,false,false,false,&r));
 rsn(sta,psk,1,0,1,0); assert(policy(PSK,sta,NULL,false,false,false,&r)==-EOPNOTSUPP);
 /* ieee80211w=1 STA (MFPC) against WPA2-only AP: valid, no PMF. */
 rsn(sta,psk,1,MFPC,-1,0); rsn(ap,psk,1,0,-1,0);
 assert(!policy(PSK,sta,ap,false,false,false,&r) && !r.pmf);
 assert(policy(PSK,sta,NULL,false,false,false,&r)==-EOPNOTSUPP && !r.pmf);
 assert(policy(PSK,sta,ap,true,false,false,&r)==-EOPNOTSUPP); /* MFP required but none */
 /* PMF-capable AP: both MFPC means PMF, whether or not cfg80211 says required. */
 rsn(ap,mixed,2,MFPC,-1,0);
 assert(!policy(PSK,sta,ap,true,false,false,&r) && r.pmf && !r.sae);
 assert(!policy(PSK,sta,ap,false,false,false,&r) && r.pmf);
 rsn(sta,psk,1,MFPC|MFPR,0,0x000fac06); assert(!policy(PSK,sta,ap,true,false,false,&r) && r.pmf);
 rsn(sta,psk,1,MFPC,0,0x000fac0b); assert(policy(PSK,sta,ap,true,false,false,&r)==-EOPNOTSUPP); /* GMAC */
 rsn(sta,psk,1,0,0,0x000fac06); assert(policy(PSK,sta,NULL,false,false,false,&r)==-EOPNOTSUPP);
 rsn(sta,psk,1,MFPR,-1,0); assert(policy(PSK,sta,ap,false,false,false,&r)==-EINVAL);
 /* Required mismatches. */
 rsn(sta,psk,1,MFPC|MFPR,-1,0); rsn(ap,psk,1,0,-1,0);
 assert(policy(PSK,sta,ap,true,false,false,&r)==-EOPNOTSUPP);
 rsn(sta,psk,1,0,-1,0); rsn(ap,psk,1,MFPC|MFPR,-1,0);
 assert(policy(PSK,sta,ap,true,false,false,&r)==-EOPNOTSUPP);
 /* AP's BIP cipher must be CMAC-128 if stated. */
 rsn(sta,psk,1,MFPC,-1,0); rsn(ap,psk,1,MFPC,0,0x000fac0c);
 assert(policy(PSK,sta,ap,false,false,false,&r)==-EOPNOTSUPP);
 rsn(ap,psk,1,MFPC,0,0x000fac06); assert(!policy(PSK,sta,ap,false,false,false,&r) && r.pmf);
 /* PSK-SHA256 needs the AP to offer it. */
 rsn(sta,psk256,1,MFPC,-1,0); assert(policy(PSK256,sta,ap,false,false,false,&r)==-EOPNOTSUPP);
 const u32 apsha[]={PSK,PSK256}; rsn(ap,apsha,2,MFPC,-1,0);
 assert(!policy(PSK256,sta,ap,false,false,false,&r) && r.pmf);
 rsn(sta,psk256,1,0,-1,0); assert(!policy(PSK256,sta,ap,false,false,false,&r) && !r.pmf);
 /* SAE through NL80211_CMD_EXTERNAL_AUTH (WPA3-Personal, transition AP). */
 rsn(sta,sae,1,MFPC|MFPR,-1,0); rsn(ap,mixed,2,MFPC,-1,0);
 assert(!policy(SAE,sta,ap,true,true,true,&r) && r.pmf && r.sae);
 assert(policy(SAE,sta,ap,true,true,false,&r)==-EOPNOTSUPP && !r.sae); /* no external auth */
 assert(policy(SAE,sta,ap,true,false,false,&r)==-EOPNOTSUPP); /* open auth w/o PMKID */
 assert(policy(PSK,sta,ap,true,true,true,&r)==-EOPNOTSUPP); /* AKM mismatch */
 rsn(other,psk,1,MFPC|MFPR,-1,0); assert(policy(PSK,other,ap,true,true,true,&r)==-EOPNOTSUPP);
 rsn(ap,saeonly,1,MFPC|MFPR,-1,0); assert(!policy(SAE,sta,ap,true,true,true,&r) && r.sae);
 rsn(ap,psk,1,MFPC,-1,0); assert(policy(SAE,sta,ap,true,true,true,&r)==-EOPNOTSUPP); /* AP lacks SAE */
 rsn(ap,sae,1,0,-1,0); assert(policy(SAE,sta,ap,true,true,true,&r)==-EOPNOTSUPP); /* SAE w/o PMF */
 rsn(sta,sae,1,0,-1,0); rsn(ap,sae,1,0,-1,0); assert(policy(SAE,sta,ap,false,true,true,&r)==-EOPNOTSUPP);
 /* SAE PMKSA caching: open system with the cached PMKID. */
 rsn(sta,sae,1,MFPC|MFPR,1,0); rsn(ap,mixed,2,MFPC,-1,0);
 assert(!policy(SAE,sta,ap,true,false,false,&r) && r.pmf && !r.sae);
 /* Unsupported ciphers/AKMs and malformed STA elements never pass. */
 const u32 two[]={PSK,SAE}; rsn(sta,two,2,MFPC,-1,0); assert(policy(PSK,sta,ap,false,false,false,&r)==-EOPNOTSUPP);
 rsn(sta,psk,1,0,-1,0); put_unaligned_be32(0x000fac02,sta+4); assert(policy(PSK,sta,NULL,false,false,false,&r)==-EOPNOTSUPP);
 rsn(sta,psk,1,0,-1,0); put_unaligned_be32(0x000fac09,sta+10); assert(policy(PSK,sta,NULL,false,false,false,&r)==-EOPNOTSUPP);
 const u32 ft[]={0x000fac04}; rsn(sta,ft,1,0,-1,0); assert(policy(0x000fac04,sta,NULL,false,false,false,&r)==-EOPNOTSUPP);
 rsn(sta,psk,1,0,-1,0); sta[1]=17; assert(policy(PSK,sta,NULL,false,false,false,&r)==-EINVAL && !r.pmf && !r.sae);
 assert(mt6797_wifi_ais_rsn_policy(NULL,&r)==-EINVAL);
 /* A malformed AP element cannot establish PMF. */
 rsn(sta,psk,1,MFPC,-1,0); rsn(ap,psk,1,MFPC,-1,0); ap[1]++; ap[ap[1]+1]=0;
 assert(policy(PSK,sta,ap,false,false,false,&r)==-EOPNOTSUPP && !r.pmf);
}
static void pmkid_cases(void) {
 const u32 sae[]={SAE}; const u8 id[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
 u8 ies[600], expect[600]; size_t n, m;
 struct mt6797_wifi_ais_rsn p;
 /* SSID + RSN(caps) + RSNXE: PMKID count/list inserted after capabilities. */
 for(int layout=0;layout<5;layout++) {
  size_t o=0; ies[o++]=0; ies[o++]=2; ies[o++]='a'; ies[o++]='b';
  int caps= layout==0 ? -1 : MFPC|MFPR, pm= layout<2 ? -1 : layout==2 ? 0 : 1;
  u32 mgmt= layout==4 ? 0x000fac06 : 0;
  o+=rsn(ies+o,sae,1,caps,pm,mgmt); ies[o++]=244; ies[o++]=1; ies[o++]=0x20;
  n=o; memcpy(expect,ies,4); m=4+rsn(expect+4,sae,1,caps<0?0:caps,1,mgmt);
  memcpy(expect+4+24,id,16); /* PMKID list follows version..capabilities+count */
  expect[m]=244; expect[m+1]=1; expect[m+2]=0x20; m+=3;
  assert(!mt6797_wifi_ais_rsn_set_pmkid(ies,&n,sizeof(ies),id));
  assert(n==m && !memcmp(ies,expect,m));
  assert(!mt6797_wifi_ais_rsn_parse(ies+4,&p) && p.n_pmkid==1 && !memcmp(p.pmkid,id,16));
  assert(p.has_group_mgmt_cipher==(layout==4));
 }
 /* Capacity, missing/duplicate RSN and malformed IE lists leave input unchanged. */
 size_t o=rsn(ies,sae,1,MFPC,-1,0); n=o; memcpy(expect,ies,o);
 assert(mt6797_wifi_ais_rsn_set_pmkid(ies,&n,o+17,id)==-EMSGSIZE && n==o && !memcmp(ies,expect,o));
 assert(!mt6797_wifi_ais_rsn_set_pmkid(ies,&n,o+18,id) && n==o+18);
 o=rsn(ies,sae,1,MFPC,-1,0); o+=rsn(ies+o,sae,1,MFPC,-1,0); n=o;
 assert(mt6797_wifi_ais_rsn_set_pmkid(ies,&n,sizeof(ies),id)==-EINVAL && n==o);
 ies[0]=0; ies[1]=1; ies[2]='x'; n=3; assert(mt6797_wifi_ais_rsn_set_pmkid(ies,&n,sizeof(ies),id)==-EINVAL);
 ies[0]=48; ies[1]=9; n=3; assert(mt6797_wifi_ais_rsn_set_pmkid(ies,&n,sizeof(ies),id)==-EINVAL && n==3);
 n=4; assert(mt6797_wifi_ais_rsn_set_pmkid(ies,&n,3,id)==-EINVAL);
}
int main(void) {
 parse_bounds(); policy_cases(); pmkid_cases();
 puts("PASS: production RSN parser bounds, WPA2/PMF/PSK-SHA256/SAE admission (MFPC/MFPR, BIP cipher, external auth, PMKSA caching) and PMKID insertion");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-wifi-rsn-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                    '-Wno-unused-function', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
