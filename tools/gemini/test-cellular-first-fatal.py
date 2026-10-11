#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual diagnostic helpers with IRQ-lock/READ_ONCE host adapters."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = root / "drivers/net/wwan/mediatek/mtk_mt6797_ccci_diag.h"
source = (header.with_name("mtk_mt6797_ccci.c")).read_text()
shim = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef pthread_mutex_t spinlock_t;
#define U32_MAX UINT32_MAX
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MTK_CCCI_HEADER_SIZE 16
#define MTK_CCCI_FEATURE_QUERY_SIZE 72
#define MTK_CCCI_RUNTIME_V2_REPLY_SIZE 116
#define MTK_CCCI_RUNTIME_AP_SIZE 2048
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define spin_lock_irqsave(p,f) do { (f)=0; assert(!pthread_mutex_lock(p)); } while (0)
#define spin_unlock_irqrestore(p,f) do { (void)(f); assert(!pthread_mutex_unlock(p)); } while (0)
'''
tests = r'''
static struct mt6797_ccci_diag d;
static u8 hs1[88], reply[116], runtime[2048], gpd[16];
static unsigned winners;
static u8 incoming[3585];
static void put32(u8 *p, u32 value)
{ for (unsigned i=0;i<4;i++) p[i]=value>>(8*i); }
static void exception_tests(void)
{
 u8 bytes[8192]={0}; struct mt6797_ccci_md_exception ex;
 for(size_t n=0;n<144;n++) {
  mt6797_ccci_md_exception_decode(bytes,n,&ex); assert(!ex.overview_valid);
 }
 mt6797_ccci_md_exception_decode(NULL,sizeof(bytes),&ex); assert(!ex.overview_valid);
 for(unsigned count=0;count<=9;count+=9) {
  put32(bytes,count); mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
  assert(!ex.overview_valid&&!ex.pl[0].header_valid);
 }
 put32(bytes,2); put32(bytes+4,256); put32(bytes+20,2048);
 bytes[8]=1; bytes[256+4]=6; bytes[2048+4]=16;
 put32(bytes+132,0x812); put32(bytes+136,0x34); put32(bytes+140,0x56);
 put32(bytes+256+320,0x12345678); put32(bytes+256+324,0xabcdef01);
 put32(bytes+256+328,0x98765432); put32(bytes+2048+640,1234);
 mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
 assert(ex.overview_valid&&ex.core_count==2&&ex.ect_status==0x812);
 assert(ex.cs_status==0x34&&ex.md32_status==0x56);
 assert(ex.pl[0].header_valid&&ex.pl[0].offender&&ex.pl[0].content_valid);
 assert(ex.pl[0].type==6&&ex.pl[0].codes[0]==0x12345678);
 assert(ex.pl[0].codes[1]==0xabcdef01&&ex.pl[0].codes[2]==0x98765432);
 assert(ex.pl[1].header_valid&&!ex.pl[1].offender&&ex.pl[1].content_valid);
 assert(ex.pl[1].type==16&&ex.pl[1].assert_line==1234);
 for(unsigned type=16;type<=18;type++) {
  bytes[2048+4]=type;
  mt6797_ccci_md_exception_decode(bytes,2048+644,&ex);
  assert(ex.pl[1].content_valid&&ex.pl[1].assert_line==1234);
  mt6797_ccci_md_exception_decode(bytes,2048+643,&ex);
  assert(ex.pl[1].header_valid&&!ex.pl[1].content_valid&&!ex.pl[1].assert_line);
 }
 mt6797_ccci_md_exception_decode(bytes,256+332,&ex);
 assert(ex.pl[0].content_valid&&!ex.pl[1].header_valid);
 mt6797_ccci_md_exception_decode(bytes,256+331,&ex);
 assert(ex.pl[0].header_valid&&!ex.pl[0].content_valid&&!ex.pl[0].codes[0]);
 for(unsigned type=0;type<256;type++) {
  bytes[256+4]=type;
  mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
  bool known=(type>=1&&type<=7)||(type>=16&&type<=18)||(type>=0x20&&type<=0x26);
  assert(ex.pl[0].header_valid&&ex.pl[0].type_known==known&&ex.pl[0].content_valid==known);
 }
 const u32 bad[]={0,143,8192-7,8192,UINT32_MAX};
 for(unsigned i=0;i<ARRAY_SIZE(bad);i++) {
  put32(bytes+4,bad[i]); mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
  assert(!ex.pl[0].header_valid&&!ex.pl[0].content_valid);
 }
 put32(bytes+4,8192-8); bytes[8192-4]=16;
 mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
 assert(ex.pl[0].header_valid&&!ex.pl[0].content_valid);
 put32(bytes,1); mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
 assert(!ex.pl[1].header_valid);
 /* Count 9 qualifies only bounded PL prefixes, never the whole overview.
  * Core ids are copied numerically without assuming a zero/one-based map.
  */
 put32(bytes,9); put32(bytes+4,256); put32(bytes+20,2048);
 put32(bytes+256,0x12345678); put32(bytes+2048,UINT32_MAX);
 bytes[8]=1; bytes[24]=0; bytes[256+4]=6; bytes[2048+4]=16;
 put32(bytes+132,UINT32_MAX); put32(bytes+136,UINT32_MAX); put32(bytes+140,UINT32_MAX);
 mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
 assert(!ex.overview_valid&&ex.pl_prefix_only&&ex.core_count==9);
 assert(!ex.ect_status&&!ex.cs_status&&!ex.md32_status);
 assert(ex.pl[0].header_valid&&ex.pl[0].type_known&&ex.pl[0].content_valid);
 assert(ex.pl[0].core_id==0x12345678&&ex.pl[0].codes[0]==0x12345678);
 assert(ex.pl[1].header_valid&&ex.pl[1].type_known&&ex.pl[1].content_valid);
 assert(ex.pl[1].core_id==UINT32_MAX&&ex.pl[1].assert_line==1234);
 for(unsigned type=0;type<256;type++) {
  bytes[256+4]=type;
  mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
  bool known=(type>=1&&type<=7)||(type>=16&&type<=18)||(type>=0x20&&type<=0x26);
  assert(!ex.overview_valid&&ex.pl_prefix_only&&!ex.ect_status);
  assert(ex.pl[0].header_valid&&ex.pl[0].type_known==known&&ex.pl[0].content_valid==known);
 }
 bytes[256+4]=6;
 for(unsigned flag=2;flag<256;flag++) {
  bytes[8]=flag; mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
  assert(!ex.pl[0].header_valid&&!ex.pl[0].content_valid&&!ex.pl[0].core_id);
  assert(ex.pl[1].content_valid);
 }
 bytes[8]=1;
 for(unsigned i=0;i<ARRAY_SIZE(bad);i++) {
  put32(bytes+4,bad[i]); mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
  assert(!ex.overview_valid&&ex.pl_prefix_only&&!ex.pl[0].header_valid);
 }
 put32(bytes+4,256);
 mt6797_ccci_md_exception_decode(bytes,256+331,&ex);
 assert(ex.pl_prefix_only&&ex.pl[0].header_valid&&!ex.pl[0].content_valid);
 const u32 invalid_counts[]={0,10,UINT32_MAX,0xab000009};
 for(unsigned i=0;i<ARRAY_SIZE(invalid_counts);i++) {
  put32(bytes,invalid_counts[i]); mt6797_ccci_md_exception_decode(bytes,sizeof(bytes),&ex);
  assert(!ex.overview_valid&&!ex.pl_prefix_only&&!ex.pl[0].header_valid&&!ex.pl[1].header_valid);
  assert(!ex.ect_status&&!ex.cs_status&&!ex.md32_status);
 }
 puts("PASS count9 bounded PL prefixes: numeric core ids, both records, unknown types, offender/offset/content bounds; no overview/status guesses or v3 substitution");
 puts("PASS public UMOLY numeric exception decoder: fatal/assert, all types, truncation, overlapping/overflowing offsets and core-count bounds");
}
static void *at_writer(void *opaque)
{
 struct mt6797_ccci_diag *at=opaque;
 for(unsigned i=0;i<1000;i++)mt6797_ccci_diag_at(at,MT6797_CCCI_AT_OPEN);
 return NULL;
}
static void at_metadata_tests(void)
{
 struct mt6797_ccci_diag at={.lock=PTHREAD_MUTEX_INITIALIZER};
 for(unsigned i=0;i<MT6797_CCCI_AT_EVENT_COUNT;i++){
  mt6797_ccci_diag_at(&at,i);assert(at.at_events[i]==1);
  at.at_events[i]=U32_MAX;mt6797_ccci_diag_at(&at,i);assert(at.at_events[i]==U32_MAX);
 }
 u32 saved[MT6797_CCCI_AT_EVENT_COUNT];memcpy(saved,at.at_events,sizeof(saved));
 mt6797_ccci_diag_at(&at,MT6797_CCCI_AT_EVENT_COUNT);mt6797_ccci_diag_at(&at,U32_MAX);
 assert(!memcmp(saved,at.at_events,sizeof(saved)));
 memset(at.at_events,0,sizeof(at.at_events));
 pthread_t threads[32];
 for(unsigned i=0;i<32;i++)assert(!pthread_create(&threads[i],NULL,at_writer,&at));
 for(unsigned i=0;i<32;i++)assert(!pthread_join(threads[i],NULL));
 assert(at.at_events[MT6797_CCCI_AT_OPEN]==32000);
 for(unsigned i=1;i<MT6797_CCCI_AT_EVENT_COUNT;i++)assert(!at.at_events[i]);
 memcpy(saved,at.at_events,sizeof(saved));assert(mt6797_ccci_diag_freeze(&at,NULL));
 for(unsigned i=0;i<MT6797_CCCI_AT_EVENT_COUNT;i++)mt6797_ccci_diag_at(&at,i);
 assert(!memcmp(saved,at.at_events,sizeof(saved)));assert(!pthread_mutex_destroy(&at.lock));
 puts("PASS actual AT metadata helper: seven payload-free counters, saturation, invalid events, 32 concurrent writers and first-fatal freeze");
}
static void reset(void)
{
 memset(&d, 0, sizeof(d)); assert(!pthread_mutex_init(&d.lock, NULL));
 memset(hs1, 0x12, sizeof(hs1)); memset(reply, 0x34, sizeof(reply));
 memset(runtime, 0x56, sizeof(runtime)); memset(gpd, 0x81, sizeof(gpd));
 memset(incoming, 0xa5, sizeof(incoming));
 incoming[16]=0x78; incoming[17]=0x56; incoming[18]=0x34; incoming[19]=0x12;
}
static void *race(void *unused)
{
 (void)unused;
 mt6797_ccci_diag_irq(&d, 1, 2, 3, 4);
 mt6797_ccci_diag_payload(&d, hs1, sizeof(hs1), reply, sizeof(reply), runtime, 324);
 mt6797_ccci_diag_tx(&d, false, 1, 0, 1);
 mt6797_ccci_diag_rx_save(&d,incoming,20,1,3,0,32);
 if (mt6797_ccci_diag_freeze(&d, gpd)) __atomic_fetch_add(&winners, 1, __ATOMIC_SEQ_CST);
 return NULL;
}
static void *rx_race(void *unused)
{ (void)unused; mt6797_ccci_diag_rx_save(&d,incoming,3584,1,3,0,32); return NULL; }
int main(void)
{
 exception_tests();
 at_metadata_tests();
 reset();
 mt6797_ccci_diag_payload(&d, hs1, 87, reply, sizeof(reply), runtime, 324);
 mt6797_ccci_diag_payload(&d, hs1, 88, reply, 115, runtime, 324);
 mt6797_ccci_diag_payload(&d, hs1, 88, reply, 116, runtime, 2049);
 assert(!d.payload_valid);
 mt6797_ccci_diag_payload(&d, hs1, 88, reply, 116, runtime, 324);
 memset(hs1, 0, sizeof(hs1)); memset(reply, 0, sizeof(reply)); memset(runtime, 0, sizeof(runtime));
 mt6797_ccci_diag_payload(&d, hs1, 88, reply, 116, runtime, 2048);
 assert(d.payload_valid && d.runtime_len == 324 && d.hs1[0] == 0x12);
 assert(d.reply[115] == 0x34 && d.runtime[323] == 0x56 && !d.runtime[324]);
 mt6797_ccci_diag_final_reply(&d, reply, 115); assert(!d.reply_finalized);
 mt6797_ccci_diag_final_reply(&d, reply, 116);
 assert(d.reply_finalized && d.reply[115] == 0);
 memset(reply, 0x77, sizeof(reply)); mt6797_ccci_diag_final_reply(&d, reply, 116);
 assert(d.reply[115] == 0);
 mt6797_ccci_diag_irq(&d, 1, 2, 3, 4); mt6797_ccci_diag_irq(&d, 5, 6, 7, 8);
 assert(d.irq_count == 2 && d.first_irq[0] == 1 && d.first_irq[3] == 4);
 assert(d.last_irq[0] == 5 && d.last_irq[3] == 8);
 mt6797_ccci_diag_tx(&d, false, 1, 0, 1);
 mt6797_ccci_diag_tx(&d, true, 1, 1, 0);
 assert(d.submitted == 1 && d.reclaimed == 1 && !d.used);
 assert(mt6797_ccci_diag_freeze(&d, gpd));
 assert(d.descriptor_valid && d.descriptor[15] == 0x81);
 memset(gpd, 0, sizeof(gpd));
 mt6797_ccci_diag_irq(&d, 9, 9, 9, 9); mt6797_ccci_diag_tx(&d, false, 9, 9, 9);
 assert(!mt6797_ccci_diag_freeze(&d, gpd));
 assert(d.irq_count == 2 && d.submitted == 1 && d.descriptor[15] == 0x81);
 assert(d.last_irq[0] == 5 && d.last_irq[3] == 8);
 pthread_mutex_destroy(&d.lock); reset();
 assert(mt6797_ccci_diag_freeze(&d, NULL) && !d.descriptor_valid && !d.payload_valid);
 pthread_mutex_destroy(&d.lock); reset();
 d.submitted = d.reclaimed = d.irq_count = U32_MAX;
 mt6797_ccci_diag_tx(&d, false, 1, 0, 1); mt6797_ccci_diag_tx(&d, true, 1, 1, 0);
 mt6797_ccci_diag_irq(&d, 1, 2, 3, 4);
 assert(d.submitted == U32_MAX && d.reclaimed == U32_MAX && d.irq_count == U32_MAX);
 pthread_mutex_destroy(&d.lock); reset();
 pthread_t threads[32];
 for (unsigned i=0; i<32; i++) assert(!pthread_create(&threads[i], NULL, race, NULL));
 for (unsigned i=0; i<32; i++) assert(!pthread_join(threads[i], NULL));
 assert(winners == 1 && d.frozen && d.payload_valid && d.runtime_len == 324);
 assert(d.descriptor_valid && d.irq_count && d.submitted);
 assert(d.rx_count && d.rx_count <= 4);
 unsigned saved=d.rx_count;
 mt6797_ccci_diag_rx_save(&d,incoming,20,1,3,0,32); assert(d.rx_count==saved);
 pthread_mutex_destroy(&d.lock);
 reset();
 mt6797_ccci_diag_rx_save(&d,NULL,20,1,3,0,32);
 mt6797_ccci_diag_rx_save(&d,incoming,3585,1,3,0,32);
 mt6797_ccci_diag_rx_save(&d,incoming,20,7,3,0,32);
 mt6797_ccci_diag_rx_save(&d,incoming,20,1,0,0,32); assert(!d.rx_count);
 mt6797_ccci_diag_rx_save(&d,incoming,15,1,1,-22,99);
 assert(d.rx_count==1 && !d.rx[0].header_valid && !d.rx[0].payload_word0_valid);
 assert(!d.rx[0].channel && d.rx[0].captured==15 && d.rx[0].decode_error==-22);
 mt6797_ccci_diag_rx_save(&d,incoming,16,1,3,0,32);
 assert(d.rx[1].header_valid && !d.rx[1].payload_word0_valid);
 mt6797_ccci_diag_rx_save(&d,incoming,20,1,3,0,32);
 assert(d.rx[2].payload_word0_valid && d.rx[2].payload_word0==0x12345678);
 mt6797_ccci_diag_rx_save(&d,incoming,3584,1,3,0,32);
 assert(d.rx_count==4 && d.rx[3].length==3584 && d.rx[3].captured==1024 && d.rx[3].truncated);
 memset(incoming,0,sizeof(incoming)); assert(d.rx[3].bytes[1000]==0xa5);
 mt6797_ccci_diag_rx_save(&d,incoming,20,1,3,0,32); assert(d.rx_count==4);
 assert(mt6797_ccci_diag_freeze(&d,NULL));
 mt6797_ccci_diag_rx_save(&d,incoming,20,1,3,0,32); assert(d.rx_count==4);
 pthread_mutex_destroy(&d.lock); reset();
 for (unsigned i=0;i<32;i++) assert(!pthread_create(&threads[i],NULL,rx_race,NULL));
 for (unsigned i=0;i<32;i++) assert(!pthread_join(threads[i],NULL));
 assert(d.rx_count==4);
 for(unsigned i=0;i<4;i++) assert(d.rx[i].captured==1024 && d.rx[i].payload_word0==0x12345678);
 pthread_mutex_destroy(&d.lock);
 puts("PASS actual first-fatal helpers: immutable bounded bytes, initial IRQ, Q0 counters, descriptor snapshot, null/oversize, saturation, 32-way freeze race");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cellular-first-fatal-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(shim + header.read_text() + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-Werror", "-pthread", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=5)

def body(name):
    start = source.index("static ", source.index(name) - 32)
    return source[start:source.index("\n}\n", start) + 3]

for name, site in (("mt6797_ccci_handshake_timeout_work", "ap-handshake-timer"),
                   ("mt6797_ccci_wdt_irq", "md-wdt-irq")):
    assert f'"{site}"' in body(name)
q = body("mt6797_ccci_quiesce_from")
assert q.index("atomic_xchg") < q.index("mt6797_ccci_first_fatal") < q.index("writel")
assert q.index("mt6797_ccci_first_fatal") < q.index("dev_err")
tx = body("mt6797_ccci_tx(")
assert tx.index("mtk_ccci_header_init") < tx.index("mt6797_ccci_diag_final_reply") < tx.index("dma_map_single")
irq = body("mt6797_ccci_cldma_irq")
assert irq.index("mt6797_ccci_diag_irq") < irq.index("writel")
fatal = body("mt6797_ccci_first_fatal")
assert "readl(" not in fatal and "writel(" not in fatal and "%p" not in fatal
assert "spin_lock" not in fatal and "DUMP_PREFIX_OFFSET" in fatal
assert fatal.index("mt6797_ccci_diag_freeze") < fatal.index("memcpy_fromio")
assert "ccci->smem_size >= MT6797_CCCI_MDSS_OFFSET + MT6797_CCCI_MDSS_SIZE" in fatal
assert source.index("spin_lock_init(&ccci->diag.lock)") < source.index("devm_request_irq")
print("PASS fatal-before-STOP, separate WDT/timer sources, IRQ capture before W1C, bounded owned SMEM observation without register or pointer output")
dispatch = body("mt6797_ccci_dispatch_rx")
assert dispatch.index("diag_drop, ret, ret ? 0 : frame.channel") < dispatch.index("kfree_skb")
assert "MT6797_CCCI_DIAG_BAD_HEADER" in dispatch and "MT6797_CCCI_DIAG_BEFORE_READY" in dispatch
assert dispatch.index("MT6797_CCCI_DIAG_BAD_SEQUENCE") < dispatch.index("mt6797_ccci_quiesce")
assert "mt6797_ccci_dispatch_rx(ccci, skb, q)" in source
print("PASS actual pre-ready RX capture:4x1KiB bound, lengths/truncation, malformed/short/opcode guards, immutable/frozen/full,32-way cap and save-before-drop")

# Compile actual dispatch through UART2 delivery and the actual drop epilogue.
# Only the network-delivery branch is excluded.
common = header.with_name("mtk_ccci_common.c").read_text()
begin = common.index("int mtk_ccci_header_decode(")
decoder = common[begin:common.index("\n}\n", begin) + 3]
pre_ready = dispatch[:dispatch.index("\n\tskb_pull(skb, MTK_CCCI_HEADER_SIZE);\n\tversion =")]
pre_ready += '\tassert(!"network branch outside this test");\n' + dispatch[dispatch.index("drop:\n"):]
dispatch_shim = r'''
#include <errno.h>
typedef uint16_t u16;
typedef uint16_t __be16;
#define MTK_CCCI_STATUS_CHANNEL_MASK 0xffffU
#define MTK_CCCI_STATUS_SEQ_MASK 0x7fff0000U
#define MTK_CCCI_STATUS_SEQ_SHIFT 16
#define MTK_CCCI_STATUS_ASSERT 0x80000000U
#define MT6797_CCCI_CHANNEL_COUNT 256
#define MTK_CCCI_CONTROL_RX 0
#define MTK_CCCI_UART2_RX 10
#define le32_to_cpu(x) (x)
#define atomic_read(p) (*(p))
struct mtk_ccci_header { u32 data0,data1,status,reserved; };
struct mtk_ccci_frame { u32 data0,data1,reserved; u16 channel,sequence; bool assert_bit; };
struct sk_buff { u8 *data; size_t len; };
struct mt6797_ccci { struct mt6797_ccci_diag diag; struct mt6797_rx_rejected rx_rejected; int ready,at_open; void *at_port; int rx_sequence[256]; };
static int deliveries;
static int mtk_ccci_net_rx_link(u16 ch){(void)ch;return -1;}
static void skb_pull(struct sk_buff *skb,size_t n){assert(skb->len>=n);skb->data+=n;skb->len-=n;}
static void wwan_port_rx(void *port,struct sk_buff *skb){(void)port;assert(skb->len==4);deliveries++;}
#define MT6797_RPC_RX 32
#define MT6797_FS_RX 14
static int mt6797_ccci_rpc_rx(struct mt6797_ccci *c, struct sk_buff *s, unsigned q)
{ (void)c; (void)s; (void)q; return -EOPNOTSUPP; }
static int mt6797_ccci_fs_rx(struct mt6797_ccci *c, struct sk_buff *s, unsigned q)
{ (void)c; (void)s; (void)q; return -EOPNOTSUPP; }
static int frees, controls, crashes, control_result;
static void kfree_skb(struct sk_buff *skb) { (void)skb; frees++; }
static int mt6797_ccci_control_rx(struct mt6797_ccci *c, struct sk_buff *s,
 const struct mtk_ccci_frame *f) { (void)c; (void)s; assert(f->channel==0); controls++; return control_result; }
static void mt6797_ccci_quiesce(struct mt6797_ccci *c,int why)
{ assert(why==-EILSEQ && c->diag.rx_count==1); mt6797_ccci_diag_freeze(&c->diag,NULL); crashes++; }
'''
dispatch_tests = r'''
int main(void)
{
 struct mt6797_ccci c={0}; u32 words[8]={0};
 struct sk_buff skb={.data=(u8 *)words,.len=15};
 pthread_mutex_init(&c.diag.lock,NULL);
 pthread_mutex_init(&c.rx_rejected.lock,NULL);
 mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(frees==1 && c.diag.rx_count==1 && !c.diag.rx[0].header_valid && !c.diag.rx[0].channel);
 assert(c.diag.rx[0].decode_error==-EMSGSIZE && !c.diag.rx[0].payload_word0_valid);
 skb.len=16; mt6797_ccci_dispatch_rx(&c,&skb,0);
 assert(controls==1 && frees==2 && c.diag.rx_count==1);
 words[2]=32; words[4]=0x4006; skb.len=20;
 mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(c.diag.rx_count==2 && c.diag.rx[1].channel==32 && c.diag.rx[1].payload_word0==0x4006);
 assert(c.diag.rx[1].cause==MT6797_CCCI_DIAG_BEFORE_READY);
 assert(c.rx_rejected.count==1&&c.rx_rejected.entry[0].channel==32);
 assert(c.rx_rejected.entry[0].op_valid&&c.rx_rejected.entry[0].op==0x4006);
 c.ready=1; control_result=-EALREADY; words[1]=4; words[2]=0; skb.len=16;
 mt6797_ccci_dispatch_rx(&c,&skb,0);
 assert(controls==2&&frees==4&&c.diag.rx_count==2);
 assert(c.rx_rejected.count==2&&c.rx_rejected.entry[1].channel==0);
 assert(c.rx_rejected.entry[1].message_valid&&c.rx_rejected.entry[1].message==4);
 assert(!c.rx_rejected.entry[1].op_valid&&c.rx_rejected.entry[1].error==-EALREADY);
 c.ready=0; skb.len=20;
 words[2]=300; mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(c.diag.rx_count==3 && c.diag.rx[2].cause==MT6797_CCCI_DIAG_BAD_CHANNEL);
 pthread_mutex_destroy(&c.diag.lock); pthread_mutex_destroy(&c.rx_rejected.lock);
 memset(&c,0,sizeof(c)); pthread_mutex_init(&c.diag.lock,NULL); pthread_mutex_init(&c.rx_rejected.lock,NULL);
 words[2]=0x80030020; c.rx_sequence[32]=1;
 mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(crashes==1 && c.diag.frozen && c.diag.rx[0].cause==MT6797_CCCI_DIAG_BAD_SEQUENCE);
 pthread_mutex_destroy(&c.diag.lock);pthread_mutex_destroy(&c.rx_rejected.lock);
 memset(&c,0,sizeof(c));pthread_mutex_init(&c.diag.lock,NULL);pthread_mutex_init(&c.rx_rejected.lock,NULL);
 words[1]=20;words[2]=10;skb.data=(u8 *)words;skb.len=20;
 mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(c.diag.at_events[MT6797_CCCI_AT_RX_SEEN]==1);
 assert(!deliveries&&!c.diag.at_events[MT6797_CCCI_AT_RX_CLOSED_DROP]);
 c.ready=1;mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(c.diag.at_events[MT6797_CCCI_AT_RX_SEEN]==2);
 assert(c.diag.at_events[MT6797_CCCI_AT_RX_CLOSED_DROP]==1&&!deliveries);
 c.at_open=1;mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(c.diag.at_events[MT6797_CCCI_AT_RX_SEEN]==3);
 assert(c.diag.at_events[MT6797_CCCI_AT_RX_DELIVERED]==1&&deliveries==1);
 skb.data=(u8 *)words;skb.len=16;words[1]=16;
 mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(c.diag.at_events[MT6797_CCCI_AT_RX_SEEN]==4&&deliveries==1);
 pthread_mutex_destroy(&c.diag.lock);
 pthread_mutex_destroy(&c.rx_rejected.lock);
 puts("PASS actual dispatch+decoder: malformed/bad-sequence guards, rejected header-only control ID/return, RPC metadata and UART2 pre-ready/closed/delivered/short counters");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cellular-preready-dispatch-") as tmp:
    path = Path(tmp)
    rejected = header.with_name("mtk_mt6797_ccci_rx_rejected.h").read_text()
    (path / "test.c").write_text(shim + header.read_text() + rejected + dispatch_shim + decoder + pre_ready + dispatch_tests)
    subprocess.run([os.environ.get("CC", "cc"), "-O2", "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-variable", "-Wno-unused-function", "-pthread",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=5)
