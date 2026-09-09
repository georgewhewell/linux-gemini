#!/usr/bin/env python3
"""Production PMF RX, metadata, BIP formatting and replay with synthetic frames.

Uses OpenSSL as an independent host AES-CMAC primitive in place of libaes.
The driver decoder, authenticated-data construction, comparison, state mutation,
key lifecycle and receive callbacks are actual production code. No firmware or
IRQ concurrency is simulated. Set OPENSSL_CFLAGS/OPENSSL_LIBS for a custom SDK.
"""
from pathlib import Path
import os
import re
import runpy
import shlex
import subprocess
import tempfile

base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-key-host.py')))
driver = base['driver']
root = driver.parents[4]
program = base['program'].replace('int main(void) {', 'static int key_main(void) {')
program = re.sub(r'static u64 mt6797_wifi_data_rx_pn\([^\n]*\n', '', program)
data = (driver / 'data_rx.c').read_text()
# This function is needed by earlier production key callbacks too.
program = program.replace('#define WLAN_CIPHER_SUITE_CCMP',
    'u64 mt6797_wifi_data_rx_pn(const u8 sequence[6]);\n#define WLAN_CIPHER_SUITE_CCMP', 1)
program += data[data.index('u64 mt6797_wifi_data_rx_pn('):data.index('int mt6797_wifi_data_rx_decode(')]
program += '#define ETH_HLEN 14\n'
protocol = (driver / 'protocol.c').read_text()
for line in protocol.splitlines():
    if line.startswith('#define MT6797_WIFI_RX_') or line.startswith('#define MT6797_WIFI_EVENT_PACKET_TYPE'):
        program += line + '\n'
for name in ['mt6797_wifi_event_parse', 'mt6797_wifi_rx_group_offset', 'mt6797_wifi_rx_parse']:
    program += base['extract'](protocol, name)
ieee = (root / 'include/linux/ieee80211.h').read_text()
for name in ['IEEE80211_STYPE_ACTION', 'IEEE80211_FCTL_RETRY', 'IEEE80211_FCTL_PM',
             'IEEE80211_FCTL_MOREDATA']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', ieee, re.M)[0] + '\n'
for name in ['WLAN_CATEGORY_PUBLIC', 'WLAN_CATEGORY_HT', 'WLAN_CATEGORY_WNM_UNPROTECTED',
             'WLAN_CATEGORY_SELF_PROTECTED', 'WLAN_CATEGORY_UNPROT_DMG',
             'WLAN_CATEGORY_VHT', 'WLAN_CATEGORY_S1G', 'WLAN_CATEGORY_VENDOR_SPECIFIC',
             'WLAN_EID_MMIE']:
    value = re.search(r'\b' + name + r'\s*=\s*(0x[0-9a-fA-F]+|\d+)', ieee)[1]
    program += '#define ' + name + ' ' + value + '\n'
program += r'''
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#define AES_BLOCK_SIZE 16
#define AES_KEYSIZE_128 16
struct aes_cmac_key { u8 bytes[16]; };
struct aes_cmac_ctx { EVP_MAC_CTX *ctx; };
static int aes_cmac_preparekey(struct aes_cmac_key *key,const u8 *raw,size_t length) {
 assert(length==16); memcpy(key->bytes,raw,16); return 0;
}
static void aes_cmac_init(struct aes_cmac_ctx *ctx,const struct aes_cmac_key *key) {
 EVP_MAC *mac=EVP_MAC_fetch(NULL,"CMAC",NULL); assert(mac);
 ctx->ctx=EVP_MAC_CTX_new(mac); EVP_MAC_free(mac); assert(ctx->ctx);
 char cipher[]="AES-128-CBC";
 OSSL_PARAM params[]={OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER,cipher,0),OSSL_PARAM_construct_end()};
 assert(EVP_MAC_init(ctx->ctx,key->bytes,16,params)==1);
}
static void aes_cmac_update(struct aes_cmac_ctx *ctx,const u8 *data,size_t length) {
 assert(EVP_MAC_update(ctx->ctx,data,length)==1);
}
static void aes_cmac_final(struct aes_cmac_ctx *ctx,u8 out[16]) {
 size_t length=0; assert(EVP_MAC_final(ctx->ctx,out,&length,16)==1 && length==16);
 EVP_MAC_CTX_free(ctx->ctx); ctx->ctx=NULL;
}
static int crypto_memneq(const void *a,const void *b,size_t length) { return CRYPTO_memcmp(a,b,length); }
static bool is_multicast_ether_addr(const u8 *a) { return a[0]&1; }
'''
for name in ['mgmt_rx.h', 'mgmt_rx.c']:
    program += re.sub(r'^#include .*\n', '', (driver / name).read_text(), flags=re.M)
program += base['function']('mt6797_radio_pmf_frame')
program += r'''
static int mt6797_radio_data_frame(struct mt6797_wifi_radio *r,const struct mt6797_wifi_rx_view *v) { (void)r; (void)v; return 0; }
static int mt6797_radio_beacon(struct mt6797_wifi_radio *r,const struct mt6797_wifi_rx_view *v) { (void)r; (void)v; return 0; }
'''
program += base['function']('mt6797_radio_link_frame')
program += r'''
static u8 packet[512];
static size_t packet_length,frame_offset;
static struct net_device netdev={.dev_addr={2,1,2,3,4,5}};
static const u8 bip_key[16]={0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
/* Independently generated OpenSSL CLI vector: FC c000, broadcast A1,
 * AP A2/A3 02060708090a, reason7, MMIE ID4, IPN8, zeroed MIC in AAD/body.
 */
static const u8 bip_mic[8]={0xf6,0x93,0x0e,0xd8,0x3e,0x95,0x79,0xc4};
static void frame_build(bool group,unsigned groups) {
 memset(packet,0,sizeof(packet));
 put_unaligned_le16(0xe001 | (groups<<9),packet+2);
 packet[5]=6; packet[6]=24; packet[8]=group?2:1; packet[9]=(group?6:4)<<4;
 put_unaligned_le16(0xe000,packet+10);
 size_t off=16;
 if(groups&8) off+=16;
 if(groups&1) { packet[off]=group?99:8; off+=16; }
 if(groups&2) off+=8;
 if(groups&4) off+=24;
 frame_offset=off;
 u8 *frame=packet+off;
 put_unaligned_le16(IEEE80211_STYPE_DEAUTH | (group?0:IEEE80211_FCTL_PROTECTED),frame);
 if(group) memset(frame+4,0xff,6); else memcpy(frame+4,netdev.dev_addr,6);
 memcpy(frame+10,bss.bssid,6); memcpy(frame+16,bss.bssid,6); frame[24]=7;
 packet_length=off+26;
 if(group) {
  u8 *mmie=frame+26; mmie[0]=76; mmie[1]=16; mmie[2]=4; mmie[4]=8;
  memcpy(mmie+10,bip_mic,8); packet_length+=18;
 }
 put_unaligned_le16(packet_length,packet);
}
static void setup_rx(bool group) {
 pmf_setup(); r.netdev=&netdev;
 if(group) { params.key=bip_key; assert(!add(4,false)); }
 else { params.cipher=WLAN_CIPHER_SUITE_CCMP; assert(!add(0,true)); }
 c.authorized=true; queued=0; frame_build(group,5);
}
static void receive(bool disconnect) {
 unsigned before=queued;
 c.cancel=false; c.peer_disconnect=false; c.reason=0;
 struct mt6797_wifi_rx_view view;
 int ret=mt6797_wifi_rx_parse(packet,packet_length,&view);
 if(!ret) assert(!mt6797_radio_link_frame(&r,&view));
 assert(queued==before+disconnect && c.cancel==disconnect && c.peer_disconnect==disconnect);
 assert(!r.lock); if(disconnect) assert(c.reason==7);
}
static void decoded_rejected(void) {
 struct mt6797_wifi_mgmt_rx out,zero={0}; memset(&out,0xa5,sizeof(out));
 assert(mt6797_wifi_mgmt_rx_decode(packet,packet_length,&out)<0);
 assert(!memcmp(&out,&zero,sizeof(out)));
}
static void primitive_known_answer(void) {
 /* RFC4493 example2 independently verifies the host AES-CMAC replacement. */
 const u8 input[16]={0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a};
 const u8 expected[16]={0x07,0x0a,0x16,0xb4,0x6b,0x4d,0x41,0x44,0xf7,0x9b,0xdd,0x9d,0xd0,0x4a,0x28,0x7c};
 struct aes_cmac_key key; struct aes_cmac_ctx ctx; u8 out[16];
 aes_cmac_preparekey(&key,bip_key,16); aes_cmac_init(&ctx,&key);
 aes_cmac_update(&ctx,input,16); aes_cmac_final(&ctx,out); assert(!memcmp(out,expected,16));
}
int main(void) {
 key_main(); primitive_known_answer();
 /* BIP uses intact MMIE ID/IPN, never the descriptor's 2-bit ID/group1 PN. */
 for(unsigned groups=0;groups<16;groups++) {
  setup_rx(true); frame_build(true,groups); packet[4]=0xc0;
  struct mt6797_wifi_mgmt_rx data;
  assert(!mt6797_wifi_mgmt_rx_decode(packet,packet_length,&data));
  assert(data.multicast && data.key_index==4 && data.pn==8);
  assert(!mt6797_wifi_mgmt_rx_bip_verify(&data,bip_key));
  receive(true); assert(c.igtk.rx_pn[0]==8); receive(false);
 }
 /* Key ID5 has its own independent known-answer MIC, not a truncated ID1. */
 pmf_setup(); r.netdev=&netdev; params.key=bip_key; assert(!add(5,false)); queued=0;
 frame_build(true,5); packet[packet_length-16]=5;
 const u8 mic5[8]={0x6a,0x81,0xcd,0xc7,0x0e,0x8c,0x39,0x27};
 memcpy(packet+packet_length-8,mic5,8); receive(true); assert(c.igtk.rx_pn[0]==8);
 /* RXD's two-byte header offset changes no authenticated or PN bytes. */
 for(unsigned group=0;group<2;group++) {
  setup_rx(group); memmove(packet+frame_offset+2,packet+frame_offset,packet_length-frame_offset);
  frame_offset+=2; packet_length+=2; packet[6]|=0x40;
  put_unaligned_le16(packet_length,packet); receive(true);
 }
 /* AAD masks Retry/Power Management/More Data and excludes sequence/duration. */
 setup_rx(true); packet[frame_offset+1]|=0x38; packet[frame_offset+2]=0xa5;
 packet[frame_offset+22]=0xf0; packet[frame_offset+23]=0xff;
 receive(true);
 /* Every authenticated-byte corruption, including MMIE ID/IPN/MIC, leaves IPN untouched. */
 for(size_t pos=4;pos<44;pos++) if(pos<22 || pos>=24) {
  setup_rx(true); packet[frame_offset+pos]^=0x04; receive(false);
  assert(c.igtk.rx_pn[0]==7);
 }
 /* Firmware's cipher6/no-error claim cannot override a bad MIC. */
 setup_rx(true); packet[packet_length-1]^=1; receive(false); assert(c.igtk.rx_pn[0]==7);
 packet[packet_length-1]^=1; receive(true);
 /* Key update and stale station/WTBL state gate validation before replay mutation. */
 setup_rx(true); c.igtk.updating=true; receive(false); assert(c.igtk.rx_pn[0]==7);
 setup_rx(true); c.igtk.installed=false; receive(false);
 setup_rx(true); c.igtk.index=5; receive(false);
 setup_rx(true); c.igtk_owner.generation++; receive(false);
 setup_rx(true); c.ais.runtime.sta.ap_bmc_group.cookie++; receive(false);
 setup_rx(true); packet[8]=1; receive(false); assert(c.igtk.rx_pn[0]==7);
 setup_rx(true); packet[7]=4; receive(false);
 /* Wrong IGTK has valid metadata but fails software authentication. */
 setup_rx(true); c.igtk.material[0]^=1; receive(false); assert(c.igtk.rx_pn[0]==7);
 /* Unprotected disconnect cannot fall through to the legacy PMF-disabled path. */
 setup_rx(false); packet[9]=0; packet[frame_offset+1]&=~0x40; receive(false);
 assert(c.pairwise_key.mgmt_rx_pn==7);
 /* CCMP management has a separate counter from data TID0. */
 for(unsigned groups=1;groups<16;groups+=2) {
  setup_rx(false); frame_build(false,groups); c.pairwise_key.rx_pn[0]=100;
  receive(true); assert(c.pairwise_key.mgmt_rx_pn==8 && c.pairwise_key.rx_pn[0]==100);
  receive(false);
 }
 setup_rx(false); memset(packet+16,0xff,6); receive(true);
 assert(c.pairwise_key.mgmt_rx_pn==0xffffffffffffULL); receive(false);
 memset(packet+16,0,6); receive(false); assert(c.pairwise_key.mgmt_rx_pn==0xffffffffffffULL);
 /* Robust actions validate/update replay state but are not delivered by this slice. */
 setup_rx(false); packet[frame_offset]=0xd0; packet[frame_offset+24]=8;
 receive(false); assert(c.pairwise_key.mgmt_rx_pn==8);
 setup_rx(true); packet[frame_offset]=0xd0; packet[frame_offset+24]=WLAN_CATEGORY_PUBLIC;
 decoded_rejected();
 setup_rx(true); c.connected=false; receive(false); assert(c.igtk.rx_pn[0]==7);
 setup_rx(true); r.stopping=true; receive(false); assert(c.igtk.rx_pn[0]==7);
 setup_rx(false); c.key_pending.generation++; receive(false);
 setup_rx(false); c.pairwise_wtbl.cookie++; receive(false);
 setup_rx(false); c.pairwise_ready=false; receive(false);
 setup_rx(false); c.pairwise_key.updating=true; receive(false);
 setup_rx(false); packet[4]=0x40; receive(false);
 setup_rx(false); packet[8]=2; receive(false);
 setup_rx(false); frame_build(false,4); decoded_rejected(); receive(false);
 /* Descriptor errors, unsupported formats, fragments and malformed MMIE never authenticate. */
 for(unsigned bit=1;bit<=12;bit++) {
  setup_rx(true); put_unaligned_le16(0xe000|(1U<<bit),packet+10); decoded_rejected(); receive(false);
 }
 setup_rx(true); packet[7]|=1; decoded_rejected();
 setup_rx(true); packet[6]|=0x80; decoded_rejected();
 setup_rx(true); packet[frame_offset+1]|=0x40; decoded_rejected();
 setup_rx(true); packet[frame_offset+1]|=0x04; decoded_rejected();
 setup_rx(true); packet[frame_offset+22]=1; decoded_rejected();
 setup_rx(true); packet[packet_length-18]=0; decoded_rejected();
 setup_rx(true); packet[packet_length-17]=15; decoded_rejected();
 setup_rx(true); packet[packet_length-15]=1; decoded_rejected();
 setup_rx(true); packet_length-=18; put_unaligned_le16(packet_length,packet); decoded_rejected();
 setup_rx(true); size_t full=packet_length;
 for(size_t n=0;n<full;n++) {
  struct mt6797_wifi_mgmt_rx out;
  assert(mt6797_wifi_mgmt_rx_decode(packet,n,&out)<0);
 }
 puts("PASS: production PMF RX and real metadata/codecs: BIP known-answer/AAD/MIC, MMIE identity/IPN, corruptions, missing metadata, all RX groups, separate CCMP management replay, peer/key/WTBL generations, plaintext rejection and no replay-state mutation on failures");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-pmf-rx-') as temporary:
    directory = Path(temporary)
    (directory / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    *shlex.split(os.environ.get('OPENSSL_CFLAGS', '')),
                    str(directory / 'test.c'), '-o', str(directory / 'test'),
                    *shlex.split(os.environ.get('OPENSSL_LIBS', '-lcrypto'))], check=True)
    subprocess.run([str(directory / 'test')], check=True)
