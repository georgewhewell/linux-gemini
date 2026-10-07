#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the real alarm-enable and write-trigger functions with fake regmap.

The register model separates pending writes from latched hardware state, so
clearing a software flag or omitting WRTGR cannot pass cancellation checks.
An ARM64 driver-object build checks the actual kernel interfaces separately.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root/'drivers/rtc/rtc-mt6397.c').read_text()
header = (root/'include/linux/mfd/mt6397/rtc.h').read_text()


def extract(pattern, text):
    found = re.search(pattern, text, re.M | re.S)
    if not found:
        raise RuntimeError('Missing driver fragment: '+pattern)
    return found.group(0)


assert re.search(r'\.alarm_irq_enable\s*=\s*mtk_rtc_alarm_irq_enable\s*,', source)
code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define HZ 100
#define jiffies_to_usecs(n) ((n) * 10000)
struct device { void *priv; };
struct rtc_device { struct { struct device *parent; } dev; };
struct mtk_rtc_data { u32 wrtgr; };
struct regmap { u32 pending, live; int update_error, write_error, poll_error, writes; };
struct mt6397_rtc {
    struct rtc_device *rtc_dev;
    int lock;
    struct regmap *regmap;
    u32 addr_base;
    const struct mtk_rtc_data *data;
};
static struct mt6397_rtc rtc;
#define dev_get_drvdata(d) ((d)->priv)
#define dev_err(...) ((void)0)
static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
'''
code += '\n'.join(re.findall(r'^#define (?:RTC_(?:BBPU|WRTGR|IRQ_EN)\w*|MTK_RTC_POLL_\w*)\s+.*$', header, re.M))+'\n'
code += r'''
static int regmap_update_bits(struct regmap *map, u32 reg, u32 mask, u32 value)
{
    assert(rtc.lock && reg == rtc.addr_base + RTC_IRQ_EN);
    if (map->update_error) return map->update_error;
    map->pending = (map->pending & ~mask) | (value & mask);
    return 0;
}
static int regmap_write(struct regmap *map, u32 reg, u32 value)
{
    assert(rtc.lock && reg == rtc.addr_base + rtc.data->wrtgr && value == 1);
    map->writes++;
    if (map->write_error) return map->write_error;
    map->live = map->pending;
    return 0;
}
static int poll_register(struct regmap *map, u32 reg, unsigned delay, unsigned timeout)
{
    assert(rtc.lock && reg == rtc.addr_base + RTC_BBPU);
    assert(delay == MTK_RTC_POLL_DELAY_US && timeout == MTK_RTC_POLL_TIMEOUT);
    return map->poll_error;
}
#define regmap_read_poll_timeout(map, reg, data, condition, delay, timeout) \
    ((data) = 0, assert(condition), poll_register(map, reg, delay, timeout))
'''
for name in ('mtk_rtc_write_trigger', 'mtk_rtc_alarm_irq_enable'):
    code += extract(r'^static int '+name+r'\(.*?^}\n', source)+'\n'
code += r'''
int main(void)
{
    struct regmap map;
    struct device device = { .priv = &rtc };
    const struct mtk_rtc_data variants[] = {
        { .wrtgr = RTC_WRTGR_MT6397 }, { .wrtgr = RTC_WRTGR_MT6358 }
    };
    unsigned sequences = 0;
    for (unsigned variant = 0; variant < 2; variant++) {
        for (unsigned enabled = 0; enabled < 2; enabled++) {
            for (unsigned fault = 0; fault < 4; fault++) {
                memset(&map, 0, sizeof(map));
                memset(&rtc, 0, sizeof(rtc));
                rtc.regmap = &map; rtc.addr_base = 0xe000; rtc.data = &variants[variant];
                map.live = map.pending = RTC_IRQ_EN_LP | (enabled ? 0 : RTC_IRQ_EN_ONESHOT_AL);
                u32 old = map.live;
                if (fault == 1) map.update_error = -EIO;
                if (fault == 2) map.write_error = -EIO;
                if (fault == 3) map.poll_error = -ETIMEDOUT;
                int result = mtk_rtc_alarm_irq_enable(&device, enabled);
                assert(!rtc.lock);
                assert(result == (fault == 0 ? 0 : fault == 3 ? -ETIMEDOUT : -EIO));
                assert(map.writes == (fault == 1 ? 0 : 1));
                if (fault == 0) {
                    assert(map.live == (RTC_IRQ_EN_LP | (enabled ? RTC_IRQ_EN_ONESHOT_AL : 0)));
                    assert(!mtk_rtc_alarm_irq_enable(&device, enabled));
                    assert(!rtc.lock && map.writes == 2);
                } else if (fault != 3) {
                    assert(map.live == old);
                }
                sequences++;
            }
        }
    }
    printf("PASS: %u RTC alarm enable/disable and register-error sequences\n", sequences);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp)/'rtc.c'; c.write_text(code)
    binary = Path(tmp)/'rtc-test'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc'))+[
        '-std=c11','-Wall','-Wextra','-Werror','-Wpedantic','-O2',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
