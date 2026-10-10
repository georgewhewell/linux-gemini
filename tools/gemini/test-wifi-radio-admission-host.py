#!/usr/bin/env python3
"""Test the production cfg80211 scan admission callback with host-only shims.
No worker, IRQ or hardware is exercised; full scan behavior needs device tests.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/net/wireless/mediatek/mt6797/radio.c').read_text()
start = source.index('static int mt6797_radio_scan(')
end = source.index('{', start) + 1
level = 1
while level:
    level += (source[end] == '{') - (source[end] == '}')
    end += 1
callback = source[start:end]
start = source.index('static int mt6797_radio_scan_probe(')
end = source.index('\nstatic void mt6797_radio_scan_work(', start)
conversion = source[start:end]
scan = (root / 'drivers/net/wireless/mediatek/mt6797/scan.c').read_text()
start = scan.index('int mt6797_wifi_scan_probe_validate(')
end = scan.index('\nint mt6797_wifi_scan_start_probe(', start)
validation = scan[start:end]
header = (root / 'drivers/net/wireless/mediatek/mt6797/scan.h').read_text()
probe_types = header[header.index('struct mt6797_wifi_scan_ssid {'):
                     header.index('enum mt6797_wifi_scan_state {')]
uapi = (root / 'include/uapi/linux/nl80211.h').read_text()
flags = re.search(r'enum nl80211_scan_flags \{.*?\n\};', uapi, re.S)[0]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
#define NL80211_BAND_5GHZ 1
#define IEEE80211_CHAN_DISABLED 1
struct channel { unsigned band, flags; };
struct cfg80211_ssid { unsigned char ssid[32]; unsigned ssid_len; };
struct cfg80211_scan_request {
 unsigned n_ssids, ie_len, n_channels; uint32_t flags;
 struct cfg80211_ssid *ssids;
 struct channel **channels;
 const u8 *ie;
};
struct mt6797_wifi_radio {
 int lock, scan_work; bool stopping, abort_scan, scan_on_link;
 struct cfg80211_scan_request *request;
 struct link { bool connected, cancel; } *connection;
};
struct wiphy { struct mt6797_wifi_radio *radio; };
static struct mt6797_wifi_radio *wiphy_priv(struct wiphy *w) { return w->radio; }
#define spin_lock_irqsave(l,f) do { (void)(l); (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(l); (void)(f); } while(0)
static unsigned queued;
#define system_long_wq 0
static void queue_work(int wq, int *work) { (void)wq; (void)work; queued++; }
'''
for name in ('MT6797_WIFI_SCAN_MAX_SSIDS', 'MT6797_WIFI_SCAN_MAX_SSID_LENGTH',
             'MT6797_WIFI_SCAN_MAX_IE_LENGTH'):
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', header, re.M)[0] + '\n'
program += probe_types + flags + '\n' + validation + conversion + callback
program += r'''
/* Length of the first requested SSID; 0 is the wildcard. */
static unsigned first_ssid_len;
static bool malformed_ie,missing_ie,missing_ssids;
static void check(unsigned flags, unsigned ssids, unsigned ies, unsigned count,
                  unsigned band, unsigned channel_flags, bool stopping,
                  unsigned busy, int expected) {
 struct mt6797_wifi_radio radio={ .stopping=stopping, .abort_scan=true };
 struct wiphy wiphy={ .radio=&radio };
 struct channel channel={ .band=band, .flags=channel_flags };
 struct channel *channels[]={&channel};
 struct cfg80211_ssid ssid_list[4]={ { .ssid_len=first_ssid_len }, { .ssid_len=0 } };
 u8 ie_list[602]={0};
 if(ies>=3 && (ies&1)) ie_list[1]=1;
 if(malformed_ie) ie_list[1]=255;
 struct cfg80211_scan_request request={ .n_ssids=ssids, .ie_len=ies,
  .n_channels=count, .flags=flags, .ssids=missing_ssids?NULL:ssid_list,
  .channels=channels, .ie=missing_ie?NULL:ie_list };
 if (busy==1) radio.request=&request;
 /* 2: joining, 3: associated, 4: associated but being torn down. */
 struct link link={ .connected=busy>=3, .cancel=busy==4 };
 if (busy>=2) radio.connection=&link;
 queued=0;
 int result=mt6797_radio_scan(&wiphy,&request);
 if(result!=expected) {
  fprintf(stderr,"flags=%#x result=%d expected=%d\n",flags,result,expected);
 }
 assert(result==expected);
 assert(queued==(expected==0));
 if(!expected) { assert(radio.request==&request); assert(!radio.abort_scan); assert(radio.scan_on_link==(busy==3)); }
}
int main(void) {
 check(0,0,0,1,0,0,false,false,0);
 check(NL80211_SCAN_FLAG_COLOCATED_6GHZ,0,0,1,1,0,false,false,0);
 for(unsigned bit=0;bit<32;bit++) {
  uint32_t flag=UINT32_C(1)<<bit;
  if(flag==NL80211_SCAN_FLAG_COLOCATED_6GHZ || flag==NL80211_SCAN_FLAG_FLUSH) continue;
  check(flag|NL80211_SCAN_FLAG_COLOCATED_6GHZ,0,0,1,0,0,false,false,-EOPNOTSUPP);
 }
 check(NL80211_SCAN_FLAG_FLUSH,0,0,1,0,0,false,false,0);
 /* Wildcard, directed and mixed requests are supported by GEN3 scan V2. */
 check(0,1,0,1,0,0,false,false,0);
 first_ssid_len=4;
 check(0,1,0,1,0,0,false,false,0);
 check(0,4,600,1,0,0,false,false,0);
 check(0,5,0,1,0,0,false,false,-EINVAL);
 first_ssid_len=33;
 check(0,1,0,1,0,0,false,false,-EINVAL);
 first_ssid_len=0;
 check(0,2,0,1,0,0,false,false,0);
 /* Byte bounds and TLV structure are checked before queuing hardware work. */
 check(0,1,13,1,0,0,false,false,0);
 check(0,1,602,1,0,0,false,false,-EINVAL);
 malformed_ie=true; check(0,1,13,1,0,0,false,false,-EINVAL); malformed_ie=false;
 missing_ie=true; check(0,1,2,1,0,0,false,false,-EINVAL); missing_ie=false;
 missing_ssids=true; check(0,1,0,1,0,0,false,false,-EINVAL); missing_ssids=false;
 check(0,0,0,0,0,0,false,false,-EINVAL);
 check(0,0,0,1,2,0,false,false,-EINVAL);
 check(0,0,0,1,0,IEEE80211_CHAN_DISABLED,false,false,-EINVAL);
 check(0,0,0,1,0,0,true,false,-ESHUTDOWN);
 check(0,0,0,1,0,0,false,true,-EBUSY);
 check(0,0,0,1,0,0,false,2,-EBUSY);
 check(0,0,0,1,0,0,false,4,-EBUSY);
 /* Associated: the scan is admitted and runs through the link sink. */
 check(0,0,0,1,0,0,false,3,0);
 puts("PASS: plain/default-iw/FLUSH and wildcard/directed/four-SSID scan admission, strict SSID/IE bounds, channel bounds, stopping, busy/joining/teardown and associated scans; rejected requests never queued");
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-radio-admission-') as tmp:
    path = Path(tmp)
    (path/'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(path/'test.c'),
                    '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
