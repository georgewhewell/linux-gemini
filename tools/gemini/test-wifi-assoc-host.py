#!/usr/bin/env python3
"""Run production association codecs and owner validation under ASan/UBSan.

Kernel primitive shims only. Does not exercise IRQs, firmware or the radio owner.
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

# Fixed-rate mode uses zero for descriptor, one for register. Import vendor macro.
vendor = Path('/home/grw/src/gemini-linux-kernel-3.18/drivers/misc/mediatek/connectivity/wlan/gen3/include/nic/nic_tx.h').read_text()
rate_macro = re.search(r'#define HAL_MAC_TX_DESC_SET_FIXED_RATE_MODE_TO_DESC[^\n]*\\\n[^\n]*', vendor)[0]
rate_mask = re.search(r'^#define TX_DESC_FIXED_RATE_MODE[^\n]*', vendor, re.M)[0]

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define U8_MAX UINT8_MAX
#define U64_MAX UINT64_MAX
#define ETH_ALEN 6
#define BIT(n) (1UL<<(n))
#define GENMASK(h,l) ((~0UL<<(l)) & (~0UL>>(63-(h))))
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define check_add_overflow(a,b,p) __builtin_add_overflow(a,b,p)
#define DECLARE_BITMAP(name,bits) unsigned long name[((bits)+63)/64]
static bool ether_addr_equal(const u8 *a,const u8 *b) { return !memcmp(a,b,6); }
static bool is_valid_ether_addr(const u8 *a) {
 const u8 zero[6]={0}; return !(a[0]&1) && memcmp(a,zero,6);
}
static u16 get_unaligned_le16(const void *p) {
 const u8 *b=p; return b[0]|(u16)b[1]<<8;
}
static u32 get_unaligned_le32(const void *p) {
 const u8 *b=p; return b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24;
}
static void put_unaligned_le16(u16 v,void *p) {
 u8 *b=p; b[0]=v; b[1]=v>>8;
}
static void put_unaligned_le32(u32 v,void *p) {
 u8 *b=p; for(unsigned i=0;i<4;i++) b[i]=v>>(8*i);
}
'''
program += rate_mask + '\n' + rate_macro + '\n'
# Import the actual kernel's wire constants; do not duplicate their values.
ieee = (root / 'include/linux/ieee80211.h').read_text()
names = ['IEEE80211_STYPE_ASSOC_REQ', 'IEEE80211_STYPE_ASSOC_RESP',
         'IEEE80211_STYPE_AUTH', 'IEEE80211_FTYPE_MGMT', 'IEEE80211_FCTL_ORDER',
         'IEEE80211_FCTL_VERS', 'IEEE80211_FCTL_FTYPE', 'IEEE80211_FCTL_STYPE',
         'IEEE80211_FCTL_TODS', 'IEEE80211_FCTL_FROMDS', 'IEEE80211_FCTL_PROTECTED',
         'IEEE80211_FCTL_MOREFRAGS', 'IEEE80211_SCTL_FRAG',
         'IEEE80211_MAX_AID', 'IEEE80211_MAX_SSID_LEN']
for name in names:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', ieee, re.M)[0] + '\n'
program += re.search(r'enum ieee80211_eid \{.*?\n\};', ieee, re.S)[0] + '\n'
for name in ['protocol.h', 'ais_resources.h', 'ais_protocol.h', 'ais_assoc.h',
             'ais_mgmt_tx.h', 'ais_protocol.c', 'ais_assoc.c', 'ais_mgmt_tx.c']:
    program += source(name) + '\n'

program += r'''
static struct mt6797_wifi_ais_bss_handle bss = {
 .generation=1,.cookie=2,.own_addr={2,1,2,3,4,5},.bss_index=3,
 .bmc={.generation=1,.cookie=3,.owner_cookie=2,.index=0,
       .role=MT6797_WIFI_AIS_WTBL_ROLE_BSS_BMC}
};
static struct mt6797_wifi_ais_sta_handle sta = {
 .generation=1,.cookie=4,.bss_cookie=2,.peer_addr={2,6,7,8,9,10},
 .bss_index=3,.sta_index=0,
 .pairwise={.generation=1,.cookie=5,.owner_cookie=4,.index=1,
            .role=MT6797_WIFI_AIS_WTBL_ROLE_STA_PAIRWISE},
 .ap_bmc_group={.generation=1,.cookie=6,.owner_cookie=4,.index=2,
               .role=MT6797_WIFI_AIS_WTBL_ROLE_AP_STA_BMC_GROUP}
};
/* Synthetic SSID and rates, unrelated to nearby networks. */
static const u8 ies[]={0,4,'t','e','s','t',1,2,0x82,0x84,50,1,0x0c};
static struct mt6797_wifi_ais_assoc_pending pending;
static struct mt6797_wifi_ais_assoc_params params={
 .ies=ies,.ies_length=sizeof(ies),.capability=0x0431,.listen_interval=10
};
static void response(u8 *f,u16 status,u16 aid) {
 memset(f,0,30); f[0]=0x10;
 memcpy(f+4,bss.own_addr,6); memcpy(f+10,sta.peer_addr,6);
 memcpy(f+16,sta.peer_addr,6);
 put_unaligned_le16(0x0431,f+24);
 put_unaligned_le16(status,f+26); put_unaligned_le16(aid,f+28);
}
static struct mt6797_wifi_ais_assoc_response match(const u8 *f,size_t n,
 int expected,enum mt6797_wifi_ais_event_disposition result) {
 struct mt6797_wifi_ais_assoc_response r;
 enum mt6797_wifi_ais_event_disposition d;
 memset(&r,0xa5,sizeof(r));
 assert(mt6797_wifi_ais_assoc_response_match(&pending,f,n,&d,&r)==expected);
 assert(d==result);
 if(d!=MT6797_WIFI_AIS_EVENT_COMPLETE) {
  assert(!r.ies && !r.ies_length && !r.association_id && !r.status_code);
 }
 return r;
}
int main(void) {
 u8 f[MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH+1], copy[sizeof(f)]; size_t n;
 assert(!mt6797_wifi_ais_assoc_request_build(&bss,&sta,&params,
                                           f,sizeof(f),&n,&pending));
 const u8 expected[]={0,0,0,0,2,6,7,8,9,10,2,1,2,3,4,5,
                     2,6,7,8,9,10,0,0,0x31,4,10,0};
 assert(n==sizeof(expected)+sizeof(ies));
 assert(!memcmp(f,expected,sizeof(expected)));
 assert(!memcmp(f+28,ies,sizeof(ies)));
 assert(pending.generation==1 && pending.bss_cookie==2 && pending.sta_cookie==4);
 /* The resulting association frame must traverse the real management TX codec. */
 struct mt6797_wifi_ais_mgmt_pid_owner tx_owner;
 struct mt6797_wifi_ais_mgmt_pid_lease lease;
 struct mt6797_wifi_ais_mgmt_tx_params tx={.sequence_number=7,.fixed_rate=0};
 u8 packet[256]; size_t transfer;
 mt6797_wifi_ais_mgmt_pid_init(&tx_owner);
 assert(!mt6797_wifi_ais_mgmt_pid_open(&tx_owner,&bss,&sta));
 assert(!mt6797_wifi_ais_mgmt_pid_acquire(&tx_owner,&lease));
 assert(!mt6797_wifi_ais_mgmt_tx_build(&tx_owner,&lease,&bss,&sta,&tx,
                                     f,n,packet,sizeof(packet),&transfer));
 assert(get_unaligned_le16(packet)==n+28);
 assert(transfer==((n+28+3)&~3UL));
 assert(packet[4]==sta.pairwise.index && packet[20]==lease.pid);
 assert(packet[8]==0 && !memcmp(packet+28,f,n));
 struct { u8 ucAntID; } vendor_rate={.ucAntID=1};
 HAL_MAC_TX_DESC_SET_FIXED_RATE_MODE_TO_DESC(&vendor_rate);
 assert(packet[24]==vendor_rate.ucAntID);
 assert(!mt6797_wifi_ais_mgmt_pid_release(&tx_owner,&lease));
 assert(!mt6797_wifi_ais_mgmt_pid_close(&tx_owner));
 struct mt6797_wifi_ais_assoc_pending saved=pending;
 memcpy(copy,f,sizeof(f));
 for(size_t cap=0;cap<n;cap++) {
  size_t out=99;
  assert(mt6797_wifi_ais_assoc_request_build(&bss,&sta,&params,
                           f,cap,&out,&pending)==-EMSGSIZE);
  assert(!out && !pending.generation && !memcmp(f,copy,sizeof(f)));
 }
 sta.bss_cookie++;
 assert(mt6797_wifi_ais_assoc_request_build(&bss,&sta,&params,
                        f,sizeof(f),&n,&pending)==-EINVAL);
 sta.bss_cookie--;
 assert(!n && !pending.generation);
 struct mt6797_wifi_ais_assoc_params p=params;
 p.ies_length=SIZE_MAX;
 assert(mt6797_wifi_ais_assoc_request_build(&bss,&sta,&p,
                        f,sizeof(f),&n,&pending)==-EMSGSIZE);
 p=params; p.listen_interval=0;
 assert(mt6797_wifi_ais_assoc_request_build(&bss,&sta,&p,
                        f,sizeof(f),&n,&pending)==-EINVAL);
 const u8 bad[][10]={{0,9},{0,0},{1,1,2},{0,0,1,0},
                    {0,0,1,1,2,0,0},{0,0,1,1,2,1,1,2},
                    {0,0,1,1,2,50,0},{0,0,1,9}};
 const size_t badlen[]={2,2,3,4,7,8,7,4};
 for(unsigned i=0;i<sizeof(badlen)/sizeof(*badlen);i++) {
  p=params; p.ies=bad[i]; p.ies_length=badlen[i];
  assert(mt6797_wifi_ais_assoc_request_build(&bss,&sta,&p,
                        f,sizeof(f),&n,&pending)==-EPROTO);
  assert(!n && !pending.generation);
 }
 pending=saved;
 response(f,0,0xc001);
 struct mt6797_wifi_ais_assoc_response r=match(f,30,0,MT6797_WIFI_AIS_EVENT_COMPLETE);
 assert(r.association_id==1 && r.capability==0x0431 && r.ies==f+30);
 /* Every possible AID: reserved high bits ignored, valid range enforced. */
 for(unsigned aid=0;aid<=UINT16_MAX;aid++) {
  response(f,0,aid); unsigned a=aid&0x3fff;
  match(f,30,a && a<=2007 ? 0:-EPROTO,
        a && a<=2007 ? MT6797_WIFI_AIS_EVENT_COMPLETE:MT6797_WIFI_AIS_EVENT_TERMINAL);
 }
 response(f,17,0xffff);
 r=match(f,30,0,MT6797_WIFI_AIS_EVENT_COMPLETE);
 assert(r.status_code==17 && !r.association_id);
 response(f,0,0xc123);
 for(size_t len=0;len<30;len++) {
  u8 *short_frame=malloc(len?len:1); memcpy(short_frame,f,len);
  match(short_frame,len,len<24?0:-EPROTO,
        len<24?MT6797_WIFI_AIS_EVENT_IGNORED:MT6797_WIFI_AIS_EVENT_TERMINAL);
  free(short_frame);
 }
 for(unsigned address=4;address<=16;address+=6) {
  f[address]^=4; match(f,30,0,MT6797_WIFI_AIS_EVENT_IGNORED); f[address]^=4;
 }
 f[0]=0x30; match(f,30,0,MT6797_WIFI_AIS_EVENT_IGNORED); f[0]=0x10;
 const u16 flags[]={0x100,0x200,0x400,0x4000,0x8000};
 for(unsigned i=0;i<sizeof(flags)/sizeof(*flags);i++) {
  put_unaligned_le16(0x10|flags[i],f);
  match(f,30,-EPROTO,MT6797_WIFI_AIS_EVENT_TERMINAL);
 }
 response(f,0,0xc123); f[22]=1;
 match(f,30,-EPROTO,MT6797_WIFI_AIS_EVENT_TERMINAL); f[22]=0;
 memcpy(f+30,ies,sizeof(ies));
 r=match(f,30+sizeof(ies),0,MT6797_WIFI_AIS_EVENT_COMPLETE);
 assert(r.ies_length==sizeof(ies) && !memcmp(r.ies,ies,sizeof(ies)));
 f[30+sizeof(ies)]=0;
 match(f,31+sizeof(ies),-EPROTO,MT6797_WIFI_AIS_EVENT_TERMINAL);
 match(f,sizeof(f),-EPROTO,MT6797_WIFI_AIS_EVENT_TERMINAL);
 /* Deterministic malformed IE streams with exact allocations and ASan redzones. */
 uint32_t random=1;
 for(unsigned trial=0;trial<10000;trial++) {
  size_t len=30+trial%257; u8 *packet=malloc(len);
  response(packet,0,0xc001);
  for(size_t j=30;j<len;j++) { random=random*1664525+1013904223; packet[j]=random>>24; }
  enum mt6797_wifi_ais_event_disposition d;
  int ret=mt6797_wifi_ais_assoc_response_match(&pending,packet,len,&d,&r);
  assert((!ret && d==MT6797_WIFI_AIS_EVENT_COMPLETE) ||
         (ret==-EPROTO && d==MT6797_WIFI_AIS_EVENT_TERMINAL));
  free(packet);
 }
 puts("PASS: association request bytes and management TX path, ownership, capacities, IE validation, 65536 AIDs, rejection, address/type matching, truncation/fragmentation and 10000 malformed IE streams");
}
'''
def run_program(program):
    with tempfile.TemporaryDirectory(prefix='gemini-assoc-') as temporary:
        path = Path(temporary)
        (path / 'test.c').write_text(program)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-ffunction-sections', '-Wl,--gc-sections',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)


if __name__ == "__main__":
    run_program(program)
