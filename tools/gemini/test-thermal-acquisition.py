#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run imported pure KUnit cases with host assertions, not a kernel emulator."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def read(path):
    return re.sub(r"^#include .*\n", "", (root / path).read_text(), flags=re.M)


shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U << (l)) & (~0U >> (31-(h))))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define MODULE_LICENSE(x)
struct device; struct device_node; struct regmap;
struct reset_controller_dev { int unused; };
struct kunit { int unused; };
struct kunit_case { void (*run_case)(struct kunit *); const char *name; };
struct kunit_suite { const char *name; struct kunit_case *test_cases; };
#define KUNIT_EXPECT_EQ(t,a,b) do { (void)(t); assert((a)==(b)); } while (0)
#define KUNIT_ASSERT_EQ KUNIT_EXPECT_EQ
#define KUNIT_EXPECT_EQ_MSG(t,a,b,...) KUNIT_EXPECT_EQ(t,a,b)
#define KUNIT_EXPECT_TRUE(t,a) do { (void)(t); assert(a); } while (0)
#define KUNIT_ASSERT_TRUE KUNIT_EXPECT_TRUE
#define KUNIT_EXPECT_FALSE(t,a) KUNIT_EXPECT_TRUE(t,!(a))
#define KUNIT_ASSERT_LT(t,a,b) KUNIT_EXPECT_TRUE(t,(a)<(b))
#define KUNIT_CASE(f) { f, #f }
#define kunit_test_suite(suite) int main(void) { \
 struct kunit t={0}; unsigned count=0; \
 for (struct kunit_case *c=(suite).test_cases; c->run_case; c++) { \
  c->run_case(&t); printf("PASS %s\n",c->name); count++; \
 } assert(count==6); return 0; }
'''
suites = {
    "reset": (
        "include/dt-bindings/reset/mt6797-resets.h",
        "drivers/clk/mediatek/reset.h",
        "drivers/clk/mediatek/clk-mt6797-reset.h",
        "drivers/clk/mediatek/clk-mt6797-reset-test.c",
    ),
    "thermal": (
        "drivers/thermal/mediatek/auxadc_thermal_internal.h",
        "drivers/thermal/mediatek/mt6797_auxadc_transaction_test.c",
    ),
}
with tempfile.TemporaryDirectory(prefix="gemini-acquisition-") as tmp:
    tmp = Path(tmp)
    for name, files in suites.items():
        source = tmp / f"{name}.c"
        source.write_text(shim + "\n".join(read(path) for path in files))
        executable = tmp / name
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                        "-Wextra", "-Werror", "-Wno-sign-compare",
                        str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=10)

# These are source-order checks, not kernfs/workqueue concurrency emulation.
driver = (root / "drivers/thermal/mediatek/auxadc_thermal.c").read_text()
probe = driver[driver.index("static int mtk_thermal_probe("):
               driver.index("static void mtk_thermal_remove(")]
assert probe.index("devres_open_group(") < probe.index("devm_thermal_of_zone_register(")
assert probe.index("mt6797_hwmon_register(") < probe.index("devres_close_group(")
failure = probe[probe.index("fail_zone:"):]
assert failure.index("devres_release_group(") < failure.index("mtk_thermal_transaction_close(")
remove = driver[driver.index("static void mtk_thermal_remove("):
                driver.index("static struct platform_driver mtk_thermal_driver")]
assert remove.index("devres_release_group(") < remove.index("mtk_thermal_transaction_close(")
print("PASS source ordering: all readers grouped and drained before transaction close")
