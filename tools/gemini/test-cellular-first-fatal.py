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
typedef pthread_mutex_t spinlock_t;
#define U32_MAX UINT32_MAX
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
assert source.index("spin_lock_init(&ccci->diag.lock)") < source.index("devm_request_irq")
print("PASS fatal-before-STOP, separate WDT/timer sources, IRQ capture before W1C, no diagnostic MMIO or pointer output")
dispatch = body("mt6797_ccci_dispatch_rx")
assert dispatch.index("diag_drop, ret, ret ? 0 : frame.channel") < dispatch.index("kfree_skb")
assert "MT6797_CCCI_DIAG_BAD_HEADER" in dispatch and "MT6797_CCCI_DIAG_BEFORE_READY" in dispatch
assert dispatch.index("MT6797_CCCI_DIAG_BAD_SEQUENCE") < dispatch.index("mt6797_ccci_quiesce")
assert "mt6797_ccci_dispatch_rx(ccci, skb, q)" in source
print("PASS actual pre-ready RX capture:4x1KiB bound, lengths/truncation, malformed/short/opcode guards, immutable/frozen/full,32-way cap and save-before-drop")

# Compile the actual dispatch path through the pre-ready branch and its actual
# drop epilogue. Only the post-ready network-delivery branch is excluded.
common = header.with_name("mtk_ccci_common.c").read_text()
begin = common.index("int mtk_ccci_header_decode(")
decoder = common[begin:common.index("\n}\n", begin) + 3]
pre_ready = dispatch[:dispatch.index("\tif (frame.data1 != skb->len")]
pre_ready += '\tassert(!"post-ready branch outside this test");\n' + dispatch[dispatch.index("drop:\n"):]
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
#define le32_to_cpu(x) (x)
#define atomic_read(p) (*(p))
struct mtk_ccci_header { u32 data0,data1,status,reserved; };
struct mtk_ccci_frame { u32 data0,data1,reserved; u16 channel,sequence; bool assert_bit; };
struct sk_buff { u8 *data; size_t len; };
struct mt6797_ccci { struct mt6797_ccci_diag diag; int ready; int rx_sequence[256]; };
#define MT6797_RPC_RX 32
#define MT6797_FS_RX 14
static int mt6797_ccci_rpc_rx(struct mt6797_ccci *c, struct sk_buff *s, unsigned q)
{ (void)c; (void)s; (void)q; return -EOPNOTSUPP; }
static int mt6797_ccci_fs_rx(struct mt6797_ccci *c, struct sk_buff *s, unsigned q)
{ (void)c; (void)s; (void)q; return -EOPNOTSUPP; }
static int frees, controls, crashes;
static void kfree_skb(struct sk_buff *skb) { (void)skb; frees++; }
static void mt6797_ccci_control_rx(struct mt6797_ccci *c, struct sk_buff *s,
 const struct mtk_ccci_frame *f) { (void)c; (void)s; assert(f->channel==0); controls++; }
static void mt6797_ccci_quiesce(struct mt6797_ccci *c,int why)
{ assert(why==-EILSEQ && c->diag.rx_count==1); mt6797_ccci_diag_freeze(&c->diag,NULL); crashes++; }
'''
dispatch_tests = r'''
int main(void)
{
 struct mt6797_ccci c={0}; u32 words[8]={0};
 struct sk_buff skb={.data=(u8 *)words,.len=15};
 pthread_mutex_init(&c.diag.lock,NULL);
 mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(frees==1 && c.diag.rx_count==1 && !c.diag.rx[0].header_valid && !c.diag.rx[0].channel);
 assert(c.diag.rx[0].decode_error==-EMSGSIZE && !c.diag.rx[0].payload_word0_valid);
 skb.len=16; mt6797_ccci_dispatch_rx(&c,&skb,0);
 assert(controls==1 && frees==2 && c.diag.rx_count==1);
 words[2]=32; words[4]=0x4006; skb.len=20;
 mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(c.diag.rx_count==2 && c.diag.rx[1].channel==32 && c.diag.rx[1].payload_word0==0x4006);
 assert(c.diag.rx[1].cause==MT6797_CCCI_DIAG_BEFORE_READY);
 words[2]=300; mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(c.diag.rx_count==3 && c.diag.rx[2].cause==MT6797_CCCI_DIAG_BAD_CHANNEL);
 pthread_mutex_destroy(&c.diag.lock); memset(&c,0,sizeof(c)); pthread_mutex_init(&c.diag.lock,NULL);
 words[2]=0x80030020; c.rx_sequence[32]=1;
 mt6797_ccci_dispatch_rx(&c,&skb,1);
 assert(crashes==1 && c.diag.frozen && c.diag.rx[0].cause==MT6797_CCCI_DIAG_BAD_SEQUENCE);
 pthread_mutex_destroy(&c.diag.lock);
 puts("PASS actual pre-ready dispatch+decoder: malformed header has no channel/opcode, control excluded, RPC retained, invalid-channel and bad-sequence before freeze");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cellular-preready-dispatch-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(shim + header.read_text() + dispatch_shim + decoder + pre_ready + dispatch_tests)
    subprocess.run([os.environ.get("CC", "cc"), "-O2", "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-variable", "-Wno-unused-function", "-pthread",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=5)
