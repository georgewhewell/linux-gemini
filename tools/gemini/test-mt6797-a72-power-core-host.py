#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production A72 transaction core and all its KUnit case bodies on host.

Only kernel primitive/assertion plumbing is substituted; no transaction or fixture
is duplicated. This is not a kernel/QEMU or hardware execution claim.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
files = [
    "drivers/soc/mediatek/mt6797-a72-power.h",
    "drivers/soc/mediatek/mt6797-a72-power-core.c",
    "drivers/soc/mediatek/mt6797-a72-power-test.c",
]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define static_assert _Static_assert
#define U32_MAX UINT32_MAX
#define min(a,b) ((a)<(b)?(a):(b))
#define BIT(n) (1UL << (n))
#define GENMASK(h,l) ((~0UL << (l)) & (~0UL >> (63-(h))))
#define FIELD_GET(mask,v) (((v) & (mask)) >> __builtin_ctzl(mask))
#define FIELD_PREP(mask,v) (((v) << __builtin_ctzl(mask)) & (mask))
#define mult_frac(x,n,d) (((x)/(d))*(n)+(((x)%(d))*(n))/(d))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define div_u64(a,b) ((a)/(b))
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_LICENSE(x)
static void verify_live_release(void);
struct kunit { unsigned assertions; };
#define KUNIT_EXPECT_EQ(t,a,b) do { (t)->assertions++; assert((a)==(b)); } while (0)
#define KUNIT_ASSERT_EQ KUNIT_EXPECT_EQ
#define KUNIT_EXPECT_LE(t,a,b) do { (t)->assertions++; assert((a)<=(b)); } while (0)
#define KUNIT_EXPECT_TRUE(t,a) do { (t)->assertions++; assert(a); } while (0)
#define KUNIT_EXPECT_FALSE(t,a) KUNIT_EXPECT_TRUE(t,!(a))
struct kunit_case { void (*run)(struct kunit *); const char *name; };
#define KUNIT_CASE(f) { f, #f }
struct kunit_suite { const char *name; struct kunit_case *test_cases; };
#define kunit_test_suite(s) \
int main(void) { struct kunit t={0}; unsigned n=0; \
for (struct kunit_case *c=(s).test_cases; c->run; c++) { \
c->run(&t); printf("PASS %s\n", c->name); n++; } \
verify_live_release(); assert(n==14); printf("PASS %u cases, %u assertions\n",n,t.assertions); }
'''
body = "\n".join(re.sub(r"^#include .*\n", "", (root / f).read_text(),
                         flags=re.MULTILINE) for f in files)
# Execute the production fault callback as well: its only allowed effect is
# fault latching and releasing a lock owned by this transaction's task.
live = (root / "drivers/soc/mediatek/mt6797-a72-power.c").read_text()
start = live.index("static void mt6797_a72_retain_fault(")
end = live.index("\n}\n", start) + 3
body += r"""
struct mutex { bool held; unsigned unlocks; };
struct mt6797_a72_preflight { bool faulted, guarded; struct mutex operation_lock; };
static void mutex_unlock(struct mutex *lock)
{
    assert(lock->held);
    lock->held = false;
    lock->unlocks++;
}
""" + live[start:end] + r"""
static void verify_live_release(void)
{
    struct mt6797_a72_preflight p = { .guarded=true, .operation_lock.held=true };
    mt6797_a72_retain_fault(&p);
    assert(p.faulted && !p.guarded && !p.operation_lock.held);
    assert(p.operation_lock.unlocks == 1);
    mt6797_a72_retain_fault(&p);
    assert(p.operation_lock.unlocks == 1);
    p = (struct mt6797_a72_preflight) { 0 };
    mt6797_a72_retain_fault(&p);
    assert(p.faulted && p.operation_lock.unlocks == 0);
    puts("PASS production software-lock release callback");
}
"""
with tempfile.TemporaryDirectory(prefix="gemini-a72-power-core-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + body)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-Wno-sign-compare",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
