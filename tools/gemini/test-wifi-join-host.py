#!/usr/bin/env python3
"""Compare production join commands with structs extracted from vendor GEN3.

Usage: test-wifi-join-host.py [vendor-kernel-root]
Requires the separately obtained vendor source; tests do not contact hardware.
"""
from pathlib import Path
import re
import runpy
import sys

common = runpy.run_path(str(Path(__file__).with_name('test-wifi-assoc-host.py')))
program = common['program'].replace('int main(void) {', 'static int assoc_main(void) {')
source = common['source']
driver = common['driver']
vendor_root = Path(sys.argv[1] if len(sys.argv) > 1 else '/home/grw/src/gemini-linux-kernel-3.18')
vendor = vendor_root / 'drivers/misc/mediatek/connectivity/wlan/gen3/include/nic_cmd_event.h'
text = vendor.read_text()
program += '\ntypedef u8 UINT_8; typedef u16 UINT_16; typedef u32 UINT_32;\n#define MAC_ADDR_LEN 6\n'
for name in ['CMD_SET_BSS_RLM_PARAM_T', 'CMD_SET_BSS_INFO', 'CMD_UPDATE_STA_RECORD_T']:
    program += re.search(r'typedef struct _' + name + r' \{.*?\} ' + name + r',.*?;', text, re.S)[0] + '\n'
protocol = (driver / 'protocol.c').read_text()
for name in ['MT6797_WIFI_COMMAND_PQ_ID', 'MT6797_WIFI_COMMAND_PACKET_TYPE']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', protocol, re.M)[0] + '\n'
program += protocol[protocol.index('int mt6797_wifi_command_init('):protocol.index('int mt6797_wifi_basic_config_build(')]
ieee = (common['root'] / 'include/linux/ieee80211.h').read_text()
for name in ['WLAN_ERP_USE_PROTECTION', 'WLAN_ERP_BARKER_PREAMBLE',
             'WLAN_CAPABILITY_SHORT_PREAMBLE', 'WLAN_CAPABILITY_SHORT_SLOT_TIME']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', ieee, re.M)[0] + '\n'
program += '#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))\n#define min(a,b) ((a)<(b)?(a):(b))\n'
definitions = (vendor.parent / 'nic/wlan_def.h').read_text()
for line in definitions.splitlines():
    if re.match(r'#define (TX_MODE_(CCK|OFDM)|PHY_RATE_(1M|2M|5_5M|11M|6M|9M|12M|18M|24M|36M|48M|54M)|RATE_CCK_\w+_LONG|RATE_OFDM_\w+)\s', line):
        program += line + '\n'
program += re.search(r'typedef enum _ENUM_SW_RATE_INDEX_T \{.*?\}.*?;', definitions, re.S)[0] + '\n'
program += source('ais_join.h') + source('ais_join.c')
program += r'''
static struct mt6797_wifi_ais_join_params join={
 .channel={.band=1,.primary=6},.ssid={'t','e','s','t'},.ssid_length=4,
 .phy_types=3,.basic_phy=1,.rcpi=100,.disconnect_threshold=10,
 .association_id=123,.listen_interval=10,.operational_rates=0x3fcf,
 .desired_rates=0x3fcf,.basic_rates=0x000f,.default_rate=0,
 .security=MT6797_WIFI_AIS_SECURITY_OPEN,.erp_protection=true,
 .short_preamble=true,.short_slot=true
};
static void golden(const struct mt6797_wifi_ais_join_params *p) {
 u8 command[132], expected[132]={0};
 CMD_SET_BSS_INFO b;
 CMD_UPDATE_STA_RECORD_T s;
 struct mt6797_wifi_ais_sta_pending owner;
 memset(&b,0,sizeof(b)); memset(&s,0,sizeof(s));
 _Static_assert(sizeof(b)==88,"vendor BSS ABI changed");
 _Static_assert(sizeof(s)==124,"vendor STA ABI changed");
 b.ucBssIndex=bss.bss_index;
 b.ucConnectionState=0; b.ucCurrentOPMode=0;
 b.ucSSIDLen=p->ssid_length; memcpy(b.aucSSID,p->ssid,p->ssid_length);
 memcpy(b.aucBSSID,sta.peer_addr,6);
 b.u2OperationalRateSet=p->operational_rates; b.u2BSSBasicRateSet=p->basic_rates;
 b.ucStaRecIdxOfAP=sta.sta_index; b.u2HwDefaultFixedRateCode=p->default_rate;
 b.ucNonHTBasicPhyType=p->basic_phy;
 b.ucAuthMode=p->security==MT6797_WIFI_AIS_SECURITY_OPEN?0:7;
 b.ucEncStatus=p->security==MT6797_WIFI_AIS_SECURITY_OPEN?1:6;
 b.ucPhyTypeSet=p->phy_types; b.ucBMCWlanIndex=bss.bmc.index;
 b.ucDisconnectDetectTh=p->disconnect_threshold;
 b.rBssRlmParam.ucBssIndex=bss.bss_index;
 b.rBssRlmParam.ucRfBand=p->channel.band;
 b.rBssRlmParam.ucPrimaryChannel=p->channel.primary;
 b.rBssRlmParam.ucErpProtectMode=p->erp_protection;
 b.rBssRlmParam.ucUseShortPreamble=p->short_preamble;
 b.rBssRlmParam.ucUseShortSlotTime=p->short_slot;
 b.rBssRlmParam.u2VhtBasicMcsSet=p->basic_rates;
 const u8 bss_header[]={96,0,0,0x80,0x12,0xa0,1,9};
 memcpy(expected,bss_header,8); memcpy(expected+8,&b,sizeof(b));
 memset(command,0xa5,sizeof(command));
 assert(!mt6797_wifi_ais_join_bss_build(&bss,&sta,p,9,command,sizeof(command)));
 assert(!memcmp(command,expected,96) && command[96]==0xa5);
 s.ucStaIndex=sta.sta_index; s.ucStaType=0x41; memcpy(s.aucMacAddr,sta.peer_addr,6);
 s.u2AssocId=p->association_id; s.u2ListenInterval=p->listen_interval;
 s.ucBssIndex=bss.bss_index; s.ucDesiredPhyTypeSet=p->phy_types;
 s.u2DesiredNonHTRateSet=p->desired_rates; s.u2BSSBasicRateSet=p->basic_rates;
 s.ucStaState=2; s.ucRCPI=p->rcpi; s.ucNeedResp=1;
 s.ucWlanIndex=sta.pairwise.index; s.ucBMCWlanIndex=sta.ap_bmc_group.index;
 s.u2HwDefaultFixedRateCode=p->default_rate;
 const u8 sta_header[]={132,0,0,0x80,0x13,0xa0,1,10};
 memcpy(expected,sta_header,8); memcpy(expected+8,&s,sizeof(s));
 assert(!mt6797_wifi_ais_join_sta_build(&bss,&sta,p,10,command,sizeof(command),&owner));
 assert(!memcmp(command,expected,132));
 assert(owner.command_sequence==10 && owner.sta_cookie==sta.cookie &&
        owner.bss_cookie==bss.cookie && owner.generation==sta.generation);
 /* Short buffers must not expose a command or pending owner. */
 for(size_t capacity=0;capacity<132;capacity++) {
  memset(command,0xa5,sizeof(command));
  assert(mt6797_wifi_ais_join_sta_build(&bss,&sta,p,10,command,capacity,&owner)==-EMSGSIZE);
  assert(!owner.generation);
  for(size_t i=0;i<sizeof(command);i++) assert(command[i]==0xa5);
  if(capacity<96) {
   assert(mt6797_wifi_ais_join_bss_build(&bss,&sta,p,9,command,capacity)==-EMSGSIZE);
   for(size_t i=0;i<sizeof(command);i++) assert(command[i]==0xa5);
  }
 }
}
static void reject(const struct mt6797_wifi_ais_join_params *p) {
 u8 command[132], unchanged[132];
 struct mt6797_wifi_ais_sta_pending owner={.generation=99};
 memset(command,0xa5,sizeof(command)); memcpy(unchanged,command,sizeof(command));
 assert(mt6797_wifi_ais_join_bss_build(&bss,&sta,p,0,command,sizeof(command))==-EINVAL);
 assert(!memcmp(command,unchanged,sizeof(command)));
 assert(mt6797_wifi_ais_join_sta_build(&bss,&sta,p,0,command,sizeof(command),&owner)==-EINVAL);
 assert(!owner.generation && !memcmp(command,unchanged,sizeof(command)));
}
static void rates_test(void) {
 const u8 values[]={2,4,11,22,12,18,24,36,48,72,96,108};
 const unsigned indices[]={RATE_1M_SW_INDEX,RATE_2M_SW_INDEX,RATE_5_5M_SW_INDEX,
  RATE_11M_SW_INDEX,RATE_6M_SW_INDEX,RATE_9M_SW_INDEX,RATE_12M_SW_INDEX,
  RATE_18M_SW_INDEX,RATE_24M_SW_INDEX,RATE_36M_SW_INDEX,RATE_48M_SW_INDEX,RATE_54M_SW_INDEX};
 const u16 codes[]={RATE_CCK_1M_LONG,RATE_CCK_2M_LONG,RATE_CCK_5_5M_LONG,
  RATE_CCK_11M_LONG,RATE_OFDM_6M,RATE_OFDM_9M,RATE_OFDM_12M,RATE_OFDM_18M,
  RATE_OFDM_24M,RATE_OFDM_36M,RATE_OFDM_48M,RATE_OFDM_54M};
 for(unsigned i=0;i<12;i++) {
  u8 peer[]={1,1,values[i]|0x80};
  struct mt6797_wifi_ais_join_params p=join;
  assert(!mt6797_wifi_ais_join_rates_parse(peer,sizeof(peer),0x420,&p));
  assert(p.operational_rates==(1U<<indices[i]) && p.basic_rates==p.operational_rates);
  assert(p.desired_rates==p.operational_rates && p.default_rate==codes[i]);
  assert(p.ssid_length==join.ssid_length && !memcmp(p.ssid,join.ssid,32));
  golden(&p);
 }
 /* Exhaustively round-trip all nonempty subsets of the supported rate set. */
 for(unsigned subset=1;subset<4096;subset++) {
  struct mt6797_wifi_ais_join_params p=join, parsed=join;
  p.desired_rates=0;
  for(unsigned i=0;i<12;i++) if(subset&(1U<<i)) p.desired_rates|=1U<<indices[i];
  p.basic_rates=p.desired_rates;
  u8 stream[16], shortbuf[16]; size_t len;
  assert(!mt6797_wifi_ais_join_rates_ies_build(&p,stream,sizeof(stream),&len));
  assert(len<=16 && stream[1]<=8);
  assert(!mt6797_wifi_ais_join_rates_parse(stream,len,0,&parsed));
  assert(parsed.operational_rates==p.desired_rates && parsed.basic_rates==p.basic_rates);
  for(size_t cap=0;cap<len;cap++) {
   size_t n=99; memset(shortbuf,0xa5,sizeof(shortbuf));
   assert(mt6797_wifi_ais_join_rates_ies_build(&p,shortbuf,cap,&n)==-EMSGSIZE);
   assert(!n); for(unsigned i=0;i<sizeof(shortbuf);i++) assert(shortbuf[i]==0xa5);
  }
 }
 const u8 erp[]={1,1,0x82,42,1,6};
 struct mt6797_wifi_ais_join_params p=join;
 assert(!mt6797_wifi_ais_join_rates_parse(erp,sizeof(erp),0x420,&p));
 assert(p.erp_protection && !p.short_preamble && p.short_slot);
 const u8 bad[][10]={{1,2,0x82},{1,0},{50,1,0x82},{1,1,0x82,1,1,0x84},
  {1,1,0x82,42,0},{1,1,0xff},{1,1,2},{1,2,0x82,0xac}};
 const size_t lengths[]={3,2,3,6,5,3,3,4};
 const int errors[]={-EPROTO,-EPROTO,-EPROTO,-EPROTO,-EPROTO,
  -EOPNOTSUPP,-EOPNOTSUPP,-EOPNOTSUPP};
 for(unsigned i=0;i<8;i++) {
  p=join;
  assert(mt6797_wifi_ais_join_rates_parse(bad[i],lengths[i],0,&p)==errors[i]);
  assert(!memcmp(&p,&join,sizeof(p)));
 }
 const u8 five[]={1,4,2,0x8c,0x98,0xb0};
 p=join; p.channel.band=2; p.channel.primary=36;
 assert(!mt6797_wifi_ais_join_rates_parse(five,sizeof(five),0,&p));
 assert(p.operational_rates==0x540 && p.default_rate==RATE_OFDM_6M);
 assert(p.phy_types==8 && p.basic_phy==3 && p.short_slot && !p.short_preamble);
 golden(&p);
 const u8 bad_five[]={1,2,0x82,0x8c};
 assert(mt6797_wifi_ais_join_rates_parse(bad_five,sizeof(bad_five),0,&p)==-EOPNOTSUPP);
 puts("PASS: all vendor legacy rate codes/indices, 4095 rate subsets, supported/extended IE packing, ERP/Barker handling, unsupported mandatory rates and 5GHz filtering");
}
int main(void) {
 const uint16_t endian=1; assert(*(const u8 *)&endian==1);
 assoc_main();
 rates_test();
 golden(&join);
 join.security=MT6797_WIFI_AIS_SECURITY_WPA2_PSK_CCMP;
 golden(&join);
 struct mt6797_wifi_ais_join_params five=join;
 five.channel.band=2; five.channel.primary=36; five.phy_types=8; five.basic_phy=3;
 five.operational_rates=0x3fc0; five.desired_rates=0x3fc0; five.basic_rates=0x540;
 five.erp_protection=false; five.short_preamble=false;
 golden(&five);
 reject(NULL);
 struct mt6797_wifi_ais_join_params p;
 #define BAD(field,value) do { p=join; p.field=(value); reject(&p); } while(0)
 BAD(ssid_length,0); BAD(ssid_length,33); BAD(association_id,0);
 BAD(association_id,2008); BAD(listen_interval,0); BAD(operational_rates,0);
 BAD(operational_rates,0x3fff); BAD(desired_rates,0); BAD(desired_rates,0x8000);
 BAD(basic_rates,0); BAD(basic_rates,0x8000); BAD(default_rate,0x1000);
 BAD(rcpi,221); BAD(security,99); BAD(channel.band,3); BAD(channel.primary,0);
 BAD(channel.primary,15); BAD(phy_types,16); BAD(basic_phy,3);
 p=five; p.operational_rates|=1; reject(&p);
 p=five; p.erp_protection=true; reject(&p);
 sta.generation++; reject(&join); sta.generation--;
 sta.ap_bmc_group.index=sta.pairwise.index; reject(&join); sta.ap_bmc_group.index=2;
 puts("PASS: BSS/STA full command bytes against vendor structures, open/WPA2 and 2.4/5GHz, padding, identity preservation, state3 response request and invalid/capacity rejection");
}
'''
common['run_program'](program)
