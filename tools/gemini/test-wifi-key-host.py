#!/usr/bin/env python3
"""Compare production CCMP key command bytes with the vendor structure.
Uses synthetic key bytes only. Does not test encryption or replay protection.
Accepts the same optional vendor-root argument as test-wifi-join-host.py.
"""
from pathlib import Path
import re
import runpy

base = runpy.run_path(str(Path(__file__).with_name('test-wifi-join-host.py')))
program = base['program'].replace('int main(void) {', 'static int join_main(void) {')
vendor = base['vendor'].read_text()
program += re.search(r'typedef struct _CMD_802_11_KEY \{.*?\} CMD_802_11_KEY,.*?;', vendor, re.S)[0] + '\n'
program += base['source']('ais_key.h') + base['source']('ais_key.c')
program += r'''
static void key_test(bool pairwise,bool remove,unsigned index) {
 u8 key[16], command[80], expected[72]={72,0,0,0x80,7,0xa0,1,19};
 CMD_802_11_KEY body;
 struct mt6797_wifi_ais_key_params p={.index=index,.pairwise=pairwise,.remove=remove};
 _Static_assert(sizeof(body)==64,"vendor key layout changed");
 memset(&body,0,sizeof(body));
 for(unsigned i=0;i<16;i++) { key[i]=i+1; p.rsc[i]=0x80+i; }
 if(!remove) { p.material=key; p.material_length=16; }
 body.ucAddRemove=!remove; body.ucTxKey=!remove && pairwise; body.ucKeyType=pairwise;
 memcpy(body.aucPeerAddr,sta.peer_addr,6); body.ucBssIdx=bss.bss_index;
 body.ucAlgorithmId=remove?0:4; body.ucKeyId=index;
 body.ucWlanIndex=pairwise?sta.pairwise.index:sta.ap_bmc_group.index;
 if(!remove) {
  body.ucKeyLen=16; memcpy(body.aucKeyMaterial,key,16); memcpy(body.aucKeyRsc,p.rsc,16);
 }
 memcpy(expected+8,&body,sizeof(body));
 memset(command,0xa5,sizeof(command));
 assert(!mt6797_wifi_ais_ccmp_key_build(&bss,&sta,&p,19,command,sizeof(command)));
 assert(!memcmp(command,expected,72));
 for(unsigned i=72;i<sizeof(command);i++) assert(command[i]==0xa5);
 if(remove) for(unsigned i=24;i<72;i++) assert(command[i]==0);
 for(size_t capacity=0;capacity<72;capacity++) {
  memset(command,0xa5,sizeof(command));
  assert(mt6797_wifi_ais_ccmp_key_build(&bss,&sta,&p,19,command,capacity)==-EMSGSIZE);
  for(unsigned i=0;i<sizeof(command);i++) assert(command[i]==0xa5);
 }
}
static void invalid(struct mt6797_wifi_ais_key_params *p) {
 u8 command[72], unchanged[72]; memset(command,0xa5,sizeof(command));
 memcpy(unchanged,command,sizeof(command));
 assert(mt6797_wifi_ais_ccmp_key_build(&bss,&sta,p,0,command,sizeof(command))==-EINVAL);
 assert(!memcmp(command,unchanged,sizeof(command)));
}
int main(void) {
 join_main();
 key_test(true,false,0); key_test(true,true,0);
 for(unsigned index=0;index<4;index++) { key_test(false,false,index); key_test(false,true,index); }
 u8 key[32]={0}; struct mt6797_wifi_ais_key_params p={.material=key,.material_length=16};
 for(size_t n=0;n<=32;n++) if(n!=16) { p.material_length=n; invalid(&p); }
 p.material_length=16; p.index=4; invalid(&p);
 p.index=1; p.pairwise=true; invalid(&p);
 p.index=0; p.remove=true; invalid(&p);
 p.remove=false; p.material=NULL; invalid(&p); p.material=key;
 sta.bss_cookie++; invalid(&p); sta.bss_cookie--;
 sta.ap_bmc_group.index=sta.pairwise.index; invalid(&p); sta.ap_bmc_group.index=2;
 struct mt6797_wifi_ais_sta_pending pending;
 assert(!mt6797_wifi_ais_sta_pending_init(&bss,&sta,19,&pending));
 u8 payload[8]={0}; payload[0]=bss.bss_index; memcpy(payload+2,sta.peer_addr,6);
 struct mt6797_wifi_event_view event={.id=0x24,.payload=payload,.payload_length=8,.packet_length=16};
 enum mt6797_wifi_ais_event_disposition disposition;
 for(unsigned reserved=0;reserved<=255;reserved++) for(unsigned sequence=0;sequence<=255;sequence++) {
  payload[1]=reserved; event.sequence=sequence;
  assert(!mt6797_wifi_ais_ccmp_ready_match(&pending,&event,&disposition));
  assert(disposition==MT6797_WIFI_AIS_EVENT_COMPLETE);
 }
 payload[0]++; assert(!mt6797_wifi_ais_ccmp_ready_match(&pending,&event,&disposition));
 assert(disposition==MT6797_WIFI_AIS_EVENT_IGNORED); payload[0]--;
 payload[2]^=4; assert(!mt6797_wifi_ais_ccmp_ready_match(&pending,&event,&disposition));
 assert(disposition==MT6797_WIFI_AIS_EVENT_IGNORED); payload[2]^=4;
 event.id=1; assert(!mt6797_wifi_ais_ccmp_ready_match(&pending,&event,&disposition));
 assert(disposition==MT6797_WIFI_AIS_EVENT_IGNORED); event.id=0x24;
 for(unsigned n=0;n<8;n++) {
  event.payload_length=n; event.packet_length=n+8;
  assert(mt6797_wifi_ais_ccmp_ready_match(&pending,&event,&disposition)==-EPROTO);
  assert(disposition==MT6797_WIFI_AIS_EVENT_TERMINAL);
 }
 pending.generation=0;
 assert(mt6797_wifi_ais_ccmp_ready_match(&pending,&event,&disposition)==-EINVAL);
 puts("PASS: vendor key ABI, pairwise/AP-group slot selection, all GTK IDs, add/remove zeroing, raw RSC copy, stale ownership and exact key/capacity bounds (synthetic material only)");
 puts("PASS: unsolicited pairwise-ready event matches BSS/AP, ignores foreign events and reserved/sequence variations, rejects truncated events/invalid ownership");
}
'''
base['common']['run_program'](program)
