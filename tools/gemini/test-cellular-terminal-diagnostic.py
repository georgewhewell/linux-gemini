#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual CCCI terminal path; only first non-shutdown failure logs.

Host adapters cover diagnostic count/order and unchanged STOP/fatal actions,
not hardware quiescence or actual modem faults.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/net/wwan/mediatek/mtk_mt6797_ccci.c").read_text()
start = source.index("static void mt6797_ccci_quiesce_from(")
end = source.index("\nstatic void mt6797_ccci_free_rings", start)
body = source[start:end]
shim = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define MT6797_CCCI_ACTIVE_IRQS 0x007f007fU
#define MT6797_MD1_CLDMA_ACTIVE_QUEUES 0x7fU
#define CLDMA_L2_TX_MASK_SET 0x428
#define CLDMA_L2_RX_MASK_SET 0x430
#define CLDMA_UL_STOP_CMD 0x5c
#define CLDMA_SO_STOP_CMD 0x3c4
#define atomic_xchg(p,v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
struct mt6797_ccci { int failed; void *dev; char *ao, *pdn; int hs; };
static char ao[0x1000], pdn[0x1000];
static struct mt6797_ccci ccci = {.dev = &ccci, .ao = ao, .pdn = pdn};
static int logs, frontends, writes, reads, crashes, fatals, logged_reason;
static void mt6797_ccci_first_fatal(struct mt6797_ccci *c, const char *site)
{ assert(c == &ccci && site && !logs && !frontends && !writes); }
static void dev_err(void *dev, const char *format, int reason)
{
 assert(dev == &ccci && ccci.failed == 1);
 assert(!strcmp(format, "MT6797_CCCI_TERMINAL_FAILURE reason=%d\n"));
 assert(reason != -ESHUTDOWN && !frontends && !writes && !reads && !crashes && !fatals);
 logs++; logged_reason = reason;
}
static void mt6797_ccci_frontend_ready(struct mt6797_ccci *c, bool ready)
{ assert(c == &ccci && c->failed && !ready); frontends++; }
static void writel(unsigned value, char *address)
{
 const unsigned values[] = { MT6797_CCCI_ACTIVE_IRQS, MT6797_CCCI_ACTIVE_IRQS,
  MT6797_MD1_CLDMA_ACTIVE_QUEUES, MT6797_MD1_CLDMA_ACTIVE_QUEUES };
 char *addresses[] = { pdn + CLDMA_L2_TX_MASK_SET, ao + CLDMA_L2_RX_MASK_SET,
  pdn + CLDMA_UL_STOP_CMD, pdn + CLDMA_SO_STOP_CMD };
 assert(frontends == 1 && writes < 4);
 assert(value == values[writes] && address == addresses[writes]); writes++;
}
static unsigned readl(char *address)
{ assert(address == pdn + CLDMA_SO_STOP_CMD && writes == 4); reads++; return 0; }
static void mt6797_md1_hs_crash(int *hs, int reason)
{ assert(hs == &ccci.hs && reads == 1); (void)reason; crashes++; }
static void mt6797_md1_cold_owner_transport_fatal(int reason)
{ assert(reason != -ESHUTDOWN && crashes == 1 && logs == 1); fatals++; }
'''
tests = r'''
static void reset(void)
{ ccci.failed = logs = frontends = writes = reads = crashes = fatals = logged_reason = 0; }
static void *racer(void *unused)
{ (void)unused; mt6797_ccci_quiesce(&ccci, -EIO); return NULL; }
int main(void)
{
 const int reasons[] = {-EIO, -ETIMEDOUT, -EPROTO, -ENOMEM, -EILSEQ, -EOPNOTSUPP};
 for (unsigned i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
  reset(); mt6797_ccci_quiesce(&ccci, reasons[i]);
  assert(logs == 1 && logged_reason == reasons[i]);
  assert(frontends == 1 && writes == 4 && reads == 1 && crashes == 1 && fatals == 1);
  for (int repeat = 0; repeat < 8; repeat++) mt6797_ccci_quiesce(&ccci, -EIO);
  mt6797_ccci_quiesce(&ccci, -ESHUTDOWN);
  assert(logs == 1 && logged_reason == reasons[i] && writes == 4 && fatals == 1);
 }
 reset(); mt6797_ccci_quiesce(&ccci, -ESHUTDOWN);
 assert(!logs && !fatals && frontends == 1 && writes == 4 && reads == 1 && crashes == 1);
 mt6797_ccci_quiesce(&ccci, -ESHUTDOWN); mt6797_ccci_quiesce(&ccci, -EIO);
 assert(!logs && !fatals && writes == 4);
 reset(); pthread_t threads[32];
 for (unsigned i = 0; i < 32; i++) assert(!pthread_create(&threads[i], NULL, racer, NULL));
 for (unsigned i = 0; i < 32; i++) assert(!pthread_join(threads[i], NULL));
 assert(logs == 1 && logged_reason == -EIO && writes == 4 && fatals == 1);
 puts("PASS actual CCCI terminal path: first failure logs once before MMIO, repeated/orderly calls quiet, unchanged STOP sequence, 32-way race");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-ccci-terminal-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(shim + body + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-pthread", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=5)
