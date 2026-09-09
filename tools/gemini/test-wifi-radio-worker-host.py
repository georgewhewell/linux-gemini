#!/usr/bin/env python3
"""Test the production connection worker with stage and cfg80211 fakes.

Actual driver types/worker; no real HIF, workqueue concurrency or kernel events.
The separate HIF and codec tests cover the stages replaced here with fakes.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
driver = root / 'drivers/net/wireless/mediatek/mt6797'
radio = (driver / 'radio.c').read_text()

def function(name):
    start = radio.rfind('\nstatic ', 0, radio.index(name)) + 1
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
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t s32; typedef int64_t s64; typedef int64_t ktime_t; typedef int spinlock_t;
struct completion { bool done; };
static void complete(struct completion *c) { c->done=true; }
struct mutex { int held; };
struct work_struct { int dummy; };
struct sk_buff_head { unsigned length; };
#define ETH_ALEN 6
#define U8_MAX UINT8_MAX
#define DECLARE_BITMAP(name,bits) unsigned long name[((bits)+63)/64]
#define GENMASK(h,l) ((~0UL<<(l)) & (~0UL>>(63-(h))))
#define IEEE80211_MAX_SSID_LEN 32
#define GFP_KERNEL 0
#define dev_warn(...) do {} while(0)
#define system_long_wq 0
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=1; (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(*(l)); *(l)=0; } while(0)
static void mutex_lock(void *p) { int *l=p; assert(!*l); *l=1; }
static void mutex_unlock(void *p) { int *l=p; assert(*l); *l=0; }
static ktime_t ktime_get(void) { return 100; }
static s64 ktime_ms_delta(ktime_t a,ktime_t b) { return a-b; }
static s64 ktime_to_ms(ktime_t t) { return t; }
static bool ether_addr_equal(const u8 *a,const u8 *b) { return !memcmp(a,b,6); }
static bool is_broadcast_ether_addr(const u8 *a) {
 const u8 b[]={255,255,255,255,255,255}; return ether_addr_equal(a,b);
}
static u16 get_unaligned_le16(const void *p) { const u8 *b=p; return b[0]|(u16)b[1]<<8; }
struct net_device { u8 dev_addr[6]; };
struct cfg80211_bss { u8 bssid[6]; };
struct wiphy { int dummy; };
struct wireless_dev { struct wiphy *wiphy; };
struct ieee80211_supported_band { int dummy; };
struct ieee80211_channel { int dummy; };
struct ieee80211_rate { int dummy; };
'''
for name in ['protocol.h', 'scan.h', 'rx_metadata.h', 'scan_target.h', 'scan_hif.h',
             'ais_resources.h', 'ais_protocol.h', 'ais_runtime.h', 'ais_auth.h',
             'ais_key.h', 'ais_mgmt_tx.h', 'ais_assoc.h', 'ais_join.h', 'ais_hif.h', 'capability.h',
             'tx_diag.h']:
    program += re.sub(r'^#include .*\n', '', (driver / name).read_text(), flags=re.M)
program += radio[radio.index('struct mt6797_radio_key {'):radio.index('#define MT6797_RADIO_TX_QUEUE_LIMIT')]
ieee = (root / 'include/linux/ieee80211.h').read_text()
for name in ['IEEE80211_FCTL_VERS', 'IEEE80211_FCTL_FTYPE', 'IEEE80211_FCTL_STYPE',
             'IEEE80211_FCTL_TODS', 'IEEE80211_FCTL_FROMDS', 'IEEE80211_FCTL_MOREFRAGS',
             'IEEE80211_FCTL_PROTECTED', 'IEEE80211_FCTL_ORDER', 'IEEE80211_SCTL_FRAG',
             'IEEE80211_STYPE_DEAUTH', 'IEEE80211_STYPE_DISASSOC', 'IEEE80211_STYPE_BEACON']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', ieee, re.M)[0] + '\n'
program += r'''
#define WLAN_REASON_UNSPECIFIED 1
#define WLAN_REASON_DEAUTH_LEAVING 3
#define WLAN_STATUS_UNSPECIFIED_FAILURE 1
#define WLAN_STATUS_SUCCESS 0
struct mt6797_wifi_hif_runtime_sink {
 int (*event)(void *,const struct mt6797_wifi_event_view *);
 int (*frame)(void *,const struct mt6797_wifi_rx_view *);
 void (*abort)(void *,int); void *context;
};
struct mt6797_wifi_hif_irq { int lifecycle_lock; bool attached; };
enum stage { NONE, SCAN_INIT, SCAN_RUN, AIS_INIT, AIS_START, AUTH, ASSOC, ATTACH };
static struct {
 struct mt6797_wifi_radio *radio;
 enum stage fail,cancel,last;
 unsigned stages,stops,closes,detaches,success,failure,disconnected,puts,frees,queued;
 u8 scan_command,ais_command;
 bool expired;
 int pm_error;
 unsigned pm_finishes,pm_aborts;
} test;
static int stage(enum stage stage) {
 assert(stage>test.last); test.last=stage; test.stages++;
 if(test.cancel==stage) test.radio->connection->cancel=true;
 return test.fail==stage?-EIO:0;
}
static void queue_work(int queue,struct work_struct *work) { (void)queue; (void)work; test.queued++; }
static int mt6797_wifi_hif_runtime_sink_attach(struct mt6797_wifi_hif_irq *irq,
 const struct mt6797_wifi_hif_runtime_sink *sink) {
 assert(!irq->attached && sink->frame && sink->abort);
 int ret=stage(ATTACH); if(!ret) irq->attached=true; return ret;
}
static int mt6797_wifi_hif_runtime_sink_detach(struct mt6797_wifi_hif_irq *irq,void *context) {
 assert(irq->attached && context==test.radio); irq->attached=false; test.detaches++; return 0;
}
int mt6797_wifi_scan_hif_init(struct mt6797_wifi_scan_hif *scan,struct mt6797_wifi_hif_irq *irq,
 const struct mt6797_wifi_scan_hif_backend *backend) {
 (void)irq; assert(!backend); int ret=stage(SCAN_INIT); if(!ret) scan->initialized=true; return ret;
}
int mt6797_wifi_scan_hif_run(struct mt6797_wifi_scan_hif *scan,
 const struct mt6797_wifi_scan_hif_request *request,struct mt6797_wifi_scan_hif_result *result) {
 assert(scan->initialized && request->band==1 && request->channel==6);
 assert(request->key.ssid_length==4 && !memcmp(request->key.ssid,"test",4));
 test.scan_command=request->command_sequence;
 assert(request->cancel_sequence==request->command_sequence+1);
 int ret=stage(SCAN_RUN); if(!ret) { memset(result,0,sizeof(*result)); result->target.rcpi=100; }
 return ret;
}
int mt6797_wifi_scan_hif_close(struct mt6797_wifi_scan_hif *scan) {
 assert(scan->initialized); test.closes++; return 0;
}
int mt6797_wifi_ais_hif_init(struct mt6797_wifi_ais_hif *ais,struct mt6797_wifi_hif_irq *irq,
 struct mt6797_wifi_runtime_config *config,const struct mt6797_wifi_capability *capability,
 unsigned sequence,unsigned token,const struct mt6797_wifi_ais_hif_backend *backend) {
 (void)irq; (void)config; (void)capability;
 assert(!backend && sequence==test.scan_command+2 && token);
 test.ais_command=sequence; int ret=stage(AIS_INIT); if(!ret) ais->initialized=true; return ret;
}
int mt6797_wifi_ais_hif_start(struct mt6797_wifi_ais_hif *ais,
 const struct mt6797_wifi_ais_hif_request *request) {
 assert(ais->initialized && request->target_owner==&test.radio->connection->scan.target_owner);
 int ret=stage(AIS_START);
 if(!ret) { ais->runtime.state=MT6797_WIFI_AIS_RUNTIME_STA_STATE1; ais->channel_deadline=test.expired?99:500; }
 return ret;
}
int mt6797_wifi_ais_hif_open_auth(struct mt6797_wifi_ais_hif *ais,
 const struct mt6797_wifi_ais_hif_auth_request *request,struct mt6797_wifi_ais_hif_auth_result *result) {
 assert(ais->runtime.state==MT6797_WIFI_AIS_RUNTIME_STA_STATE1);
 assert(request->response_timeout_ms==400 && request->sequence_number==1);
 memset(result,0,sizeof(*result)); return stage(AUTH);
}
int mt6797_wifi_ais_hif_associate(struct mt6797_wifi_ais_hif *ais,
 const struct mt6797_wifi_ais_hif_join_request *request,struct mt6797_wifi_ais_join_params *out) {
 assert(ais->initialized && request->negotiated.rcpi==100);
 int ret=stage(ASSOC);
 if(!ret) { ais->runtime.state=MT6797_WIFI_AIS_RUNTIME_STA_STATE3;
  ais->join.response_length=33; memset(ais->join.response,0,33);
  ais->auth.pid_owner.next_pid=9; *out=request->negotiated;
 }
 return ret;
}
int mt6797_wifi_ais_hif_stop(struct mt6797_wifi_ais_hif *ais) {
 assert(!test.radio->irq->attached);
 assert(ais->runtime.state==MT6797_WIFI_AIS_RUNTIME_STA_STATE1 || ais->runtime.state==MT6797_WIFI_AIS_RUNTIME_STA_STATE3);
 test.stops++; ais->runtime.state=MT6797_WIFI_AIS_RUNTIME_STOPPED; return 0;
}
static void cfg80211_connect_result(struct net_device *netdev,const u8 *bssid,
 const u8 *req,size_t req_len,const u8 *resp,size_t resp_len,u16 status,int allocation) {
 (void)netdev; (void)bssid; (void)allocation;
 if(!status) { assert(test.radio->connection->connected && test.radio->irq->attached);
  assert(req && req_len && resp && resp_len==3); test.success++;
 } else { assert(!req && !req_len && !resp && !resp_len); test.failure++; }
}
static void cfg80211_disconnected(struct net_device *netdev,u16 reason,
 const u8 *ie,size_t length,bool local,int allocation) {
 (void)netdev; (void)ie; (void)allocation;
 assert(reason && !length && local && !test.radio->irq->attached); test.disconnected++;
}
static void cfg80211_put_bss(struct wiphy *wiphy,struct cfg80211_bss *bss) {
 (void)wiphy; assert(bss); test.puts++;
}
static void kfree_sensitive(void *p) { assert(!test.radio->connection); test.frees++; free(p); }
'''
program += '''
int mt6797_wifi_ais_ccmp_ready_match(const struct mt6797_wifi_ais_sta_pending *p, const struct mt6797_wifi_event_view *e, enum mt6797_wifi_ais_event_disposition *d) { (void)p; (void)e; *d=MT6797_WIFI_AIS_EVENT_IGNORED; return 0; }
'''
program += '''
static void netif_carrier_on(struct net_device *d) { (void)d; }
static void netif_carrier_off(struct net_device *d) { (void)d; }
static void netif_stop_queue(struct net_device *d) { (void)d; }
static void netif_wake_queue(struct net_device *d) { (void)d; }
static void mt6797_radio_purge_tx(struct mt6797_wifi_radio *r) { (void)r; }
static int mt6797_radio_data_frame(struct mt6797_wifi_radio *r, const struct mt6797_wifi_rx_view *v) { (void)r; (void)v; return 0; }
static int mt6797_radio_beacon(struct mt6797_wifi_radio *r, const struct mt6797_wifi_rx_view *v) { (void)r; (void)v; return 0; }
static int mt6797_radio_finish_pm(struct mt6797_wifi_radio *r) { assert(r->connection_lock.held && test.success==1); test.pm_finishes++; if(test.pm_error)return test.pm_error; r->connection->pm_started=true; return 0; }
static int mt6797_radio_pm_send(struct mt6797_wifi_radio *r,bool connected) { assert(!connected && r->connection_lock.held && !r->irq->attached); test.pm_aborts++; r->connection->pm_started=false; return 0; }
'''
for name in ['mt6797_radio_link_event', 'mt6797_radio_connection_cancelled', 'mt6797_radio_link_abort',
             'mt6797_radio_link_frame', 'mt6797_radio_connection_work']:
    program += function(name)
program += r'''
static struct cfg80211_bss bss={.bssid={2,6,7,8,9,10}};
static void setup(struct mt6797_wifi_radio *radio,struct mt6797_wifi_hif_irq *irq,
 enum stage fail,enum stage cancel) {
 memset(radio,0,sizeof(*radio)); memset(irq,0,sizeof(*irq)); memset(&test,0,sizeof(test));
 test.radio=radio; test.fail=fail; test.cancel=cancel;
 radio->irq=irq; radio->command_sequence=2; radio->management_pid=7;
 radio->connection=calloc(1,sizeof(*radio->connection));
 radio->connection->bss=&bss;
 radio->connection->join.negotiated.channel=(struct mt6797_wifi_ais_channel){1,6};
 memcpy(radio->connection->join.negotiated.ssid,"test",4);
 radio->connection->join.negotiated.ssid_length=4;
 radio->connection->join.association.ies=radio->connection->ies;
 radio->connection->join.association.ies_length=6;
}
static void check_freed(void) {
 assert(!test.radio->connection && test.puts==1 && test.frees==1);
 assert(test.failure+test.disconnected==1 && !test.radio->irq->attached);
 mt6797_radio_connection_work(&test.radio->connection_work); /* Stale queued work is harmless. */
 assert(test.puts==1 && test.frees==1);
}
int main(void) {
 struct mt6797_wifi_radio r; struct mt6797_wifi_hif_irq irq;
 for(enum stage fail=SCAN_INIT;fail<=ATTACH;fail++) {
  setup(&r,&irq,fail,NONE); mt6797_radio_connection_work(&r.connection_work);
  assert(!test.success && test.last==fail); check_freed();
  assert(test.closes==(fail>SCAN_INIT));
  assert(test.stops==(fail>AIS_START));
 }
	for(enum stage cancel=SCAN_INIT;cancel<=ATTACH;cancel++) {
  setup(&r,&irq,NONE,cancel); mt6797_radio_connection_work(&r.connection_work);
  assert(!test.success && test.last==cancel); check_freed();
  assert(test.stops==(cancel>=AIS_START));
 }
 setup(&r,&irq,NONE,NONE); r.connection->cancel=true;
 mt6797_radio_connection_work(&r.connection_work); assert(!test.stages); check_freed();
 setup(&r,&irq,NONE,NONE); test.expired=true;
 mt6797_radio_connection_work(&r.connection_work);
 assert(test.last==AIS_START && test.stops==1 && !test.success); check_freed();
 setup(&r,&irq,NONE,NONE); r.command_sequence=254;
 mt6797_radio_connection_work(&r.connection_work);
 assert(test.success==1 && !test.failure && r.connection && test.scan_command==3);
 assert(test.ais_command==5 && r.command_sequence==12);
 assert(!test.puts && !test.frees && !test.stops);
 assert(test.pm_finishes==1 && r.connection->pm_started);
 r.connection->cancel=true; r.connection->reason=3;
 mt6797_radio_connection_work(&r.connection_work);
 assert(test.disconnected==1 && test.detaches==1 && test.stops==1 && r.management_pid==9);
 assert(test.pm_aborts==1);
 check_freed();
 setup(&r,&irq,NONE,NONE); test.pm_error=-ETIMEDOUT;
 mt6797_radio_connection_work(&r.connection_work);
 assert(test.success==1 && test.disconnected==1 && test.pm_finishes==1 && !test.pm_aborts);
 check_freed();
 puts("PASS: production connection worker stage failures/cancellations, grant expiry, callback ordering, command reservation, retained connection, cleanup and exactly-once BSS/free ownership");
}
'''
if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='gemini-radio-worker-') as temporary:
        path = Path(temporary)
        (path / 'test.c').write_text(program)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
