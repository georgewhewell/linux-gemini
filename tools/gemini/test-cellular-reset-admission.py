#!/usr/bin/env python3
"""Execute actual supplier reset callback and reset-core acquisition unwind."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
owner = (root / "drivers/soc/mediatek/mt6797-md1-cold-owner.c").read_text()
core = (root / "drivers/reset/core.c").read_text()

def function(source, name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    end = source.index("\n}\n", start) + 3
    return source[start:end]

c = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*(x)))
#define dev_info(...) do {} while (0)
struct device { int unused; };
struct reset_control { int id; };
struct reset_control_bulk_data { const char *id; struct reset_control *rstc; };
struct mt6797_md1_cold_owner_cldma {
 unsigned coda_version, ul_status, so_status, l2_tx_status, l2_rx_status, ip_busy;
};
struct mt6797_md1_cold_owner_backend {
 struct device *dev;
 struct reset_control_bulk_data cldma_resets[2];
 bool cldma_reset_completed;
};
static struct mt6797_md1_cold_owner_cldma snapshot;
static int calls, fail_at, watchdog_error, observe_error, reads;
static int mtk_wdt_require_boot_recovery(struct device *dev)
{ (void)dev; assert(calls == 0); return watchdog_error; }
static int reset_call(struct reset_control *r, int deassert)
{
 assert(r->id == calls/2);
 assert(deassert == calls%2);
 calls++;
 return calls == fail_at ? -EIO : 0;
}
static int reset_control_assert(struct reset_control *r) { return reset_call(r,0); }
static int reset_control_deassert(struct reset_control *r) { return reset_call(r,1); }
static int mt6797_md1_cold_cldma_registers_observe(void *b,
 struct mt6797_md1_cold_owner_cldma *state)
{ (void)b; assert(calls == 4); reads++; *state=snapshot; return observe_error; }
''' + function(owner, "mt6797_md1_cold_cldma_reset") + r'''
enum reset_control_flags { EXCLUSIVE };
static struct reset_control controls[2] = {{0},{1}};
static int gets, puts_count, fail_get;
#define IS_ERR(p) ((intptr_t)(p) < 0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
static struct reset_control *__reset_control_get(struct device *d, const char *id,
 int index, enum reset_control_flags flags)
{
 (void)d; (void)id; assert(index==0 && flags==EXCLUSIVE);
 gets++;
 return gets == fail_get ? (void *)(intptr_t)-EBUSY : &controls[gets-1];
}
static void reset_control_put_internal(struct reset_control *r)
{ assert(r == &controls[puts_count]); puts_count++; }
''' + function(core, "__reset_control_bulk_get") + r'''
int main(void)
{
 struct mt6797_md1_cold_owner_backend b = {
  .cldma_resets={{"ao", &controls[0]}, {"pd", &controls[1]}}
 };
 watchdog_error=-EPERM;
 assert(mt6797_md1_cold_cldma_reset(&b)==-EPERM && !calls && !reads);
 watchdog_error=0;
 for (fail_at=1; fail_at<=4; fail_at++) {
  calls=0; b.cldma_reset_completed=true;
  assert(mt6797_md1_cold_cldma_reset(&b)==-EIO);
  assert(calls==fail_at && !b.cldma_reset_completed && !reads);
 }
 fail_at=0; calls=0; observe_error=-EIO;
 assert(mt6797_md1_cold_cldma_reset(&b)==-EIO && !b.cldma_reset_completed);
 observe_error=0;
 for (unsigned field=0; field<5; field++) {
  unsigned *fields[]={&snapshot.ul_status,&snapshot.so_status,&snapshot.l2_tx_status,
   &snapshot.l2_rx_status,&snapshot.ip_busy};
  *fields[field]=1; calls=0;
  assert(mt6797_md1_cold_cldma_reset(&b)==-EUCLEAN && !b.cldma_reset_completed);
  *fields[field]=0;
 }
 calls=0;
 assert(!mt6797_md1_cold_cldma_reset(&b) && b.cldma_reset_completed);
 for (fail_get=1; fail_get<=2; fail_get++) {
  gets=puts_count=0;
  assert(__reset_control_bulk_get(NULL,2,b.cldma_resets,EXCLUSIVE)==-EBUSY);
  assert(gets==fail_get && puts_count==fail_get-1);
 }
 gets=puts_count=0; fail_get=0;
 assert(!__reset_control_bulk_get(NULL,2,b.cldma_resets,EXCLUSIVE));
 assert(gets==2 && !puts_count);
 puts("PASS actual reset callback: guard, AO/PD order, four failures, fresh status, bulk acquisition unwind");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cellular-reset-") as directory:
    path = Path(directory)
    (path / "reset.c").write_text(c)
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                    str(path / "reset.c"), "-o", str(path / "reset")], check=True)
    subprocess.run([str(path / "reset")], check=True, timeout=5)

power = function(owner, "mt6797_md1_cold_power_on")
assert power.index("mt6797_md1_cold_cldma_reset(") < power.index("mt6797_md1_pmic_snapshot(")
probe = function(owner, "mt6797_md1_cold_owner_probe")
assert probe.index("devm_reset_control_bulk_get_exclusive(") < probe.index("mt6797_md1_firmware_load(")
assert probe.index('cldma_resets[0].id = "cldma-ao"') < probe.index('cldma_resets[1].id = "cldma-pdn"')
print("PASS reset acquisition before writes and pulse before modem power")

transport = (root / "drivers/net/wwan/mediatek/mtk_mt6797_ccci.c").read_text()
defines = "\n".join(line for line in transport.splitlines() if line.startswith((
    "#define CLDMA_UL_STATUS", "#define CLDMA_SO_STATUS", "#define CLDMA_L2_TX_STATUS",
    "#define CLDMA_L2_RX_STATUS", "#define CLDMA_UL_START_ADDR", "#define CLDMA_SO_START_ADDR",
    "#define MT6797_CCCI_QUEUE_COUNT")))
clean = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define dev_info(...) do {} while (0)
struct mt6797_ccci { unsigned char *ao, *pdn; void *dev; };
static unsigned calls, inject;
static u32 readl(void *address) { (void)address; return ++calls == inject ? 1 : 0; }
''' + defines + "\n" + function(transport, "mt6797_ccci_clean_reset_check") + r'''
int main(void)
{
 unsigned char ao[0x1e00],pdn[0x1e00];
 struct mt6797_ccci ccci={ao,pdn,0};
 assert(!mt6797_ccci_clean_reset_check(&ccci));
 assert(calls == 19);
 for (inject=1; inject<=19; inject++) {
  calls=0;
  assert(mt6797_ccci_clean_reset_check(&ccci)==-EUCLEAN && calls==19);
 }
 puts("PASS actual pre-ring snapshot: every queue/IRQ/busy/head read rejects nonzero state");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cellular-clean-") as directory:
    path = Path(directory)
    (path / "clean.c").write_text(clean)
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                    str(path / "clean.c"), "-o", str(path / "clean")], check=True)
    subprocess.run([str(path / "clean")], check=True, timeout=5)
start = function(transport, "mt6797_ccci_start")
assert start.index("mt6797_ccci_clean_reset_check(") < start.index("writel(")
