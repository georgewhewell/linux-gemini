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
static void reset(void)
{
 memset(&d, 0, sizeof(d)); assert(!pthread_mutex_init(&d.lock, NULL));
 memset(hs1, 0x12, sizeof(hs1)); memset(reply, 0x34, sizeof(reply));
 memset(runtime, 0x56, sizeof(runtime)); memset(gpd, 0x81, sizeof(gpd));
}
static void *race(void *unused)
{
 (void)unused;
 mt6797_ccci_diag_irq(&d, 1, 2, 3, 4);
 mt6797_ccci_diag_payload(&d, hs1, sizeof(hs1), reply, sizeof(reply), runtime, 324);
 mt6797_ccci_diag_tx(&d, false, 1, 0, 1);
 if (mt6797_ccci_diag_freeze(&d, gpd)) __atomic_fetch_add(&winners, 1, __ATOMIC_SEQ_CST);
 return NULL;
}
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
