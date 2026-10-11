#!/usr/bin/env python3
"""Execute actual supplier reset callback and reset-core acquisition unwind."""
import os
from pathlib import Path
import subprocess
import tempfile
import re

root = Path(__file__).resolve().parents[2]
owner = (root / "drivers/soc/mediatek/mt6797-md1-cold-owner.c").read_text()
core = (root / "drivers/reset/core.c").read_text()

def function(source, name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    end = source.index("\n}\n", start) + 3
    return source[start:end]

defines = '\n'.join(re.findall(r'^#define MT6797_MD1_(?:SPM_(?:C2K_PWR_CON|PWR_STATUS(?:_2ND|_C2K)?|PWR_RST_B|PWR_ISO|PWR_ON|PWR_ON_2ND|PWR_CLK_DIS)|INFRA_(?:C2K_PROT_MASK|PROTECT_EN|PROTECT_STA1))\s+.*$', owner, re.M))
c = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#define __iomem
typedef uint32_t u32;
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*(x)))
#define dev_info(...) do {} while (0)
#define dev_err(dev, ...) do { (void)(dev); if (0) printf(__VA_ARGS__); } while (0)
#define BIT(n) (1U<<(n))
#define MT6797_MD1_RES_INFRA 0
struct regmap { u32 values[256]; };
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
 void *ap_md_smem;
 u32 ap_md_smem_size;
 void *md1_md3_smem;
 u32 md1_md3_smem_size;
 bool md1_md3_smem_initialized;
 struct regmap *spm;void *base[1];
};
struct mt6797_md1_cold_state { int unused; };
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
static int cold_error, cold_reads, clears, barriers, smem_reads, corrupt_tail;
static unsigned char infra[1024];
static int register_error, register_reads;
static int regmap_read(struct regmap *map, unsigned offset, u32 *value)
{ register_reads++; if(register_error)return register_error;*value=map->values[offset/4];return 0; }
static int mt6797_md1_cold_verify(void *context, struct mt6797_md1_cold_state *state)
{ (void)context; (void)state; cold_reads++; assert(!clears); return cold_error; }
static void memset_io(void *memory, int value, size_t size)
{ assert(cold_reads==1 && !cold_error && !watchdog_error); clears++; memset(memory,value,size); }
static void wmb(void) { assert(clears==2); barriers++; }
static u32 readl(void *memory)
{ u32 word; memcpy(&word,memory,4);
 if(memory==infra+0x220 || memory==infra+0x228)return word;
 assert(barriers==1);smem_reads++;return word|corrupt_tail; }
''' + defines + '\n' + function(owner, "mt6797_md1_cold_verify_c2k_off") + '\n' + function(owner, "mt6797_md1_cold_clear_smem") + r'''
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
 /* Actual read-only C2K guard rejects power, reset and isolation
  * mismatches before either shared-memory reservation can be cleared. */
 struct regmap spm={0};b.spm=&spm;b.base[0]=infra;
 spm.values[0x328/4]=0x12;
 u32 protection=0xe000;memcpy(infra+0x220,&protection,4);memcpy(infra+0x228,&protection,4);
 assert(!mt6797_md1_cold_verify_c2k_off(&b));
 for(unsigned bit=0;bit<5;bit++) {
  spm.values[0x328/4]^=1U<<bit;
  assert(mt6797_md1_cold_verify_c2k_off(&b)==-EBUSY);
  spm.values[0x328/4]^=1U<<bit;
 }
 for(unsigned which=0;which<2;which++) {
  spm.values[(0x180+4*which)/4]=1U<<28;
  assert(mt6797_md1_cold_verify_c2k_off(&b)==-EBUSY);
  spm.values[(0x180+4*which)/4]=0;
  for(unsigned bit=13;bit<=15;bit++) {
   u32 altered=protection^(1U<<bit);memcpy(infra+0x220+8*which,&altered,4);
   assert(!mt6797_md1_cold_verify_c2k_off(&b));
   memcpy(infra+0x220+8*which,&protection,4);
  }
 }
 /* Trial126 LK state: both C2K acknowledgements off, control0x12, bus
  * protection unset. No request/transition is needed for an already-off master. */
 u32 lk_en=0x104b8,lk_sta=0xe04944b8;
 memcpy(infra+0x220,&lk_en,4);memcpy(infra+0x228,&lk_sta,4);
 spm.values[0x180/4]=0x2b3000fc;spm.values[0x184/4]=0x2b3000ec;
 assert(!mt6797_md1_cold_verify_c2k_off(&b));
 unsigned char intermodem[72];memset(intermodem,0xa5,sizeof(intermodem));
 b.md1_md3_smem=intermodem+4;b.md1_md3_smem_size=64;
 /* Guard words model the AP-MD3 region that must remain untouched. */
 unsigned char shared[72]; memset(shared,0xa5,sizeof(shared));
 b.ap_md_smem=shared+4; b.ap_md_smem_size=64; calls=0;
 watchdog_error=-EPERM;
 assert(mt6797_md1_cold_clear_smem(&b)==-EPERM && !cold_reads && !clears);
 watchdog_error=0; cold_error=-EBUSY;
 assert(mt6797_md1_cold_clear_smem(&b)==-EBUSY && cold_reads==1 && !clears);
 cold_error=0; cold_reads=0;
 b.ap_md_smem_size=0;
 assert(mt6797_md1_cold_clear_smem(&b)==-EINVAL && !cold_reads && !clears);
 b.ap_md_smem_size=63;
 assert(mt6797_md1_cold_clear_smem(&b)==-EINVAL && !cold_reads && !clears);
 b.ap_md_smem_size=64;
 register_error=-EIO;
 assert(mt6797_md1_cold_clear_smem(&b)==-EIO&&!clears&&!b.md1_md3_smem_initialized);
 register_error=0;cold_reads=0;spm.values[0x328/4]=0x1d;
 assert(mt6797_md1_cold_clear_smem(&b)==-EBUSY&&!clears&&!b.md1_md3_smem_initialized);
 spm.values[0x328/4]=0x12;cold_reads=0;
 assert(!mt6797_md1_cold_clear_smem(&b) && cold_reads==1 && clears==2 && barriers==1 && smem_reads==2);
 assert(b.md1_md3_smem_initialized);
 assert(mt6797_md1_cold_clear_smem(&b)==-EINVAL&&clears==2);
 for(unsigned i=0;i<sizeof(intermodem);i++) assert(intermodem[i]==(i<4||i>=68?0xa5:0));
 for(unsigned i=0;i<sizeof(shared);i++) assert(shared[i]==(i<4||i>=68?0xa5:0));
 cold_reads=clears=barriers=smem_reads=0; corrupt_tail=1;b.md1_md3_smem_initialized=false;
 assert(mt6797_md1_cold_clear_smem(&b)==-EIO && clears==2 && smem_reads==1);
 assert(!b.md1_md3_smem_initialized && shared[0]==0xa5 && shared[71]==0xa5);
 puts("PASS actual cold-memory and reset callbacks: guards, reservation bounds, clear/barrier/readback, AO/PD order, failure propagation");
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
assert power.index("mt6797_md1_cold_clear_smem(") < power.index("mt6797_md1_cold_cldma_reset(")
probe = function(owner, "mt6797_md1_cold_owner_probe")
assert probe.index("devm_reset_control_bulk_get_exclusive(") < probe.index("mt6797_md1_firmware_load(")
assert probe.index('cldma_resets[0].id = "cldma-ao"') < probe.index('cldma_resets[1].id = "cldma-pdn"')
print("PASS reset acquisition before writes and pulse before modem power")

transport = (root / "drivers/net/wwan/mediatek/mtk_mt6797_ccci.c").read_text()
defines = "\n".join(line for line in transport.splitlines() if line.startswith((
    "#define CLDMA_UL_STATUS", "#define CLDMA_SO_STATUS", "#define CLDMA_L2_TX_STATUS",
    "#define CLDMA_L2_RX_STATUS", "#define CLDMA_IP_BUSY", "#define CLDMA_UL_START_ADDR", "#define CLDMA_SO_START_ADDR",
    "#define MT6797_CCCI_QUEUE_COUNT")))
clean = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
struct mt6797_ccci { unsigned char *ao, *pdn; void *dev; };
static unsigned calls, inject, wake, logged_busy, logs;
static void *busy_addr;
static u32 readl(void *address)
{
 ++calls;
 if (address == busy_addr) { assert(calls==5); return wake; }
 return calls == inject ? 1 : 0;
}
static void dev_info(void *dev, const char *format, u32 ul, u32 so,
 u32 txirq, u32 rxirq, u32 busy, u32 heads)
{
 (void)dev; (void)ul; (void)so; (void)txirq; (void)rxirq; (void)heads;
 assert(strstr(format,"IP_BUSY observed-only after MD1 GO"));
 logged_busy=busy; logs++;
}
''' + defines + "\n" + function(transport, "mt6797_ccci_clean_reset_check") + r'''
int main(void)
{
 unsigned char ao[0x1e00],pdn[0x1e00];
 struct mt6797_ccci ccci={ao,pdn,0};
 busy_addr=pdn+0x04b4;
 for (wake=0; wake<=1; wake++) {
  inject=calls=logs=0;
  assert(!mt6797_ccci_clean_reset_check(&ccci));
  assert(calls==19 && logs==1 && logged_busy==wake);
  for (inject=1; inject<=19; inject++) {
   if (inject==5) continue; /* IP_BUSY is independently MD-driven after GO. */
   calls=logs=0;
   assert(mt6797_ccci_clean_reset_check(&ccci)==-EUCLEAN && calls==19);
   assert(logs==1 && logged_busy==wake);
  }
 }
 puts("PASS actual POST-GO snapshot: busy0/1 observed, all18 AP queue/IRQ/head reads still reject activity; no writes");
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
check = function(transport, "mt6797_ccci_clean_reset_check")
assert "writel(" not in check
probe = function(transport, "mt6797_ccci_probe")
assert probe.index("mt6797_ccci_validate_handoff(") < probe.index("mt6797_md1_cold_owner_acquire_cldma(")
assert probe.index("mt6797_md1_cold_owner_acquire_cldma(") < probe.index("if (!mapping.reset_completed)") < probe.index("mt6797_ccci_start(")
handoff = function(transport, "mt6797_ccci_validate_handoff")
assert "evidence.token != MT6797_MD1_POST_VECTOR_GO" in handoff and "!evidence.passive_complete" in handoff
