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
#define WARN_ON_ONCE(x) (x)
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
generic_header = (root / "include/linux/pinctrl/pinconf-generic.h").read_text()
code += fragment(generic_header, r"^#define PIN_CONF_PACKED[^\n]+\n")
for name in ("param", "argument", "packed"):
    code += fragment(generic_header, rf"^static inline [^\n]+ pinconf_to_config_{name}\(.*?^}}\n")
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
    int (*drive_get)(struct mtk_pinctrl *, const struct mtk_pin_desc *, int *);
    int (*adv_drive_get)(struct mtk_pinctrl *, const struct mtk_pin_desc *, u32 *);
    int (*bias_get_combo)(struct mtk_pinctrl *, const struct mtk_pin_desc *, u32 *, u32 *);
    int (*adv_pull_get)(struct mtk_pinctrl *, const struct mtk_pin_desc *, bool, u32 *);
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
                  ("mtk_pinconf_drive_get_rev1", "int"), ("mtk_pinconf_bias_set_pupd_r1_r0", "static int"),
                  ("mtk_pinconf_bias_set_pu_pd", "static int")):
    code += fragment(core, rf"^{ret} {name}\(.*?^}}\n")
code += r'''
/* These unselected pull formats must never receive an MMC0/1 request. */
static int mtk_pinconf_bias_set_pu_pd_rsel(struct mtk_pinctrl *h,
    const struct mtk_pin_desc *d, u32 p, u32 a)
{ (void)h; (void)d; (void)p; (void)a; assert(false); return -ENOTSUPP; }
static int mtk_pinconf_bias_set_pullsel_pullen(struct mtk_pinctrl *h,
    const struct mtk_pin_desc *d, u32 p, u32 a)
{ (void)h; (void)d; (void)p; (void)a; assert(false); return -ENOTSUPP; }
'''
code += fragment(core, r"^int mtk_pinconf_bias_set_combo\(.*?^}\n")
code += fragment(paris, r"^static int mtk_drv_uA_to_adv\(.*?^}\n")
code += fragment(paris, r"^static const int mtk_drv_adv_uA\[\][^\n]+\n")
code += fragment(paris, r"^static int mtk_drv_adv_to_uA\(.*?^}\n")
code += fragment(paris, r"^static int mtk_pinconf_get\(.*?^}\n")
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
static void apply_sim_board(struct pinctrl_dev *p)
{
'''
# Apply this declared state in the real generic parser's parameter order.
sim_board = fragment(dts, r"\tmd1_sim_pins:.*?(?=\n\t};)")
generic = (root / "drivers/pinctrl/pinconf-generic.c").read_text()
params = re.findall(r'\{ "([^"]+)", (PIN_CONFIG_\w+), (\d+)\s*[,}]',
                    fragment(generic, r"^static const struct pinconf_generic_params dt_params\[\].*?^};\n"))
pinfuncs = (root / "include/dt-bindings/pinctrl/mt6797-pinfunc.h").read_text()
sim_pins = []
for group in re.findall(r"\t\tpins-[^\n]+ \{(.*?)\n\t\t};", sim_board, re.S):
    functions = re.findall(r"MT6797_GPIO(\d+)__FUNC_(?:MD1_SIM[12]|MD_INT[01]_C2K_UIM[01])_\w+", group)
    numbers = [int(n) for n in functions]
    sim_pins += numbers
    for symbol in re.findall(r"MT6797_GPIO\d+__FUNC_(?:MD1_SIM[12]|MD_INT[01]_C2K_UIM[01])_\w+", group):
        match = re.search(rf"^#define {symbol} \(MTK_PIN_NO\((\d+)\) \| 1\)$", pinfuncs, re.M)
        assert match, symbol
        code += f"    assert(!mtk_hw_set_value(p->hw, &soc.pins[{match[1]}], PINCTRL_PIN_REG_MODE, 1));\n"
    for prop, param, default in params:
        match = re.search(rf"\b{re.escape(prop)}(?:\s*=\s*<([^>]+)>)?;", group)
        if match:
            for n in numbers:
                code += f"    assert(!mtk_pinconf_set(p, {n}, {param}, {match[1] or default}));\n"
assert sorted(sim_pins) == [112, 113, 126, 127, 128, 155, 156, 157]
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
    /* Exact GPL SIM fields; test all supported encodings, not board strength. */
    const unsigned sim_pin[] = { 126, 127, 128, 155, 156, 157 };
    const unsigned sim_ies[] = { 17, 18, 19, 21, 22, 23 };
    const unsigned sim_smt[] = { 11, 12, 12, 14, 15, 15 };
    for (unsigned i = 0; i < ARRAY_SIZE(sim_pin); i++) {
        unsigned n = sim_pin[i], b = 24 + i;
        unsigned dir_off = n / 32 * 0x10, dir_bit = n % 32;
        check_field(&h, n, PINCTRL_PIN_REG_IES, 0x020, sim_ies[i], 1);
        check_field(&h, n, PINCTRL_PIN_REG_SMT, 0x060, sim_smt[i], 1);
        check_field(&h, n, PINCTRL_PIN_REG_PUPD, 0x100, b, 1);
        check_field(&h, n, PINCTRL_PIN_REG_R0, 0x110, b, 1);
        check_field(&h, n, PINCTRL_PIN_REG_R1, 0x120, b, 1);
        assert(soc.pull_type[n] == MTK_PULL_PUPD_R1R0_TYPE);
        assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_INPUT_ENABLE, 1));
        expect(2, 0x020, BIT(sim_ies[i]), BIT(sim_ies[i]));
        assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_INPUT_SCHMITT_ENABLE, 1));
        expect(0, dir_off, BIT(dir_bit), 0);
        expect(2, 0x060, BIT(sim_smt[i]), BIT(sim_smt[i]));
        assert(!memcmp(banks, expected, sizeof(banks)));
        assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_BIAS_DISABLE, 0));
        expect(2, 0x100, BIT(b), BIT(b));
        expect(2, 0x110, BIT(b), 0);
        expect(2, 0x120, BIT(b), 0);
        assert(!memcmp(banks, expected, sizeof(banks)));
        assert(mtk_pinconf_set(&p, n, PIN_CONFIG_BIAS_PULL_UP, 1) == -EINVAL);
        assert(!memcmp(banks, expected, sizeof(banks)));
        for (unsigned arg = MTK_PUPD_SET_R1R0_01;
             arg <= MTK_PUPD_SET_R1R0_11; arg++) {
            unsigned bits = arg - MTK_PUPD_SET_R1R0_00;
            assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_BIAS_PULL_UP, arg));
            expect(2, 0x100, BIT(b), 0);
            expect(2, 0x110, BIT(b), (bits & 1) ? BIT(b) : 0);
            expect(2, 0x120, BIT(b), (bits & 2) ? BIT(b) : 0);
            assert(!memcmp(banks, expected, sizeof(banks)));
        }
    }
    for (unsigned i = 0; i < 2; i++) {
        unsigned rst = i ? 156 : 127, sio = rst + 1, bit = i ? 15 : 12;
        int value;
        assert(!mtk_hw_set_value(&h, &soc.pins[rst], PINCTRL_PIN_REG_SMT, 0));
        expect(2, 0x060, BIT(bit), 0);
        assert(!mtk_hw_get_value(&h, &soc.pins[sio], PINCTRL_PIN_REG_SMT, &value));
        assert(value == 0);
        assert(!mtk_hw_set_value(&h, &soc.pins[sio], PINCTRL_PIN_REG_SMT, 1));
        expect(2, 0x060, BIT(bit), BIT(bit));
        assert(!mtk_hw_get_value(&h, &soc.pins[rst], PINCTRL_PIN_REG_SMT, &value));
        assert(value == 1 && !memcmp(banks, expected, sizeof(banks)));
    }
    /* Separate SIM detect enables from signal R1/R0 and adjacent MMC fields. */
    for (unsigned i = 0; i < 2; i++) {
        unsigned n = 112 + i, ies = 20 + 4 * i, smt = 13 + 3 * i;
        unsigned pu = 3 + 4 * i;
        check_field(&h, n, PINCTRL_PIN_REG_IES, 0x020, ies, 1);
        check_field(&h, n, PINCTRL_PIN_REG_SMT, 0x060, smt, 1);
        check_field(&h, n, PINCTRL_PIN_REG_PU, 0x180, pu, 1);
        check_field(&h, n, PINCTRL_PIN_REG_PD, 0x150, i, 1);
        assert(soc.pull_type[n] == MTK_PULL_PU_PD_TYPE);
        assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_INPUT_ENABLE, 1));
        expect(2, 0x020, BIT(ies), BIT(ies));
        assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_INPUT_SCHMITT_ENABLE, 1));
        expect(0, 0x030, BIT(16 + i), 0);
        expect(2, 0x060, BIT(smt), BIT(smt));
        assert(!memcmp(banks, expected, sizeof(banks)));
        assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_BIAS_PULL_UP, 1));
        expect(2, 0x180, BIT(pu), BIT(pu));
        expect(2, 0x150, BIT(i), 0);
        assert(!memcmp(banks, expected, sizeof(banks)));
        assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_BIAS_PULL_DOWN, 1));
        expect(2, 0x180, BIT(pu), 0);
        expect(2, 0x150, BIT(i), BIT(i));
        assert(!memcmp(banks, expected, sizeof(banks)));
        assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_BIAS_DISABLE, 0));
        expect(2, 0x180, BIT(pu), 0);
        expect(2, 0x150, BIT(i), 0);
        assert(!memcmp(banks, expected, sizeof(banks)));
        assert(mtk_pinconf_set(&p, n, PIN_CONFIG_BIAS_PULL_UP,
                              MTK_PUPD_SET_R1R0_10) == -EINVAL);
        assert(!memcmp(banks, expected, sizeof(banks)));
    }
    /* Unsupported neighbors must neither change pads nor GPIO direction. */
    assert(mtk_pinconf_set(&p, 111, PIN_CONFIG_INPUT_SCHMITT_ENABLE, 1) == -ENOTSUPP);
    assert(mtk_pinconf_set(&p, 126, PIN_CONFIG_DRIVE_STRENGTH, 4) == -ENOTSUPP);
    assert(!memcmp(banks, expected, sizeof(banks)));
    /* Generic output-enable/disable changes only DIR, never DO or pad state. */
    for (unsigned n = 0; n <= 261; n++) {
        unsigned off = n / 32 * 0x10, bit = n % 32;
        unsigned long config = pinconf_to_config_packed(PIN_CONFIG_OUTPUT_ENABLE, 99);
        for (unsigned arg = 0; arg < 3; arg++) {
            assert(!mtk_pinconf_set(&p, n, PIN_CONFIG_OUTPUT_ENABLE, arg));
            expect(0, off, BIT(bit), arg ? BIT(bit) : 0);
            assert(!memcmp(banks, expected, sizeof(banks)));
            int err = mtk_pinconf_get(&p, n, &config);
            assert(err == (arg ? 0 : -EINVAL));
            if (arg) assert(pinconf_to_config_argument(config) == 1);
        }
    }
    unsigned long config = pinconf_to_config_packed(PIN_CONFIG_OUTPUT_ENABLE, 0);
    /* Pseudo-input and virtual EINT descriptors have no direction field. */
    for (unsigned n = 262; n < soc.npins; n++) {
        assert(mtk_pinconf_set(&p, n, PIN_CONFIG_OUTPUT_ENABLE, 1) == -ENOTSUPP);
        assert(mtk_pinconf_get(&p, n, &config) == -ENOTSUPP);
        assert(!memcmp(banks, expected, sizeof(banks)));
    }
    assert(mtk_pinconf_get(&p, soc.npins, &config) == -EINVAL);
    assert(mtk_pinconf_set(&p, soc.npins, PIN_CONFIG_OUTPUT_ENABLE, 1) == -EINVAL);
    struct mtk_pin_reg_calc no_dir[PINCTRL_PIN_REG_MAX];
    memcpy(no_dir, soc.reg_cal, sizeof(no_dir));
    no_dir[PINCTRL_PIN_REG_DIR] = (struct mtk_pin_reg_calc){0};
    struct mtk_pin_soc missing = soc;
    missing.reg_cal = no_dir;
    h.soc = &missing;
    assert(mtk_pinconf_set(&p, 126, PIN_CONFIG_OUTPUT_ENABLE, 1) == -ENOTSUPP);
    assert(mtk_pinconf_get(&p, 126, &config) == -ENOTSUPP);
    assert(!memcmp(banks, expected, sizeof(banks)));
    h.soc = &soc;
    /* Existing Schmitt semantics remain intact in either direction order. */
    assert(!mtk_pinconf_set(&p, 126, PIN_CONFIG_INPUT_SCHMITT_ENABLE, 0));
    expect(0, 0x030, BIT(30), BIT(30));
    expect(2, 0x060, BIT(11), 0);
    assert(!mtk_pinconf_set(&p, 126, PIN_CONFIG_OUTPUT_ENABLE, 0));
    expect(0, 0x030, BIT(30), 0);
    assert(!memcmp(banks, expected, sizeof(banks)));
    assert(!mtk_pinconf_set(&p, 126, PIN_CONFIG_INPUT_SCHMITT_ENABLE, 1));
    expect(2, 0x060, BIT(11), BIT(11));
    assert(!memcmp(banks, expected, sizeof(banks)));
    apply_sim_board(&p);
    /* Independent board DWS/GPL gpio_cfg.h detect-pad contract. */
    expect(0, 0x3e0, 0xff, 0x11);
    expect(0, 0x030, BIT(16) | BIT(17), 0);
    expect(2, 0x020, BIT(20) | BIT(24), BIT(20) | BIT(24));
    expect(2, 0x060, BIT(13) | BIT(16), BIT(13) | BIT(16));
    expect(2, 0x180, BIT(3) | BIT(7), BIT(3) | BIT(7));
    expect(2, 0x150, 3, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(sim_pin); i++) {
        unsigned n = sim_pin[i], b = 24 + i;
        bool input = n == 128 || n == 157;
        expect(0, n / 8 * 0x10 + 0x300, 15U << (n % 8 * 4), 1U << (n % 8 * 4));
        expect(0, n / 32 * 0x10, BIT(n % 32), input ? 0 : BIT(n % 32));
        expect(2, 0x020, BIT(sim_ies[i]), BIT(sim_ies[i]));
        expect(2, 0x060, BIT(sim_smt[i]), BIT(sim_smt[i]));
        expect(2, 0x100, BIT(b), input ? 0 : BIT(b));
        expect(2, 0x110, BIT(b), 0);
        expect(2, 0x120, BIT(b), input ? BIT(b) : 0);
        config = pinconf_to_config_packed(PIN_CONFIG_INPUT_SCHMITT_ENABLE, 0);
        assert(mtk_pinconf_get(&p, n, &config) == (input ? 0 : -EINVAL));
    }
    assert(!memcmp(banks, expected, sizeof(banks)));
    puts("MMC0 native field groups, board electrical contract and MMC1 isolation: PASS");
    puts("SIM native fields, shared Schmitt and explicit pull encodings: PASS");
    puts("SIM detect sparse fields, PU/PD and declared input/pull-up state: PASS");
    puts("Paris output direction only and declared SIM state in generic property order: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="mt6797-mmc0-pinctrl-") as tmp:
    src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
    src.write_text(code)
    subprocess.run(shlex.split(os.environ.get("HOSTCC", "cc")) + [
        "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-Wno-pointer-sign",
        *shlex.split(os.environ.get("CFLAGS", "")), str(src), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
