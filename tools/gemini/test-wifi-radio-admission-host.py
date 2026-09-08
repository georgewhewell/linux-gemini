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
uapi = (root / 'include/uapi/linux/nl80211.h').read_text()
flags = re.search(r'enum nl80211_scan_flags \{.*?\n\};', uapi, re.S)[0]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#define NL80211_BAND_5GHZ 1
#define IEEE80211_CHAN_DISABLED 1
struct channel { unsigned band, flags; };
struct cfg80211_scan_request {
 unsigned n_ssids, ie_len, n_channels; uint32_t flags;
 struct channel **channels;
};
struct mt6797_wifi_radio {
 int lock, scan_work; bool stopping, abort_scan;
 struct cfg80211_scan_request *request;
 void *connection;
};
struct wiphy { struct mt6797_wifi_radio *radio; };
static struct mt6797_wifi_radio *wiphy_priv(struct wiphy *w) { return w->radio; }
#define spin_lock_irqsave(l,f) do { (void)(l); (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(l); (void)(f); } while(0)
static unsigned queued;
#define system_long_wq 0
static void queue_work(int wq, int *work) { (void)wq; (void)work; queued++; }
'''
program += flags + '\n' + callback
program += r'''
static void check(unsigned flags, unsigned ssids, unsigned ies, unsigned count,
                  unsigned band, unsigned channel_flags, bool stopping,
                  unsigned busy, int expected) {
 struct mt6797_wifi_radio radio={ .stopping=stopping, .abort_scan=true };
 struct wiphy wiphy={ .radio=&radio };
 struct channel channel={ .band=band, .flags=channel_flags };
 struct channel *channels[]={&channel};
 struct cfg80211_scan_request request={ .n_ssids=ssids, .ie_len=ies,
  .n_channels=count, .flags=flags, .channels=channels };
 if (busy==1) radio.request=&request;
 if (busy==2) radio.connection=(void *)1;
 queued=0;
 int result=mt6797_radio_scan(&wiphy,&request);
 if(result!=expected) {
  fprintf(stderr,"flags=%#x result=%d expected=%d\n",flags,result,expected);
 }
 assert(result==expected);
 assert(queued==(expected==0));
 if(!expected) { assert(radio.request==&request); assert(!radio.abort_scan); }
}
int main(void) {
 check(0,0,0,1,0,0,false,false,0);
 check(NL80211_SCAN_FLAG_COLOCATED_6GHZ,0,0,1,1,0,false,false,0);
 for(unsigned bit=0;bit<32;bit++) {
  uint32_t flag=UINT32_C(1)<<bit;
  if(flag==NL80211_SCAN_FLAG_COLOCATED_6GHZ) continue;
  check(flag|NL80211_SCAN_FLAG_COLOCATED_6GHZ,0,0,1,0,0,false,false,-EOPNOTSUPP);
 }
 check(0,1,0,1,0,0,false,false,-EOPNOTSUPP);
 check(0,0,1,1,0,0,false,false,-EOPNOTSUPP);
 check(0,0,0,0,0,0,false,false,-EINVAL);
 check(0,0,0,1,2,0,false,false,-EINVAL);
 check(0,0,0,1,0,IEEE80211_CHAN_DISABLED,false,false,-EINVAL);
 check(0,0,0,1,0,0,true,false,-ESHUTDOWN);
 check(0,0,0,1,0,0,false,true,-EBUSY);
 check(0,0,0,1,0,0,false,2,-EBUSY);
 puts("PASS: plain/default-iw scans, unsupported flags/SSID/IE, channel bounds, stopping and busy; rejected requests never queued");
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-radio-admission-') as tmp:
    path = Path(tmp)
    (path/'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(path/'test.c'),
                    '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
