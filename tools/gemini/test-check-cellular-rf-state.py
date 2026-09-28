#!/usr/bin/env python3
"""Host tests for check-cellular-rf-state.py and its read-only capture script."""
import importlib.util
import os
from pathlib import Path
import re
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('rf', HERE / 'check-cellular-rf-state.py')
rf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rf)
VENDOR = Path(os.environ.get('GEMINI_VENDOR_TREE', '/mnt/Home/src/gemini-vendor-kernel-3.18'))


def capture(pins=None, regs=None, regulators=None, gpio_label=True):
    pins = {**{p: m for p, (m, _) in rf.EXPECTED_PINS.items()}, **(pins or {})}
    base = {0x0a04: 0x8008, 0x0a08: 0x8808, 0x0a46: 0x8808, 0x0a68: 0x8808, 0x0a7a: 0x8002}
    base.update(regs or {})
    lines = ['# capture-cellular-rf-state v1']
    for pin, mode in sorted(pins.items()):
        label = f'{pin}:10005000.pinctrl ' if gpio_label else '0:? '
        lines.append(f'pin {pin} (GPIO{pin}) {label}{pin:03d}: {mode}1000000')
    lines += [f'{r:04x}: {v:04x}' for r, v in sorted(base.items())]
    volts = dict(rf.GEMIAN_MICROVOLTS, **(regulators or {}))
    lines += [f'regulator {n} microvolts={uv} state=enabled' for n, uv in volts.items()]
    return '\n'.join(lines) + '\n'


class CheckTests(unittest.TestCase):
    def test_vendor_state_passes(self):
        for label in (True, False):
            bad, notes = rf.check(capture(gpio_label=label))
            self.assertEqual(bad, [])
            self.assertFalse([n for n in notes if 'OFF' in n or 'Gemian' in n])

    def test_rf_pin_left_in_gpio_mode_is_reported(self):
        bad, _ = rf.check(capture(pins={208: 0, 204: 1}))
        self.assertEqual(len(bad), 2)
        self.assertIn('pin 204: mode 1 but vendor DWS mode 2 PA_VM0', bad)
        self.assertIn('pin 208: mode 0 but vendor DWS mode 1 BPI_BUS20//ANT_SEL2', bad)

    def test_missing_pin_is_reported(self):
        text = '\n'.join(l for l in capture().splitlines() if not l.startswith('pin 183 '))
        self.assertEqual(rf.check(text)[0],
                         ['pin 183: missing from capture (vendor mode 1 RFIC0_BSI_CK)'])

    def test_pmic_software_mode_and_off_output(self):
        bad, notes = rf.check(capture(regs={0x0a46: 0x0002, 0x0a04: 0x8808}))
        self.assertEqual(len(bad), 2)
        self.assertTrue(any(b.startswith('pmic 0a46 vrf18: en=1 on_ctrl=0') for b in bad))
        self.assertTrue(any(b.startswith('pmic 0a04 vtcxo24:') and 'srclk_en_sel=1' in b for b in bad))
        self.assertIn('pmic 0a46 vrf18: output sampled OFF (DA_QI_EN=0)', notes)

    def test_vmipi_has_no_vendor_mode_write(self):
        bad, notes = rf.check(capture(regs={0x0a7a: 0x0000}))
        self.assertEqual(bad, [])
        self.assertIn('pmic 0a7a vmipi: output sampled OFF (DA_QI_EN=0)', notes)

    def test_voltage_difference_is_a_note(self):
        bad, notes = rf.check(capture(regulators={'vrf18': 1800000}))
        self.assertEqual(bad, [])
        self.assertIn('regulator vrf18: 1800000 uV, Gemian snapshot 1810000 uV', notes)

    def test_inconsistent_pin_line_rejected(self):
        with self.assertRaises(ValueError):
            rf.parse('pin 183 (GPIO183) 0:? 184: 1100\n')

    def test_capture_script_covers_every_expectation(self):
        script = (HERE / 'capture-cellular-rf-state.sh').read_text()
        pin_re = re.search(r"grep -E '\^pin \(([^)]*)\) '", script).group(1)
        compiled = re.compile(rf'^({pin_re})$')
        for pin in rf.EXPECTED_PINS:
            self.assertTrue(compiled.match(str(pin)), pin)
        regs = re.search(r'for r in ([0-9a-f ]+); do', script).group(1).split()
        for reg in rf.PMIC_CON0:
            self.assertIn(f'{reg:04x}', regs)
        # regmap debugfs line "%04x: %04x\n" is 11 bytes; 16-bit stride-2 regmap.
        self.assertIn('bs=11 skip=$((0x$r / 2)) count=1', script)
        self.assertNotRegex(script, r'(^|[^>])>\s*/sys|echo [^|]*> *"?\$')

    @unittest.skipUnless((VENDOR / 'tools/dct/config/MT6797.fig').exists(), 'vendor tree absent')
    def test_embedded_table_matches_vendor_dws(self):
        derived = rf.derive_pins(
            VENDOR / 'drivers/misc/mediatek/dws/mt6797/aeon6797_6m_n.dws',
            VENDOR / 'tools/dct/config/MT6797.fig')
        self.assertEqual(derived, rf.EXPECTED_PINS)


if __name__ == '__main__':
    unittest.main(verbosity=2)
