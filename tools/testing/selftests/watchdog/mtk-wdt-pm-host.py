#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run the real MediaTek watchdog PM/start/stop functions with fake MMIO.

This host test checks callback ordering and state transitions, not hardware
timing. Run from a kernel checkout with a native C compiler in CC (default cc).
An arm64 object build is also required to check the real kernel interfaces.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'drivers/watchdog/mtk_wdt.c').read_text()
pm = (root / 'include/linux/pm.h').read_text()
suspend = (root / 'include/linux/suspend.h').read_text()


def extract(pattern, text=source):
    match = re.search(pattern, text, re.M | re.S)
    if not match:
        raise RuntimeError(f'Source fragment missing: {pattern}')
    return match.group(0)


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define __iomem
typedef uint32_t u32;
typedef int spinlock_t;
struct notifier_block { int unused; };
struct reset_controller_dev { int unused; };
struct watchdog_device {
    unsigned int timeout, pretimeout, status;
    void *priv;
};
struct device { void *priv; };
#define watchdog_get_drvdata(w) ((w)->priv)
#define dev_get_drvdata(d) ((d)->priv)
#define watchdog_active(w) ((w)->status & 1)
#define watchdog_hw_running(w) ((w)->status & 2)
#define container_of(p, type, field) ((type *)((char *)(p) - offsetof(type, field)))
#define spin_lock_irqsave(lock, flags) do { assert(!*(lock)); *(lock) = 1; (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { assert(*(lock)); assert(!(flags)); *(lock) = 0; } while (0)
#define NOTIFY_OK 1
#define notifier_from_errno(err) ((err) ? 0x8000 | (NOTIFY_OK - (err)) : NOTIFY_OK)
static u32 readl(void *p) { return *(u32 *)p; }
static void *mode_addr;
static unsigned int stop_writes;
static void iowrite32(u32 value, void *p) {
    /* WDT_MODE_EN is bit 0. Count even a transient disable followed by start. */
    if (p == mode_addr && !(value & 1))
        stop_writes++;
    *(u32 *)p = value;
}
#define ioread32 readl
#define pm_sleep_ptr(fn) (fn)
struct dev_pm_ops {
    int (*suspend_noirq)(struct device *);
    int (*resume_noirq)(struct device *);
    int (*freeze_noirq)(struct device *);
    int (*thaw_noirq)(struct device *);
    int (*poweroff_noirq)(struct device *);
    int (*restore_noirq)(struct device *);
};
'''

functions = [
    'mtk_wdt_ping', 'mtk_wdt_set_timeout', '__mtk_wdt_stop', '__mtk_wdt_start',
    'mtk_wdt_stop', 'mtk_wdt_start', 'mtk_wdt_pm_notify', 'mtk_wdt_is_running',
    'mtk_wdt_suspend_noirq', 'mtk_wdt_resume_noirq',
]
code = prefix + '\n'.join(re.findall(r'^#define WDT_.*$', source, re.M)) + '\n'
code += '\n'.join(re.findall(r'^#define PM_(?:POST|HIBERNATION|RESTORE|SUSPEND)_.*$', suspend, re.M)) + '\n'
code += extract(r'struct mtk_wdt_dev \{.*?^};') + '\n'
code += extract(r'^static bool suspend_keep_running;') + '\n'
# The mode is fixed at boot/module load, never changed during a PM transition.
assert 'module_param(suspend_keep_running, bool, 0400);' in source
code += extract(r'^#define NOIRQ_SYSTEM_SLEEP_PM_OPS.*?\n\n', pm)
for name in functions:
    code += extract(r'^static (?:int|bool) ' + name + r'\(.*?^}\n') + '\n'
code += extract(r'^static const struct dev_pm_ops mtk_wdt_pm_ops = \{.*?^};') + '\n'
code += r'''
static struct mtk_wdt_dev w;
static struct device d = { .priv = &w };
static u32 regs[64];

static bool running(void) { return regs[WDT_MODE / 4] & WDT_MODE_EN; }
static void setup(unsigned int status, bool enabled)
{
    memset(&w, 0, sizeof(w));
    memset(regs, 0, sizeof(regs));
    w.wdt_base = regs;
    w.wdt_dev.priv = &w;
    w.wdt_dev.timeout = 31;
    w.wdt_dev.status = status | 4; /* preserve a nowayout-like policy bit */
    mode_addr = &regs[WDT_MODE / 4];
    stop_writes = 0;
    regs[WDT_MODE / 4] = enabled ? WDT_MODE_EN : 0;
}
static void notify(unsigned long action)
{
    assert(mtk_wdt_pm_notify(&w.pm_nb, action, NULL) == NOTIFY_OK);
    assert(!w.lock);
}
static void image_io(void)
{
    /* Even core-worker or userspace pings must not enable the paused WDT. */
    assert(w.hibernation_paused);
    for (int i = 0; i < 100; i++) {
        assert(!running());
        assert(!mtk_wdt_ping(&w.wdt_dev));
    }
    assert(!running());
}
static void test_suspend(unsigned int status, bool enabled)
{
    setup(status, enabled);
    notify(PM_SUSPEND_PREPARE);
    assert(running() == enabled);
    assert(!mtk_wdt_pm_ops.suspend_noirq(&d));
    assert(running() == (enabled && suspend_keep_running));
    if (suspend_keep_running) {
        assert(stop_writes == 0);
        if (enabled)
            assert(regs[WDT_RST / 4] == WDT_RST_RELOAD);
    }
    assert(!mtk_wdt_pm_ops.resume_noirq(&d));
    notify(PM_POST_SUSPEND);
    assert(running() == enabled);
    assert(w.wdt_dev.status == (status | 4));
}
static void test_hibernation(unsigned int status, bool enabled)
{
    setup(status, enabled);
    notify(PM_HIBERNATION_PREPARE);
    image_io();
    assert(!mtk_wdt_pm_ops.freeze_noirq(&d));
    assert(!mtk_wdt_pm_ops.thaw_noirq(&d));
    image_io(); /* swsusp_write() follows thaw */
    notify(PM_POST_HIBERNATION);
    assert(!w.hibernation_paused);
    assert(running() == enabled);
    assert(w.wdt_dev.status == (status | 4));
}
static void test_restore_failure(unsigned int status, bool enabled)
{
    setup(status, enabled);
    notify(PM_RESTORE_PREPARE);
    image_io(); /* swsusp_read() precedes device freeze */
    assert(!mtk_wdt_pm_ops.freeze_noirq(&d));
    assert(!mtk_wdt_pm_ops.thaw_noirq(&d));
    image_io();
    notify(PM_POST_RESTORE);
    assert(running() == enabled);
}
static void test_abort_before_device_freeze(void)
{
    setup(1, true);
    notify(PM_HIBERNATION_PREPARE);
    notify(PM_POST_HIBERNATION);
    assert(running());
    notify(PM_RESTORE_PREPARE);
    notify(PM_POST_RESTORE);
    assert(running());
}
static void test_start_stop_while_paused(void)
{
    setup(0, false);
    notify(PM_HIBERNATION_PREPARE);
    assert(!mtk_wdt_start(&w.wdt_dev));
    image_io();
    notify(PM_POST_HIBERNATION);
    assert(running()); /* userspace opened it after PREPARE */

    setup(1, true);
    notify(PM_HIBERNATION_PREPARE);
    assert(!mtk_wdt_stop(&w.wdt_dev));
    image_io();
    notify(PM_POST_HIBERNATION);
    assert(!running()); /* userspace explicitly stopped it */
}
static void test_restored_snapshot(void)
{
    struct mtk_wdt_dev saved;
    setup(1, true);
    notify(PM_HIBERNATION_PREPARE);
    assert(!mtk_wdt_pm_ops.freeze_noirq(&d));
    saved = w;
    assert(!mtk_wdt_pm_ops.thaw_noirq(&d));
    image_io();
    setup(1, true); /* boot kernel */
    notify(PM_RESTORE_PREPARE);
    image_io();
    w = saved; /* restore kernel memory including notifier state */
    assert(!mtk_wdt_pm_ops.restore_noirq(&d));
    image_io();
    notify(PM_POST_HIBERNATION); /* restored kernel completes hibernate() */
    assert(running());
}
static void test_duplicate_notifications(void)
{
    setup(1, true);
    notify(PM_HIBERNATION_PREPARE);
    notify(PM_HIBERNATION_PREPARE);
    image_io();
    notify(PM_POST_HIBERNATION);
    assert(running());
    assert(!mtk_wdt_stop(&w.wdt_dev));
    notify(PM_POST_HIBERNATION);
    assert(!running());
}
int main(void)
{
    for (unsigned int keep_running = 0; keep_running < 2; keep_running++) {
        suspend_keep_running = keep_running;
        for (unsigned int status = 0; status < 4; status++) {
            test_suspend(status, status != 0);
            test_hibernation(status, status != 0);
            test_restore_failure(status, status != 0);
        }
        test_abort_before_device_freeze();
        test_start_stop_while_paused();
        test_restored_snapshot();
        test_duplicate_notifications();
    }
    puts("mtk-wdt-pm-host: PASS (32 PM sequences, real driver functions)");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='mtk-wdt-pm-') as tmp:
    c = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    c.write_text(code)
    subprocess.run(shlex.split(os.environ.get('HOSTCC', 'cc')) +
                   ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
