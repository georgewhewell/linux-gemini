#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise T-PHY power callbacks through the real PHY core with a fake rail."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
tphy = (root / "drivers/phy/mediatek/phy-mtk-tphy.c").read_text()
core = (root / "drivers/phy/phy-core.c").read_text()


def fragment(source, pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise RuntimeError("Missing source fragment: " + pattern)
    return match.group(0)


code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
#define ENOTSUPP 524
#define dev_err(...) ((void)0)
#define mutex_lock(...) ((void)0)
#define mutex_unlock(...) ((void)0)
enum { PHY_TYPE_USB2, PHY_TYPE_USB3, PHY_TYPE_PCIE, PHY_TYPE_SATA, MTK_PHY_V1 };
struct mtk_phy_pdata { bool u3phya_vusb10; int slew_ref_clock_mhz;
                     int slew_rate_coefficient, version; };
struct mtk_tphy { const struct mtk_phy_pdata *pdata; };
struct mtk_phy_instance { int type; struct { void *phya; } u3_banks; };
struct regulator { int users, error; };
struct device { struct mtk_tphy *parent; };
struct phy;
struct phy_ops { int (*power_on)(struct phy *); int (*power_off)(struct phy *); };
struct phy { struct device dev; struct mtk_phy_instance *data;
             struct regulator *pwr; struct phy_ops *ops; int power_count; };
#define phy_get_drvdata(phy) ((phy)->data)
#define dev_get_drvdata(parent) (parent)
static uint32_t registers[2];
static int events, pm_error, pm_refs, u2_calls, pcie_calls;
static bool check_sequence;
static struct regulator rail;
static int regulator_enable(struct regulator *r)
{
    if (r->error) return r->error;
    r->users++;
    return 0;
}
static void regulator_disable(struct regulator *r)
{
    assert(r->users > 0);
    r->users--;
    if (check_sequence && !r->users)
        assert(!(registers[1] & BIT(29)));
}
static int phy_pm_runtime_get_sync(struct phy *phy)
{ (void)phy; if (!pm_error) pm_refs++; return pm_error; }
static void phy_pm_runtime_put_sync(struct phy *phy)
{ (void)phy; pm_refs--; }
static void phy_pm_runtime_put(struct phy *phy)
{ phy_pm_runtime_put_sync(phy); }
static void usleep_range(int lo, int hi)
{
    assert(lo >= 50 && hi >= lo && rail.users && pm_refs);
    events++;
}
static void udelay(int delay)
{
    assert(delay >= 10 && rail.users && !(registers[1] & BIT(29)));
    events++;
}
static void mtk_phy_set_bits(void *p, uint32_t bits)
{
    assert(p == &registers[1] && bits == BIT(29));
    assert(events == 1 && rail.users && pm_refs);
    *(uint32_t *)p |= bits;
    events++;
}
static void mtk_phy_clear_bits(void *p, uint32_t bits)
{
    assert(p == &registers[1] && bits == BIT(29));
    assert(events == 2 && rail.users && pm_refs);
    *(uint32_t *)p &= ~bits;
    events++;
}
#define STUB(name, counter) \
static void name(struct mtk_tphy *t, struct mtk_phy_instance *i) \
{ (void)t; (void)i; counter++; }
STUB(u2_phy_instance_power_on, u2_calls)
STUB(u2_phy_instance_power_off, u2_calls)
STUB(hs_slew_rate_calibrate, u2_calls)
STUB(pcie_phy_instance_power_on, pcie_calls)
STUB(pcie_phy_instance_power_off, pcie_calls)
'''
for name in ("U3P_U3_PHYA_REG1", "P3A_RG_VUSB10_ON"):
    code += fragment(tphy, rf"^#define {name}\s+[^\n]+\n")
code += fragment(tphy, r"^static const struct mtk_phy_pdata mt6797_pdata = \{.*?^};\n")
for name in ("u3_phy_instance_power_on", "u3_phy_instance_power_off",
             "mtk_phy_power_on", "mtk_phy_power_off"):
    code += fragment(tphy, rf"^static (?:void|int) {name}\(.*?^}}\n")
for name in ("phy_power_on", "phy_power_off"):
    code += fragment(core, rf"^int {name}\(.*?^}}\n")
code += r'''
int main(void)
{
    const struct mtk_phy_pdata generic = { 0 };
    struct mtk_tphy t = { .pdata = &mt6797_pdata };
    struct mtk_phy_instance i = { .type = PHY_TYPE_USB3,
                                 .u3_banks.phya = registers };
    struct phy_ops ops = { mtk_phy_power_on, mtk_phy_power_off };
    struct phy p = { .dev.parent = &t, .data = &i, .pwr = &rail, .ops = &ops };
    const uint32_t untouched = 0x9a55a55a & ~BIT(29);
    check_sequence = true;
    registers[0] = 0x12345678;
    registers[1] = untouched;
    rail.error = -EIO;
    assert(phy_power_on(&p) == -EIO);
    assert(!events && !p.power_count && !rail.users);
    rail.error = 0;
    pm_error = -EIO;
    assert(phy_power_on(&p) == -EIO);
    assert(!events && !p.power_count && !rail.users);
    pm_error = 0;
    for (int cycle = 0; cycle < 3; cycle++) {
        events = 0;
        assert(!phy_power_on(&p) && !phy_power_on(&p));
        assert(events == 2 && registers[1] == (untouched | BIT(29)));
        assert(!phy_power_off(&p));
        assert(events == 2 && rail.users == 1 && p.power_count == 1);
        assert(!phy_power_off(&p));
        assert(events == 4 && registers[1] == untouched);
        assert(!rail.users && !pm_refs && !p.power_count);
    }
    /* A USB2 consumer can keep the common rail enabled after USB3 stops. */
    rail.users = 1;
    events = 0;
    assert(!phy_power_on(&p) && !phy_power_off(&p));
    assert(events == 4 && rail.users == 1 && registers[1] == untouched);
    regulator_disable(&rail);
    check_sequence = false;
    for (int soc = 0; soc < 2; soc++) {
        t.pdata = soc ? &mt6797_pdata : &generic;
        for (int type = PHY_TYPE_USB2; type <= PHY_TYPE_SATA; type++) {
            if (soc && type == PHY_TYPE_USB3) continue;
            events = u2_calls = pcie_calls = 0;
            i.type = type;
            assert(!phy_power_on(&p) && !phy_power_off(&p));
            assert(!events && registers[1] == untouched);
            assert(u2_calls == (type == PHY_TYPE_USB2 ? 3 : 0));
            assert(pcie_calls == (type == PHY_TYPE_PCIE ? 2 : 0));
            assert(!rail.users && !pm_refs && !p.power_count);
        }
    }
    assert(registers[0] == 0x12345678);
    puts("T-PHY power sequencing, failures, reference counts and other PHYs: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="mt6797-tphy-") as tmp:
    src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
    src.write_text(code)
    subprocess.run(shlex.split(os.environ.get("HOSTCC", "cc")) + [
        "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
