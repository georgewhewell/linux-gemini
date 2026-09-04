#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual idle and suspend functions with fault-injected backends."""

import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
driver = root / "drivers/gpu/drm/panfrost"


def function(file, signature):
    text = (driver / file).read_text()
    return signature + text.split(signature, 1)[1].split("\n}", 1)[0] + "\n}\n"


code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
#define GPU_PM_RT 0
#define MT6797_MFG_DEBUG_SEL 0x180
#define MT6797_MFG_DEBUG_A 0x184
#define MT6797_MFG_DEBUG_IDLE BIT(2)
struct panfrost_device;
struct panfrost_compatible {
    int (*vendor_power_off)(struct panfrost_device *);
    unsigned int pm_features;
};
struct device { struct panfrost_device *data; };
struct panfrost_device {
    struct { struct device *dev; } base;
    struct panfrost_compatible *comp;
    void *mfgcfg, *bus_clock, *clock, *rstc;
};
static int fail_read, fail_select, fail_restore, poll_error, devfreq_error;
static int reads, writes, polls, mutations, generic_off, errors;
static bool jobs_idle;
static unsigned int selector;
static void *dev_get_drvdata(struct device *d) { return d->data; }
#define dev_err(...) (++errors)
static int regmap_read(void *map, unsigned int reg, unsigned int *value)
{
    (void)map; assert(reg == MT6797_MFG_DEBUG_SEL); ++reads;
    if (fail_read) return -EIO;
    *value = selector; return 0;
}
static int regmap_write(void *map, unsigned int reg, unsigned int value)
{
    (void)map; assert(reg == MT6797_MFG_DEBUG_SEL); ++writes;
    if ((writes == 1 && fail_select) || (writes == 2 && fail_restore))
        return -EIO;
    selector = value; return 0;
}
#define regmap_read_poll_timeout(map, reg, val, cond, delay, timeout) \
    ((void)(map), assert((reg) == MT6797_MFG_DEBUG_A), \
     assert((delay) == 1 && (timeout) == 100000), ++polls, \
     (val) = poll_error ? 0 : MT6797_MFG_DEBUG_IDLE, \
     assert(!!(cond) == !poll_error), poll_error)
static bool panfrost_jm_is_idle(struct panfrost_device *p)
{ (void)p; return jobs_idle; }
static int panfrost_devfreq_suspend(struct panfrost_device *p)
{ (void)p; ++mutations; return devfreq_error; }
static void panfrost_jm_suspend_irq(struct panfrost_device *p)
{ (void)p; ++mutations; }
static void panfrost_mmu_suspend_irq(struct panfrost_device *p)
{ (void)p; ++mutations; }
static void panfrost_gpu_suspend_irq(struct panfrost_device *p)
{ (void)p; ++mutations; }
static void panfrost_gpu_power_off(struct panfrost_device *p)
{ (void)p; ++generic_off; }
static void clk_disable(void *p) { (void)p; ++mutations; }
static void reset_control_assert(void *p) { (void)p; ++mutations; }
'''
code += function("panfrost_gpu.c", "int panfrost_gpu_mt6797_power_off(")
code += function("panfrost_device.c", "static int panfrost_device_runtime_suspend(")
code += r'''
static void reset_fixture(void)
{
    fail_read = fail_select = fail_restore = poll_error = devfreq_error = 0;
    reads = writes = polls = mutations = generic_off = errors = 0;
    selector = 0x57; jobs_idle = true;
}
int main(void)
{
    struct panfrost_compatible comp = { .vendor_power_off = panfrost_gpu_mt6797_power_off };
    struct device dev;
    struct panfrost_device gpu = { .base.dev = &dev, .comp = &comp };
    dev.data = &gpu;
    reset_fixture();
    assert(panfrost_device_runtime_suspend(&dev) == 0);
    assert(selector == 0x57 && writes == 2 && polls == 1);
    assert(mutations == 4 && generic_off == 0 && errors == 0);
    reset_fixture(); jobs_idle = false;
    assert(panfrost_device_runtime_suspend(&dev) == -EBUSY);
    assert(reads == 0 && mutations == 0);
    reset_fixture(); fail_read = 1;
    assert(panfrost_device_runtime_suspend(&dev) == -EIO);
    assert(writes == 0 && polls == 0 && mutations == 0 && errors == 1);
    reset_fixture(); fail_select = 1;
    assert(panfrost_device_runtime_suspend(&dev) == -EIO);
    assert(writes == 2 && polls == 0 && selector == 0x57 && mutations == 0);
    reset_fixture(); poll_error = -ETIMEDOUT;
    assert(panfrost_device_runtime_suspend(&dev) == -ETIMEDOUT);
    assert(writes == 2 && selector == 0x57 && mutations == 0);
    reset_fixture(); poll_error = -EIO;
    assert(panfrost_device_runtime_suspend(&dev) == -EIO);
    assert(writes == 2 && selector == 0x57 && mutations == 0);
    reset_fixture(); fail_restore = 1;
    assert(panfrost_device_runtime_suspend(&dev) == -EIO);
    assert(mutations == 0 && errors == 1);
    reset_fixture(); devfreq_error = -EIO;
    assert(panfrost_device_runtime_suspend(&dev) == -EIO);
    assert(selector == 0x57 && mutations == 1 && generic_off == 0);
    reset_fixture(); comp.vendor_power_off = NULL;
    assert(panfrost_device_runtime_suspend(&dev) == 0);
    assert(reads == 0 && mutations == 4 && generic_off == 1);
    puts("MT6797 GPU shutdown: 9 fault-injected paths PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-poweroff-test-") as tmp:
    source = Path(tmp) / "test.c"
    binary = Path(tmp) / "test"
    source.write_text(code)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
