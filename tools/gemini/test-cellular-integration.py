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
print("PASS watchdog ordering, PLL timeouts and exclusive modem driver configuration")

# Execute the actual cold-owner core and its KUnit cases with host adapters.
# Provider stubs model acquisition failure, not electrical hardware behavior.
def without_includes(source):
    return re.sub(r"^#include[^\n]*\n", "", source, flags=re.M)

cold_header = (root / "include/linux/soc/mediatek/mt6797-md1-cold-owner.h").read_text()
cold_tests = (root / "drivers/soc/mediatek/mt6797-md1-cold-owner-kunit-test.c").read_text()
acquire = re.search(r"^static int mt6797_md1_cold_sim_pins_get\(.*?^}\n", owner, re.M | re.S).group()
select = re.search(r"^static int mt6797_md1_cold_select_sim_pins\(.*?^}\n", owner, re.M | re.S).group()
cold = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define __iomem
#define EPROBE_DEFER 517
#define BIT(x) (1U << (x))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define pr_warn(...) ((void)0)
struct mutex { bool held; };
static void mutex_init(struct mutex *m) { m->held = false; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
struct kunit { unsigned checks; };
#define KUNIT_ASSERT_EQ(t,a,b) do { (t)->checks++; assert((a) == (b)); } while (0)
#define KUNIT_EXPECT_EQ KUNIT_ASSERT_EQ
#define KUNIT_EXPECT_TRUE(t,a) do { (t)->checks++; assert(a); } while (0)
#define KUNIT_EXPECT_FALSE(t,a) KUNIT_EXPECT_TRUE(t, !(a))
struct kunit_case { void (*run)(struct kunit *); };
struct kunit_suite { const char *name; struct kunit_case *test_cases; };
#define KUNIT_CASE(fn) { .run = fn }
#define kunit_test_suite(s)
#define MODULE_LICENSE(s)
''' + without_includes(cold_header) + without_includes(core) + without_includes(cold_tests) + r'''
struct device { unsigned unused; };
struct pinctrl { unsigned unused; };
struct pinctrl_state { unsigned unused; };
struct mt6797_md1_cold_owner_backend {
 struct device *dev;
 struct pinctrl *sim_pinctrl;
 struct pinctrl_state *sim_pins;
};
static struct pinctrl controller;
static struct pinctrl_state state;
static int get_error, lookup_error, select_error;
static unsigned get_calls, lookup_calls, select_calls;
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
static int dev_err_probe(struct device *d, int err, const char *message)
{ (void)d; (void)message; return err; }
static struct pinctrl *devm_pinctrl_get(struct device *d)
{ assert(d); get_calls++; return get_error ? ERR_PTR(get_error) : &controller; }
static struct pinctrl_state *pinctrl_lookup_state(struct pinctrl *p, const char *name)
{ assert(p == &controller && !strcmp(name, "sim")); lookup_calls++;
  return lookup_error ? ERR_PTR(lookup_error) : &state; }
static int pinctrl_select_state(struct pinctrl *p, struct pinctrl_state *s)
{ assert(p == &controller && s == &state); select_calls++; return select_error; }
''' + acquire + select + r'''
int main(void)
{
 struct device device = {0};
 struct mt6797_md1_cold_owner_backend backend = { .dev = &device };
 const int errors[] = {-EPROBE_DEFER, -EINVAL, -ENOENT};
 for (unsigned i = 0; i < ARRAY_SIZE(errors); i++) {
  get_calls = lookup_calls = select_calls = 0;
  get_error = errors[i]; lookup_error = 0;
  assert(mt6797_md1_cold_sim_pins_get(&backend) == errors[i]);
  assert(get_calls == 1 && !lookup_calls && !select_calls);
  get_calls = 0; get_error = 0; lookup_error = errors[i];
  assert(mt6797_md1_cold_sim_pins_get(&backend) == errors[i]);
  assert(get_calls == 1 && lookup_calls == 1 && !select_calls);
 }
 get_error = lookup_error = 0;
 assert(!mt6797_md1_cold_sim_pins_get(&backend) && !select_calls);
 assert(!mt6797_md1_cold_select_sim_pins(&backend) && select_calls == 1);
 select_error = -EIO;
 assert(mt6797_md1_cold_select_sim_pins(&backend) == -EIO && select_calls == 2);
 struct kunit test = {0};
 unsigned cases = 0;
 for (struct kunit_case *c = mt6797_md1_cold_owner_test_suite.test_cases; c->run; c++) {
  c->run(&test); cases++;
 }
 printf("PASS actual SIM acquisition without selection, selection error, cold core and %u KUnit cases/%u assertions\n", cases, test.checks);
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cellular-sim-owner-") as directory:
    path = Path(directory)
    (path / "cold.c").write_text(cold)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-sign-compare", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    "-fno-omit-frame-pointer", str(path / "cold.c"), "-o", str(path / "cold")], check=True)
    subprocess.run([str(path / "cold")], check=True, timeout=5)
