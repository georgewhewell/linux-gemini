#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Capture and classify one RAM-booted Gemini GPU activation.

set -eu

if [ "$#" -ne 0 ]; then
	echo "usage: $0" >&2
	exit 2
fi

: "${DMESG:=dmesg}"

gpu_device=/sys/bus/platform/devices/13040000.gpu
expected_frequencies="238000000 365000000 442500000 520000000 610000000 780000000 900000000"
reject_pattern='Fatal error during GPU init|reset timed out'
reject_pattern="$reject_pattern|error powering up gpu|power transition timeout"
reject_pattern="$reject_pattern|failed to (get|enable) regulators"
reject_pattern="$reject_pattern|Couldn't set OPP regulators|Couldn't set recommended OPP"
reject_pattern="$reject_pattern|Couldn't initialize GPU devfreq"
reject_pattern="$reject_pattern|devfreq init failed|unexpected product ID"
reject_pattern="$reject_pattern|failed to (read product ID|initialize regmap|register regulator)"
reject_pattern="$reject_pattern|Failed to power (on|off) domain .*MFG|using dummy regulator"
clock_pattern='(^|[[:space:]])'
clock_pattern="$clock_pattern(mfgpll|mfg_sel|mfg_52m_sel|mfg_bg3d|infra_mfg_vcg)"
clock_pattern="$clock_pattern([[:space:]]|$)"
failed=0

reject()
{
	echo "REJECT: $*" >&2
	failed=1
}

boot_log=$($DMESG) || {
	echo "cannot read the kernel log" >&2
	exit 2
}

gpu_log=$(printf '%s\n' "$boot_log" |
	grep -E '13040000\.gpu|rt5735|mfg|MFG|panfrost' || true)

echo "[gpu-kernel-log]"
printf '%s\n' "$gpu_log"

if ! printf '%s\n' "$gpu_log" |
	grep -q '13040000\.gpu.*mali-t880 id '; then
	reject "Panfrost did not identify the Mali-T880"
fi

if printf '%s\n' "$gpu_log" | grep -Eq "$reject_pattern"; then
	reject "kernel log contains a GPU, VGPU, or MFG failure"
fi

if [ ! -L "$gpu_device/driver" ] ||
	[ "$(basename "$(readlink "$gpu_device/driver")")" != panfrost ]; then
	reject "13040000.gpu is not bound to Panfrost"
fi

rt5735_device=
for candidate in /sys/bus/i2c/drivers/rt5735-regulator/*-001c; do
	[ -e "$candidate" ] || continue
	rt5735_device=$candidate
	break
done
if [ -z "$rt5735_device" ]; then
	reject "no RT5735 device at address 0x1c is bound"
fi

devfreq_device=
for candidate in "$gpu_device"/devfreq/*; do
	[ -d "$candidate" ] || continue
	devfreq_device=$candidate
	break
done
if [ -z "$devfreq_device" ]; then
	reject "Panfrost has no devfreq device"
else
	echo "[gpu-devfreq]"
	for attribute in available_frequencies cur_freq governor min_freq max_freq; do
		if [ ! -r "$devfreq_device/$attribute" ]; then
			reject "devfreq attribute $attribute is unavailable"
			continue
		fi
		printf '%s=' "$attribute"
		cat "$devfreq_device/$attribute"
	done

	actual_frequencies=$(cat "$devfreq_device/available_frequencies")
	if [ "$actual_frequencies" != "$expected_frequencies" ]; then
		reject "devfreq OPP list does not match the Gemini type-12 table"
	fi
fi

echo "[mfg-clocks]"
if [ -r /sys/kernel/debug/clk/clk_summary ]; then
	grep -E "$clock_pattern" \
		/sys/kernel/debug/clk/clk_summary || true
else
	reject "clk_summary is unavailable"
fi

echo "[vgpu-regulator]"
if [ -r /sys/kernel/debug/regulator/regulator_summary ]; then
	grep -E '(^|[[:space:]])(vgpu|rt5735-vsel0)([[:space:]]|$)' \
		/sys/kernel/debug/regulator/regulator_summary ||
		reject "VGPU is absent from regulator_summary"
else
	reject "regulator_summary is unavailable"
fi

if [ "$failed" -ne 0 ]; then
	echo "GPU_ACTIVATION=REJECT"
	exit 1
fi

echo "GPU_ACTIVATION=PASS"
