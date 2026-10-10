#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production scan worker and cancellation callbacks with fake HIF.

The scan transaction and command codec are also production code. Completion
delivery is synchronous, so this checks ownership and fault policy rather than
real workqueue/IRQ concurrency or firmware timing.
"""
from pathlib import Path
import os
import re
import runpy
import shlex
import subprocess
import tempfile

base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-worker-host.py')))
driver = base['driver']
program = base['program'].split('enum stage {')[0]
program = program.replace('struct ieee80211_channel { int dummy; };',
    'struct ieee80211_channel { unsigned band,hw_value,flags; };')
program = program.replace('struct mt6797_wifi_hif_irq { int lifecycle_lock; bool attached; };',
    'struct mt6797_wifi_hif_irq { int lifecycle_lock,mode; bool attached; };')
program += r'''
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define check_add_overflow(a,b,p) __builtin_add_overflow(a,b,p)
#define NL80211_BAND_2GHZ 0
#define MT6797_WIFI_HIF_IRQ_RUNTIME 2
#define MT6797_WIFI_MCR_WTDR1 0x34
#define IEEE80211_CHAN_NO_IR 2
#define IEEE80211_CHAN_RADAR 4
struct cfg80211_ssid { u8 ssid[32]; unsigned ssid_len; };
struct cfg80211_scan_request {
 unsigned n_channels,n_ssids,ie_len; struct ieee80211_channel **channels;
 struct cfg80211_ssid *ssids; const u8 *ie;
};
struct cfg80211_scan_info { bool aborted; };
static struct mt6797_wifi_radio r;
static struct mt6797_wifi_hif_irq irq;
static unsigned writes,cancels,fences,waits,callbacks,attaches,detaches;
static int action,last_fault;
static bool reported_aborted;
static void *wiphy_priv(struct wiphy *w) { (void)w; return &r; }
static void reinit_completion(struct completion *c) { c->done=false; }
static unsigned long msecs_to_jiffies(unsigned n) { return n; }
static void put_unaligned_le16(u16 n,void *p) { u8 *b=p; b[0]=n; b[1]=n>>8; }
static void put_unaligned_le32(u32 n,void *p) { u8 *b=p; b[0]=n; b[1]=n>>8; b[2]=n>>16; b[3]=n>>24; }
static unsigned long wait_for_completion_timeout(struct completion *,unsigned long);
static int mt6797_wifi_hif_runtime_send(struct mt6797_wifi_hif_irq *,u32,const u8 *,size_t);
static int mt6797_radio_frame(void *context,const struct mt6797_wifi_rx_view *frame) {
 (void)context; (void)frame; return 0;
}
static int mt6797_wifi_hif_irq_mode(struct mt6797_wifi_hif_irq *h) { return h->mode; }
static int mt6797_wifi_hif_runtime_sink_attach(struct mt6797_wifi_hif_irq *h,
 const struct mt6797_wifi_hif_runtime_sink *sink) {
 assert(h==&irq && h->lifecycle_lock && !h->attached && sink->context==&r);
 assert(sink->event && sink->frame && sink->abort); h->attached=true; attaches++; return 0;
}
static int mt6797_wifi_hif_runtime_sink_detach(struct mt6797_wifi_hif_irq *h,void *context) {
 assert(h==&irq && h->lifecycle_lock && h->attached && context==&r);
 h->attached=false; detaches++; return 0;
}
static void mt6797_wifi_hif_runtime_fail(struct mt6797_wifi_hif_irq *h,int reason) {
 assert(h==&irq && h->lifecycle_lock && reason<0); fences++; last_fault=reason; h->mode=3;
}
static void cfg80211_scan_done(struct cfg80211_scan_request *request,
 const struct cfg80211_scan_info *info) {
 assert(request && !r.request && !r.armed && !irq.lifecycle_lock && !r.lock);
 callbacks++; reported_aborted=info->aborted;
}
'''
protocol = (driver / 'protocol.c').read_text()
for name in ('MT6797_WIFI_COMMAND_PQ_ID', 'MT6797_WIFI_COMMAND_PACKET_TYPE'):
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', protocol, re.M)[0] + '\n'
program += protocol[protocol.index('int mt6797_wifi_command_init('):
                    protocol.index('int mt6797_wifi_basic_config_build(')]
program += re.sub(r'^#include .*\n', '', (driver / 'scan.c').read_text(), flags=re.M)
for name in ('mt6797_radio_scan_event', 'mt6797_radio_event', 'mt6797_radio_abort',
             'mt6797_radio_cancel_scan', 'mt6797_radio_scan_probe',
             'mt6797_radio_scan_work', 'mt6797_radio_abort_scan'):
    program += base['function'](name)
program += r'''
static void deliver_done(bool foreign,bool malformed) {
 u8 payload[4]={r.scan.scan_sequence,0,0,0};
 if(foreign) payload[0]--;
 struct mt6797_wifi_event_view event={.id=MT6797_WIFI_SCAN_DONE_EVENT_ID,
     .payload=payload,.payload_length=malformed?3:sizeof(payload)};
 assert(mt6797_radio_event(&r,&event)==(malformed?-EPROTO:0));
}
static int mt6797_wifi_hif_runtime_send(struct mt6797_wifi_hif_irq *h,
 u32 address,const u8 *packet,size_t length) {
 assert(h==&irq && h->lifecycle_lock && !r.lock && address==0x34);
 if(packet[4]==0x1b) { assert(length==MT6797_WIFI_SCAN_CANCEL_LENGTH); cancels++; return 0; }
 assert(packet[4]==3 && length==MT6797_WIFI_SCAN_COMMAND_LENGTH && r.armed);
 writes++;
 if(action==6) { mt6797_radio_cancel_scan(&r); assert(!r.scan_done.done && r.armed); }
 return 0;
}
static unsigned long wait_for_completion_timeout(struct completion *completion,
 unsigned long timeout) {
 assert(completion==&r.scan_done && timeout==MT6797_WIFI_SCAN_TIMEOUT_MS);
 assert(r.armed && !r.lock && irq.lifecycle_lock==!r.scan_on_link);
 waits++;
 if(action==1 || action==3 || action==7) {
  mt6797_radio_abort_scan(NULL,&r.wdev);
  assert(r.abort_scan && r.armed && !completion->done && !r.scan_error);
 }
 if(action==4) { mt6797_radio_abort(&r,-EIO); assert(completion->done); return 1; }
 if(action==2 || action==3) return 0; /* An actual missing SCAN_DONE still fences. */
 if(action==7) {
  deliver_done(true,false);
  assert(!completion->done && r.scan.state==MT6797_WIFI_SCAN_WAIT_DONE);
 }
 deliver_done(false,action==5);
 assert(completion->done);
 return 1;
}
static void setup(struct cfg80211_scan_request *request,bool on_link) {
 memset(&r,0,sizeof(r)); memset(&irq,0,sizeof(irq));
 writes=cancels=fences=waits=callbacks=attaches=detaches=0;
 action=last_fault=0; reported_aborted=false; irq.mode=2;
 r.irq=&irq; r.request=request; r.scan_on_link=on_link;
 static struct mt6797_radio_connection connection;
 memset(&connection,0,sizeof(connection)); connection.connected=true;
 if(on_link) r.connection=&connection;
}
static void check(bool on_link,bool aborted,unsigned commands,int fault) {
 assert(callbacks==1 && reported_aborted==aborted && writes==commands);
 assert(!r.request && !r.scan_on_link && !r.armed && !r.lock && !irq.lifecycle_lock);
 assert(attaches==!on_link && detaches==!on_link && !irq.attached);
 assert(fences==!!fault && last_fault==fault);
}
int main(void) {
 struct ieee80211_channel channels[40];
 struct ieee80211_channel *pointers[40];
 for(unsigned i=0;i<ARRAY_SIZE(channels);i++) {
  channels[i]=(struct ieee80211_channel){.band=1,.hw_value=36+i}; pointers[i]=&channels[i];
 }
 struct cfg80211_scan_request request={.n_channels=ARRAY_SIZE(channels),.channels=pointers};
 for(unsigned on_link=0;on_link<2;on_link++) {
  setup(&request,on_link); mt6797_radio_scan_work(&r.scan_work);
  check(on_link,false,2,0); assert(waits==2 && !cancels);
  setup(&request,on_link); mt6797_radio_cancel_scan(&r);
  mt6797_radio_scan_work(&r.scan_work); check(on_link,true,0,0); assert(!waits && !cancels);
  for(action=1;action<=7;action++) {
   int selected=action; setup(&request,on_link); action=selected;
   mt6797_radio_scan_work(&r.scan_work);
   int fault=(action==2 || action==3)?-ETIMEDOUT:
       action==4?-EIO:action==5?-EPROTO:0;
   check(on_link,true,1,fault); assert(waits==1);
   assert(cancels==(action==2 || action==3 || action==4));
  }
 }
 puts("PASS: actual scan worker drains administrative cancellation to owned SCAN_DONE, ignores stale completion, reports aborted once, and retains timeout/protocol/transport fences both off-link and on-link");
}
'''
with tempfile.TemporaryDirectory(prefix='wifi-scan-cancel-') as temporary:
    source = Path(temporary) / 'scan-cancel.c'
    binary = Path(temporary) / 'scan-cancel'
    source.write_text(program)
    subprocess.run(shlex.split(os.environ.get('HOSTCC', 'cc')) + [
        '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-Wno-unused-parameter', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
