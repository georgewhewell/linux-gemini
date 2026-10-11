#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise native MMC pad tables through the real field and Paris helpers."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
pin = root / "drivers/pinctrl/mediatek"
driver = (pin / "pinctrl-mt6797.c").read_text()
pins = (pin / "pinctrl-mtk-mt6797.h").read_text()
header = (pin / "pinctrl-mtk-common-v2.h").read_text()
core = (pin / "pinctrl-mtk-common-v2.c").read_text()
paris = (pin / "pinctrl-paris.c").read_text()
paris_header = (pin / "pinctrl-paris.h").read_text()
dts = (root / "arch/arm64/boot/dts/mediatek/mt6797-gemini-pda.dts").read_text()


def fragment(source, pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise RuntimeError("Missing production fragment: " + pattern)
    return match.group(0)


code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ENOTSUPP 524
#define NO_EINT_SUPPORT UINT16_MAX
#define MTK_ENABLE 1
#define MTK_DISABLE 0
#define MTK_OUTPUT 1
#define MTK_PULLUP 1
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
static unsigned get_count_order(unsigned value)
{ return value > 1 ? 32U - __builtin_clz(value - 1) : 0; }
static void writel_relaxed(u32 value, void *p) { *(u32 *)p = value; }
static u32 readl_relaxed(void *p) { return *(u32 *)p; }
'''
code += fragment(header, r"^#define PIN_FIELD_CALC\(.*?(?=^#define PIN_RSEL)")
code += fragment(header, r"^#define MTK_INPUT.*?(?=^#define EINT_NA)")
code += "\n".join(re.findall(r"^#define MTK_(?:PUPD_SET|PULL_SET)[^\n]+", (root / "include/dt-bindings/pinctrl/mt65xx.h").read_text(), re.M)) + "\n"
code += fragment(header, r"^enum \{\n\s+PINCTRL_PIN_REG_MODE,.*?^};\n")
code += fragment(header, r"^enum \{\n\s+DRV_FIXED,.*?^};\n")
code += fragment((root / "include/linux/pinctrl/pinconf-generic.h").read_text(), r"^enum pin_config_param \{.*?^};\n")
code += "\n".join(re.findall(r"^#define MTK_PIN_CONFIG_[^\n]+", paris, re.M)) + "\n"
for name in ("mtk_pin_field", "mtk_pin_field_calc", "mtk_pin_reg_calc", "mtk_func_desc", "mtk_eint_desc", "mtk_pin_desc"):
    code += fragment(header, rf"^struct {name} \{{.*?^}};\n")
code += fragment(paris_header, r"^#define MTK_RANGE.*?(?=^#define MTK_EINT_PIN)")
code += fragment(pins, r"^static const struct mtk_pin_desc mtk_pins_mt6797\[\] = \{.*?^};\n")
code += fragment(driver, r"^static const struct mtk_pin_field_calc mt6797_pin_mode_range\[\].*?(?=^static const char \* const)")
code += fragment(driver, r"^static const unsigned int mt6797_pull_type\[.*?^};\n")
code += r'''
struct mtk_pin_soc {
    const struct mtk_pin_reg_calc *reg_cal;
    const struct mtk_pin_desc *pins;
    unsigned npins;
    const unsigned *pull_type;
    int (*drive_set)(struct mtk_pinctrl *, const struct mtk_pin_desc *, u32);
    int (*bias_set_combo)(struct mtk_pinctrl *, const struct mtk_pin_desc *, u32, u32);
    int (*adv_drive_set)(struct mtk_pinctrl *, const struct mtk_pin_desc *, u32);
    int (*adv_pull_set)(struct mtk_pinctrl *, const struct mtk_pin_desc *, bool, u32);
};
struct mtk_pinctrl {
    const struct mtk_pin_soc *soc;
    unsigned nbase;
    unsigned char *base[5];
    int lock;
};
struct pinctrl_dev { struct mtk_pinctrl *hw; };
#define pinctrl_dev_get_drvdata(p) ((p)->hw)
'''
# Declare this before the callback types without replacing production helpers.
code = code.replace("struct mtk_pin_soc {", "struct mtk_pinctrl;\nstruct mtk_pin_soc {")
code += fragment(core, r"^struct mtk_drive_desc \{.*?^};\n")
code += fragment(core, r"^static const struct mtk_drive_desc mtk_drive\[\] = \{.*?^};\n")
for name, ret in (("mtk_w32", "static void"), ("mtk_r32", "static u32"), ("mtk_rmw", "void"),
                  ("mtk_hw_pin_field_lookup", "static int"), ("mtk_hw_pin_field_get", "int"),
                  ("mtk_hw_bits_part", "static void"), ("mtk_hw_write_cross_field", "static void"),
                  ("mtk_hw_read_cross_field", "static void"), ("mtk_hw_set_value", "int"),
                  ("mtk_hw_get_value", "int"), ("mtk_pinconf_drive_set_rev1", "int"),
                  ("mtk_pinconf_drive_get_rev1", "int"), ("mtk_pinconf_bias_set_pupd_r1_r0", "static int")):
    code += fragment(core, rf"^{ret} {name}\(.*?^}}\n")
code += r'''
/* These unselected pull formats must never receive an MMC0/1 request. */
static int mtk_pinconf_bias_set_pu_pd_rsel(struct mtk_pinctrl *h,
    const struct mtk_pin_desc *d, u32 p, u32 a)
{ (void)h; (void)d; (void)p; (void)a; assert(false); return -ENOTSUPP; }
static int mtk_pinconf_bias_set_pu_pd(struct mtk_pinctrl *h,
    const struct mtk_pin_desc *d, u32 p, u32 a, bool only)
{ (void)h; (void)d; (void)p; (void)a; (void)only; assert(false); return -ENOTSUPP; }
static int mtk_pinconf_bias_set_pullsel_pullen(struct mtk_pinctrl *h,
    const struct mtk_pin_desc *d, u32 p, u32 a)
{ (void)h; (void)d; (void)p; (void)a; assert(false); return -ENOTSUPP; }
'''
code += fragment(core, r"^int mtk_pinconf_bias_set_combo\(.*?^}\n")
code += fragment(paris, r"^static int mtk_drv_uA_to_adv\(.*?^}\n")
code += fragment(paris, r"^static int mtk_pinconf_set\(.*?^}\n")
code += r'''
static const struct mtk_pin_soc soc = {
    .reg_cal = mt6797_reg_cals, .pins = mtk_pins_mt6797,
    .npins = ARRAY_SIZE(mtk_pins_mt6797), .pull_type = mt6797_pull_type,
    .drive_set = mtk_pinconf_drive_set_rev1,
    .bias_set_combo = mtk_pinconf_bias_set_combo,
};
static u32 banks[5][0x1000 / 4], expected[5][0x1000 / 4];
static void expect(unsigned bank, unsigned offset, u32 mask, u32 value)
{ expected[bank][offset / 4] = (expected[bank][offset / 4] & ~mask) | (value & mask); }
static void check_field(struct mtk_pinctrl *h, unsigned pin, unsigned reg,
                        unsigned offset, unsigned bit, unsigned width)
{
    struct mtk_pin_field f;
    assert(!mtk_hw_pin_field_get(h, &soc.pins[pin], reg, &f));
    assert(f.index == 2 && f.offset == offset && f.bitpos == bit);
    assert(f.mask == (1U << width) - 1 && !f.next);
}
static void apply_board(struct pinctrl_dev *p)
{
'''
# Execute the actual current board properties, not a duplicate list of settings.
board = fragment(dts, r"\tmmc0_pins_default:.*?(?=\n\thall_pins:)")
property_params = {"drive-strength": "PIN_CONFIG_DRIVE_STRENGTH", "input-enable": "PIN_CONFIG_INPUT_ENABLE",
                   "input-schmitt-enable": "PIN_CONFIG_INPUT_SCHMITT_ENABLE", "bias-pull-up": "PIN_CONFIG_BIAS_PULL_UP",
                   "bias-pull-down": "PIN_CONFIG_BIAS_PULL_DOWN", "mediatek,tdsel": "MTK_PIN_CONFIG_TDSEL",
                   "mediatek,rdsel": "MTK_PIN_CONFIG_RDSEL"}
board_pins = []
for group in re.findall(r"\t\tpins-[^\n]+ \{(.*?)\n\t\t};", board, re.S):
    numbers = [int(n) for n in re.findall(r"MT6797_GPIO(\d+)__FUNC_MSDC0_", group)]
    board_pins += numbers
    for prop, param in property_params.items():
        match = re.search(rf"\b{re.escape(prop)}(?:\s*=\s*<([^>]+)>)?;", group)
        if match:
            arg = match.group(1) or "1"
            for number in numbers:
                code += f"    assert(!mtk_pinconf_set(p, {number}, {param}, {arg}));\n"
assert sorted(board_pins) == list(range(114, 126))
code += r'''
}
int main(void)
{
    struct mtk_pinctrl h = { .soc = &soc, .nbase = 5 };
    struct pinctrl_dev p = { .hw = &h };
    const unsigned shared_bit[12] = { 0,0,0,0,1,1,1,1,2,3,2,2 };
    memset(banks, 0xa5, sizeof(banks));
    memcpy(expected, banks, sizeof(expected));
    for (unsigned i = 0; i < 5; i++) h.base[i] = (unsigned char *)banks[i];
    for (unsigned i = 0; i < 12; i++) {
        unsigned n = i + 114, g = shared_bit[i];
        check_field(&h, n, PINCTRL_PIN_REG_IES, 0x020, i, 1);
        check_field(&h, n, PINCTRL_PIN_REG_SMT, 0x060, g, 1);
        check_field(&h, n, PINCTRL_PIN_REG_DRV, 0x1a0, g * 3, 3);
        check_field(&h, n, PINCTRL_PIN_REG_TDSEL, 0x0d0, g * 4, 4);
        check_field(&h, n, PINCTRL_PIN_REG_RDSEL, 0x080, (i < 8 ? i / 4 : 2) * 6, 6);
        check_field(&h, n, PINCTRL_PIN_REG_PUPD, 0x100, i + 6, 1);
        check_field(&h, n, PINCTRL_PIN_REG_R0, 0x110, i + 6, 1);
        check_field(&h, n, PINCTRL_PIN_REG_R1, 0x120, i + 6, 1);
        assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_DRIVE_STRENGTH, 4));
        expect(2, 0x1a0, 7U << (g * 3), 1U << (g * 3));
        assert(!memcmp(banks, expected, sizeof(banks)));
        int value;
        assert(!mtk_pinconf_drive_get_rev1(&h, &soc.pins[n], &value) && value == 4);
        for (unsigned ma = 2; ma <= 16; ma += 2) {
            assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_DRIVE_STRENGTH, ma));
            expect(2, 0x1a0, 7U << (g * 3), (ma / 2 - 1) << (g * 3));
            assert(!memcmp(banks, expected, sizeof(banks)));
        }
        assert(mtk_pinconf_set(&p, n, PIN_CONFIG_DRIVE_STRENGTH, 3) == -ENOTSUPP);
        assert(mtk_pinconf_set(&p, n, PIN_CONFIG_DRIVE_STRENGTH, 18) == -ENOTSUPP);
        assert(mtk_hw_set_value(&h, &soc.pins[n], PINCTRL_PIN_REG_TDSEL, 16) == -EINVAL);
        assert(mtk_hw_set_value(&h, &soc.pins[n], PINCTRL_PIN_REG_RDSEL, 64) == -EINVAL);
        assert(!memcmp(banks, expected, sizeof(banks)));
    }
    apply_board(&p);
    /* Independent public GPL msdc_io.h masks and board awake values. */
    expect(0, 0x030, 0x3ffc0000, 0); /* Schmitt config makes GPIO114-125 inputs. */
    expect(2, 0x020, 0xfff, 0xfff);
    expect(2, 0x060, 0xf, 0xf);
    expect(2, 0x1a0, 0xfff, 0x249); /* All four selectors=1, i.e.4mA. */
    expect(2, 0x0d0, 0xffff, 0xcccc);
    expect(2, 0x080, 0x3ffff, 0);
    expect(2, 0x100, 0x3ffc0, 0x18000); /* CLK/DS down; CMD/DAT/RST up. */
    expect(2, 0x110, 0x3ffc0, 0x1ffc0); /* 10Kohm except RST. */
    expect(2, 0x120, 0x3ffc0, 0x20000); /* RST 50Kohm. */
    assert(!memcmp(banks, expected, sizeof(banks)));
    /* Shared CMD/DS/RST drive and RDSEL fields, independent CLK drive. */
    assert(!mtk_pinconf_set(&p, 124, PIN_CONFIG_DRIVE_STRENGTH, 6));
    expect(2, 0x1a0, 0x1c0, 0x80);
    for (unsigned n = 122; n <= 125; n++) {
        int value;
        assert(!mtk_pinconf_drive_get_rev1(&h, &soc.pins[n], &value));
        assert(value == (n == 123 ? 4 : 6));
    }
    assert(!mtk_pinconf_set(&p, 123, MTK_PIN_CONFIG_RDSEL, 17));
    expect(2, 0x080, 0x3f000, 17U << 12);
    for (unsigned n = 122; n <= 125; n++) {
        int value;
        assert(!mtk_hw_get_value(&h, &soc.pins[n], PINCTRL_PIN_REG_RDSEL, &value));
        assert(value == 17);
    }
    assert(!memcmp(banks, expected, sizeof(banks)));
    /* Existing MMC1 fields stay separate in their shared IOCFG_B registers. */
    assert(!mtk_pinconf_set(&p, 130, PIN_CONFIG_DRIVE_STRENGTH, 4));
    expect(2, 0x1b0, 0x7000000, 0x1000000);
    assert(!mtk_pinconf_set(&p, 129, PIN_CONFIG_INPUT_ENABLE, 1));
    expect(2, 0x020, BIT(25), BIT(25));
    assert(!mtk_pinconf_set(&p, 134, PIN_CONFIG_BIAS_PULL_DOWN, MTK_PUPD_SET_R1R0_01));
    expect(2, 0x100, BIT(23), BIT(23));
    expect(2, 0x110, BIT(23), BIT(23));
    expect(2, 0x120, BIT(23), 0);
    assert(!memcmp(banks, expected, sizeof(banks)));
    /* Unsupported neighbors must neither change pads nor GPIO direction. */
    assert(mtk_pinconf_set(&p, 113, PIN_CONFIG_INPUT_SCHMITT_ENABLE, 1) == -ENOTSUPP);
    assert(mtk_pinconf_set(&p, 126, PIN_CONFIG_DRIVE_STRENGTH, 4) == -ENOTSUPP);
    assert(!memcmp(banks, expected, sizeof(banks)));
    puts("MMC0 native field groups, board electrical contract and MMC1 isolation: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="mt6797-mmc0-pinctrl-") as tmp:
    src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
    src.write_text(code)
    subprocess.run(shlex.split(os.environ.get("HOSTCC", "cc")) + [
        "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
        *shlex.split(os.environ.get("CFLAGS", "")), str(src), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
