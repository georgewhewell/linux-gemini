#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the collector's real IRQ identity function against synthetic sysfs."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


CAPTURE = Path(__file__).with_name("capture-gpu-preflight.sh").read_text()
FUNCTION = "irq_hardware_matches()" + CAPTURE.split(
    "irq_hardware_matches()", 1
)[1].split("\n}", 1)[0] + "\n}\n"


class IrqIdentity(unittest.TestCase):
    def check_identity(self, linux_irq, hwirq, chip="GICv3", expected="296"):
        with tempfile.TemporaryDirectory(prefix="gemini-irq-test-") as tmp:
            irq = Path(tmp) / "kernel/irq" / linux_irq
            irq.mkdir(parents=True)
            if hwirq is not None:
                (irq / "hwirq").write_text(hwirq + "\n")
            (irq / "chip_name").write_text(chip + "\n")
            return subprocess.run(
                ["sh", "-eu", "-c", FUNCTION + 'irq_hardware_matches "$1" "$2"',
                 "test", linux_irq, expected],
                env={**os.environ, "SYSFS_ROOT": tmp}, check=False,
            ).returncode

    def test_dynamic_linux_irq(self):
        for linux_irq in ("215", "296", "400"):
            self.assertEqual(self.check_identity(linux_irq, "296"), 0)

    def test_all_three_gic_intids(self):
        for linux_irq, hwirq in (("215", "296"), ("214", "295"), ("213", "294")):
            self.assertEqual(self.check_identity(linux_irq, hwirq, expected=hwirq), 0)

    def test_linux_index_cannot_substitute_for_hardware_identity(self):
        self.assertNotEqual(self.check_identity("296", "295"), 0)

    def test_wrong_chip_or_missing_identity(self):
        self.assertNotEqual(self.check_identity("215", "296", chip="dummy"), 0)
        self.assertNotEqual(self.check_identity("215", None), 0)


if __name__ == "__main__":
    unittest.main()
