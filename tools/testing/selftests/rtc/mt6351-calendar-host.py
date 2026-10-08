#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Test actual MT6397-family calendar callbacks and RTC-core range handling.

Fake regmap checks locking, register addresses, latched writes and preservation
of alarm metadata. POSIX calendar conversion is the independent date oracle.
All valid hardware dates are exercised for all three chip-data variants.
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
core = (root/'drivers/rtc/class.c').read_text()
interface = (root/'drivers/rtc/interface.c').read_text()


def extract(pattern, text):
    found = re.search(pattern, text, re.M | re.S)
    if not found:
        raise RuntimeError('Missing driver fragment: '+pattern)
    return found.group(0)


code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef uint16_t u16;
typedef uint32_t u32;
typedef int64_t time64_t;
typedef uint64_t timeu64_t;
#define BIT(n) (1U << (n))
#define HZ 100
#define jiffies_to_usecs(n) ((n) * 10000)
struct device { void *priv; struct device *parent; };
struct mutex { bool held; };
struct rtc_device {
    struct device dev;
    time64_t range_min, range_max, start_secs, offset_secs;
    bool set_start_time;
};
struct rtc_time {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};
struct rtc_wkalrm { bool enabled, pending; struct rtc_time time; };
struct regmap { u16 live[64], pending[64]; unsigned writes; };
#define dev_get_drvdata(d) ((d)->priv)
#define dev_err(...) ((void)0)
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static int device_property_read_u32(struct device *dev, const char *key, u32 *value)
{
    (void)dev; (void)value;
    assert(!strcmp(key, "start-year"));
    return -ENOENT;
}
'''
code += '\n'.join(line for line in header.splitlines() if not line.startswith('#include'))+'\n'
code += r'''
static struct mt6397_rtc rtc;
static unsigned index_of(u32 reg)
{
    assert(rtc.lock.held && reg >= rtc.addr_base);
    assert((reg - rtc.addr_base) % 2 == 0 && reg - rtc.addr_base < 128);
    return (reg - rtc.addr_base) / 2;
}
static int regmap_read(struct regmap *map, u32 reg, u32 *value)
{
    unsigned index = index_of(reg);
    *value = map->live[index];
    if (index == RTC_IRQ_STA / 2)  /* read-to-clear */
        map->live[index] = map->pending[index] = 0;
    return 0;
}
static int regmap_bulk_read(struct regmap *map, u32 reg, u16 *values, unsigned count)
{
    unsigned index = index_of(reg);
    assert(count == RTC_OFFSET_COUNT && index + count <= 64);
    memcpy(values, &map->live[index], count * sizeof(*values)); return 0;
}
static int regmap_bulk_write(struct regmap *map, u32 reg, const u16 *values, unsigned count)
{
    unsigned index = index_of(reg);
    assert(count == RTC_OFFSET_COUNT && index + count <= 64);
    memcpy(&map->pending[index], values, count * sizeof(*values)); return 0;
}
static int regmap_write(struct regmap *map, u32 reg, u32 value)
{
    unsigned index = index_of(reg);
    if (reg == rtc.addr_base + rtc.data->wrtgr) {
        assert(value == 1); map->writes++;
        memcpy(map->live, map->pending, sizeof(map->live));
    } else {
        assert(reg == rtc.addr_base + RTC_AL_MASK);
        map->pending[index] = value;
    }
    return 0;
}
static int regmap_update_bits(struct regmap *map, u32 reg, u32 mask, u32 value)
{
    unsigned index = index_of(reg);
    map->pending[index] = (map->pending[index] & ~mask) | (value & mask);
    return 0;
}
static int poll_register(u32 reg, unsigned delay, unsigned timeout)
{
    assert(index_of(reg) == RTC_BBPU / 2);
    assert(delay == MTK_RTC_POLL_DELAY_US && timeout == MTK_RTC_POLL_TIMEOUT);
    return 0;
}
#define regmap_read_poll_timeout(map, reg, data, condition, delay, timeout) \
    ((data) = 0, assert(condition), poll_register(reg, delay, timeout))
static time64_t mktime64(unsigned year, unsigned mon, unsigned day,
                        unsigned hour, unsigned min, unsigned sec)
{
    struct tm tm = { .tm_year = year - 1900, .tm_mon = mon - 1,
        .tm_mday = day, .tm_hour = hour, .tm_min = min, .tm_sec = sec };
    return timegm(&tm);
}
static time64_t rtc_tm_to_time64(struct rtc_time *rtc_tm)
{
    return mktime64(rtc_tm->tm_year + 1900, rtc_tm->tm_mon + 1, rtc_tm->tm_mday,
                    rtc_tm->tm_hour, rtc_tm->tm_min, rtc_tm->tm_sec);
}
static void rtc_time64_to_tm(time64_t secs, struct rtc_time *rtc_tm)
{
    time_t time = secs; struct tm tm;
    assert(gmtime_r(&time, &tm));
    *rtc_tm = (struct rtc_time){ .tm_year = tm.tm_year, .tm_mon = tm.tm_mon,
        .tm_mday = tm.tm_mday, .tm_hour = tm.tm_hour, .tm_min = tm.tm_min,
        .tm_sec = tm.tm_sec, .tm_wday = tm.tm_wday };
}
'''
for name in ('mtk_rtc_write_trigger', 'mtk_rtc_disable_alarm', '__mtk_rtc_read_time', 'mtk_rtc_read_time',
             'mtk_rtc_set_time', 'mtk_rtc_read_alarm', 'mtk_rtc_set_alarm'):
    code += extract(r'^static int '+name+r'\(.*?^}\n', source)+'\n'
for name in ('mt6358_rtc_data', 'mt6397_rtc_data', 'mt6351_rtc_data'):
    code += extract(r'^static const struct mtk_rtc_data '+name+r' = \{.*?^};\n', source)+'\n'
code += r'''
struct of_device_id { const char *compatible; const struct mtk_rtc_data *data; };
'''
code += extract(r'^static const struct of_device_id mt6397_rtc_of_match\[\] = \{.*?^};\n', source)
code += 'static void configure_range(struct mt6397_rtc *rtc) {\n'
code += extract(r'^\trtc->rtc_dev->range_min = .*?\trtc->rtc_dev->set_start_time = true;', source)
code += '\n}\n'
code += extract(r'^static void rtc_device_get_offset\(.*?^}\n', core)
for name, ret in (('rtc_add_offset', 'void'), ('rtc_subtract_offset', 'void'), ('rtc_valid_range', 'int')):
    code += extract(r'^static '+ret+' '+name+r'\(.*?^}\n', interface)+'\n'
code += r'''
static void same_date(struct rtc_time *a, struct rtc_time *b)
{
    assert(a->tm_year == b->tm_year && a->tm_mon == b->tm_mon && a->tm_mday == b->tm_mday);
    assert(a->tm_hour == b->tm_hour && a->tm_min == b->tm_min && a->tm_sec == b->tm_sec);
}
int main(void)
{
    struct regmap map;
    struct rtc_device classdev = {0};
    struct device device = { .priv = &rtc };
    const struct mtk_rtc_data *variants[] = { &mt6351_rtc_data, &mt6397_rtc_data, &mt6358_rtc_data };
    unsigned cases = 0;
    for (unsigned i = 0; i < 5; i++) {
        const struct of_device_id *match = &mt6397_rtc_of_match[i];
        assert(match->data->year_offset == (!strcmp(match->compatible, "mediatek,mt6351-rtc") ? 68 : 0));
    }
    for (unsigned variant = 0; variant < 3; variant++) {
        memset(&rtc, 0, sizeof(rtc)); rtc.regmap = &map; rtc.addr_base = 0xe000;
        rtc.data = variants[variant]; rtc.rtc_dev = &classdev;
        configure_range(&rtc); rtc_device_get_offset(&classdev);
        assert(classdev.range_min == mktime64(variant == 0 ? 1968 : 1900, 1, 1, 0, 0, 0));
        assert(classdev.range_max == mktime64(variant == 0 ? 2095 : 2027, 12, 31, 23, 59, 59));
        assert(classdev.start_secs == mktime64(1968, 1, variant == 0 ? 1 : 2, 0, 0, 0));
        assert(classdev.offset_secs == (variant == 0 ? 0 : 4039286400LL));
        for (time64_t day = classdev.range_min; day <= classdev.range_max; day += 86400) {
            struct rtc_time date, read, written;
            rtc_time64_to_tm(day + 86399, &date);
            memset(&map, 0, sizeof(map));
            u16 values[RTC_OFFSET_COUNT] = { 59, 59, 23, date.tm_mday,
                date.tm_wday + 1, date.tm_mon + 1, date.tm_year - rtc.data->year_offset };
            memcpy(&map.live[RTC_TC_SEC / 2], values, sizeof(values));
            memcpy(map.pending, map.live, sizeof(map.live));
            assert(!mtk_rtc_read_time(&device, &read)); same_date(&date, &read);
            assert(read.tm_wday == date.tm_wday && !rtc.lock.held);
            // Exercise the actual RTC core mapping in both directions as well.
            rtc_add_offset(&classdev, &read);
            rtc_subtract_offset(&classdev, &read); same_date(&date, &read);
            written = read; assert(!mtk_rtc_set_time(&device, &written));
            assert(!memcmp(values, &map.live[RTC_TC_SEC / 2], sizeof(values)));
            assert(map.writes == 1 && !rtc.lock.held);
            // RTC alarm registers share upper bits with unrelated PMIC data.
            for (unsigned j = 0; j < RTC_OFFSET_COUNT; j++)
                map.live[RTC_AL_SEC / 2 + j] = map.pending[RTC_AL_SEC / 2 + j] = 0xa580;
            map.live[RTC_IRQ_EN / 2] = map.pending[RTC_IRQ_EN / 2] = RTC_IRQ_EN_LP;
            // A match latched while the alarm was disabled must not survive.
            map.live[RTC_IRQ_STA / 2] = RTC_IRQ_STA_AL;
            struct rtc_wkalrm alarm = { .enabled = true, .time = date }, got = {0};
            assert(!mtk_rtc_set_alarm(&device, &alarm));
            assert(map.live[RTC_IRQ_STA / 2] == 0);
            assert(!mtk_rtc_read_alarm(&device, &got)); same_date(&date, &got.time);
            assert(got.enabled && !got.pending && !rtc.lock.held && map.writes == 3);
            assert(map.live[RTC_IRQ_EN / 2] == (RTC_IRQ_EN_LP | RTC_IRQ_EN_ONESHOT_AL));
            assert(map.live[RTC_AL_MASK / 2] == RTC_AL_MASK_DOW);
            const unsigned masks[] = { RTC_AL_SEC_MASK, RTC_AL_MIN_MASK, RTC_AL_HOU_MASK,
                RTC_AL_DOM_MASK, RTC_AL_DOW_MASK, RTC_AL_MTH_MASK, RTC_AL_YEA_MASK };
            for (unsigned j = 0; j < RTC_OFFSET_COUNT; j++)
                assert((map.live[RTC_AL_SEC / 2 + j] & ~masks[j]) == (0xa580 & ~masks[j]));
            assert((map.live[RTC_AL_SEC / 2 + RTC_OFFSET_YEAR] & RTC_AL_YEA_MASK) == values[RTC_OFFSET_YEAR]);
            cases++;
        }
    }
    rtc.data = &mt6351_rtc_data; configure_range(&rtc); rtc_device_get_offset(&classdev);
    struct rtc_time boundary;
    rtc_time64_to_tm(mktime64(2095, 12, 31, 23, 59, 59), &boundary);
    assert(!rtc_valid_range(&classdev, &boundary));
    rtc_time64_to_tm(mktime64(2096, 1, 1, 0, 0, 0), &boundary);
    assert(rtc_valid_range(&classdev, &boundary) == -ERANGE);
    rtc_time64_to_tm(mktime64(1967, 12, 31, 23, 59, 59), &boundary);
    assert(rtc_valid_range(&classdev, &boundary) == -ERANGE);
    rtc_time64_to_tm(mktime64(1970, 1, 1, 0, 0, 0), &boundary);
    assert(!rtc_valid_range(&classdev, &boundary));
    printf("PASS: %u daily RTC time/alarm round trips, chip isolation and core range checks\n", cases);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp)/'rtc.c'; c.write_text(code)
    binary = Path(tmp)/'rtc-test'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-pointer-sign',
        '-Wno-sign-compare', '-O2', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
