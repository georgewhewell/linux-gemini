#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual cfg80211 station callbacks with host locking/clock shims."""
from pathlib import Path
import os
import runpy
import shlex
import subprocess
import tempfile

base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-worker-host.py')))
program = base['program'].split('enum stage {')[0]
program = program.replace('static ktime_t ktime_get(void) { return 100; }',
    'static ktime_t now_ms=100000; static ktime_t ktime_get(void) { return now_ms; }')
program += r'''
#define BIT(n) (1U << (n))
#define BIT_ULL(n) (1ULL << (n))
#define NSEC_PER_SEC 1000000000ULL
enum { NL80211_STA_INFO_CONNECTED_TIME, NL80211_STA_INFO_RX_BYTES64,
       NL80211_STA_INFO_RX_PACKETS, NL80211_STA_INFO_STA_FLAGS,
       NL80211_STA_INFO_SIGNAL };
enum { NL80211_STA_FLAG_AUTHENTICATED, NL80211_STA_FLAG_ASSOCIATED,
       NL80211_STA_FLAG_AUTHORIZED };
struct station_info {
 u64 filled,rx_bytes; u32 connected_time,rx_packets;
 struct { u32 mask,set; } sta_flags;
 s8 signal;
};
static struct mt6797_wifi_radio r;
static void *wiphy_priv(struct wiphy *w) { (void)w; return &r; }
static void ether_addr_copy(u8 *dst,const u8 *src) { memcpy(dst,src,6); }
static u64 div_u64(u64 n,u32 d) { return n/d; }
'''
for name in ('mt6797_radio_station_info', 'mt6797_radio_get_station',
             'mt6797_radio_dump_station'):
    program += base['function'](name)
program += r'''
int main(void) {
 struct cfg80211_bss bss={.bssid={2,1,2,3,4,5}};
 struct mt6797_radio_connection c={.bss=&bss,.connected=true,
     .connected_at_ns=95000000000ULL,.rx_bytes=0x100000123ULL,.rx_packets=456};
 struct station_info info={0};
 struct wiphy w;
 struct wireless_dev other={0};
 u8 mac[6],wrong[6]={2,1,2,3,4,6};
 r.connection=&c;
 assert(!mt6797_radio_get_station(&w,&r.wdev,bss.bssid,&info));
 assert(!r.lock && info.connected_time==5 && info.rx_bytes==0x100000123ULL);
 assert(info.rx_packets==456 && (info.filled & BIT_ULL(NL80211_STA_INFO_RX_BYTES64)));
 assert(!(info.filled & BIT_ULL(NL80211_STA_INFO_SIGNAL)));
 assert(info.sta_flags.mask==(BIT(NL80211_STA_FLAG_AUTHENTICATED)|
     BIT(NL80211_STA_FLAG_ASSOCIATED)|BIT(NL80211_STA_FLAG_AUTHORIZED)));
 assert(!(info.sta_flags.set & BIT(NL80211_STA_FLAG_AUTHORIZED)));
 c.authorized=true; c.signal_valid=true; c.signal=-67; c.signal_at_ns=98000000000ULL;
 memset(&info,0,sizeof(info));
 assert(!mt6797_radio_dump_station(&w,&r.wdev,0,mac,&info));
 assert(!r.lock && ether_addr_equal(mac,bss.bssid));
 assert(info.signal==-67 && (info.filled & BIT_ULL(NL80211_STA_INFO_SIGNAL)));
 assert(info.sta_flags.set==info.sta_flags.mask);
 now_ms=103001; memset(&info,0,sizeof(info));
 assert(!mt6797_radio_get_station(&w,&r.wdev,bss.bssid,&info));
 assert(info.connected_time==8 && !(info.filled & BIT_ULL(NL80211_STA_INFO_SIGNAL)));
 memset(&info,0,sizeof(info));
 assert(mt6797_radio_get_station(&w,&r.wdev,wrong,&info)==-ENOENT && !info.filled);
 assert(mt6797_radio_get_station(&w,&other,bss.bssid,&info)==-ENODEV && !info.filled);
 assert(mt6797_radio_dump_station(&w,&other,0,mac,&info)==-ENODEV && !info.filled);
 assert(mt6797_radio_dump_station(&w,&r.wdev,1,mac,&info)==-ENOENT && !info.filled);
 assert(mt6797_radio_dump_station(&w,&r.wdev,-1,mac,&info)==-ENOENT && !info.filled);
 for(unsigned state=0;state<4;state++) {
  r.connection=&c; r.stopping=false; c.cancel=false; c.connected=true;
  if(state==0) r.connection=NULL;
  if(state==1) r.stopping=true;
  if(state==2) c.cancel=true;
  if(state==3) c.connected=false;
  assert(mt6797_radio_get_station(&w,&r.wdev,bss.bssid,&info)==-ENOENT);
  assert(mt6797_radio_dump_station(&w,&r.wdev,0,mac,&info)==-ENOENT);
  assert(!r.lock && !info.filled);
 }
 /* A new zero-initialized connection cannot inherit counters or signal. */
 c=(struct mt6797_radio_connection){.bss=&bss,.connected=true,
      .connected_at_ns=ktime_get_boottime_ns()};
 assert(!mt6797_radio_get_station(&w,&r.wdev,bss.bssid,&info));
 assert(!info.rx_bytes && !info.rx_packets && !info.connected_time);
 assert(!(info.filled & BIT_ULL(NL80211_STA_INFO_SIGNAL)));
 puts("PASS: station peer/lifetime checks, 64-bit RX, authorization, age and fresh RSSI");
}
'''
with tempfile.TemporaryDirectory(prefix='wifi-station-') as temp:
    source = Path(temp) / 'station.c'
    binary = Path(temp) / 'station'
    source.write_text(program)
    subprocess.run(shlex.split(os.environ.get('HOSTCC', 'cc')) + [
        '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
