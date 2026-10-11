#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual diagnostic helpers with IRQ-lock/READ_ONCE host adapters."""
import os
from pathlib import Path
import re
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
assert q.index("mt6797_ccci_at_rx_close") < q.index("atomic_xchg") < q.index("mt6797_ccci_first_fatal") < q.index("writel")
assert source.index("skb_queue_head_init(&ccci->at_rx_queue)") < source.index("wwan_create_port")
remove = body("mt6797_ccci_remove_port")
assert remove.index("mt6797_ccci_at_rx_close") < remove.index("ccci->at_port = NULL") < remove.index("wwan_remove_port")
cleanup = body("mt6797_ccci_cleanup_port")
assert cleanup.index("mt6797_ccci_at_terminal_close") < cleanup.index("cancel_work_sync") < cleanup.index("mt6797_ccci_remove_port")
remove = body("mt6797_ccci_remove(")
assert remove.index("mt6797_ccci_at_terminal_close") < remove.index("disable_irq")
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
# Use the actual native ownership helpers with a small skb/WWAN-core adapter.
# WWAN's first-start/last-stop ordering is modeled below; no RX algorithm is
# duplicated. Opaque numeric frames contain no command/subscriber text.
at_limits = "\n".join(re.findall(r"^#define MT6797_CCCI_(?:RX_SIZE|AT_RX_\w+)[^\n]*", source, re.M))
at_helpers = "\n".join(body(name) for name in (
    "mt6797_ccci_at_rx_close", "mt6797_ccci_remove_port",
    "mt6797_ccci_at_start", "mt6797_ccci_at_stop", "mt6797_ccci_at_rx("))
dispatch_shim = r'''
#include <errno.h>
#include <stdlib.h>
#include <sched.h>
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
#define atomic_read(p) __atomic_load_n(p,__ATOMIC_SEQ_CST)
#define atomic_set(p,v) __atomic_store_n(p,v,__ATOMIC_SEQ_CST)
struct mtk_ccci_header { u32 data0,data1,status,reserved; };
struct mtk_ccci_frame { u32 data0,data1,reserved; u16 channel,sequence; bool assert_bit; };
struct sk_buff { u8 *data,*base; size_t len; unsigned truesize; struct sk_buff *next; bool heap; };
struct sk_buff_head { pthread_mutex_t lock; struct sk_buff *head,*tail; unsigned qlen; };
struct mt6797_ccci;
struct wwan_port { struct mt6797_ccci *ccci; struct sk_buff_head rxq; pthread_mutex_t ops_lock; unsigned start_count; bool removed,txoff; };
struct mt6797_ccci {
 struct mt6797_ccci_diag diag; struct mt6797_rx_rejected rx_rejected;
 int ready,at_open,failed,teardown; struct wwan_port *at_port;
 struct sk_buff_head at_rx_queue; unsigned at_rx_bytes,at_rx_memory; bool at_rx_stopped;
 int rx_sequence[256];
};
static pthread_mutex_t *at_lock;
static __thread bool at_locked;
#undef spin_lock_irqsave
#undef spin_unlock_irqrestore
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!pthread_mutex_lock(p));if((p)==at_lock){assert(!at_locked);at_locked=true;}}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);if((p)==at_lock){assert(at_locked);at_locked=false;}assert(!pthread_mutex_unlock(p));}while(0)
static int deliveries,frees,live,controls,crashes,control_result;
static void queue_init(struct sk_buff_head *q){memset(q,0,sizeof(*q));assert(!pthread_mutex_init(&q->lock,NULL));}
static void __skb_queue_tail(struct sk_buff_head *q,struct sk_buff *s){
 assert(s&&!s->next);if(q->tail)q->tail->next=s;else q->head=s;q->tail=s;q->qlen++;
}
static struct sk_buff *__skb_dequeue(struct sk_buff_head *q){
 struct sk_buff *s=q->head;if(!s)return NULL;q->head=s->next;s->next=NULL;if(!q->head)q->tail=NULL;assert(q->qlen);q->qlen--;return s;
}
static void kfree_skb(struct sk_buff *s){
 __atomic_add_fetch(&frees,1,__ATOMIC_SEQ_CST);if(s->heap){__atomic_sub_fetch(&live,1,__ATOMIC_SEQ_CST);free(s->base);free(s);}
}
static void skb_queue_purge(struct sk_buff_head *q){
 assert(!pthread_mutex_lock(&q->lock));struct sk_buff *s;while((s=__skb_dequeue(q)))kfree_skb(s);assert(!pthread_mutex_unlock(&q->lock));
}
static int mtk_ccci_net_rx_link(u16 ch){(void)ch;return -1;}
static void skb_pull(struct sk_buff *s,size_t n){assert(s->len>=n);s->data+=n;s->len-=n;}
static void *wwan_port_get_drvdata(struct wwan_port *p){return p->ccci;}
static void wwan_port_txon(struct wwan_port *p){p->txoff=false;}
static void wwan_port_txoff(struct wwan_port *p){p->txoff=true;}
static pthread_barrier_t entered,release_delivery,close_entered,release_close;
static bool pause_delivery,pause_close;
static void barrier(pthread_barrier_t *b){int r=pthread_barrier_wait(b);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
static void wwan_port_rx(struct wwan_port *p,struct sk_buff *s){
 assert(at_locked&&!p->removed&&s->len>=4);
 if(pause_delivery){pause_delivery=false;barrier(&entered);barrier(&release_delivery);}
 assert(!pthread_mutex_lock(&p->rxq.lock));__skb_queue_tail(&p->rxq,s);assert(!pthread_mutex_unlock(&p->rxq.lock));deliveries++;
}
static void mt6797_ccci_at_stop(struct wwan_port *);
/* Core removal prevents new ops, invokes stop only when started, then purges. */
static void wwan_remove_port(struct wwan_port *p){
 assert(!at_locked);assert(!pthread_mutex_lock(&p->ops_lock));
 if(p->start_count){mt6797_ccci_at_stop(p);p->start_count=0;}
 p->removed=true;assert(!pthread_mutex_unlock(&p->ops_lock));skb_queue_purge(&p->rxq);
}
#define MT6797_RPC_RX 32
#define MT6797_FS_RX 14
static int mt6797_ccci_rpc_rx(struct mt6797_ccci *c,struct sk_buff *s,unsigned q){(void)c;(void)s;(void)q;return -EOPNOTSUPP;}
static int mt6797_ccci_fs_rx(struct mt6797_ccci *c,struct sk_buff *s,unsigned q){(void)c;(void)s;(void)q;return -EOPNOTSUPP;}
static int mt6797_ccci_control_rx(struct mt6797_ccci *c,struct sk_buff *s,const struct mtk_ccci_frame *f){(void)c;(void)s;assert(f->channel==0);controls++;return control_result;}
static void mt6797_ccci_quiesce(struct mt6797_ccci *c,int why){assert(why==-EILSEQ&&c->diag.rx_count==1);mt6797_ccci_diag_freeze(&c->diag,NULL);crashes++;}
'''
dispatch_tests = r'''
static void init(struct mt6797_ccci *c,struct wwan_port *p){
 memset(c,0,sizeof(*c));memset(p,0,sizeof(*p));
 assert(!pthread_mutex_init(&c->diag.lock,NULL));assert(!pthread_mutex_init(&c->rx_rejected.lock,NULL));
 queue_init(&c->at_rx_queue);queue_init(&p->rxq);assert(!pthread_mutex_init(&p->ops_lock,NULL));
 c->at_port=p;p->ccci=c;at_lock=&c->at_rx_queue.lock;
}
static void destroy(struct mt6797_ccci *c,struct wwan_port *p){
 assert(!c->at_rx_queue.qlen&&!c->at_rx_bytes&&!c->at_rx_memory&&!p->rxq.qlen);
 assert(!pthread_mutex_destroy(&c->diag.lock));assert(!pthread_mutex_destroy(&c->rx_rejected.lock));
 assert(!pthread_mutex_destroy(&c->at_rx_queue.lock));assert(!pthread_mutex_destroy(&p->rxq.lock));assert(!pthread_mutex_destroy(&p->ops_lock));
}
static struct sk_buff *frame(u32 id,unsigned len,unsigned memory,bool header){
 assert(len>=4);struct sk_buff *s=calloc(1,sizeof(*s));assert(s);
 s->base=calloc(1,len+(header?16:0));assert(s->base);s->data=s->base;s->len=len+(header?16:0);s->truesize=memory;s->heap=true;
 if(header){u32 *h=(u32 *)s->data;h[1]=s->len;h[2]=10;memcpy(s->data+16,&id,4);}else memcpy(s->data,&id,4);
 __atomic_add_fetch(&live,1,__ATOMIC_SEQ_CST);return s;
}
static u32 pop(struct wwan_port *p){
 assert(!pthread_mutex_lock(&p->rxq.lock));struct sk_buff *s=__skb_dequeue(&p->rxq);assert(!pthread_mutex_unlock(&p->rxq.lock));
 assert(s);u32 id;memcpy(&id,s->data,4);kfree_skb(s);return id;
}
/* Core callbacks run only at first open/last close under ops_lock. */
static int core_start(struct wwan_port *p){
 assert(!pthread_mutex_lock(&p->ops_lock));int ret=p->removed?-ENODEV:0;
 if(!ret&&!p->start_count)ret=mt6797_ccci_at_start(p);
 if(!ret)p->start_count++;
 assert(!pthread_mutex_unlock(&p->ops_lock));return ret;
}
static void core_stop(struct wwan_port *p){
 assert(!pthread_mutex_lock(&p->ops_lock));assert(p->start_count);
 if(!--p->start_count){mt6797_ccci_at_stop(p);if(pause_close){barrier(&close_entered);barrier(&release_close);}skb_queue_purge(&p->rxq);}
 assert(!pthread_mutex_unlock(&p->ops_lock));
}
static void *start_thread(void *p){assert(!core_start(p));return NULL;}
static void *stop_thread(void *p){core_stop(p);return NULL;}
struct incoming {struct mt6797_ccci *c;struct sk_buff *s;};
static void *rx_thread(void *arg){struct incoming *i=arg;mt6797_ccci_at_rx(i->c,i->s);return NULL;}
static void *terminal_thread(void *p){mt6797_ccci_at_rx_close(p,true);return NULL;}
static void *remove_thread(void *p){mt6797_ccci_remove_port(p);return NULL;}
static void begin_pause(void){assert(!pthread_barrier_init(&entered,NULL,2));assert(!pthread_barrier_init(&release_delivery,NULL,2));pause_delivery=true;}
static void end_pause(void){assert(!pthread_barrier_destroy(&entered));assert(!pthread_barrier_destroy(&release_delivery));}
static void diagnostic_tests(void){
 struct mt6797_ccci c;struct wwan_port p;init(&c,&p);u32 words[8]={0};struct sk_buff skb={.data=(u8 *)words,.len=15};
 mt6797_ccci_dispatch_rx(&c,&skb,1);assert(frees==1&&c.diag.rx_count==1&&!c.diag.rx[0].header_valid&&!c.diag.rx[0].channel);
 assert(c.diag.rx[0].decode_error==-EMSGSIZE&&!c.diag.rx[0].payload_word0_valid);
 skb.len=16;mt6797_ccci_dispatch_rx(&c,&skb,0);assert(controls==1&&frees==2&&c.diag.rx_count==1);
 words[2]=32;words[4]=0x4006;skb.len=20;mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(c.diag.rx_count==2&&c.diag.rx[1].channel==32&&c.diag.rx[1].payload_word0==0x4006);
 assert(c.diag.rx[1].cause==MT6797_CCCI_DIAG_BEFORE_READY&&c.rx_rejected.count==1&&c.rx_rejected.entry[0].channel==32);
 assert(c.rx_rejected.entry[0].op_valid&&c.rx_rejected.entry[0].op==0x4006);
 c.ready=1;control_result=-EALREADY;words[1]=4;words[2]=0;skb.len=16;mt6797_ccci_dispatch_rx(&c,&skb,0);
 assert(controls==2&&frees==4&&c.diag.rx_count==2&&c.rx_rejected.count==2&&c.rx_rejected.entry[1].channel==0);
 assert(c.rx_rejected.entry[1].message_valid&&c.rx_rejected.entry[1].message==4&&!c.rx_rejected.entry[1].op_valid&&c.rx_rejected.entry[1].error==-EALREADY);
 c.ready=0;skb.len=20;words[2]=300;mt6797_ccci_dispatch_rx(&c,&skb,1);assert(c.diag.rx_count==3&&c.diag.rx[2].cause==MT6797_CCCI_DIAG_BAD_CHANNEL);destroy(&c,&p);
 init(&c,&p);words[2]=0x80030020;c.rx_sequence[32]=1;mt6797_ccci_dispatch_rx(&c,&skb,1);assert(crashes==1&&c.diag.frozen&&c.diag.rx[0].cause==MT6797_CCCI_DIAG_BAD_SEQUENCE);destroy(&c,&p);
}
static void retention_tests(void){
 struct mt6797_ccci c;struct wwan_port p;init(&c,&p);
 assert(!core_start(&p)&&c.at_open&&p.txoff);core_stop(&p); /* preserve pre-HS2 discovery */
 mt6797_ccci_dispatch_rx(&c,frame(1,4,4096,true),1);assert(!c.at_rx_queue.qlen&&!p.rxq.qlen);
 c.ready=1;mt6797_ccci_dispatch_rx(&c,frame(2,4,4096,true),1);
 assert(c.at_rx_queue.qlen==1&&c.at_rx_bytes==4&&c.at_rx_memory==4096);
 assert(!c.diag.at_events[MT6797_CCCI_AT_RX_CLOSED_DROP]&&!p.rxq.qlen);
 mt6797_ccci_dispatch_rx(&c,frame(3,4,4096,true),1);assert(!core_start(&p)&&!p.txoff);
 assert(!c.at_rx_queue.qlen&&!c.at_rx_bytes&&!c.at_rx_memory&&pop(&p)==2&&pop(&p)==3);
 mt6797_ccci_dispatch_rx(&c,frame(4,4,4096,true),1);assert(pop(&p)==4);
 assert(!core_start(&p));core_stop(&p);assert(c.at_open&&p.start_count==1);core_stop(&p);assert(!c.at_open);
 unsigned before=c.diag.at_events[MT6797_CCCI_AT_RX_DELIVERED];u32 short_header[4]={0,16,10,0};struct sk_buff short_skb={.data=(u8 *)short_header,.len=16};
 mt6797_ccci_dispatch_rx(&c,&short_skb,1);assert(!c.at_rx_queue.qlen&&c.diag.at_events[MT6797_CCCI_AT_RX_DELIVERED]==before);
 for(unsigned i=0;i<32;i++)mt6797_ccci_at_rx(&c,frame(100+i,MT6797_CCCI_AT_RX_PAYLOAD,8192,false));
 assert(c.at_rx_queue.qlen==32&&c.at_rx_bytes==MT6797_CCCI_AT_RX_BYTES&&c.at_rx_memory==MT6797_CCCI_AT_RX_MEMORY);
 mt6797_ccci_at_rx(&c,frame(200,4,4096,false));assert(c.at_rx_queue.qlen==32&&c.diag.at_events[MT6797_CCCI_AT_RX_CLOSED_DROP]==1);
 assert(!core_start(&p));for(unsigned i=0;i<32;i++)assert(pop(&p)==100+i);core_stop(&p);
 mt6797_ccci_at_rx(&c,frame(201,MT6797_CCCI_AT_RX_PAYLOAD+1,4096,false));assert(!c.at_rx_queue.qlen);
 mt6797_ccci_at_rx(&c,frame(202,4,UINT32_MAX,false));assert(!c.at_rx_queue.qlen&&!c.at_rx_memory);
 /* Tiny payloads cannot evade the separate allocation-accounting budget. */
 mt6797_ccci_at_rx(&c,frame(203,4,MT6797_CCCI_AT_RX_MEMORY,false));assert(c.at_rx_queue.qlen==1&&c.at_rx_bytes==4);
 mt6797_ccci_at_rx(&c,frame(204,4,1,false));assert(c.at_rx_queue.qlen==1&&c.at_rx_memory==MT6797_CCCI_AT_RX_MEMORY);
 mt6797_ccci_at_rx_close(&c,true);assert(!c.at_rx_queue.qlen&&!c.at_rx_bytes&&!c.at_rx_memory&&c.at_rx_stopped);
 assert(core_start(&p)==-ESHUTDOWN);mt6797_ccci_at_rx(&c,frame(205,4,4096,false));assert(!p.rxq.qlen&&!c.at_rx_queue.qlen);
 mt6797_ccci_at_rx_close(&c,true);mt6797_ccci_remove_port(&c);destroy(&c,&p);
 init(&c,&p);c.ready=1;mt6797_ccci_at_rx(&c,frame(206,4,4096,false));mt6797_ccci_remove_port(&c); /* never-opened probe cleanup */
 assert(p.removed&&core_start(&p)==-ENODEV);mt6797_ccci_at_rx(&c,frame(207,4,4096,false));destroy(&c,&p);
 for(unsigned which=0;which<3;which++){init(&c,&p);c.ready=1;if(which==0)c.failed=1;else if(which==1)c.teardown=1;else c.ready=0;
 mt6797_ccci_at_rx(&c,frame(208,4,4096,false));assert(!c.at_rx_queue.qlen&&!p.rxq.qlen);if(which<2)assert(core_start(&p)==-ESHUTDOWN);destroy(&c,&p);}
}
static void concurrency_tests(void){
 for(unsigned iteration=0;iteration<32;iteration++){
  struct mt6797_ccci c;struct wwan_port p;pthread_t a,b;init(&c,&p);c.ready=1;
  mt6797_ccci_at_rx(&c,frame(300,4,4096,false));mt6797_ccci_at_rx(&c,frame(301,4,4096,false));
  begin_pause();assert(!pthread_create(&a,NULL,start_thread,&p));barrier(&entered);
  struct incoming incoming={&c,frame(302,4,4096,false)};assert(!pthread_create(&b,NULL,rx_thread,&incoming));
  barrier(&release_delivery);assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));end_pause();
  assert(pop(&p)==300&&pop(&p)==301&&pop(&p)==302&&!c.at_rx_queue.qlen&&!c.at_rx_bytes&&!c.at_rx_memory);
  /* A delivery holding RX ownership fences last close. A later arrival across
   * stop -> core purge goes to the native FIFO and survives that core purge. */
  begin_pause();incoming=(struct incoming){&c,frame(303,4,4096,false)};assert(!pthread_create(&a,NULL,rx_thread,&incoming));barrier(&entered);
  assert(!pthread_barrier_init(&close_entered,NULL,2));assert(!pthread_barrier_init(&release_close,NULL,2));pause_close=true;
  assert(!pthread_create(&b,NULL,stop_thread,&p));barrier(&release_delivery);barrier(&close_entered);
  assert(!c.at_open&&p.rxq.qlen==1);mt6797_ccci_at_rx(&c,frame(304,4,4096,false));assert(c.at_rx_queue.qlen==1);
  barrier(&release_close);assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));end_pause();pause_close=false;
  assert(!pthread_barrier_destroy(&close_entered));assert(!pthread_barrier_destroy(&release_close));
  assert(!p.rxq.qlen&&!core_start(&p)&&pop(&p)==304);core_stop(&p);
  /* Fatal purge serializes with a start already draining; no frame can arrive
   * after its terminal gate. Test removal through the same race on alternating runs. */
  mt6797_ccci_at_rx(&c,frame(305,4,4096,false));mt6797_ccci_at_rx(&c,frame(306,4,4096,false));
  begin_pause();assert(!pthread_create(&a,NULL,start_thread,&p));barrier(&entered);
  assert(!pthread_create(&b,NULL,iteration&1?remove_thread:terminal_thread,&c));barrier(&release_delivery);
  assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));end_pause();
  assert(c.at_rx_stopped&&!c.at_rx_queue.qlen&&!c.at_rx_bytes&&!c.at_rx_memory);
  mt6797_ccci_at_rx(&c,frame(307,4,4096,false));assert(!c.at_rx_queue.qlen);
  if(!(iteration&1)){assert(pop(&p)==305&&pop(&p)==306);mt6797_ccci_remove_port(&c);}
  else assert(!p.rxq.qlen&&p.removed);
  destroy(&c,&p);
 }
}
int main(void){
 diagnostic_tests();retention_tests();concurrency_tests();assert(!live);
 puts("PASS actual dispatch/AT owners: pre-HS2 open, bounded opaque preopen FIFO, count/payload/truesize caps, multiple opens, stop/core-purge boundary,32 start/RX/fatal/remove races, terminal idempotence and never-opened removal");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cellular-preready-dispatch-") as tmp:
    path = Path(tmp)
    rejected = header.with_name("mtk_mt6797_ccci_rx_rejected.h").read_text()
    (path / "test.c").write_text(shim + header.read_text() + rejected + dispatch_shim + at_limits + "\n" + at_helpers + decoder + pre_ready + dispatch_tests)
    subprocess.run([os.environ.get("CC", "cc"), "-O2", "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-variable", "-Wno-unused-function", "-pthread",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=10)
