#!/usr/bin/env python3
"""Exercise real target-scan callbacks with damaged and adjacent-channel beacons."""
from pathlib import Path
import os
import re
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
driver = root / 'drivers/net/wireless/mediatek/mt6797'

def source(name):
    return re.sub(r'^#include .*\n', '', (driver / name).read_text(), flags=re.M)

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
typedef uint16_t __le16;
typedef int64_t ktime_t;
typedef int spinlock_t;
#define __packed __attribute__((packed))
#define ETH_ALEN 6
#define ETH_HLEN 14
#define U64_MAX UINT64_MAX
#define BIT(n) (1UL << (n))
#define GENMASK(h,l) ((~0UL << (l)) & (~0UL >> (63-(h))))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define check_add_overflow(a,b,p) __builtin_add_overflow(a,b,p)
struct completion { bool done; };
static void complete(struct completion *c) { c->done = true; }
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=1; (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(*(l)); *(l)=0; } while(0)
static s64 ktime_to_ms(ktime_t t) { return t; }
static u16 get_unaligned_le16(const void *p) {
 const u8 *b=p; return b[0] | (u16)b[1] << 8;
}
static u32 get_unaligned_le32(const void *p) {
 const u8 *b=p; return b[0] | (u32)b[1]<<8 | (u32)b[2]<<16 | (u32)b[3]<<24;
}
static void put_unaligned_le16(u16 v, void *p) {
 u8 *b=p; b[0]=v; b[1]=v>>8;
}
static void put_unaligned_le32(u32 v, void *p) {
 u8 *b=p; for(unsigned i=0;i<4;i++) b[i]=v>>(8*i);
}
static bool ether_addr_equal(const u8 *a,const u8 *b) { return !memcmp(a,b,6); }
static void ether_addr_copy(u8 *a,const u8 *b) { memcpy(a,b,6); }
static bool is_valid_ether_addr(const u8 *a) {
 const u8 zero[6]={0}; return !(a[0]&1) && memcmp(a,zero,6);
}
static const void *memchr_inv(const void *p,int v,size_t n) {
 const u8 *b=p; for(size_t i=0;i<n;i++) if(b[i]!=(u8)v) return b+i; return NULL;
}
static __le16 cpu_to_le16(u16 v) { return v; }
'''
ieee = (root / 'include/linux/ieee80211.h').read_text()
for name in ['IEEE80211_MAX_SSID_LEN', 'IEEE80211_STYPE_BEACON',
             'IEEE80211_STYPE_PROBE_RESP', 'IEEE80211_FCTL_VERS',
             'IEEE80211_FCTL_FTYPE', 'IEEE80211_FCTL_STYPE',
             'IEEE80211_FCTL_TODS', 'IEEE80211_FCTL_FROMDS',
             'IEEE80211_FCTL_PROTECTED', 'WLAN_CAPABILITY_ESS',
             'WLAN_CAPABILITY_IBSS']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', ieee, re.M)[0] + '\n'
program += re.search(r'enum ieee80211_eid \{.*?\n\};', ieee, re.S)[0] + '\n'
ht = (root / 'include/linux/ieee80211-ht.h').read_text()
program += re.search(r'struct ieee80211_ht_operation \{.*?\n\}[^;]*;', ht, re.S)[0] + '\n'
program += r'''
static bool ieee80211_is_beacon(__le16 fc) {
 return (fc & (IEEE80211_FCTL_FTYPE|IEEE80211_FCTL_STYPE)) == IEEE80211_STYPE_BEACON;
}
static bool ieee80211_is_probe_resp(__le16 fc) {
 return (fc & (IEEE80211_FCTL_FTYPE|IEEE80211_FCTL_STYPE)) == IEEE80211_STYPE_PROBE_RESP;
}
'''
for name in ['protocol.h', 'rx_metadata.h', 'scan_target.h', 'scan.h', 'scan_hif.h']:
    program += source(name) + '\n'
protocol = source('protocol.c')
program += protocol[:protocol.index('int mt6797_wifi_tx_build(')]
program += source('rx_metadata.c') + source('scan_target.c') + source('scan.c')
callbacks = source('scan_hif.c')
program += callbacks[callbacks.index('static int mt6797_wifi_scan_hif_claim_terminal('):
                     callbacks.index('static unsigned long mt6797_wifi_scan_hif_timeout(')]
program += r'''
static struct mt6797_wifi_scan_hif scan;
static ktime_t now;
static const u8 target[6]={2,1,2,3,4,5}, foreign[6]={2,6,7,8,9,10};
static ktime_t clock_now(void *context) { assert(context==&scan); return now; }
static void setup(void) {
 memset(&scan,0,sizeof(scan)); now=100;
 scan.backend.now=clock_now; scan.backend.context=&scan;
 scan.expected_band=1; scan.expected_channel=6; scan.armed=scan.dirty=true;
 struct mt6797_wifi_scan_target_key key={.ssid={'t','e','s','t'},.ssid_length=4};
 memcpy(key.bssid,target,6);
 assert(!mt6797_wifi_scan_target_open(&scan.target_owner,&key,2000));
 struct mt6797_wifi_scan_channel channel={.band=1,.number=6};
 u8 command[MT6797_WIFI_SCAN_COMMAND_LENGTH];
 assert(!mt6797_wifi_scan_start(&scan.transaction,&channel,1,3,7,command,sizeof(command)));
}
/* GEN3 software-frame descriptor: group3, 24-byte header, beacon fixed body. */
static size_t packet(u8 *p,const u8 *bssid,u8 channel) {
 memset(p,0,160);
 put_unaligned_le16(85,p); put_unaligned_le16(0xe801,p+2);
 p[5]=channel; p[6]=24; put_unaligned_le32(100<<8,p+24);
 put_unaligned_le16(IEEE80211_STYPE_BEACON,p+40);
 memcpy(p+50,bssid,6); memcpy(p+56,bssid,6);
 put_unaligned_le16(WLAN_CAPABILITY_ESS,p+74);
 const u8 ie[]={0,4,'t','e','s','t',3,1,channel}; memcpy(p+76,ie,sizeof(ie));
 return 85;
}
static int deliver(u8 *p,size_t n) {
 struct mt6797_wifi_rx_view frame;
 assert(!mt6797_wifi_rx_parse(p,n,&frame));
 return mt6797_wifi_scan_hif_frame(&scan,&frame);
}
static void damaged_observations(void) {
 for(unsigned kind=0;kind<6;kind++) {
  setup(); u8 p[160]; size_t n=packet(p,target,6);
  assert(!deliver(p,n));
  u64 cookie=scan.target_owner.selected.cookie;
  now=110; n=packet(p,kind==0?foreign:target,kind==5?11:6);
  switch(kind) {
   case 0: case 1: p[84]=11; break; /* DS channel contradicts received channel. */
   case 2: p[77]=255; break; /* Truncated SSID IE. */
   case 3: put_unaligned_le32(221<<8,p+24); break; /* Unusable RCPI. */
   case 4: p[56]=1; break; /* Invalid multicast BSSID. */
   case 5: break; /* Valid same-BSSID frame from another channel. */
  }
  assert(!deliver(p,n));
  assert(!scan.terminal_reason && !scan.completion.done && !scan.scan_done);
  assert(scan.target_owner.state==MT6797_WIFI_SCAN_TARGET_OPEN);
  assert(scan.target_owner.selected.cookie==cookie);
  assert(scan.target_owner.selected.channel==6 && scan.target_owner.selected.observed_at_ms==100);
  /* No bad observation can stop a later valid target or invent SCAN_DONE. */
  now=120; n=packet(p,target,6); assert(!deliver(p,n));
  assert(scan.target_owner.selected.cookie>cookie);
  u8 body[4]={6,0,0,0};
  struct mt6797_wifi_event_view event={.id=0x0d,.payload=body,.payload_length=4};
  assert(!mt6797_wifi_scan_hif_event(&scan,&event) && !scan.completion.done);
  body[0]=7;
  assert(!mt6797_wifi_scan_hif_event(&scan,&event) && scan.completion.done && scan.scan_done);
  assert(!scan.terminal_reason);
 }
}
static void owned_failures(void) {
 setup(); u8 p[160]; size_t n=packet(p,target,6); assert(!deliver(p,n));
 now=99; assert(deliver(p,n)==-ERANGE);
 assert(scan.terminal_reason==-ERANGE && scan.completion.done);
 setup(); u8 body[3]={7,0,0};
 struct mt6797_wifi_event_view event={.id=0x0d,.payload=body,.payload_length=3};
 assert(mt6797_wifi_scan_hif_event(&scan,&event)==-EPROTO);
 assert(scan.terminal_reason==-EPROTO && scan.completion.done && !scan.scan_done);
 setup(); mt6797_wifi_scan_hif_abort(&scan,-EIO);
 assert(scan.terminal_reason==-EIO && scan.completion.done && !scan.scan_done);
}
int main(void) {
 damaged_observations(); owned_failures();
 puts("PASS actual scan callbacks: damaged foreign/target beacons, RCPI/IE/BSSID rejection and off-channel isolation preserve selection and owned completion; clock, malformed owned SCAN_DONE and transport abort remain terminal");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-scan-observation-') as tmp:
    path = Path(tmp)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    (path/'test.c').write_text(program)
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
    # The former blanket frame-error policy must fail this same wire fixture.
    start = program.index('\t/* Air frames are observations')
    end = program.index('\tret = mt6797_wifi_scan_target_observe', start)
    (path/'regression.c').write_text(program[:start] + program[end:])
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path/'regression.c'), '-o', str(path/'regression')], check=True)
    old = subprocess.run([str(path/'regression')], capture_output=True, text=True)
    assert old.returncode != 0 and '!deliver' in old.stderr
    print('PASS negative control: former frame-error policy rejected by the real callback fixture')
