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
LOG_FUNCTION = "gpu_log_failures()" + CAPTURE.split(
    "gpu_log_failures()", 1
)[1].split("\n}", 1)[0] + "\n}\n"
REJECT_DEFS = CAPTURE[CAPTURE.index("\nreject_pattern="):CAPTURE.index("\nidentity_pattern=")]


class IrqIdentity(unittest.TestCase):
    def check_identity(self, linux_irq, hwirq, chip="MT_SYSIRQ", expected="264"):
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
                capture_output=True, text=True,
            ).returncode

    def test_dynamic_linux_irq(self):
        for linux_irq in ("215", "296", "400"):
            self.assertEqual(self.check_identity(linux_irq, "264"), 0)

    def test_all_three_sysirq_spis(self):
        for linux_irq, hwirq in (("215", "264"), ("214", "263"), ("213", "262")):
            self.assertEqual(self.check_identity(linux_irq, hwirq, expected=hwirq), 0)

    def test_linux_index_cannot_substitute_for_hardware_identity(self):
        self.assertNotEqual(self.check_identity("264", "263"), 0)

    def test_wrong_chip_or_missing_identity(self):
        self.assertNotEqual(self.check_identity("215", "264", chip="GICv3"), 0)
        self.assertNotEqual(self.check_identity("215", None), 0)

    def test_parent_gic_intid_is_not_sysirq_spi(self):
        self.assertNotEqual(self.check_identity("215", "296"), 0)


class GpuLogScope(unittest.TestCase):
    def classify(self, line):
        return subprocess.run(
            ["sh", "-eu", "-c", REJECT_DEFS + "\n" + LOG_FUNCTION + "gpu_log_failures"],
            input=line + "\n", capture_output=True, text=True, check=False,
        )

    def test_captured_usb_warning_is_not_gpu_failure(self):
        line = "[    0.904174] mtu3 11271000.usb: supply vusb33 not found, using dummy regulator"
        result = self.classify(line)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")

    def test_gpu_failures_still_rejected_and_reported(self):
        for line in (
            "panfrost 13040000.gpu: supply mali not found, using dummy regulator",
            "panfrost 13040000.gpu: MFG idle check failed: -110",
            "panfrost 13040000.gpu: shader power transition timeout",
            "mtk-scpsys 10006000.power-controller: Failed to power off domain mfg",
            "rt5735: failed to enable regulators",
        ):
            result = self.classify(line)
            self.assertEqual(result.returncode, 0, line)
            self.assertIn(line, result.stdout)


if __name__ == "__main__":
    unittest.main()
