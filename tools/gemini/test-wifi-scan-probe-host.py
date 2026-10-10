#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual scan V2 packing and mixed regulatory scan worker phases.

The underlying fixture supplies synchronous HIF/IRQ fakes. Wire offsets come
from public GEN3 CMD_SCAN_REQ_V2/PARAM_SSID declarations and sender behavior,
not from offsets exported by the implementation being tested.
"""
from pathlib import Path
import os
import runpy
import shlex
import subprocess
import tempfile

base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-scan-cancel-host.py')))
program = base['program'].split('static void deliver_done(')[0].replace(
    'static int action,last_fault;', 'static int last_fault;')
program += r'''
static u32 le32(const u8 *b) { return b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24; }
static void zero(const u8 *p,size_t n) { for(size_t i=0;i<n;i++) assert(!p[i]); }
static struct cfg80211_scan_request *current;
static unsigned visited,cancel_phase,timeout_phase,coverage[4];
static bool mixed;
static int mt6797_wifi_hif_runtime_send(struct mt6797_wifi_hif_irq *h,
 u32 address,const u8 *packet,size_t length) {
 assert(h==&irq && h->lifecycle_lock && !r.lock && address==0x34);
 if(packet[4]==0x1b) { assert(length==12); cancels++; return 0; }
 assert(packet[4]==3 && r.armed && get_unaligned_le16(packet)==length);
 writes++;
 unsigned phase=mixed?(writes<=2?1:writes==3?2:3):writes;
 bool active=phase!=2, broadcast=mixed && (writes==2 || writes==5);
 unsigned first=phase==1?0:phase==2?1:3,count=phase==2?2:1;
 assert(packet[10]==active && packet[167]==count);
 assert(length==(active?240:236));
 assert(packet[11]==(!active || broadcast?1:4));
 assert(!packet[12] && !packet[13] && !packet[14] && !packet[15]);
 if(!active || broadcast) zero(packet+16,144);
 if(!active) assert(!get_unaligned_le16(packet+232));
 else {
  if(!broadcast) {
  assert(le32(packet+16)==32 && !memcmp(packet+20,current->ssids[0].ssid,32));
  assert(le32(packet+52)==3 && !memcmp(packet+56,current->ssids[1].ssid,3));
  if(mixed) { assert(le32(packet+88)==1 && packet[92]==current->ssids[3].ssid[0]); zero(packet+124,36); }
  else { assert(le32(packet+88)==2 && !memcmp(packet+92,current->ssids[2].ssid,2)); assert(le32(packet+124)==1 && packet[128]==current->ssids[3].ssid[0]); }
  }
  assert(get_unaligned_le16(packet+232)==3);
  assert(!memcmp(packet+234,current->ie,3)); zero(packet+237,3);
 }
 for(unsigned i=0;i<packet[167];i++,visited++) {
  assert(first+i<current->n_channels); coverage[first+i]++;
  struct ieee80211_channel *channel=current->channels[first+i];
  assert(packet[168+2*i]==(channel->band?2:1) && packet[169+2*i]==channel->hw_value);
  assert(packet[10]==!(channel->flags&(IEEE80211_CHAN_NO_IR|IEEE80211_CHAN_RADAR)));
 }
 return 0;
}
static unsigned long wait_for_completion_timeout(struct completion *completion,
 unsigned long timeout) {
 assert(completion==&r.scan_done && timeout==30000 && r.armed && !r.lock);
 waits++;
 if(cancel_phase==waits) {
  mt6797_radio_abort_scan(NULL,&r.wdev);
  assert(r.abort_scan && !completion->done && !r.scan_error);
 }
 if(timeout_phase==waits) return 0;
 u8 payload[4]={r.scan.scan_sequence-1,0,0,0};
 struct mt6797_wifi_event_view event={.id=0x0d,.payload=payload,.payload_length=4};
 assert(!mt6797_radio_event(&r,&event) && !completion->done);
 payload[0]++; assert(!mt6797_radio_event(&r,&event) && completion->done);
 return 1;
}
static void setup(struct cfg80211_scan_request *request,bool on_link) {
 memset(&r,0,sizeof(r)); memset(&irq,0,sizeof(irq));
 writes=cancels=fences=waits=callbacks=attaches=detaches=visited=0;
 memset(coverage,0,sizeof(coverage));
 cancel_phase=timeout_phase=0; last_fault=0; reported_aborted=false; irq.mode=2;
 r.irq=&irq; r.request=current=request; r.scan_on_link=on_link;
 static struct mt6797_radio_connection connection;
 memset(&connection,0,sizeof(connection)); connection.connected=true;
 if(on_link) r.connection=&connection;
}
static void packing(void) {
 struct mt6797_wifi_scan_channel channels[32];
 for(unsigned i=0;i<32;i++) channels[i]=(struct mt6797_wifi_scan_channel){.band=2,.number=36+i};
 struct mt6797_wifi_scan_ssid ssids[4];
 memset(ssids,0,sizeof(ssids));
 for(unsigned i=0;i<4;i++) { ssids[i].length=32; for(unsigned j=0;j<32;j++) ssids[i].bytes[j]=(u8)(i*64+j); }
 u8 ies[600]={0};
 struct mt6797_wifi_scan_probe probe={.active=true,.ssids=ssids,.ssid_count=4,.ies=ies,.ie_length=600};
 u8 command[840],passive[236]; size_t length=0;
 memset(command,0xa5,sizeof(command));
 struct mt6797_wifi_scan_transaction transaction={0};
 assert(!mt6797_wifi_scan_start_probe(&transaction,channels,32,&probe,3,7,command,836,&length));
 assert(length==836 && get_unaligned_le16(command)==836 && command[4]==3 && command[7]==3 && command[8]==7);
 assert(command[10]==1 && command[11]==4 && command[166]==4 && command[167]==32);
 zero(command+12,4); /* The public GEN3 sender leaves ucSSIDNum/probe count zero. */
 for(unsigned i=0;i<4;i++) {
  assert(le32(command+16+i*36)==32 && !memcmp(command+20+i*36,ssids[i].bytes,32));
 }
 assert(get_unaligned_le16(command+232)==600 && !memcmp(command+234,ies,600));
 zero(command+834,2); for(unsigned i=836;i<840;i++) assert(command[i]==0xa5);
 /* Passive phases have no probe payload and remain byte-exact with the old API. */
 transaction=(struct mt6797_wifi_scan_transaction){0}; probe.active=false;
 assert(!mt6797_wifi_scan_start_probe(&transaction,channels,32,&probe,3,7,command,836,&length));
 assert(length==236 && !command[10] && command[11]==1 && !get_unaligned_le16(command+232)); zero(command+16,144);
 transaction=(struct mt6797_wifi_scan_transaction){0};
 assert(!mt6797_wifi_scan_start(&transaction,channels,32,3,7,passive,sizeof(passive)));
 assert(!memcmp(command,passive,236));
 /* Every legal IE length checks DWORD padding and exact send length. */
 probe.active=true; probe.ssid_count=1; ssids[0].length=0;
 for(unsigned n=0;n<=600;n++) {
  memset(ies,0,sizeof(ies)); if(n>=3 && (n&1)) ies[1]=1;
  probe.ie_length=n; transaction=(struct mt6797_wifi_scan_transaction){0};
  memset(command,0xa5,sizeof(command)); length=999;
  int ret=mt6797_wifi_scan_start_probe(&transaction,channels,1,&probe,3,7,command,836,&length);
  if(n==1) { assert(ret==-EINVAL && transaction.state==0 && length==999 && command[0]==0xa5); continue; }
  assert(!ret && length==((234+n+3)&~3U) && command[10]==1 && command[11]==1);
  assert(get_unaligned_le16(command+232)==n); zero(command+234+n,length-234-n);
  assert(command[length]==0xa5);
 }
 for(unsigned bad=0;bad<9;bad++) {
  memset(ies,0,sizeof(ies)); probe=(struct mt6797_wifi_scan_probe){.active=true,.ssids=ssids,.ssid_count=1,.ies=ies,.ie_length=2};
  ssids[0].length=1; size_t capacity=836;
  if(bad==0) probe.ssid_count=5;
  if(bad==1) probe.ssids=NULL;
  if(bad==2) ssids[0].length=33;
  if(bad==3) probe.ie_length=601;
  if(bad==4) probe.ies=NULL;
  if(bad==5) ies[1]=1;
  if(bad==6) probe.ie_length=1;
  if(bad==7) capacity=235;
  if(bad==8) { probe.ie_length=3; ies[1]=1; capacity=239; }
  transaction=(struct mt6797_wifi_scan_transaction){0}; memset(command,0xa5,sizeof(command)); length=999;
  assert(mt6797_wifi_scan_start_probe(&transaction,channels,1,&probe,3,7,command,capacity,&length)==(bad>=7?-EMSGSIZE:-EINVAL));
  assert(transaction.state==0 && length==999 && command[0]==0xa5);
 }
}
int main(void) {
 packing();
 struct ieee80211_channel channels[4]={
  {.band=0,.hw_value=1}, {.band=1,.hw_value=36,.flags=IEEE80211_CHAN_NO_IR},
  {.band=1,.hw_value=52,.flags=IEEE80211_CHAN_RADAR}, {.band=1,.hw_value=100}};
 struct ieee80211_channel *pointers[4]={&channels[0],&channels[1],&channels[2],&channels[3]};
 struct cfg80211_ssid ssids[4]={0};
 ssids[0].ssid_len=32; for(unsigned j=0;j<32;j++) ssids[0].ssid[j]=(u8)(128+j);
 ssids[1].ssid_len=3; ssids[1].ssid[0]=0; ssids[1].ssid[1]=255; ssids[1].ssid[2]=1;
 ssids[3].ssid_len=1; ssids[3].ssid[0]=7;
 u8 ies[3]={127,1,0};
 struct cfg80211_scan_request request={.n_channels=4,.channels=pointers,.n_ssids=4,.ssids=ssids,.ie=ies,.ie_len=3};
 for(unsigned variant=0;variant<2;variant++) {
 mixed=variant; ssids[2].ssid_len=mixed?0:2; ssids[2].ssid[0]=1; ssids[2].ssid[1]=255;
 unsigned phases=mixed?5:3;
 for(unsigned on_link=0;on_link<2;on_link++) {
  setup(&request,on_link); mt6797_radio_scan_work(&r.scan_work);
  assert(writes==phases && waits==phases && visited==(mixed?6:4) && !cancels && !fences && callbacks==1 && !reported_aborted);
  assert(coverage[0]==(mixed?2:1) && coverage[1]==1 && coverage[2]==1 && coverage[3]==(mixed?2:1));
  assert(attaches==!on_link && detaches==!on_link);
  for(unsigned phase=1;phase<=phases;phase++) {
   setup(&request,on_link); cancel_phase=phase; mt6797_radio_scan_work(&r.scan_work);
   assert(writes==phase && waits==phase && !cancels && !fences && callbacks==1 && reported_aborted);
   assert(!r.request && !r.armed && !r.lock && !irq.lifecycle_lock);
   setup(&request,on_link); timeout_phase=phase; mt6797_radio_scan_work(&r.scan_work);
   assert(writes==phase && waits==phase && cancels==1 && fences==1 && last_fault==-ETIMEDOUT && callbacks==1 && reported_aborted);
   assert(!r.request && !r.armed && !r.lock && !irq.lifecycle_lock);
  }
 }
 }
 puts("PASS: GEN3 directed/wildcard/four-SSID wire packing, 32-channel and 600-byte IE bounds, all legal IE alignment/padding, rejection before ownership, byte-exact passive API, mixed named/wildcard probe coverage, NO_IR/DFS channel coverage and cancellation/timeout fences across every active/passive scan phase off-link and on-link");
}
'''
with tempfile.TemporaryDirectory(prefix='wifi-scan-probe-') as temporary:
    source = Path(temporary) / 'scan-probe.c'
    binary = Path(temporary) / 'scan-probe'
    source.write_text(program)
    subprocess.run(shlex.split(os.environ.get('HOSTCC', 'cc')) + [
        '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-Wno-unused-parameter', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
