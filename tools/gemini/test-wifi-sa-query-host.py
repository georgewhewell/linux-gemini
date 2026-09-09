#!/usr/bin/env python3
"""Production SA Query trigger, timers, RX matching, response worker and TX.

Synthetic firmware transport/workqueue; no IRQ concurrency or on-air encryption
is simulated. Inherits the PMF RX/CMAC and key-lifecycle production harnesses.
"""
from pathlib import Path
import os
import re
import runpy
import shlex
import subprocess
import tempfile

base = runpy.run_path(str(Path(__file__).with_name('test-wifi-pmf-rx-host.py')))
driver = base['driver']
program = base['program'].replace('int main(void) {', 'static int pmf_main(void) {')
key = base['base']
management = (driver / 'ais_mgmt_tx.c').read_text()
for line in management.splitlines():
    if line.startswith('#define MT6797_WIFI_AIS_MGMT_') and 'EVENT_TX_DONE' not in line:
        program += line + '\n'
program += '#define U64_MAX UINT64_MAX\n#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))\n'
ieee = (driver.parents[4] / 'include/linux/ieee80211.h').read_text()
for name in ['IEEE80211_FTYPE_MGMT', 'IEEE80211_STYPE_AUTH', 'IEEE80211_STYPE_ASSOC_REQ']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', ieee, re.M)[0] + '\n'
vendor = Path('/home/grw/src/gemini-linux-kernel-3.18/drivers/misc/mediatek/connectivity/wlan/gen3/include/nic/nic_tx.h').read_text()
program += 'typedef u8 UINT_8; typedef u16 UINT_16; typedef u32 UINT_32;\n'
program += re.search(r'typedef struct _HW_MAC_TX_DESC_T \{.*?\} HW_MAC_TX_DESC_T,.*?;', vendor, re.S)[0] + '\n'
program += re.search(r'^#define TX_DESC_PROTECTED_FRAME\s+[^\n]+', vendor, re.M)[0] + '\n'
for name in ['mt6797_wifi_ais_mgmt_pid_open', 'mt6797_wifi_ais_mgmt_pid_acquire',
             'mt6797_wifi_ais_mgmt_pid_release', 'mt6797_wifi_ais_mgmt_pid_close',
             'mt6797_wifi_ais_mgmt_pid_mark_terminal', 'mt6797_wifi_ais_mgmt_frame_valid',
             'mt6797_wifi_ais_mgmt_tx_build']:
    program += key['extract'](management, name)
program += r'''
enum sa_mode { DEFERRED, IMMEDIATE, WRONG_PID, WRONG_WLAN, WRONG_SN, MALFORMED,
 FAILED_STATUS, TIMEOUT, TRANSPORT, NO_CREDIT, ABORTED, MALFORMED_THEN_VALID, DUPLICATE, INFLIGHT_REQUEST,
 QUERY_EARLY_RESPONSE, QUERY_RESPONSE_WAIT, QUERY_REPLY_NO_DONE };
static enum sa_mode sa_mode;
enum preflight_hook { PREFLIGHT_NONE, PREFLIGHT_EXPIRE, PREFLIGHT_CONFIRM, PREFLIGHT_RESTART };
static enum preflight_hook preflight_hook;
static unsigned sa_writes;
static u32 random_value=0x1234;
static u32 get_random_u32(void) { return random_value; }
static u8 sa_packet[MT6797_WIFI_TX_DESCRIPTOR_LENGTH+28];
static int sa_offer(enum sa_mode mode) {
 u8 payload[16]={0};
 payload[0]=c.management_lease.pid; payload[4]=c.management_lease.wlan_index;
 put_unaligned_le16(c.management_sequence,payload+2);
 struct mt6797_wifi_event_view e={.id=0x0f,.payload=payload,.payload_length=16,.packet_length=24};
 if(mode==WRONG_PID) payload[0]++;
 if(mode==WRONG_WLAN) payload[4]++;
 if(mode==WRONG_SN) payload[2]^=1;
 if(mode==MALFORMED) e.payload_length--;
 if(mode==FAILED_STATUS) payload[1]=1;
 return mt6797_radio_link_event(&r,&e);
}
static void query_frame(u8 action,u8 first,u8 second,u64 pn) {
 frame_build(false,5);
 put_unaligned_le16(IEEE80211_STYPE_ACTION|IEEE80211_FCTL_PROTECTED,packet+frame_offset);
 packet[frame_offset+24]=WLAN_CATEGORY_SA_QUERY; packet[frame_offset+25]=action;
 packet[frame_offset+26]=first; packet[frame_offset+27]=second;
 for(unsigned i=0;i<6;i++) packet[16+i]=pn>>(8*i);
 packet_length=frame_offset+28; put_unaligned_le16(packet_length,packet);
}
static void query_receive(void) {
 struct mt6797_wifi_rx_view v;
 int ret=mt6797_wifi_rx_parse(packet,packet_length,&v);
 if(ret) { assert(ret==-EBADMSG); return; } /* Lower RX layer drops descriptor integrity errors. */
 assert(!mt6797_radio_link_frame(&r,&v));
 assert(!r.lock);
}
static void reply_to_last_query(void) {
 u8 first=sa_packet[sizeof(sa_packet)-2],second=sa_packet[sizeof(sa_packet)-1];
 query_frame(WLAN_ACTION_SA_QUERY_RESPONSE,first,second,c.pairwise_key.mgmt_rx_pn+1);
 query_receive();
}
static int sa_send(struct mt6797_wifi_hif_irq *hif,u32 address,const u8 *data,size_t length) {
 assert(hif==&irq && irq.lifecycle_lock && r.connection_lock.held && !r.lock);
 assert(address==MT6797_WIFI_MCR_WTDR1 && length==sizeof(sa_packet));
 sa_writes++; memcpy(sa_packet,data,length);
 assert(c.management_waiting && !c.management_done);
 if(sa_mode==IMMEDIATE || sa_mode==DUPLICATE) assert(!sa_offer(IMMEDIATE));
 if(sa_mode==DUPLICATE) assert(!sa_offer(FAILED_STATUS));
 if(sa_mode==INFLIGHT_REQUEST) {
  query_frame(WLAN_ACTION_SA_QUERY_REQUEST,0x56,0x78,9); query_receive();
 }
 if(sa_mode==QUERY_EARLY_RESPONSE || sa_mode==QUERY_REPLY_NO_DONE) reply_to_last_query();
 return sa_mode==TRANSPORT?-EIO:sa_mode==NO_CREDIT?-EAGAIN:0;
}
static unsigned long sa_wait(struct completion *completion,unsigned long timeout) {
 assert(completion==&c.management_completion && timeout>0 && timeout<=1000);
 assert(irq.lifecycle_lock && r.connection_lock.held && !r.lock);
 if(sa_mode==ABORTED) mt6797_radio_link_abort(&r,-ESHUTDOWN);
 else if(sa_mode==MALFORMED || sa_mode==MALFORMED_THEN_VALID) {
  assert(sa_offer(MALFORMED)==-EPROTO);
  if(sa_mode==MALFORMED_THEN_VALID) assert(!sa_offer(IMMEDIATE));
 }
 else if(sa_mode!=TIMEOUT && sa_mode!=QUERY_REPLY_NO_DONE) {
  if(sa_mode==QUERY_RESPONSE_WAIT) reply_to_last_query();
  assert(!sa_offer(sa_mode));
 }
 return completion->done;
}
static void trigger_frame(u16 subtype,u16 reason,unsigned status);
static void sa_mutex_lock(void *lock) {
 mutex_lock(lock);
 if(lock==&irq.lifecycle_lock && preflight_hook!=PREFLIGHT_NONE) {
  enum preflight_hook hook=preflight_hook; preflight_hook=PREFLIGHT_NONE;
  if(hook==PREFLIGHT_EXPIRE) fake_now_us=c.sa_query_deadline;
  else {
   reply_to_last_query();
   if(hook==PREFLIGHT_RESTART) {
    fake_now_us=c.sa_query_next_start; random_value=0xbabe;
    trigger_frame(IEEE80211_STYPE_DEAUTH,6,0); query_receive();
   }
  }
 }
}
#define mutex_lock sa_mutex_lock
#define mt6797_wifi_hif_runtime_send sa_send
#define wait_for_completion_timeout sa_wait
'''
for name in ['mt6797_radio_sa_query_tx', 'mt6797_radio_sa_query_response',
             'mt6797_radio_management_work', 'mt6797_radio_sa_query_schedule',
             'mt6797_radio_sa_query_timeout', 'mt6797_radio_sa_query_work', 'mt6797_radio_disconnect']:
    program += key['function'](name)
program += r'''
static void sa_setup(void) {
 fake_now_us=100000; random_value=0x1234; preflight_hook=PREFLIGHT_NONE;
 setup_rx(false); params.cipher=WLAN_CIPHER_SUITE_AES_CMAC; assert(!add(4,false));
 c.management_sequence=2; sa_mode=DEFERRED; sa_writes=queued=terminals=0;
 memset(sa_packet,0,sizeof(sa_packet));
 query_frame(WLAN_ACTION_SA_QUERY_REQUEST,0x12,0x34,8);
}
static void run_worker(void) {
 mt6797_radio_management_work(&r.management_work);
 assert(!r.connection_lock.held && !irq.lifecycle_lock && !r.lock && !c.management_waiting);
}
static void assert_response(u8 first,u8 second) {
 assert(sa_writes==1 && !terminals && !c.cancel && !c.sa_query_response_pending);
 assert(c.ais.auth.pid_owner.state==MT6797_WIFI_AIS_MGMT_PID_CLOSED && !c.ais.auth.pid_owner.held);
 const u8 *f=sa_packet+MT6797_WIFI_TX_DESCRIPTOR_LENGTH;
 HW_MAC_TX_DESC_T descriptor; memcpy(&descriptor,sa_packet,sizeof(descriptor));
 _Static_assert(sizeof(descriptor)==MT6797_WIFI_TX_DESCRIPTOR_LENGTH,"vendor TX descriptor size");
 _Static_assert(offsetof(HW_MAC_TX_DESC_T,ucHeaderPadding)==6,"vendor protection offset");
 assert(descriptor.ucHeaderPadding==TX_DESC_PROTECTED_FRAME);
 assert(get_unaligned_le16(sa_packet)==sizeof(sa_packet) && sa_packet[3]==0x88 && sa_packet[4]==1);
 assert(sa_packet[5]==0xcc && sa_packet[6]==0x80 && !sa_packet[9]);
 assert(sa_packet[20]==c.management_lease.pid && sa_packet[21]==2);
 assert(get_unaligned_le16(f)==(IEEE80211_STYPE_ACTION|IEEE80211_FCTL_PROTECTED));
 assert(ether_addr_equal(f+4,bss.bssid) && ether_addr_equal(f+10,netdev.dev_addr));
 assert(ether_addr_equal(f+16,bss.bssid) && get_unaligned_le16(f+22)==c.management_sequence<<4);
 assert(f[24]==WLAN_CATEGORY_SA_QUERY && f[25]==WLAN_ACTION_SA_QUERY_RESPONSE && f[26]==first && f[27]==second);
 assert(get_unaligned_le16(sa_packet+14)==(0x8000|c.management_sequence));
}
static void initiation_tests(void);
int main(void) {
 pmf_main();
 for(unsigned mode=DEFERRED;mode<=IMMEDIATE;mode++) {
  sa_setup(); sa_mode=mode; query_receive();
  assert(queued==1 && c.sa_query_response_pending && c.pairwise_key.mgmt_rx_pn==8 && !c.cancel);
  run_worker(); assert_response(0x12,0x34);
  run_worker(); assert(sa_writes==1); /* Consumed request is not sent twice. */
 }
 /* One queued ID is retained, not overwritten by a burst of requests. */
 sa_setup(); query_receive(); query_frame(WLAN_ACTION_SA_QUERY_REQUEST,0xab,0xcd,9); query_receive();
 assert(queued==1 && c.pairwise_key.mgmt_rx_pn==9); run_worker(); assert_response(0x12,0x34);
 /* A new authenticated request during TX survives for a later worker run. */
 sa_setup(); sa_mode=INFLIGHT_REQUEST; query_receive(); run_worker();
 assert(sa_writes==1 && c.sa_query_response_pending && queued==2);
 sa_mode=DEFERRED; run_worker(); assert(sa_writes==2 && !c.sa_query_response_pending);
 assert(sa_packet[sizeof(sa_packet)-2]==0x56 && sa_packet[sizeof(sa_packet)-1]==0x78);
 /* Replays and responses do not elicit another response. */
 sa_setup(); query_receive(); run_worker(); query_receive(); assert(!c.sa_query_response_pending && queued==1);
 query_frame(WLAN_ACTION_SA_QUERY_RESPONSE,0x12,0x34,9); query_receive();
 assert(!c.sa_query_response_pending && queued==1 && c.pairwise_key.mgmt_rx_pn==9);
 /* Unicast, body and action-code checks precede replay and scheduling. */
 sa_setup(); packet[frame_offset+25]=2; query_receive(); assert(!queued && c.pairwise_key.mgmt_rx_pn==7);
 sa_setup(); packet_length--; put_unaligned_le16(packet_length,packet); query_receive(); assert(!queued);
 sa_setup(); memset(packet+frame_offset+4,0xff,6); query_receive(); assert(!queued);
 sa_setup(); packet[frame_offset+1]&=~0x40; packet[9]=0; query_receive(); assert(!queued);
 /* A valid RX request is revalidated when the deferred worker owns the connection. */
 for(unsigned change=0;change<14;change++) {
  sa_setup(); query_receive();
  switch(change) {
   case 0: c.cancel=true; break; case 1: r.stopping=true; break;
   case 2: c.pmf_negotiated=false; break; case 3: c.pairwise_ready=false; break;
   case 4: c.pairwise_key.installed=false; break; case 5: c.pairwise_key.updating=true; break;
   case 6: c.key_pending.generation++; break; case 7: c.pairwise_wtbl.cookie++; break;
   case 8: c.igtk[0].installed=false; break; case 9: c.connected=false; break;
   case 10: r.connection=NULL; break;
   case 11: c.igtk_owner.generation++; break; case 12: c.igtk_wtbl.cookie++; break;
   case 13: c.igtk_updating=true; break;
  }
  run_worker(); assert(!sa_writes && !terminals);
 }
 /* Retiring the old IGTK keeps protected unicast response service available. */
 sa_setup(); query_receive();
 const u8 next_igtk[16]={0x42}; params.key=next_igtk;
 assert(!add(5,false) && !del(4,false));
 run_worker(); assert_response(0x12,0x34);
 /* Foreign PID/WLAN/SN cannot complete this TX; missing completion fences. */
 for(unsigned mode=WRONG_PID;mode<=MALFORMED_THEN_VALID;mode++) {
  sa_setup(); sa_mode=mode; query_receive(); run_worker(); assert(sa_writes==1);
  if(mode==FAILED_STATUS || mode==NO_CREDIT) {
   assert(!terminals && !c.cancel && !c.ais.auth.pid_owner.held);
   assert(c.ais.auth.pid_owner.state==MT6797_WIFI_AIS_MGMT_PID_CLOSED);
  } else {
   assert(terminals==1 && c.cancel && c.ais.auth.pid_owner.held);
   assert(c.ais.auth.pid_owner.state==MT6797_WIFI_AIS_MGMT_PID_TERMINAL);
  }
 }
 sa_setup(); sa_mode=DUPLICATE; query_receive(); run_worker(); assert_response(0x12,0x34);
 /* Generic auth/assoc entry never silently admits a protected action. */
 sa_setup(); struct mt6797_wifi_ais_mgmt_pid_owner *o=&c.ais.auth.pid_owner;
 assert(!mt6797_wifi_ais_mgmt_pid_open(o,&c.ais.runtime.bss,&c.ais.runtime.sta));
 assert(!mt6797_wifi_ais_mgmt_pid_acquire(o,&c.management_lease));
 struct mt6797_wifi_ais_mgmt_tx_params p={.sa_query_protected=true};
 u8 f[28]={0},out[MT6797_WIFI_TX_DESCRIPTOR_LENGTH+29]; size_t length;
 put_unaligned_le16(IEEE80211_STYPE_ACTION|IEEE80211_FCTL_PROTECTED,f);
 memcpy(f+4,bss.bssid,6); memcpy(f+10,netdev.dev_addr,6); memcpy(f+16,bss.bssid,6);
 f[24]=WLAN_CATEGORY_SA_QUERY; f[25]=WLAN_ACTION_SA_QUERY_REQUEST;
 assert(!mt6797_wifi_ais_mgmt_tx_build(o,&c.management_lease,&c.ais.runtime.bss,&c.ais.runtime.sta,&p,f,28,out,sizeof(out)-1,&length));
 for(unsigned bad=0;bad<8;bad++) {
  u8 bad_frame[28]; memcpy(bad_frame,f,28); p.sa_query_protected=true;
  size_t frame_len=28,capacity=sizeof(out)-1; memset(out,0xa5,sizeof(out));
  switch(bad) {
   case 0: p.sa_query_protected=false; break; case 1: bad_frame[1]&=~0x40; break;
   case 2: bad_frame[24]=0; break; case 3: bad_frame[25]=2; break;
   case 4: bad_frame[22]=1; break; case 5: frame_len=27; break;
   case 6: capacity--; break; case 7: bad_frame[4]^=2; break;
  }
  assert(mt6797_wifi_ais_mgmt_tx_build(o,&c.management_lease,&c.ais.runtime.bss,&c.ais.runtime.sta,&p,bad_frame,frame_len,out,capacity,&length)<0);
  assert(!length); for(unsigned i=0;i<sizeof(out);i++) assert(out[i]==0xa5);
 }
 puts("PASS: production SA Query authenticated RX/response worker/protected TX/PID completion; exact ID, bounded pending queue, replay, deferred key ownership, early/deferred/foreign/malformed/duplicate completion, no-credit/transport/timeout/abort fencing and codec rejection");
 initiation_tests();
}
'''
program += r'''
static void trigger_frame(u16 subtype,u16 reason,unsigned status) {
 frame_build(false,5); packet[9]=0;
 put_unaligned_le16(subtype,packet+frame_offset);
 put_unaligned_le16(reason,packet+frame_offset+24);
 put_unaligned_le16(0xe000|status,packet+10);
}
static void start_query(void) {
 trigger_frame(IEEE80211_STYPE_DEAUTH,6,BIT(2)); query_receive();
 assert(c.sa_query_active && c.sa_query_generation==1 && !c.sa_query_count && !c.sa_query_attempts);
 assert(c.sa_query_deadline==fake_now_us+1024000 && r.sa_query_work.pending);
 assert(!queued && !sa_writes && !c.cancel && c.authorized && c.pairwise_key.mgmt_rx_pn==7);
}
static void query_timer_now(void) {
 r.sa_query_work.pending=false;
 mt6797_radio_sa_query_work(&r.sa_query_work.work);
 assert(!r.lock && !r.connection_lock.held && !irq.lifecycle_lock && !c.management_waiting);
}
static void query_timer_next(void) {
 assert(r.sa_query_work.pending); fake_now_us=r.sa_query_work.due; query_timer_now();
}
static void assert_query_stopped(void) {
 assert(!c.sa_query_active && !c.sa_query_count && !c.sa_query_attempts && !r.sa_query_work.pending);
 u8 zero[sizeof(c.sa_query_ids)]={0}; assert(!memcmp(c.sa_query_ids,zero,sizeof(zero)));
 assert(!c.sa_query_owner.generation && !c.sa_query_wtbl.generation);
}
static void initiation_tests(void) {
 /* Both applicable reasons/subtypes work with or without cipher-mismatch. */
 for(unsigned subtype=0;subtype<2;subtype++) for(unsigned reason=6;reason<=7;reason++) for(unsigned mismatch=0;mismatch<2;mismatch++) {
  sa_setup(); trigger_frame(subtype?IEEE80211_STYPE_DISASSOC:IEEE80211_STYPE_DEAUTH,reason,mismatch?BIT(2):0);
  query_receive(); assert(c.sa_query_active && !c.cancel && !queued && !sa_writes);
 }
 for(unsigned bad=0;bad<17;bad++) {
  sa_setup(); trigger_frame(IEEE80211_STYPE_DEAUTH,6,BIT(2));
  switch(bad) {
   case 0: packet[frame_offset+24]=3; break;
   case 1: memset(packet+frame_offset+4,0xff,6); break;
   case 2: packet[frame_offset+4]^=4; break; case 3: packet[frame_offset+10]^=4; break;
   case 4: packet[frame_offset+16]^=4; break; case 5: packet[7]=4; break;
   case 6: packet[8]=2; break; case 7: packet[frame_offset+1]|=0x40; break;
   case 8: packet[9]=0x40; break; case 9: packet[frame_offset+22]=1; break;
   case 10: c.authorized=false; break; case 11: c.pmf_negotiated=false; break;
   case 12: c.pairwise_ready=false; break; case 13: c.igtk[0].installed=false; break;
   case 14: c.key_pending.generation++; break; case 15: c.pairwise_wtbl.cookie++; break;
   case 16: packet_length--; put_unaligned_le16(packet_length,packet); break;
  }
  query_receive(); assert(!c.sa_query_active && !r.sa_query_work.pending && !sa_writes);
  if(bad!=11) assert(!c.cancel); /* Legacy PMF-disabled deauth behavior is unchanged. */
 }
 for(unsigned bit=1;bit<=12;bit++) if(bit!=2) {
  sa_setup(); trigger_frame(IEEE80211_STYPE_DEAUTH,6,BIT(2)|BIT(bit)); query_receive();
  assert(!c.sa_query_active && !c.cancel);
 }
 /* Trigger bursts cannot reset the deadline or queue additional first attempts. */
 sa_setup(); start_query(); ktime_t deadline=c.sa_query_deadline;
 fake_now_us+=5000; trigger_frame(IEEE80211_STYPE_DISASSOC,7,0); query_receive();
 assert(c.sa_query_generation==1 && c.sa_query_deadline==deadline && r.sa_query_work.scheduled==1);
 /* Five bounded attempts, fresh IDs despite repeated RNG output, then local timeout. */
 sa_setup(); start_query(); deadline=c.sa_query_deadline;
 for(unsigned i=0;i<5;i++) {
  query_timer_next(); assert(sa_writes==i+1 && c.sa_query_count==i+1 && c.sa_query_attempts==i+1);
  assert(get_unaligned_le16(c.sa_query_ids[i])==0x1234+i);
  assert(sa_packet[MT6797_WIFI_TX_DESCRIPTOR_LENGTH+25]==WLAN_ACTION_SA_QUERY_REQUEST && sa_packet[6]==0x80);
  assert(c.sa_query_active && !c.cancel && c.sa_query_deadline==deadline);
  assert(r.sa_query_work.due<=deadline);
  ktime_t now=fake_now_us; query_timer_now(); /* Early timer delivery creates no duplicate request. */
  assert(sa_writes==i+1 && fake_now_us==now && r.sa_query_work.pending);
 }
 assert(r.sa_query_work.due==deadline); query_timer_next(); assert_query_stopped();
 assert(sa_writes==5 && c.cancel && !c.peer_disconnect && c.reason==WLAN_REASON_PREV_AUTH_NOT_VALID);
 assert(queued==1 && !terminals); query_timer_now(); assert(sa_writes==5 && queued==1);
 /* A delayed first worker expires the original query instead of sending late. */
 sa_setup(); start_query(); fake_now_us=c.sa_query_deadline+1; query_timer_now();
 assert_query_stopped(); assert(c.cancel && !sa_writes && !terminals);
 /* Expiry or confirmation while waiting for the transport lock prevents a stale TX. */
 sa_setup(); start_query(); preflight_hook=PREFLIGHT_EXPIRE; query_timer_next();
 assert_query_stopped(); assert(c.cancel && !sa_writes && !terminals && !c.ais.auth.pid_owner.held);
 sa_setup(); start_query(); query_timer_next(); preflight_hook=PREFLIGHT_CONFIRM; query_timer_next();
 assert_query_stopped(); assert(!c.cancel && sa_writes==1 && !terminals);
 /* A new generation started during that boundary survives the obsolete worker's cleanup. */
 sa_setup(); start_query(); query_timer_next(); preflight_hook=PREFLIGHT_RESTART; query_timer_next();
 assert(c.sa_query_active && c.sa_query_generation==2 && !c.sa_query_count && !c.sa_query_attempts);
 assert(sa_writes==1 && r.sa_query_work.pending && !terminals);
 query_timer_next(); assert(sa_writes==2 && c.sa_query_count==1 && get_unaligned_le16(c.sa_query_ids[0])==0xbabe);
 /* A response to any transmitted attempt, not only the newest ID, succeeds. */
 sa_setup(); start_query(); query_timer_next(); u8 old_id[2]; memcpy(old_id,c.sa_query_ids[0],2);
 query_timer_next(); query_timer_next(); assert(c.sa_query_count==3);
 query_frame(WLAN_ACTION_SA_QUERY_RESPONSE,old_id[0],old_id[1],8); query_receive();
 assert_query_stopped(); assert(!c.cancel && !terminals && c.authorized);
 query_timer_now(); assert(sa_writes==3);
 /* A completed query is rate limited; a later query gets a new generation. */
 trigger_frame(IEEE80211_STYPE_DEAUTH,6,0); query_receive(); assert(!c.sa_query_active && c.sa_query_generation==1);
 fake_now_us=c.sa_query_next_start; random_value=0xabcd; query_receive();
 assert(c.sa_query_active && c.sa_query_generation==2); query_timer_next();
 query_frame(WLAN_ACTION_SA_QUERY_RESPONSE,old_id[0],old_id[1],9); query_receive(); assert(c.sa_query_active);
 reply_to_last_query(); assert_query_stopped(); assert(!c.cancel);
 /* IGTK overlap/rekey leaves an active PTK-authenticated query's identity
  * and deadline intact, including removal of the old IGTK. */
 sa_setup(); start_query(); query_timer_next(); deadline=c.sa_query_deadline;
 const u8 next_igtk[16]={0x42}; params.key=next_igtk;
 assert(!add(5,false) && !del(4,false));
 assert(c.sa_query_active && c.sa_query_count==1 && c.sa_query_deadline==deadline && r.sa_query_work.pending);
 reply_to_last_query(); assert_query_stopped(); assert(!c.cancel && c.authorized && !terminals);
 /* Wrong/unprotected/replayed/foreign/stale-owner responses never finish a query. */
 for(unsigned bad=0;bad<6;bad++) {
  sa_setup(); start_query(); query_timer_next();
  query_frame(WLAN_ACTION_SA_QUERY_RESPONSE,c.sa_query_ids[0][0],c.sa_query_ids[0][1],8);
  switch(bad) {
   case 0: packet[frame_offset+26]^=4; break;
   case 1: packet[frame_offset+1]&=~0x40; packet[9]=0; break;
   case 2: packet[16]=7; break; case 3: packet[frame_offset+10]^=4; break;
   case 4: c.sa_query_owner.generation++; break; case 5: c.sa_query_wtbl.cookie++; break;
  }
  query_receive(); assert(c.sa_query_active && !c.cancel && r.sa_query_work.pending);
 }
 /* A response at/after the deadline cannot cancel a timeout queued for this association. */
 sa_setup(); start_query(); query_timer_next(); fake_now_us=c.sa_query_deadline;
 reply_to_last_query(); assert(c.sa_query_active); query_timer_now();
 assert_query_stopped(); assert(c.cancel && !terminals);
 /* Matching response can precede TX-done, but never substitutes for TX ownership completion. */
 for(unsigned mode=QUERY_EARLY_RESPONSE;mode<=QUERY_REPLY_NO_DONE;mode++) {
  sa_setup(); sa_mode=mode; start_query(); query_timer_next(); assert_query_stopped();
  assert(sa_writes==1);
  if(mode==QUERY_REPLY_NO_DONE) assert(terminals==1 && c.cancel && c.ais.auth.pid_owner.held);
  else assert(!terminals && !c.cancel && !c.ais.auth.pid_owner.held);
 }
 /* No-credit IDs are removed; repeated credit failures stay bounded and time out locally. */
 sa_setup(); sa_mode=NO_CREDIT; start_query(); query_timer_next();
 assert(!c.sa_query_count && c.sa_query_attempts==1 && !terminals);
 reply_to_last_query(); assert(c.sa_query_active);
 for(unsigned i=1;i<5;i++) query_timer_next();
 assert(!c.sa_query_count && c.sa_query_attempts==5 && sa_writes==5);
 query_timer_next(); assert_query_stopped(); assert(c.cancel && !terminals);
 /* Failed over-the-air status still leaves a submitted ID available for an authenticated reply. */
 sa_setup(); sa_mode=FAILED_STATUS; start_query(); query_timer_next();
 assert(c.sa_query_count==1 && !terminals); reply_to_last_query(); assert_query_stopped();
 /* Transport uncertainty cancels timer state and fences before any retry. */
 for(unsigned mode=TIMEOUT;mode<=ABORTED;mode++) if(mode!=NO_CREDIT) {
  sa_setup(); sa_mode=mode; start_query(); query_timer_next(); assert_query_stopped();
  assert(c.cancel && terminals==1 && sa_writes==1); query_timer_now(); assert(sa_writes==1);
 }
 /* Key deletion/deauthorization, explicit disconnect and generation changes retire the query. */
 for(unsigned change=0;change<7;change++) {
  sa_setup(); start_query(); query_timer_next();
  switch(change) {
   case 0: assert(!del(0,true)); break; case 1: assert(!del(4,false)); break;
   case 2: assert(!authorize(false)); break;
   case 3: assert(!mt6797_radio_disconnect(NULL,&netdev,3)); break;
   case 4: c.key_pending.generation++; query_timer_next(); break;
   case 5: c.pairwise_wtbl.cookie++; query_timer_next(); break;
   case 6: r.stopping=true; query_timer_next(); break;
  }
  assert_query_stopped(); query_timer_now(); assert(sa_writes==1);
 }
 sa_setup(); start_query(); r.connection=NULL; query_timer_now(); assert(!sa_writes && !terminals);
 /* An obsolete timer callback sees only the replacement connection's current state. */
 sa_setup(); query_timer_now(); assert(!sa_writes && !c.sa_query_active);
 /* An AP request during our query is answered without changing our ID set/deadline. */
 sa_setup(); start_query(); query_timer_next(); deadline=c.sa_query_deadline;
 query_frame(WLAN_ACTION_SA_QUERY_REQUEST,0x55,0xaa,8); query_receive(); run_worker();
 assert(c.sa_query_active && c.sa_query_count==1 && c.sa_query_deadline==deadline && sa_writes==2);
 /* Zero and wraparound random candidates remain valid opaque, unique two-byte IDs. */
 for(unsigned value=0;value<2;value++) {
  sa_setup(); random_value=value?0xffff:0; start_query(); query_timer_next(); query_timer_next();
  assert(get_unaligned_le16(c.sa_query_ids[0])==(u16)random_value);
  assert(get_unaligned_le16(c.sa_query_ids[1])==(u16)(random_value+1));
 }
 puts("PASS: production SA Query initiation/timers: plaintext reason6/7 admission, fixed deadline/rate limit, generated unique bounded IDs, earlier/early/deadline response matching, five retries, no-credit rollback, local versus transport timeout, concurrent responder, key/owner/cancel/obsolete-work lifecycle");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-sa-query-') as temporary:
    directory = Path(temporary)
    (directory / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    *shlex.split(os.environ.get('OPENSSL_CFLAGS', '')),
                    str(directory / 'test.c'), '-o', str(directory / 'test'),
                    *shlex.split(os.environ.get('OPENSSL_LIBS', '-lcrypto'))], check=True)
    subprocess.run([str(directory / 'test')], check=True)
