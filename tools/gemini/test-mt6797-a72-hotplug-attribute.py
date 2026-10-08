#!/usr/bin/env python3
"""Compile the real MT6797 A72 cpu_ops hotplug hooks and CPU9 admission natively.

The cpu_can_disable() answer is what arch_cpu_is_hotpluggable() reports, and a
false answer suppresses /sys/devices/system/cpu/cpuN/online entirely.  The
hooks below are extracted verbatim from the kernel sources and executed on the
host with the exact CPU numbers the Gemini DT assigns to the Cortex-A72 cores.
"""
import os
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


# The working hotplug hooks must exist and be in the ops table.
hooks = "".join(
    extract(source, sig)
    for source, sig in (
        (psci, "static bool mt6797_psci_is_late_a72("),
        (psci, "static bool mt6797_psci_cpu_can_disable("),
        (psci, "static int mt6797_psci_cpu_disable("),
        (psci, "static void mt6797_psci_cpu_die("),
        (psci, "static int mt6797_psci_cpu_kill("),
    )
)
table = psci[psci.index("const struct cpu_operations mt6797_psci_ops = {"):]
table = table[: table.index("\n};\n")]
assert ".cpu_can_disable = mt6797_psci_cpu_can_disable," in table
# The online attribute and native owner-gated shutdown hooks are exposed.
hotplug = table[table.index("#ifdef CONFIG_HOTPLUG_CPU"):]
assert ".cpu_disable = mt6797_psci_cpu_disable," in hotplug
assert ".cpu_die = mt6797_psci_cpu_die," in table
assert ".cpu_kill = mt6797_psci_cpu_kill," in table

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
static int admitted,psci_disabled,died,killed,firmware_result;
static int mt6797_a72_power_cpu_disable_admit(unsigned cpu)
{return cpu<8||cpu>9?-EOPNOTSUPP:admitted?0:-EPERM;}
static int standard_disable(unsigned cpu){assert(cpu==8||cpu==9);psci_disabled++;return firmware_result;}
static void standard_die(unsigned cpu){assert(cpu==8||cpu==9);died++;}
static int standard_kill(unsigned cpu){assert(cpu==8||cpu==9);killed++;return firmware_result;}
static struct {int (*cpu_disable)(unsigned);void (*cpu_die)(unsigned);int (*cpu_kill)(unsigned);} cpu_psci_ops={standard_disable,standard_die,standard_kill};
static int mt6797_a72_power_cpu_kill(unsigned cpu,int (*fn)(unsigned))
{assert(fn==standard_kill);return fn(cpu);}
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
	/* Teardown requires owner admission before reaching standard PSCI. */
	assert(mt6797_psci_cpu_disable(8) == -EPERM);
	assert(mt6797_psci_cpu_disable(9) == -EPERM);
	assert(!mt6797_psci_cpu_can_disable(10));
	assert(!psci_disabled);admitted=1;
	assert(!mt6797_psci_cpu_disable(8) && psci_disabled==1);
	firmware_result=-EPERM;assert(mt6797_psci_cpu_disable(9)==-EPERM);
	mt6797_psci_cpu_die(8);assert(died==1);
	firmware_result=-EIO;assert(mt6797_psci_cpu_kill(8)==-EIO && killed==1);
	firmware_result=0;assert(!mt6797_psci_cpu_kill(9) && killed==2);

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
