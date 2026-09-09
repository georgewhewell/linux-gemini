#!/usr/bin/env python3
"""Validate PM notification codecs against GEN3 structs and bounded beacons.
No radio/firmware timing or workqueue concurrency is simulated here.
"""
from pathlib import Path
import re
import runpy

base = runpy.run_path(str(Path(__file__).with_name('test-wifi-join-host.py')))
program = base['program'].replace('int main(void) {', 'static int join_main(void) {')
for name in ['CMD_INDICATE_PM_BSS_CONNECTED', 'CMD_INDICATE_PM_BSS_ABORT']:
    tag = name + ('_T' if name.endswith('CONNECTED') else '')
    program += re.search(r'typedef struct _' + tag + r' \{.*?\} ' + name + r',.*?;', base['text'], re.S)[0] + '\n'
program += r'''
static void beacon(u8 *frame) {
 memset(frame,0,48); frame[0]=0x80;
 memcpy(frame+4,bss.own_addr,6); memcpy(frame+10,sta.peer_addr,6);
 memcpy(frame+16,sta.peer_addr,6);
 put_unaligned_le16(100,frame+32);
 frame[36]=5; frame[37]=4; frame[38]=1; frame[39]=3;
}
int main(void) {
 join_main();
 struct mt6797_wifi_ais_pm_params p;
 u8 frame[48],command[20],expected[20]; beacon(frame);
 for(size_t n=0;n<42;n++) {
  u8 *short_frame=malloc(n?n:1); memcpy(short_frame,frame,n);
  memset(&p,0xa5,sizeof(p));
  assert(mt6797_wifi_ais_pm_beacon_parse(short_frame,n,&p));
  assert(!p.dtim_period && !p.beacon_interval && !p.association_id);
  free(short_frame);
 }
 assert(!mt6797_wifi_ais_pm_beacon_parse(frame,42,&p));
 assert(p.dtim_period==3 && p.beacon_interval==100 && !p.association_id);
 const u16 flags[]={1,2,4,0x10,0x100,0x200,0x400,0x4000,0x8000};
 for(unsigned i=0;i<sizeof(flags)/sizeof(*flags);i++) {
  put_unaligned_le16(0x80|flags[i],frame);
  assert(mt6797_wifi_ais_pm_beacon_parse(frame,42,&p));
 }
 beacon(frame); frame[22]=1; assert(mt6797_wifi_ais_pm_beacon_parse(frame,42,&p));
 beacon(frame); frame[32]=0; assert(mt6797_wifi_ais_pm_beacon_parse(frame,42,&p));
 beacon(frame); frame[39]=0; assert(mt6797_wifi_ais_pm_beacon_parse(frame,42,&p));
 beacon(frame); frame[38]=3; assert(mt6797_wifi_ais_pm_beacon_parse(frame,42,&p));
 beacon(frame); frame[37]=3; assert(mt6797_wifi_ais_pm_beacon_parse(frame,41,&p));
 beacon(frame); memcpy(frame+42,frame+36,6); assert(mt6797_wifi_ais_pm_beacon_parse(frame,48,&p));
 beacon(frame); frame[42]=1; assert(mt6797_wifi_ais_pm_beacon_parse(frame,43,&p));
 beacon(frame); frame[36]=1; assert(mt6797_wifi_ais_pm_beacon_parse(frame,42,&p)==-ENOENT);
 beacon(frame); assert(!mt6797_wifi_ais_pm_beacon_parse(frame,42,&p)); p.association_id=123;
 for(unsigned connected=0;connected<2;connected++) {
  size_t length=connected?20:12;
  memset(expected,0,sizeof(expected));
  const u8 header[]={length,0,0,0x80,connected?0x16:0x17,0xa0,1,9};
  memcpy(expected,header,8);
  if(connected) {
   CMD_INDICATE_PM_BSS_CONNECTED vendor={0};
   _Static_assert(sizeof(vendor)==12,"vendor PM connected ABI changed");
   vendor.ucBssIndex=bss.bss_index; vendor.ucDtimPeriod=3;
   vendor.u2AssocId=123; vendor.u2BeaconInterval=100;
   memcpy(expected+8,&vendor,sizeof(vendor));
  } else {
   CMD_INDICATE_PM_BSS_ABORT vendor={0};
   _Static_assert(sizeof(vendor)==4,"vendor PM abort ABI changed");
   vendor.ucBssIndex=bss.bss_index; memcpy(expected+8,&vendor,sizeof(vendor));
  }
  memset(command,0xa5,sizeof(command));
  assert(!mt6797_wifi_ais_pm_build(&bss,&sta,&p,connected,9,command,sizeof(command)));
  assert(!memcmp(command,expected,length));
  for(size_t cap=0;cap<length;cap++) {
   memset(command,0xa5,sizeof(command));
   assert(mt6797_wifi_ais_pm_build(&bss,&sta,&p,connected,9,command,cap)==-EMSGSIZE);
   for(unsigned i=0;i<sizeof(command);i++)assert(command[i]==0xa5);
  }
  sta.generation++;
  assert(mt6797_wifi_ais_pm_build(&bss,&sta,&p,connected,9,command,sizeof(command)));
  sta.generation--;
 }
 struct mt6797_wifi_ais_pm_params bad=p;
 bad.association_id=0; assert(mt6797_wifi_ais_pm_build(&bss,&sta,&bad,true,9,command,20)==-EINVAL);
 bad=p; bad.association_id=2008; assert(mt6797_wifi_ais_pm_build(&bss,&sta,&bad,true,9,command,20)==-EINVAL);
 bad=p; bad.dtim_period=0; assert(mt6797_wifi_ais_pm_build(&bss,&sta,&bad,true,9,command,20)==-EINVAL);
 bad=p; bad.beacon_interval=0; assert(mt6797_wifi_ais_pm_build(&bss,&sta,&bad,true,9,command,20)==-EINVAL);
 assert(!mt6797_wifi_ais_pm_build(&bss,&sta,NULL,false,9,command,20));
 puts("PASS: full PM connected/abort command bytes match vendor ABI; beacon TIM bounds, duplicates, DTIM counts, flags, fragmentation, capacity and owner rejection");
}
'''
if __name__ == '__main__':
    base['common']['run_program'](program)
