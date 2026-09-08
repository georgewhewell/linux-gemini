#!/usr/bin/env python3
"""Production RX delivery policy with decoder, skb and network-stack fakes.
Separate data-rx test exercises the actual decoder. No real IRQ concurrency.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile
base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-worker-host.py')))
driver = base['driver']
program = base['program'].split('enum stage {')[0]
program = program.replace('struct net_device { u8 dev_addr[6]; };',
    'struct net_device { u8 dev_addr[6]; unsigned mtu,rx_packets,rx_bytes,rx_dropped; };')
program += re.sub(r'^#include .*\n', '', (driver / 'data_rx.h').read_text(), flags=re.M)
program += r'''
#define dev_warn_ratelimited(...) do {} while(0)
#define BIT(n) (1U << (n))
#define ETH_HLEN 14
#define VLAN_HLEN 4
#define CHECKSUM_NONE 0
#define DEV_STATS_INC(d,n) ((d)->n++)
#define DEV_STATS_ADD(d,n,v) ((d)->n+=(v))
struct sk_buff { u8 data[1600]; size_t length; unsigned protocol,ip_summed; };
static struct mt6797_wifi_data_rx input;
static int decode_error;
static bool allocation_failure;
static unsigned delivered,freed;
static struct sk_buff *netdev_alloc_skb_ip_align(struct net_device *dev,size_t length) {
 (void)dev; assert(length<=1600); return allocation_failure?NULL:calloc(1,sizeof(struct sk_buff));
}
static void skb_put_data(struct sk_buff *skb,const u8 *p,size_t n) { memcpy(skb->data+skb->length,p,n); skb->length+=n; }
static unsigned eth_type_trans(struct sk_buff *skb,struct net_device *dev) { (void)dev; return (unsigned)skb->data[12]<<8|skb->data[13]; }
static void netif_rx(struct sk_buff *skb) {
 assert(skb->length==input.length && !memcmp(skb->data,input.ethernet,ETH_HLEN) && !memcmp(skb->data+ETH_HLEN,input.payload,input.length-ETH_HLEN));
 assert(skb->ip_summed==CHECKSUM_NONE); delivered++; free(skb);
}
static void dev_kfree_skb_any(struct sk_buff *skb) { freed++; free(skb); }
int mt6797_wifi_data_rx_decode(const u8 *packet,size_t available,struct mt6797_wifi_data_rx *out) {
 (void)packet; (void)available; if(decode_error) return decode_error; *out=input; return 0;
}
'''
src = (driver / 'data_rx.c').read_text()
program += src[src.index('int mt6797_wifi_data_rx_replay('):src.index('int mt6797_wifi_data_rx_decode(')]
program += base['function']('mt6797_radio_data_frame')
program += r'''
static struct mt6797_wifi_radio r;
static struct mt6797_radio_connection c;
static struct net_device dev={.dev_addr={2,6,7,8,9,10},.mtu=1500};
static struct cfg80211_bss bss={.bssid={2,1,2,3,4,5}};
static u8 ethernet[18];
static void setup(void) {
 memset(&r,0,sizeof(r)); memset(&c,0,sizeof(c)); memset(&input,0,sizeof(input));
 memset(ethernet,0,sizeof(ethernet)); memcpy(ethernet,dev.dev_addr,6); memcpy(ethernet+6,bss.bssid,6); ethernet[12]=8;
 r.netdev=&dev; r.connection=&c; c.bss=&bss; c.connected=c.authorized=c.pairwise_ready=true;
 c.join.negotiated.security=MT6797_WIFI_AIS_SECURITY_WPA2_PSK_CCMP;
 c.ais.runtime.sta.pairwise.index=3; c.ais.runtime.sta.ap_bmc_group.index=4;
 c.pairwise_key.installed=c.group_key.installed=true; c.group_key.index=1;
 memcpy(input.ethernet,ethernet,ETH_HLEN); input.payload=ethernet+ETH_HLEN; input.length=sizeof(ethernet); memcpy(input.transmitter,bss.bssid,6);
 input.wlan_index=3; input.protected=true; input.pn=1;
 decode_error=0; allocation_failure=false; delivered=freed=0;
 dev.rx_packets=dev.rx_bytes=dev.rx_dropped=0;
}
static void rx(bool accept) {
 unsigned before=delivered, drops=dev.rx_dropped;
 memcpy(input.ethernet,ethernet,ETH_HLEN);
 struct mt6797_wifi_rx_view view={0};
 assert(!mt6797_radio_data_frame(&r,&view));
 assert(!r.lock && delivered==before+accept && dev.rx_dropped==drops+!accept);
}
int main(void) {
 setup(); rx(true); assert(c.pairwise_key.rx_pn[0]==1 && dev.rx_bytes==18);
 rx(false); assert(c.pairwise_key.rx_pn[0]==1);
 input.pn=2; rx(true);
 input.tid=1; input.pn=1; rx(true); assert(c.pairwise_key.rx_pn[0]==2 && c.pairwise_key.rx_pn[1]==1);
 for(unsigned which=0;which<12;which++) {
  setup();
  switch(which) {
  case 0: r.connection=NULL; break;
  case 1: c.cancel=true; break;
  case 2: c.connected=false; break;
  case 3: r.stopping=true; break;
  case 4: input.transmitter[1]^=1; break;
  case 5: input.bss_index=1; break;
  case 6: ethernet[1]^=1; break;
  case 7: input.wlan_index=4; break;
  case 8: input.key_index=1; break;
  case 9: c.pairwise_key.installed=false; break;
  case 10: c.pairwise_key.updating=true; break;
  case 11: c.pairwise_ready=false; break;
  }
  rx(false); assert(c.pairwise_key.rx_pn[0]==0);
 }
 setup(); c.authorized=false; rx(false); assert(!c.pairwise_key.rx_pn[0]);
 input.eapol=true; input.protected=false; ethernet[12]=0x88; ethernet[13]=0x8e;
 rx(true); assert(!c.pairwise_key.rx_pn[0]);
 ethernet[7]^=1; rx(false); /* EAPOL bypass requires AP source. */
 setup(); input.protected=false; rx(false); /* WPA2 never admits plaintext ordinary data. */
 c.join.negotiated.security=MT6797_WIFI_AIS_SECURITY_OPEN; rx(true);
 input.protected=true; rx(false);
 setup(); memset(ethernet,255,6); input.multicast=true; input.wlan_index=4; input.key_index=1;
 rx(true); assert(c.group_key.rx_pn[0]==1 && !c.pairwise_key.rx_pn[0]);
 input.pn=2; c.group_key.updating=true; rx(false); assert(c.group_key.rx_pn[0]==1);
 c.group_key.updating=false; input.eapol=true; rx(false);
 setup(); allocation_failure=true; rx(false); assert(!c.pairwise_key.rx_pn[0]);
 setup(); decode_error=-EPROTO; rx(false); assert(!freed);
 setup(); input.length=1600; rx(false); assert(!freed);
 puts("PASS: production RX delivery policy: AP/BSS/WTBL/destination ownership, closed-port EAPOL, plaintext rejection, key-transition gating, per-TID/group replay, allocation and delivery accounting");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-radio-data-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
