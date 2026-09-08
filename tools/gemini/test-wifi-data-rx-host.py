#!/usr/bin/env python3
"""Production native/translated RX decoder and PN checks, synthetic vendor-layout frames.
No firmware behavior or live encrypted traffic is claimed.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile
base = runpy.run_path(str(Path(__file__).with_name('test-wifi-data-tx-host.py')))
driver = base['driver']
program = base['program'].split('int main(void) {')[0]
ieee = (driver.parents[4] / 'include/linux/ieee80211.h').read_text()
for name in ['IEEE80211_FTYPE_DATA', 'IEEE80211_STYPE_DATA', 'IEEE80211_STYPE_QOS_DATA',
             'IEEE80211_QOS_CTL_A_MSDU_PRESENT', 'IEEE80211_QOS_CTL_TID_MASK']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', ieee, re.M)[0] + '\n'
program += r'''
#define ETH_P_802_3_MIN 0x0600
#define ETH_P_AARP 0x80f3
#define ETH_P_IPX 0x8137
static bool is_multicast_ether_addr(const u8 *a) { return a[0]&1; }
'''
for name in ['data_rx.h', 'data_rx.c']:
    program += re.sub(r'^#include .*\n', '', (driver / name).read_text(), flags=re.M)
program += r'''
static u8 packet[160];
static size_t setup(bool protected,bool eapol) {
 memset(packet,0,sizeof(packet));
 size_t offset=protected?48:32, length=offset+18;
 put_unaligned_le16(length,packet);
 put_unaligned_le16(0x4000|(1<<12)|(protected?1<<9:0),packet+2);
 packet[6]=0x80|14; packet[8]=3; packet[9]=protected?0x40:0;
 put_unaligned_le16(0xc000,packet+10); /* Inverse no-AMPDU flags. */
 put_unaligned_le16(protected?0x4208:0x0208,packet+16);
 const u8 ap[]={2,1,2,3,4,5}, own[]={2,6,7,8,9,10};
 memcpy(packet+18,ap,6);
 memcpy(packet+offset,own,6); memcpy(packet+offset+6,ap,6);
 packet[offset+12]=eapol?0x88:0x08; packet[offset+13]=eapol?0x8e:0;
 if(protected) packet[32]=1;
 return length;
}
static size_t native(bool protected, bool eapol, bool qos, unsigned groups, bool pad) {
 size_t ethlen=setup(protected,eapol); u8 eth[18];
 memcpy(eth,packet+ethlen-18,18); memset(packet,0,sizeof(packet));
 const unsigned sizes[]={16,8,24,16}; size_t offset=16;
 for(unsigned i=0;i<4;i++) if(groups&(1<<i)) offset+=sizes[i];
 if(pad) offset+=2;
 size_t hdr=qos?26:24, len=offset+hdr+8+4;
 put_unaligned_le16(len,packet); put_unaligned_le16(0x4000|groups<<9|0x101,packet+2);
 packet[6]=hdr|(pad?0x40:0); packet[8]=3; packet[9]=protected?0x40:0;
 put_unaligned_le16(0xc000,packet+10);
 u8 *f=packet+offset;
 put_unaligned_le16(0x0208|(protected?0x4000:0)|(qos?0x80:0),f);
 memcpy(f+4,eth,6); memcpy(f+10,eth+6,6); memcpy(f+16,eth+6,6);
 const u8 snap[]={0xaa,0xaa,3,0,0,0}; memcpy(f+hdr,snap,6);
 memcpy(f+hdr+6,eth+12,6);
 if(groups&1) packet[16+((groups&8)?16:0)]=9;
 return len;
}
static void native_tests(void) {
 struct mt6797_wifi_data_rx d;
 for(unsigned groups=0;groups<16;groups++) for(unsigned q=0;q<2;q++)
 for(unsigned pad=0;pad<2;pad++) for(unsigned enc=0;enc<2;enc++) {
  size_t len=native(enc,!enc,q,groups,pad);
  int ret=mt6797_wifi_data_rx_decode(packet,len,&d);
  assert((ret==0)==(!enc||(groups&1)));
  if(!ret) {
   assert(d.length==18 && d.eapol==!enc && d.protected==enc);
   assert(d.ethernet[0]==2 && d.ethernet[6]==2 && d.ethernet[12]==(enc?8:0x88));
   assert(d.payload==packet+len-4 && d.pn==(enc?9:0));
  }
  for(size_t cap=0;cap<len;cap++) {
   u8 *short_packet=malloc(cap?cap:1); memcpy(short_packet,packet,cap);
   assert(mt6797_wifi_data_rx_decode(short_packet,cap,&d)); free(short_packet);
  }
 }
 size_t len=native(false,true,false,0,false);
 for(unsigned byte=0;byte<6;byte++) {
  packet[40+byte]^=1;
  assert(mt6797_wifi_data_rx_decode(packet,len,&d)==-EOPNOTSUPP);
  packet[40+byte]^=1;
 }
 packet[45]=0xf8; assert(!mt6797_wifi_data_rx_decode(packet,len,&d));
 for(unsigned q=0;q<16;q++) {
  len=native(true,false,true,1,false); packet[9]|=q; packet[32+24]=q;
  assert(!mt6797_wifi_data_rx_decode(packet,len,&d) && d.tid==q);
  packet[32+24]^=1; assert(mt6797_wifi_data_rx_decode(packet,len,&d)==-EPROTO);
 }
 for(unsigned bit=0;bit<14;bit++) {
  len=native(true,false,false,1,false); put_unaligned_le16(0xc000|1<<bit,packet+10);
  assert(mt6797_wifi_data_rx_decode(packet,len,&d));
 }
 len=native(false,true,false,0,false); put_unaligned_le16(0xc004,packet+10);
 assert(!mt6797_wifi_data_rx_decode(packet,len,&d));
 const u16 invalid[]={0x0209,0x0308,0x0008,0x0608,0x8208,0x0248};
 for(unsigned i=0;i<ARRAY_SIZE(invalid);i++) {
  len=native(false,true,false,0,false); put_unaligned_le16(invalid[i],packet+16);
  assert(mt6797_wifi_data_rx_decode(packet,len,&d));
 }
 len=native(false,true,true,0,false); packet[16+24]=0x80;
 assert(mt6797_wifi_data_rx_decode(packet,len,&d)==-EOPNOTSUPP);
 for(unsigned frag=1;frag<16;frag++) {
  len=native(false,true,false,0,false); packet[16+22]=frag;
  assert(mt6797_wifi_data_rx_decode(packet,len,&d)==-EPROTO);
 }
 /* Also shorten the declared native payload, with a complete FIFO item. */
 len=native(false,true,false,0,false);
 for(unsigned n=16;n<len-4;n++) {
  put_unaligned_le16(n,packet); assert(mt6797_wifi_data_rx_decode(packet,n,&d));
 }
}

int main(void) {
 native_tests();
 struct mt6797_wifi_data_rx d;
 size_t length=setup(true,false);
 assert(!mt6797_wifi_data_rx_decode(packet,length,&d));
 assert(d.protected && d.pn==1 && !d.eapol && d.length==18 && d.wlan_index==3);
 for(unsigned byte=0;byte<6;byte++) {
  memset(packet+32,0,16); packet[32+byte]=0x81;
  assert(!mt6797_wifi_data_rx_decode(packet,length,&d));
  assert(d.pn==(u64)0x81<<(8*byte));
 }
 for(size_t cap=0;cap<length;cap++) {
  u8 *short_packet=malloc(cap?cap:1); memcpy(short_packet,packet,cap);
  assert(mt6797_wifi_data_rx_decode(short_packet,cap,&d)); free(short_packet);
 }
 for(unsigned bit=0;bit<14;bit++) {
  setup(true,false); put_unaligned_le16(0xc000|1<<bit,packet+10);
  assert(mt6797_wifi_data_rx_decode(packet,length,&d));
 }
 for(unsigned tid=0;tid<16;tid++) {
  setup(true,false); put_unaligned_le16(0x4288,packet+16); packet[26]=tid; packet[9]|=tid;
  assert(!mt6797_wifi_data_rx_decode(packet,length,&d) && d.tid==tid);
  packet[26]^=1; assert(mt6797_wifi_data_rx_decode(packet,length,&d)==-EPROTO);
 }
 setup(true,false); put_unaligned_le16(0x4288,packet+16); packet[26]=0x80;
 assert(mt6797_wifi_data_rx_decode(packet,length,&d)==-EOPNOTSUPP);
 const u16 invalid[]={0x4209,0x4308,0x4008,0x4608,0xc208,0x4248};
 for(unsigned i=0;i<ARRAY_SIZE(invalid);i++) {
  setup(true,false); put_unaligned_le16(invalid[i],packet+16);
  assert(mt6797_wifi_data_rx_decode(packet,length,&d));
 }
 for(unsigned fragment=1;fragment<16;fragment++) {
  setup(true,false); packet[24]=fragment; assert(mt6797_wifi_data_rx_decode(packet,length,&d));
 }
 for(unsigned cipher=0;cipher<16;cipher++) {
  setup(true,false); packet[9]=cipher<<4;
  assert((mt6797_wifi_data_rx_decode(packet,length,&d)==0)==(cipher==4));
 }
 for(unsigned type=0;type<8;type++) {
  setup(true,false); put_unaligned_le16(type<<13|0x1200,packet+2);
  assert((mt6797_wifi_data_rx_decode(packet,length,&d)==0)==(type==2));
 }
 length=setup(false,true); put_unaligned_le16(0xc004,packet+10);
 assert(!mt6797_wifi_data_rx_decode(packet,length,&d) && d.eapol && !d.protected);
 packet[44]=8; packet[45]=0;
 assert(mt6797_wifi_data_rx_decode(packet,length,&d)==-EBADMSG);
 setup(false,false); packet[9]=0x40;
 assert(mt6797_wifi_data_rx_decode(packet,length,&d)==-EBADMSG);
 u64 last=0;
 assert(mt6797_wifi_data_rx_replay(&last,0)==-EALREADY && last==0);
 assert(!mt6797_wifi_data_rx_replay(&last,1) && last==1);
 assert(mt6797_wifi_data_rx_replay(&last,1)==-EALREADY && last==1);
 assert(!mt6797_wifi_data_rx_replay(&last,0xffffffffffffULL));
 assert(mt6797_wifi_data_rx_replay(&last,2)==-EALREADY && last==0xffffffffffffULL);
 assert(mt6797_wifi_data_rx_replay(&last,0x1000000000000ULL)==-EINVAL);
 puts("PASS: production native/translated RX decoder: all group layouts/padding, SNAP decapsulation, PN byte order, all truncations, data/error/cipher/header/TID checks, plaintext EAPOL exception and strict 48-bit replay monotonicity");
}
'''
if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='gemini-data-rx-') as temporary:
        path = Path(temporary)
        (path / 'test.c').write_text(program)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
