#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read-only classification of the MT6797 GPU probe/autosuspend preflight.

set -eu

if [ "$#" -ne 0 ]; then
	echo "usage: $0" >&2
	exit 2
fi

: "${DEBUGFS_ROOT:=/sys/kernel/debug}"
: "${DEVMEM:=devmem}"
: "${DMESG:=dmesg}"
: "${PROC_ROOT:=/proc}"
: "${SYSFS_ROOT:=/sys}"

gpu_device=$SYSFS_ROOT/bus/platform/devices/13040000.gpu
rt5735_driver=$SYSFS_ROOT/bus/i2c/drivers/rt5735-regulator
genpd_summary=$DEBUGFS_ROOT/pm_genpd/pm_genpd_summary
clk_summary=$DEBUGFS_ROOT/clk/clk_summary
regulator_summary=$DEBUGFS_ROOT/regulator/regulator_summary
mfg_domain_mask=0x3f00
mfg_protection_mask=0xa00000
failed=0

reject()
{
	echo "REJECT: $*" >&2
	failed=1
}

find_summary_line()
{
	name=$1
	file=$2

	awk -v name="$name" '
		$1 == name { print; found = 1; exit }
		END { if (!found) exit 1 }
	' "$file"
}

boot_log=$($DMESG) || {
	echo "cannot read the kernel log" >&2
	exit 2
}
gpu_log=$(printf '%s\n' "$boot_log" |
	grep -E '13040000\.gpu|rt5735|mfg|MFG|panfrost' || true)

echo "[gpu-kernel-log]"
printf '%s\n' "$gpu_log"

reject_pattern='Fatal error during GPU init|reset timed out|failed to .*reset'
reject_pattern="$reject_pattern|error powering up gpu|power transition timeout"
reject_pattern="$reject_pattern|Failed to power (on|off) domain mfg"
reject_pattern="$reject_pattern|Failed to restore bus protection"
reject_pattern="$reject_pattern|failed to (get|enable) regulators|using dummy regulator"
reject_pattern="$reject_pattern|Couldn't set (OPP regulators|recommended OPP)"
reject_pattern="$reject_pattern|Couldn't initialize GPU devfreq"
reject_pattern="$reject_pattern|failed to select GPU OPP profile|devfreq init failed"
topology_pattern='shader_present=0x[1-9a-fA-F][0-9a-fA-F]*'
topology_pattern="$topology_pattern l2_present=0x[1-9a-fA-F][0-9a-fA-F]*"

if ! printf '%s\n' "$gpu_log" |
	grep -q '13040000\.gpu.*mali-t880 id '; then
	reject "Panfrost did not identify the Mali-T880 after reset deassertion"
fi
if ! printf '%s\n' "$gpu_log" | grep -Eq "$topology_pattern"; then
	reject "Panfrost did not report nonzero shader and L2 topology"
fi
if printf '%s\n' "$gpu_log" | grep -Eq "$reject_pattern"; then
	reject "kernel log contains a GPU, VGPU, MFG, reset, or devfreq failure"
fi
if [ ! -L "$gpu_device/driver" ] ||
	[ "$(basename "$(readlink "$gpu_device/driver")")" != panfrost ]; then
	reject "13040000.gpu is not bound to Panfrost"
fi
rt5735_device=
for candidate in "$rt5735_driver"/*-001c; do
	[ -e "$candidate" ] || continue
	rt5735_device=$candidate
	break
done
if [ -z "$rt5735_device" ]; then
	reject "no RT5735 VGPU provider at I2C address 0x1c is bound"
fi

echo "[gpu-irqs]"
if [ ! -r "$PROC_ROOT/interrupts" ]; then
	reject "interrupt registration state is unavailable"
else
	for irq_name in panfrost-job panfrost-mmu panfrost-gpu; do
		if line=$(grep -E "[[:space:]]$irq_name([[:space:]]|$)" \
			"$PROC_ROOT/interrupts" | head -n 1); then
			printf '%s\n' "$line"
		else
			reject "$irq_name is not registered"
		fi
	done
fi

echo "[gpu-runtime-pm]"
runtime_status=missing
if [ -r "$gpu_device/power/runtime_status" ]; then
	tries=0
	while [ "$tries" -lt 5 ]; do
		runtime_status=$(cat "$gpu_device/power/runtime_status")
		[ "$runtime_status" = suspended ] && break
		tries=$((tries + 1))
		sleep 1
	done
	printf 'runtime_status=%s\n' "$runtime_status"
else
	reject "GPU runtime_status is unavailable"
fi
if [ "$runtime_status" != suspended ]; then
	reject "GPU did not autosuspend after probe; do not force it or submit jobs"
fi
for attribute in control autosuspend_delay_ms runtime_active_time runtime_suspended_time; do
	if [ -r "$gpu_device/power/$attribute" ]; then
		printf '%s=' "$attribute"
		cat "$gpu_device/power/$attribute"
	else
		reject "GPU runtime-PM attribute $attribute is unavailable"
	fi
done

echo "[mfg-power-domains]"
if [ ! -r "$genpd_summary" ]; then
	reject "pm_genpd_summary is unavailable"
else
	for domain in mfg_async mfg mfg_core0 mfg_core1 mfg_core2 mfg_core3; do
		if line=$(find_summary_line "$domain" "$genpd_summary"); then
			printf '%s\n' "$line"
			state=$(printf '%s\n' "$line" | awk '{ print $2 }')
			case $state in
			off-*) ;;
			*) reject "$domain is not off after GPU autosuspend" ;;
			esac
		else
			reject "$domain is absent from pm_genpd_summary"
		fi
	done
fi

echo "[mfg-registers]"
if ! spm_status=$($DEVMEM 0x10006180 32); then
	reject "cannot read spm_pwr_status at 0x10006180"
	spm_status=unreadable
fi
printf 'spm_pwr_status=%s\n' "$spm_status"
if ! spm_status_2nd=$($DEVMEM 0x10006184 32); then
	reject "cannot read spm_pwr_status_2nd at 0x10006184"
	spm_status_2nd=unreadable
fi
printf 'spm_pwr_status_2nd=%s\n' "$spm_status_2nd"
if ! protecten=$($DEVMEM 0x10001220 32); then
	reject "cannot read infra_topaxi_protecten at 0x10001220"
	protecten=unreadable
fi
printf 'infra_topaxi_protecten=%s\n' "$protecten"
if ! protectsta=$($DEVMEM 0x10001228 32); then
	reject "cannot read infra_topaxi_protectsta1 at 0x10001228"
	protectsta=unreadable
fi
printf 'infra_topaxi_protectsta1=%s\n' "$protectsta"
if ! swsysrst=$($DEVMEM 0x10007018 32); then
	reject "cannot read toprgu_wdt_swsysrst at 0x10007018"
	swsysrst=unreadable
fi
printf 'toprgu_wdt_swsysrst=%s\n' "$swsysrst"
echo "reset_evidence=probe-identification+toprgu-readback"
case "$spm_status:$spm_status_2nd:$protecten:$protectsta:$swsysrst" in
*unreadable*) ;;
*)
	if [ $((spm_status & mfg_domain_mask)) -ne 0 ] ||
		[ $((spm_status_2nd & mfg_domain_mask)) -ne 0 ]; then
		reject "MFG status bits 13:8 are not off in both SPM status words"
	fi
	if [ $((protecten & mfg_protection_mask)) -ne $((mfg_protection_mask)) ] ||
		[ $((protectsta & mfg_protection_mask)) -ne $((mfg_protection_mask)) ]; then
		reject "MFG TOPAXI protection bits 21 and 23 are not asserted"
	fi
	if [ $((swsysrst & 0x4)) -ne 0 ]; then
		reject "MFG reset remains asserted after Panfrost initialization"
	fi
	;;
esac

echo "[mfg-clocks]"
if [ ! -r "$clk_summary" ]; then
	reject "clk_summary is unavailable"
else
	# Multi-domain MediaTek Panfrost keeps these votes until system suspend.
	for clock in mfg_bg3d infra_mfg_vcg mfg_52m_sel mfg_sel; do
		if line=$(find_summary_line "$clock" "$clk_summary"); then
			printf '%s\n' "$line"
			enable_count=$(printf '%s\n' "$line" | awk '{ print $2 }')
			prepare_count=$(printf '%s\n' "$line" | awk '{ print $3 }')
			if [ "$enable_count" -eq 0 ] ||
				[ "$prepare_count" -eq 0 ]; then
				reject "$clock lost Panfrost's runtime clock vote"
			fi
		else
			reject "$clock is absent from clk_summary"
		fi
	done
fi

echo "[vgpu-regulator]"
if [ ! -r "$regulator_summary" ]; then
	reject "regulator_summary is unavailable"
elif line=$(find_summary_line vgpu "$regulator_summary"); then
	printf '%s\n' "$line"
	use_count=$(printf '%s\n' "$line" | awk '{ print $2 }')
	if [ "$use_count" -eq 0 ]; then
		reject "VGPU lost Panfrost's OPP regulator vote"
	fi
else
	reject "VGPU is absent from regulator_summary"
fi

echo "[gpu-devfreq]"
devfreq_device=
for candidate in "$gpu_device"/devfreq/*; do
	[ -d "$candidate" ] || continue
	devfreq_device=$candidate
	break
done
if [ -z "$devfreq_device" ]; then
	reject "Panfrost has no devfreq device"
else
	for attribute in available_frequencies cur_freq governor min_freq max_freq; do
		if [ -r "$devfreq_device/$attribute" ]; then
			printf '%s=' "$attribute"
			cat "$devfreq_device/$attribute"
		else
			reject "devfreq attribute $attribute is unavailable"
		fi
	done
	actual_frequencies=$(cat "$devfreq_device/available_frequencies")
	if printf '%s\n' "$gpu_log" | grep -q 'GPU DVFS table type 12,'; then
		expected_frequencies='238000000 365000000 442500000 520000000'
		expected_frequencies="$expected_frequencies 610000000 780000000 900000000"
	elif printf '%s\n' "$gpu_log" |
		grep -Eq 'GPU DVFS table type (6|11),'; then
		expected_frequencies=204000000
	elif printf '%s\n' "$gpu_log" |
		grep -Eq 'GPU DVFS table type (0|1|2|4|5|8|9|10),'; then
		expected_frequencies=238000000
	elif printf '%s\n' "$gpu_log" | grep -q 'GPU DVFS table unknown,'; then
		expected_frequencies=204000000
	else
		reject "kernel log does not identify the selected MT6797 OPP profile"
		expected_frequencies=
	fi
	if [ -n "$expected_frequencies" ] &&
		[ "$actual_frequencies" != "$expected_frequencies" ]; then
		reject "devfreq OPP list does not match the selected MT6797 speed bin"
	fi
	cur_freq=$(cat "$devfreq_device/cur_freq")
	case " $actual_frequencies " in
	*" $cur_freq "*) ;;
	*) reject "current GPU frequency is outside the selected OPP list" ;;
	esac
fi

if [ "$failed" -ne 0 ]; then
	echo "GPU_PREFLIGHT=REJECT"
	exit 1
fi

echo "GPU_PREFLIGHT=PASS"
