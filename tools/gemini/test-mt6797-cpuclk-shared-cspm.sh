#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Structural regression check for the MT6797 CPU clock/DVFSP CSPM window.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cpuclk="$root/drivers/clk/mediatek/clk-mt6797-cpu.c"
handoff="$root/drivers/soc/mediatek/mt6797-dvfsp-handoff.c"
dtsi="$root/arch/arm64/boot/dts/mediatek/mt6797.dtsi"

require()
{
	pattern=$1
	file=$2
	if ! rg -q -- "$pattern" "$file"; then
		echo "missing shared CSPM invariant in $file: $pattern" >&2
		exit 1
	fi
}

# The DT intentionally describes the same CSPM register window to both users.
require 'dvfsp-handoff@11015000' "$dtsi"
require 'clock-controller@1001a000' "$dtsi"
require 'reg-names = "mcumixed", "cspm", "mcucfg"' "$dtsi"
if [ "$(rg -o '0x11015000' "$dtsi" | wc -l)" -ne 2 ]; then
	echo 'MT6797 DT must describe exactly two CSPM users at 0x11015000' >&2
	exit 1
fi

# DVFSP remains the exclusive owner.  CPU clock may map, but must not reserve,
# the shared window or whichever provider probes second will fail with -EBUSY.
require 'handoff->cspm = devm_platform_ioremap_resource\(pdev, 0\)' "$handoff"
require 'platform_get_resource_byname\(pdev, IORESOURCE_MEM, "cspm"\)' "$cpuclk"
require 'observer->cspm = devm_ioremap\(&pdev->dev, cspm->start,' "$cpuclk"
if rg -q 'devm_platform_ioremap_resource_byname\(pdev, "cspm"\)' "$cpuclk"; then
	echo 'CPU clock must not claim the shared CSPM window exclusively' >&2
	exit 1
fi

echo 'test-mt6797-cpuclk-shared-cspm: PASS'
