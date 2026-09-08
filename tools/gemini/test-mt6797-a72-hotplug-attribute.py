#!/usr/bin/env python3
"""Compile the real MT6797 A72 cpu_ops hotplug hooks and CPU9 admission natively.

The cpu_can_disable() answer is what arch_cpu_is_hotpluggable() reports, and a
false answer suppresses /sys/devices/system/cpu/cpuN/online entirely.  The
hooks below are extracted verbatim from the kernel sources and executed on the
host with the exact CPU numbers the Gemini DT assigns to the Cortex-A72 cores.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
psci = (root / "arch/arm64/kernel/mt6797_psci.c").read_text()
core = (root / "drivers/soc/mediatek/mt6797-a72-power-core.c").read_text()


def extract(source, signature):
    start = source.index(signature)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


# The default (non-P32) hotplug hooks must exist and be in the ops table.
default_hooks = psci[psci.index("#ifndef CONFIG_ARM64_MT6797_A72_P32_ROLLBACK"):]
hooks = "".join(
    extract(source, sig)
    for source, sig in (
        (psci, "static bool mt6797_psci_is_late_a72("),
        (psci, "static bool mt6797_psci_cpu_can_disable("),
        (default_hooks, "static int mt6797_psci_cpu_disable("),
    )
)
table = psci[psci.index("const struct cpu_operations mt6797_psci_ops = {"):]
table = table[: table.index("\n};\n")]
assert ".cpu_can_disable = mt6797_psci_cpu_can_disable," in table
# cpu_disable is installed unconditionally under HOTPLUG_CPU, cpu_die only
# behind the P32 rollback build, so op_cpu_disable() aborts early otherwise.
hotplug = table[table.index("#ifdef CONFIG_HOTPLUG_CPU"):]
before_p32 = hotplug[: hotplug.index("#ifdef CONFIG_ARM64_MT6797_A72_P32_ROLLBACK")]
assert ".cpu_disable = mt6797_psci_cpu_disable," in before_p32
assert ".cpu_die" not in before_p32 and ".cpu_kill" not in before_p32
# The non-P32 cpu_disable definition must be what the table references.
assert re.search(r"#ifndef CONFIG_ARM64_MT6797_A72_P32_ROLLBACK\s*\nstatic int mt6797_psci_cpu_disable\(", psci)

admission = "".join(
    extract(core, sig)
    for sig in (
        "int mt6797_a72_validate_cpu8_boot(",
        "int mt6797_a72_validate_cpu9_boot(",
    )
)

c = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
''' + hooks + admission + r'''
int main(void)
{
	unsigned int cpu;

	for (cpu = 0; cpu < 8; cpu++) {
		assert(!mt6797_psci_cpu_can_disable(cpu));
		assert(mt6797_psci_cpu_disable(cpu) == -EOPNOTSUPP);
	}
	/* The online attribute must exist for both late A72 cores... */
	assert(mt6797_psci_cpu_can_disable(8));
	assert(mt6797_psci_cpu_can_disable(9));
	/* ...while hot-unplug of them stays refused, fail closed. */
	assert(mt6797_psci_cpu_disable(8) == -EPERM);
	assert(mt6797_psci_cpu_disable(9) == -EPERM);
	assert(!mt6797_psci_cpu_can_disable(10));

	/* CPU8: CPU4 online and an armed transaction, never faulted. */
	assert(mt6797_a72_validate_cpu8_boot(8, true, true, false) == 0);
	assert(mt6797_a72_validate_cpu8_boot(9, true, true, false) == -EPERM);
	assert(mt6797_a72_validate_cpu8_boot(8, false, true, false) == -EAGAIN);
	assert(mt6797_a72_validate_cpu8_boot(8, true, false, false) == -EAGAIN);
	assert(mt6797_a72_validate_cpu8_boot(8, true, true, true) == -EUCLEAN);
	/* CPU9: only after CPU8 is online, no rail transaction of its own. */
	assert(mt6797_a72_validate_cpu9_boot(9, true, false) == 0);
	assert(mt6797_a72_validate_cpu9_boot(8, true, false) == -EPERM);
	assert(mt6797_a72_validate_cpu9_boot(9, false, false) == -EAGAIN);
	assert(mt6797_a72_validate_cpu9_boot(9, true, true) == -EUCLEAN);
	assert(mt6797_a72_validate_cpu9_boot(9, false, true) == -EUCLEAN);
	puts("mt6797-a72-hotplug-attribute: pass");
	return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp, "t.c")
    src.write_text(c)
    exe = Path(tmp, "t")
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Werror", "-o", str(exe), str(src)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    sys.stdout.write(out)
    assert out.strip() == "mt6797-a72-hotplug-attribute: pass"
