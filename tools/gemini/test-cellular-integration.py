#!/usr/bin/env python3
"""Exercise the actual watchdog admission predicate and integrated safety wiring."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
wdt = (root / "drivers/watchdog/mtk_wdt.c").read_text()
start = wdt.index("\t\tif ((mode & mask)")
end = wdt.index("ret = -EUCLEAN;", start) + len("ret = -EUCLEAN;")
predicate = wdt[start:end]
defines = "\n".join(re.findall(
    r"^#define (?:WDT_MAX_TIMEOUT|WDT_LENGTH_TIMEOUT\(n\)|WDT_MODE_EN|"
    r"WDT_MODE_AUTO_START|WDT_MODE_IRQ_EN|WDT_MODE_DUAL_EN)\s+.*$", wdt, re.M))
c = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define GENMASK(h,l) ((~0U << (l)) & (~0U >> (31-(h))))
''' + defines + r'''
static int check(u32 mode, u32 length, unsigned timeout)
{
 struct { struct { unsigned timeout; } wdt_dev; } state = {{timeout}}, *mtk_wdt = &state;
 u32 mask = WDT_MODE_EN | WDT_MODE_AUTO_START | WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN;
 int ret = 0;
''' + predicate + r'''
 return ret;
}
int main(void)
{
 const unsigned mode = WDT_MODE_EN | WDT_MODE_AUTO_START;
 const unsigned length = WDT_LENGTH_TIMEOUT(31 << 6);
 assert(!check(mode, length, 31));
 assert(!check(mode, length | 8, 31)); /* write-key bits are not timeout */
 for (unsigned timeout = 0; timeout < 31; timeout++)
  assert(check(mode, WDT_LENGTH_TIMEOUT(timeout << 6), timeout) == -EUCLEAN);
 assert(check(mode, length, 30) == -EUCLEAN);
 assert(check(mode, WDT_LENGTH_TIMEOUT(30 << 6), 31) == -EUCLEAN);
 assert(check(mode | WDT_MODE_IRQ_EN, length, 31) == -EUCLEAN);
 assert(check(mode | WDT_MODE_DUAL_EN, length, 31) == -EUCLEAN);
 assert(check(WDT_MODE_EN, length, 31) == -EUCLEAN);
 assert(check(WDT_MODE_AUTO_START, length, 31) == -EUCLEAN);
 puts("PASS actual watchdog predicate: reset mode, hardware length and31-second timeout");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cellular-guard-") as directory:
    path = Path(directory)
    (path / "guard.c").write_text(c)
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                    str(path / "guard.c"), "-o", str(path / "guard")], check=True)
    subprocess.run([str(path / "guard")], check=True, timeout=5)

owner = (root / "drivers/soc/mediatek/mt6797-md1-cold-owner.c").read_text()
start = owner.index("static int mt6797_md1_cold_pll_advance(")
advance = owner[start:owner.index("\n}\n", start) + 3]
pll = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#define __iomem
#define BIT(x) (1U << (x))
enum { MT6797_MD1_RES_CLKSW, MT6797_MD1_RES_DCM, MT6797_MD1_RES_L1_A0 };
enum { MT6797_MD1_COLD_OWNER_PLL_PSMCU, MT6797_MD1_COLD_OWNER_PLL_L1MCU,
 MT6797_MD1_COLD_OWNER_PLL_DFE_IMC, MT6797_MD1_COLD_OWNER_PLL_L1SYS,
 MT6797_MD1_COLD_OWNER_PLL_FLEX1, MT6797_MD1_COLD_OWNER_PLL_FLEX2,
 MT6797_MD1_COLD_OWNER_PLL_DCM };
struct mt6797_md1_cold_owner_backend { void *base[3]; unsigned pll_stage; };
struct op { unsigned stage, region, offset, value, replace; };
static struct op seen[16];
static unsigned count, current, fail_at;
static int write_op(void *base, unsigned off, unsigned value, unsigned replace)
{
 seen[count++] = (struct op){current, (unsigned)(uintptr_t)base, off, value, replace};
 return count == fail_at ? -EIO : 0;
}
#define mt6797_md1_cold_write_or(b,o,v) write_op(b,o,v,0)
#define mt6797_md1_cold_write_value(b,o,v) write_op(b,o,v,1)
''' + advance + r'''
int main(void)
{
 const struct op expected[] = {
  {0,0,0x24,2,0}, {1,0,0x24,0x100,0},
  {1,0,0x2c,0x30302020,1}, {1,0,0x44,0x2030,1},
  {5,0,0x24,0x3fe00,0}, {5,0,0x28,0x30203031,1},
  {5,1,0x1000,0x67550000,1}, {5,2,0x188,0x80,0}, {6,0,0x24,1,0}
 };
 struct mt6797_md1_cold_owner_backend b = {{(void *)0,(void *)1,(void *)2},0};
 for (current=0; current<7; current++) {
  assert(!mt6797_md1_cold_pll_advance(&b));
  assert(b.pll_stage == current+1);
 }
 assert(count == sizeof(expected)/sizeof(*expected));
 for (unsigned i=0; i<count; i++) {
  assert(seen[i].stage == expected[i].stage);
  assert(seen[i].region == expected[i].region);
  assert(seen[i].offset == expected[i].offset);
  assert(seen[i].value == expected[i].value);
  assert(seen[i].replace == expected[i].replace);
 }
 assert(mt6797_md1_cold_pll_advance(&b) == -EINVAL);
 for (fail_at=1; fail_at<=9; fail_at++) {
  count=0; b.pll_stage=0;
  for (current=0; current<7; current++) {
   int ret=mt6797_md1_cold_pll_advance(&b);
   if (ret) {
    assert(ret == -EIO && b.pll_stage == current && count == fail_at);
    break;
   }
  }
  assert(current<7);
 }
 puts("PASS actual PLL advance: vendor stage/write order and every write failure");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cellular-pll-") as directory:
    path = Path(directory)
    (path / "pll.c").write_text(pll)
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                    str(path / "pll.c"), "-o", str(path / "pll")], check=True)
    subprocess.run([str(path / "pll")], check=True, timeout=5)
probe = owner[owner.index("static int mt6797_md1_cold_owner_probe("):]
firmware = probe[probe.index('"mediatek,stage-mainline-firmware-load"'):]
assert firmware.index("mtk_wdt_require_boot_recovery(") < firmware.index("mt6797_md1_firmware_load(")
initial = probe[probe.index("/* Vendor CCCI: final LK remap") :]
assert initial.index("mtk_wdt_require_boot_recovery(") < initial.index("mt6797_md1_cold_apply_initial_mpu(")
verify = owner[owner.index("static int mt6797_md1_cold_firmware_verify("):
               owner.index("static int mt6797_md1_cold_apply_initial_mpu(")]
assert "mtk_wdt_require_boot_recovery(" in verify
loader = (root / "drivers/soc/mediatek/mt6797-md1-firmware-loader.c").read_text()
assert loader.index("ret = verify_cold(") < loader.index("memcpy_toio(")
adapter_start = owner.index("static int mt6797_md1_cold_pll_poll(")
adapter = owner[adapter_start:owner.index("\n}\n", adapter_start)]
assert adapter.index("return -ETIMEDOUT;") < adapter.index("ret = mt6797_md1_cold_pll_advance(")
core = (root / "drivers/soc/mediatek/mt6797-md1-cold-owner-core.c").read_text()
assert "pll_degraded" not in core
kconfig = (root / "drivers/soc/mediatek/Kconfig.cellular").read_text()
assert "depends on MTK_MT6797_MD1=n" in kconfig
assert "depends on MTK_MT6797_MD1_CLOCKED_OBSERVER=n" in kconfig
overlay = (root / "arch/arm64/boot/dts/mediatek/mt6797-gemini-pda-cellular.dtso").read_text()
assert 'status = "okay"' not in overlay
assert overlay.count('status = "disabled"') == 6
assert "mediatek,boot-watchdog = <&watchdog>" in overlay
print("PASS guard-before-write/copy ordering, fail-closed PLL, exclusive/default-disabled profile")
