#!/usr/bin/env python3
"""Production translated RX decoder and PN checks, synthetic vendor-layout frames.
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
static bool is_multicast_ether_addr(const u8 *a) { return a[0]&1; }
'''
for name in ['data_rx.h', 'data_rx.c']:
    program += re.sub(r'^#include .*\n', '', (driver / name).read_text(), flags=re.M)
program += r'''
static u8 packet[96];
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
int main(void) {
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
 puts("PASS: production RX decoder: PN byte order, all truncations, data/error/cipher/header/TID checks, plaintext EAPOL exception and strict 48-bit replay monotonicity");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-data-rx-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
