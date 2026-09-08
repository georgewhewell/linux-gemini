#!/usr/bin/env python3
"""Exercise production link/cancellation callbacks with host primitive shims.
No real workqueue, IRQ concurrency, firmware or cfg80211 event delivery tested.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
driver = root / 'drivers/net/wireless/mediatek/mt6797'
radio = (driver / 'radio.c').read_text()
ieee = (root / 'include/linux/ieee80211.h').read_text()

def function(name):
    start = radio.index(name)
    start = radio.rfind('\nstatic ', 0, start) + 1
    end = radio.index('{', start) + 1
    depth = 1
    while depth:
        depth += (radio[end] == '{') - (radio[end] == '}')
        end += 1
    return radio[start:end] + '\n'

program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
struct completion { bool done; };
static void complete(struct completion *c) { c->done=true; }
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
static u16 get_unaligned_le16(const void *p) {
 const u8 *b=p; return b[0]|(u16)b[1]<<8;
}
static bool ether_addr_equal(const u8 *a,const u8 *b) { return !memcmp(a,b,6); }
static bool is_broadcast_ether_addr(const u8 *a) {
 const u8 broadcast[6]={255,255,255,255,255,255}; return ether_addr_equal(a,broadcast);
}
struct net_device { u8 dev_addr[6]; };
struct cfg80211_bss { u8 bssid[6]; };
struct mt6797_radio_connection {
 struct cfg80211_bss *bss; bool cancel,peer_disconnect; u16 reason;
 bool key_waiting; int key_error; struct completion key_completion;
};
struct mt6797_wifi_radio {
 int lock,connection_work; bool stopping;
 struct net_device *netdev;
 struct mt6797_radio_connection *connection;
};
struct wiphy { struct mt6797_wifi_radio *radio; };
static struct mt6797_wifi_radio *wiphy_priv(struct wiphy *w) { return w->radio; }
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=1; (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(*(l)); *(l)=0; } while(0)
#define system_long_wq 0
#define WLAN_REASON_UNSPECIFIED 1
static unsigned queued;
static void queue_work(int queue,int *work) { (void)queue; (void)work; queued++; }
'''
program += re.sub(r'^#include .*\n', '', (driver / 'protocol.h').read_text(), flags=re.M)
for name in ['IEEE80211_FCTL_VERS', 'IEEE80211_FCTL_FTYPE', 'IEEE80211_FCTL_STYPE',
             'IEEE80211_FCTL_TODS', 'IEEE80211_FCTL_FROMDS', 'IEEE80211_FCTL_MOREFRAGS',
             'IEEE80211_FCTL_PROTECTED', 'IEEE80211_FCTL_ORDER', 'IEEE80211_SCTL_FRAG',
             'IEEE80211_STYPE_DEAUTH', 'IEEE80211_STYPE_DISASSOC']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', ieee, re.M)[0] + '\n'
program += '''
static unsigned data_calls;
static int mt6797_radio_data_frame(struct mt6797_wifi_radio *r, const struct mt6797_wifi_rx_view *v) { (void)r; (void)v; data_calls++; return 0; }
'''
for name in ['mt6797_radio_connection_cancelled', 'mt6797_radio_link_abort',
             'mt6797_radio_link_frame', 'mt6797_radio_disconnect']:
    program += function(name)
program += r'''
static struct net_device netdev={.dev_addr={2,1,2,3,4,5}};
static struct cfg80211_bss bss={.bssid={2,6,7,8,9,10}};
static struct mt6797_radio_connection connection={.bss=&bss};
static struct mt6797_wifi_radio r={.netdev=&netdev,.connection=&connection};
static void frame(u8 *p,u16 control) {
 memset(p,0,26); p[0]=control; p[1]=control>>8;
 memcpy(p+4,netdev.dev_addr,6); memcpy(p+10,bss.bssid,6); memcpy(p+16,bss.bssid,6);
 p[24]=7;
}
static void check(const u8 *p,size_t length,bool translated,unsigned header,bool expected) {
 struct mt6797_wifi_rx_view v={.payload=p,.payload_length=length,
  .kind=MT6797_WIFI_RX_IEEE80211,.header_length=header,.header_translated=translated};
 queued=0; connection.cancel=false; connection.peer_disconnect=false; connection.reason=0;
 assert(!mt6797_radio_link_frame(&r,&v));
 assert(queued==expected && connection.cancel==expected && connection.peer_disconnect==expected);
 assert(connection.reason==(expected?7:0)); assert(!r.lock);
}
int main(void) {
 u8 p[26];
 struct mt6797_wifi_rx_view data_view={.kind=MT6797_WIFI_RX_ETHERNET};
 assert(!mt6797_radio_link_frame(&r,&data_view) && data_calls==1);
 data_view.kind=MT6797_WIFI_RX_NATIVE_DATA;
 assert(!mt6797_radio_link_frame(&r,&data_view) && data_calls==2);
 frame(p,0xc0); check(p,26,false,24,true);
 frame(p,0xa0); check(p,26,false,24,true);
 for(unsigned type=0;type<2;type++) {
  u16 subtype=type?0xa0:0xc0;
  const u16 invalid[]={1,2,3,0x100,0x200,0x400,0x4000,0x8000};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
   frame(p,subtype|invalid[i]); check(p,26,false,24,false);
  }
  frame(p,subtype|0x800); check(p,26,false,24,true); /* Retry remains valid. */
 }
 frame(p,0xc0);
 for(unsigned fragment=1;fragment<=15;fragment++) { p[22]=fragment; check(p,26,false,24,false); }
 p[22]=0;
 for(size_t len=0;len<26;len++) {
  u8 *short_frame=malloc(len?len:1); memcpy(short_frame,p,len);
  check(short_frame,len,false,24,false); free(short_frame);
 }
 check(NULL,26,false,24,false);
 check(p,26,true,24,false); check(p,26,false,28,false);
 for(unsigned address=4;address<=16;address+=6) {
  p[address]^=4; check(p,26,false,24,false); p[address]^=4;
 }
 memset(p+4,255,6); check(p,26,false,24,true);
 frame(p,0x80); check(p,26,false,24,false); /* Beacon. */
 r.connection=NULL; check(p,26,false,24,false);
 queued=0; mt6797_radio_link_abort(&r,-5); assert(!queued);
 struct wiphy w={.radio=&r};
 assert(!mt6797_radio_disconnect(&w,&netdev,8) && !queued);
 assert(mt6797_radio_connection_cancelled(&r));
 r.connection=&connection; connection.cancel=false;
 assert(!mt6797_radio_connection_cancelled(&r));
 r.stopping=true; assert(mt6797_radio_connection_cancelled(&r)); r.stopping=false;
 queued=0; assert(!mt6797_radio_disconnect(&w,&netdev,8));
 assert(queued==1 && connection.cancel && connection.reason==8);
 assert(mt6797_radio_connection_cancelled(&r));
 queued=0; connection.cancel=false; mt6797_radio_link_abort(&r,-5);
 assert(queued==1 && connection.cancel && connection.reason==1);
 puts("PASS: real link/cancel callbacks; AP unicast/broadcast/retry disconnects, malformed controls/fragments/truncations/addresses, no connection, user cancellation and HIF abort");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-radio-link-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
