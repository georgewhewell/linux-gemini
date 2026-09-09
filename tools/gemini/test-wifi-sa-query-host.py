#!/usr/bin/env python3
"""Production authenticated SA Query RX, response worker, TX codec and completion.

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
 FAILED_STATUS, TIMEOUT, TRANSPORT, NO_CREDIT, ABORTED, MALFORMED_THEN_VALID, DUPLICATE, INFLIGHT_REQUEST };
static enum sa_mode sa_mode;
static unsigned sa_writes;
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
 assert(!mt6797_wifi_rx_parse(packet,packet_length,&v));
 assert(!mt6797_radio_link_frame(&r,&v));
 assert(!r.lock);
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
 return sa_mode==TRANSPORT?-EIO:sa_mode==NO_CREDIT?-EAGAIN:0;
}
static unsigned long sa_wait(struct completion *completion,unsigned long timeout) {
 assert(completion==&c.management_completion && timeout==1000);
 assert(irq.lifecycle_lock && r.connection_lock.held && !r.lock);
 if(sa_mode==ABORTED) mt6797_radio_link_abort(&r,-ESHUTDOWN);
 else if(sa_mode==MALFORMED || sa_mode==MALFORMED_THEN_VALID) {
  assert(sa_offer(MALFORMED)==-EPROTO);
  if(sa_mode==MALFORMED_THEN_VALID) assert(!sa_offer(IMMEDIATE));
 }
 else if(sa_mode!=TIMEOUT) assert(!sa_offer(sa_mode));
 return completion->done;
}
#define mt6797_wifi_hif_runtime_send sa_send
#define wait_for_completion_timeout sa_wait
'''
for name in ['mt6797_radio_management_ready', 'mt6797_radio_sa_query_response',
             'mt6797_radio_management_work']:
    program += key['function'](name)
program += r'''
static void sa_setup(void) {
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
 for(unsigned change=0;change<11;change++) {
  sa_setup(); query_receive();
  switch(change) {
   case 0: c.cancel=true; break; case 1: r.stopping=true; break;
   case 2: c.pmf_negotiated=false; break; case 3: c.pairwise_ready=false; break;
   case 4: c.pairwise_key.installed=false; break; case 5: c.pairwise_key.updating=true; break;
   case 6: c.key_pending.generation++; break; case 7: c.pairwise_wtbl.cookie++; break;
   case 8: c.igtk.installed=false; break; case 9: c.connected=false; break;
   case 10: r.connection=NULL; break;
  }
  run_worker(); assert(!sa_writes && !terminals);
 }
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
