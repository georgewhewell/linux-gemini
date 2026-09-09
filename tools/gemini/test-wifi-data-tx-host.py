#!/usr/bin/env python3
"""Production Ethernet descriptor codec; synthetic frames and vendor rate mode.
No firmware queue credit, transport, encryption or over-the-air validation.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile
base = runpy.run_path(str(Path(__file__).with_name('test-wifi-assoc-host.py')))
driver = base['driver']
program = base['program'].split('#ifndef __MT6797_WIFI_PROTOCOL_H')[0]
program += r'''
#define ETH_HLEN 14
#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806
#define ETH_P_PAE 0x888e
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
static u16 get_unaligned_be16(const void *p) { const u8 *b=p; return (u16)b[0]<<8|b[1]; }
'''
for name in ['protocol.h', 'protocol.c']:
    program += re.sub(r'^#include .*\n', '', (driver / name).read_text(), flags=re.M)
program += r'''
int main(void) {
 u8 frame[15]={0}, packet[44]; size_t length=0;
 struct mt6797_wifi_tx_params p={.traffic=MT6797_WIFI_TX_EAPOL,.wlan_index=7};
 frame[12]=0x88; frame[13]=0x8e;
 for(unsigned rate=0;rate<0x4000;rate++) {
  p.fixed_rate=rate; p.own_mac_index=rate%64;
  memset(packet,0xa5,sizeof(packet));
  assert(!mt6797_wifi_tx_build(packet,sizeof(packet),frame,sizeof(frame),&p,&length));
  assert(length==44 && get_unaligned_le16(packet)==43 && packet[43]==0);
  assert(packet[3]==0x88 && packet[4]==7 && packet[7]==p.own_mac_index<<2);
  struct { u8 ucAntID; } vendor_rate={.ucAntID=1};
  HAL_MAC_TX_DESC_SET_FIXED_RATE_MODE_TO_DESC(&vendor_rate);
  assert(packet[24]==vendor_rate.ucAntID && get_unaligned_le16(packet+26)==rate<<2);
  assert(!memcmp(packet+28,frame,sizeof(frame)));
 }
 p.fixed_rate=0x4000;
 assert(mt6797_wifi_tx_build(packet,sizeof(packet),frame,sizeof(frame),&p,&length)==-EINVAL);
 p.fixed_rate=0;
 for(unsigned own=64;own<=255;own++) {
  p.own_mac_index=own;
  assert(mt6797_wifi_tx_build(packet,sizeof(packet),frame,sizeof(frame),&p,&length)==-EINVAL);
 }
 p.own_mac_index=0;
 for(size_t cap=0;cap<44;cap++)
  assert(mt6797_wifi_tx_build(packet,cap,frame,sizeof(frame),&p,&length)==-EMSGSIZE);
 frame[12]=8; frame[13]=6; p.traffic=MT6797_WIFI_TX_ARP;
 assert(!mt6797_wifi_tx_build(packet,sizeof(packet),frame,sizeof(frame),&p,&length));
 assert(packet[3]==0x20 && packet[11]==0x80 && !get_unaligned_le16(packet+26));
 assert(packet[6]==0); /* Non-QoS ARP has TID0, including on open links. */
 p.protected=true;
 assert(!mt6797_wifi_tx_build(packet,sizeof(packet),frame,sizeof(frame),&p,&length));
 assert(packet[6]==0x80); /* CCMP protection must not introduce a QoS TID. */
 frame[13]=0; p.traffic=MT6797_WIFI_TX_DATA; p.protected=true;
 assert(!mt6797_wifi_tx_build(packet,sizeof(packet),frame,sizeof(frame),&p,&length));
 assert(packet[6]==0x80 && packet[11]==0);
 puts("PASS: Ethernet TX codec all 16384 fixed-rate codes including 1 Mbps, own-MAC bounds, vendor descriptor-rate mode, queues, protection and aligned capacity/padding");
}
'''
if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='gemini-data-tx-') as temporary:
        path = Path(temporary)
        (path / 'test.c').write_text(program)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
