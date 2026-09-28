#!/usr/bin/env python3
"""Compare a capture-cellular-rf-state.sh snapshot with vendor RF expectations.

Host-only.  Expectations and their sources:

* Pin modes: vendor drivers/misc/mediatek/dws/mt6797/aeon6797_6m_n.dws
  <def_mode> joined with tools/dct/config/MT6797.fig mode names (vendor
  8cfe6596).  Preloader/LK program these; Linux claims none of these pins in
  the M39 DT, so any difference means something reprogrammed or never set them.
* PMIC RF LDO control: vendor power/mt6797/pmic_initial_setting.c:355-385
  (PMIC_INIT_SETTING_V1, run by the vendor kernel's pmic probe) sets ON_CTRL=1
  (bit 3, hardware on/off by SRCLKEN) and SRCLK_EN_SEL (bits 13:11) = 0 for
  VTCXO24 (0x0a04) and 1 for VTCXO28/VRF18/VRF12 (0x0a08/0x0a46/0x0a68).
  Mainline performs no such initialisation.
* Voltages: Gemian regulator snapshot in the ixoo audit repository
  docs/hardware/gemini-gemian-baseline.md:583-615.  Informational only.

Pass --dws/--fig to re-derive the embedded pin table from the vendor tree.
"""
import argparse
import re
import sys
import xml.etree.ElementTree as ET

# pin: (mode, function) from aeon6797_6m_n.dws def_mode + MT6797.fig names.
EXPECTED_PINS = {
    100: (1, 'SRCLKENAI0'), 102: (1, 'SRCLKENA0'), 103: (1, 'SRCLKENA1'),
    112: (1, 'MD_INT1_C2K_UIM1_HOT_PLUG_IN'), 113: (1, 'MD_INT0_C2K_UIM0_HOT_PLUG_IN'),
    126: (1, 'MD1_SIM1_SCLK'), 127: (1, 'MD1_SIM1_SRST'), 128: (1, 'MD1_SIM1_SIO'),
    155: (1, 'MD1_SIM2_SCLK'), 156: (1, 'MD1_SIM2_SRST'), 157: (1, 'MD1_SIM2_SIO'),
    183: (1, 'RFIC0_BSI_CK'), 184: (1, 'RFIC0_BSI_EN'), 185: (1, 'RFIC0_BSI_D0'),
    186: (1, 'RFIC0_BSI_D1'), 187: (1, 'RFIC0_BSI_D2'),
    188: (1, 'MIPI0_SCLK'), 189: (1, 'MIPI0_SDATA'), 190: (1, 'MIPI1_SCLK'),
    191: (1, 'MIPI1_SDATA'),
    192: (1, 'BPI_BUS4'), 193: (1, 'BPI_BUS5'),
    194: (0, 'GPIO194'), 195: (0, 'GPIO195'), 196: (0, 'GPIO196'), 197: (0, 'GPIO197'),
    198: (1, 'BPI_BUS10'), 199: (1, 'BPI_BUS11'), 200: (1, 'BPI_BUS12'),
    201: (1, 'BPI_BUS13'), 202: (1, 'BPI_BUS14'), 203: (1, 'BPI_BUS15'),
    204: (2, 'PA_VM0'), 205: (2, 'PA_VM1'),
    206: (1, 'BPI_BUS18'), 207: (1, 'BPI_BUS19'),
    208: (1, 'BPI_BUS20//ANT_SEL2'), 209: (1, 'BPI_BUS21//ANT_SEL3'),
    210: (2, 'DET_BPI0'), 211: (2, 'DET_BPI1'),
    212: (1, 'BPI_BUS0'), 213: (1, 'BPI_BUS1'), 214: (1, 'BPI_BUS2'), 215: (1, 'BPI_BUS3'),
    216: (1, 'MIPI2_SCLK'), 217: (1, 'MIPI2_SDATA'), 218: (1, 'MIPI3_SCLK'),
    219: (1, 'MIPI3_SDATA'),
}

# register: (name, expected ON_CTRL, expected SRCLK_EN_SEL); None = no vendor write.
PMIC_CON0 = {
    0x0a04: ('vtcxo24', 1, 0),
    0x0a08: ('vtcxo28', 1, 1),
    0x0a46: ('vrf18', 1, 1),
    0x0a68: ('vrf12', 1, 1),
    0x0a7a: ('vmipi', None, None),
}

GEMIAN_MICROVOLTS = {
    'vrf18': 1810000, 'vrf12': 1200000, 'vmipi': 1800000,
    'vtcxo24': 2375000, 'vtcxo28': 2800000, 'vxo22': 2200000,
}

# pinctrl core: 'pin N (NAME) ' [+ 'GPIO:LABEL ' with GPIOLIB], then paris
# mtk_pctrl_show_one_pin(): '%03d: ' followed by the mode digit.
PIN_RE = re.compile(r'^pin (\d+) \([^)]*\) (?:\d+:\S+ )?(\d+): (\d)')
REG_RE = re.compile(r'^([0-9a-f]{4}): ([0-9a-f]{4})$')
REGULATOR_RE = re.compile(r'^regulator (\S+) microvolts=(\S+) state=(\S+)$')


def derive_pins(dws_path, fig_path):
    """Re-derive EXPECTED_PINS from the vendor DWS and .fig for the same pins."""
    fig = {}
    with open(fig_path, encoding='ascii', errors='replace') as handle:
        lines = handle.read().splitlines()
    for line in lines:
        m = re.match(r'GPIO(\d+) = (.*)', line)
        if m:
            fig[int(m.group(1))] = dict(
                (int(a), b) for a, b in re.findall(r'MODE(\d)\(([^)]*)\)', m.group(2)))
    gpio = ET.parse(dws_path).getroot().find(".//module[@name='gpio']")
    derived = {}
    for node in gpio:
        m = re.match(r'gpio(\d+)$', node.tag)
        if not m or int(m.group(1)) not in EXPECTED_PINS:
            continue
        pin = int(m.group(1))
        mode = int(node.findtext('def_mode'))
        derived[pin] = (mode, fig[pin][mode])
    return derived


def parse(text):
    pins, regs, regulators = {}, {}, {}
    for raw in text.splitlines():
        line = raw.strip()
        m = PIN_RE.match(line)
        if m:
            if m.group(1) != str(int(m.group(2))):
                raise ValueError(f'inconsistent pin line: {line!r}')
            pins[int(m.group(1))] = int(m.group(3))
            continue
        m = REG_RE.match(line)
        if m:
            regs[int(m.group(1), 16)] = int(m.group(2), 16)
            continue
        m = REGULATOR_RE.match(line)
        if m:
            regulators[m.group(1)] = (m.group(2), m.group(3))
    return pins, regs, regulators


def check(text):
    """Return (mismatches, notes) as lists of strings."""
    pins, regs, regulators = parse(text)
    bad, notes = [], []
    for pin, (mode, function) in sorted(EXPECTED_PINS.items()):
        if pin not in pins:
            bad.append(f'pin {pin}: missing from capture (vendor mode {mode} {function})')
        elif pins[pin] != mode:
            bad.append(f'pin {pin}: mode {pins[pin]} but vendor DWS mode {mode} {function}')
    for reg, (name, on_ctrl, sel) in sorted(PMIC_CON0.items()):
        if reg not in regs:
            bad.append(f'pmic {reg:04x} {name}: missing from capture')
            continue
        value = regs[reg]
        fields = (f'en={(value >> 1) & 1} on_ctrl={(value >> 3) & 1} '
                  f'srclk_en_sel={(value >> 11) & 7} da_qi_en={(value >> 15) & 1}')
        if on_ctrl is not None and ((value >> 3) & 1, (value >> 11) & 7) != (on_ctrl, sel):
            bad.append(f'pmic {reg:04x} {name}: {fields}; vendor kernel sets '
                       f'on_ctrl={on_ctrl} srclk_en_sel={sel}')
        else:
            notes.append(f'pmic {reg:04x} {name}: {fields}')
        if not (value >> 15) & 1:
            notes.append(f'pmic {reg:04x} {name}: output sampled OFF (DA_QI_EN=0)')
    for name, uv in sorted(GEMIAN_MICROVOLTS.items()):
        if name not in regulators:
            notes.append(f'regulator {name}: not captured')
        elif regulators[name][0] != str(uv):
            notes.append(f'regulator {name}: {regulators[name][0]} uV, Gemian snapshot {uv} uV')
    return bad, notes


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('capture')
    parser.add_argument('--dws')
    parser.add_argument('--fig')
    args = parser.parse_args()
    if bool(args.dws) != bool(args.fig):
        parser.error('--dws and --fig go together')
    if args.dws and derive_pins(args.dws, args.fig) != EXPECTED_PINS:
        print('embedded pin table differs from vendor DWS', file=sys.stderr)
        return 2
    with open(args.capture, encoding='utf-8', errors='replace') as handle:
        bad, notes = check(handle.read())
    for line in notes:
        print('NOTE', line)
    for line in bad:
        print('MISMATCH', line)
    print(f'RESULT mismatches={len(bad)}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
