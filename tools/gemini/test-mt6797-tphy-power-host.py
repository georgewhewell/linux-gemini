#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run MT6797 calibration and PHY power callbacks through the real PHY core."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
tphy = (root / "drivers/phy/mediatek/phy-mtk-tphy.c").read_text()
core = (root / "drivers/phy/phy-core.c").read_text()
atag = (root / "drivers/nvmem/mediatek-atag-devinfo.c").read_text()


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
#include <string.h>
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
#define FIELD_PREP(mask, val) (((uint32_t)(val) << __builtin_ctz(mask)) & (mask))
#define FIELD_GET(mask, val) (((val) & (mask)) >> __builtin_ctz(mask))
#define ENOTSUPP 524
#define __iomem
#define dev_err(...) ((void)0)
#define mutex_lock(...) ((void)0)
#define mutex_unlock(...) ((void)0)
typedef uint8_t u8;
typedef uint32_t u32;
enum { PHY_TYPE_USB2, PHY_TYPE_USB3, PHY_TYPE_PCIE, PHY_TYPE_SATA, MTK_PHY_V1 };
struct mtk_phy_pdata { bool u3phya_vusb10; int slew_ref_clock_mhz;
                     int slew_rate_coefficient, version; };
struct mtk_tphy { const struct mtk_phy_pdata *pdata; };
struct u3phy_banks { void *phya, *phyd, *chip, *spllc; };
struct mtk_phy_instance { int type; struct u3phy_banks u3_banks;
                         u32 efuse_intr, efuse_rx_imp, efuse_tx_imp; };
struct regulator { int users, error; };
struct device { struct mtk_tphy *parent; };
struct phy;
struct phy_ops { int (*power_on)(struct phy *); int (*power_off)(struct phy *); };
struct phy { struct device dev; struct mtk_phy_instance *data;
             struct regulator *pwr; struct phy_ops *ops; int power_count; };
#define phy_get_drvdata(phy) ((phy)->data)
#define dev_get_drvdata(parent) (parent)
static u32 registers[0x1000 / 4], expected[0x1000 / 4];
#define READY registers[0xb04 / 4]
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
    if (check_sequence && !r->users) assert(!(READY & BIT(29)));
}
static int phy_pm_runtime_get_sync(struct phy *phy)
{ (void)phy; if (!pm_error) pm_refs++; return pm_error; }
static void phy_pm_runtime_put_sync(struct phy *phy)
{ (void)phy; pm_refs--; }
static void phy_pm_runtime_put(struct phy *phy)
{ phy_pm_runtime_put_sync(phy); }
static void usleep_range(int lo, int hi)
{
    assert(rail.users && pm_refs && hi >= lo);
    if (lo == 50) {
        assert(events == 0);
        events++;
    } else {
        assert(lo == 800 && events == 2);
        assert(READY & BIT(29));
    }
}
static void udelay(int delay)
{
    assert(delay >= 10 && rail.users && !(READY & BIT(29)));
    assert(events == 3);
    events++;
}
static void writel(u32 val, void *p)
{
    assert((u32 *)p >= registers && (u32 *)p < registers + 0x1000 / 4);
    assert(rail.users && pm_refs && events == 2 && (READY & BIT(29)));
    *(u32 *)p = val;
}
static void mtk_phy_update_bits(void *p, u32 mask, u32 val)
{ writel((*(u32 *)p & ~mask) | (val & mask), p); }
#define mtk_phy_update_field(p, mask, val) \
    mtk_phy_update_bits(p, mask, FIELD_PREP(mask, val))
static void mtk_phy_set_bits(void *p, u32 bits)
{
    if (p == &READY) {
        assert(bits == BIT(29) && events == 1 && rail.users && pm_refs);
        *(u32 *)p |= bits;
        events++;
    } else writel(*(u32 *)p | bits, p);
}
static void mtk_phy_clear_bits(void *p, u32 bits)
{
    assert(p == &READY && bits == BIT(29));
    assert(events == 2 && rail.users && pm_refs);
    *(u32 *)p &= ~bits;
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
static u32 get_unaligned_le32(const u8 *p)
{ return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void put_unaligned_le32(u32 v, u8 *p)
{ for (int i = 0; i < 4; i++) p[i] = v >> (i * 8); }
'''
code += "\n".join(re.findall(r"^#define (?:U3P_|P3A_|P3D_|P3C_|XC3_)[^\n]+", tphy, re.M)) + "\n"
code += fragment(atag, r"^#define MT6797_ATAG_DEVINFO_TAG.*?^struct mt6797_atag_devinfo \{.*?^};\n")
for name in ("mt6797_atag_word", "mt6797_atag_usb3_cells"):
    code += fragment(atag, rf"^static (?:void|u32) {name}\(.*?^}}\n")
code += fragment(tphy, r"^static const struct mtk_phy_pdata mt6797_pdata = \{.*?^};\n")
for name in ("mt6797_u3_phy_tune", "u3_phy_instance_power_on", "u3_phy_instance_power_off",
             "mtk_phy_power_on", "mtk_phy_power_off"):
    code += fragment(tphy, rf"^static (?:void|int) {name}\(.*?^}}\n")
for name in ("phy_power_on", "phy_power_off"):
    code += fragment(core, rf"^int {name}\(.*?^}}\n")
code += r'''
static void expect(unsigned offset, u32 mask, u32 value)
{ expected[offset / 4] = (expected[offset / 4] & ~mask) | (value & mask); }
static void expect_vendor_tuning(u32 intr, u32 rx, u32 tx)
{
    /* Independent register/mask values from the MT6797 vendor sequence. */
    expect(0xc00, 0xc00, 0x800);
    expect(0x018, 0x200, 0x200);
    expect(0x310, 0xf, 0xf);
    expect(0x914, 0xff000000, 0x80000000 | (rx ? rx : 0xf) << 24);
    expect(0x910, 0x9f000000, 0x80000000 | (tx ? tx : 0x10) << 24);
    expect(0xb00, 0xfc00, (intr ? intr : 0x2d) << 10);
    expect(0xa04, 0x7f7f00, 0x566c00);
    expect(0x95c, 0x1f00000f, 0x02000002);
    expect(0xc60, 0xc00000, 0x400000);
    expect(0xc64, 0x3, 0x1);
    expect(0x30c, ~0U, 0);
}
int main(void)
{
    const struct mtk_phy_pdata generic = { 0 };
    struct mtk_tphy t = { .pdata = &mt6797_pdata };
    struct mtk_phy_instance i = { .type = PHY_TYPE_USB3,
        .u3_banks = { .phya = registers + 0xb00 / 4, .phyd = registers + 0x900 / 4,
                      .spllc = registers, .chip = registers + 0x300 / 4 } };
    struct phy_ops ops = { mtk_phy_power_on, mtk_phy_power_off };
    struct phy p = { .dev.parent = &t, .data = &i, .pwr = &rail, .ops = &ops };
    u8 property[412];
    struct mt6797_atag_devinfo cells;
    const u32 calibration[] = { 0, 5 << 16, 0xffff0000 };
    check_sequence = true;
    memset(registers, 0xa5, sizeof(registers));
    READY &= ~BIT(29);
    memcpy(expected, registers, sizeof(expected));
    rail.error = -EIO;
    assert(phy_power_on(&p) == -EIO);
    assert(!events && !p.power_count && !rail.users);
    rail.error = 0;
    pm_error = -EIO;
    assert(phy_power_on(&p) == -EIO);
    assert(!events && !p.power_count && !rail.users);
    assert(!memcmp(registers, expected, sizeof(registers)));
    pm_error = 0;
    for (unsigned cycle = 0; cycle < sizeof(calibration) / sizeof(calibration[0]); cycle++) {
        memset(property, 0xcc, sizeof(property));
        memset(&cells, 0x5a, sizeof(cells));
        put_unaligned_le32(calibration[cycle], property + 156);
        mt6797_atag_usb3_cells(&cells, property);
        for (int n = 0; n < 100; n++) assert(cells.cells[n] == 0x5a);
        i.efuse_intr = get_unaligned_le32(cells.cells + 100);
        i.efuse_rx_imp = get_unaligned_le32(cells.cells + 104);
        i.efuse_tx_imp = get_unaligned_le32(cells.cells + 108);
        assert(i.efuse_intr == (cycle == 2 ? 63 : 0));
        assert(i.efuse_rx_imp == (cycle == 2 ? 31 : cycle == 1 ? 5 : 0));
        assert(i.efuse_tx_imp == (cycle == 2 ? 31 : 0));
        expect_vendor_tuning(i.efuse_intr, i.efuse_rx_imp, i.efuse_tx_imp);
        events = 0;
        assert(!phy_power_on(&p) && !phy_power_on(&p));
        expected[0xb04 / 4] |= BIT(29);
        assert(events == 2 && !memcmp(registers, expected, sizeof(registers)));
        assert(!phy_power_off(&p));
        assert(events == 2 && rail.users == 1 && p.power_count == 1);
        assert(!phy_power_off(&p));
        expected[0xb04 / 4] &= ~BIT(29);
        assert(events == 4 && !memcmp(registers, expected, sizeof(registers)));
        assert(!rail.users && !pm_refs && !p.power_count);
    }
    /* A USB2 consumer can keep the common rail enabled after USB3 stops. */
    rail.users = 1;
    events = 0;
    assert(!phy_power_on(&p) && !phy_power_off(&p));
    assert(events == 4 && rail.users == 1);
    assert(!memcmp(registers, expected, sizeof(registers)));
    regulator_disable(&rail);
    check_sequence = false;
    for (int soc = 0; soc < 2; soc++) {
        t.pdata = soc ? &mt6797_pdata : &generic;
        for (int type = PHY_TYPE_USB2; type <= PHY_TYPE_SATA; type++) {
            if (soc && type == PHY_TYPE_USB3) continue;
            events = u2_calls = pcie_calls = 0;
            i.type = type;
            assert(!phy_power_on(&p) && !phy_power_off(&p));
            assert(!events && !memcmp(registers, expected, sizeof(registers)));
            assert(u2_calls == (type == PHY_TYPE_USB2 ? 3 : 0));
            assert(pcie_calls == (type == PHY_TYPE_PCIE ? 2 : 0));
            assert(!rail.users && !pm_refs && !p.power_count);
        }
    }
    puts("T-PHY calibration, vendor tuning, power sequencing and other PHYs: PASS");
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
