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
: "${SLEEP:=sleep}"
: "${SYSFS_ROOT:=/sys}"
: "${G3_OBSERVE_SECONDS:=2}"

gpu_device=$SYSFS_ROOT/bus/platform/devices/13040000.gpu
gpu_dt=$SYSFS_ROOT/firmware/devicetree/base/gpu@13040000
rt5735_dt=$SYSFS_ROOT/firmware/devicetree/base/i2c@11010000/regulator@1c
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

case $G3_OBSERVE_SECONDS in
''|*[!0-9]*)
	echo "G3_OBSERVE_SECONDS must be an integer from 1 through 60" >&2
	exit 2
	;;
esac
if [ "$G3_OBSERVE_SECONDS" -lt 1 ] || [ "$G3_OBSERVE_SECONDS" -gt 60 ]; then
	echo "G3_OBSERVE_SECONDS must be an integer from 1 through 60" >&2
	exit 2
fi

read_dt_string()
{
	tr -d '\000' < "$1"
}

read_dt_cell()
{
	od -An -tx1 -N4 "$1" | tr -d ' \n'
}

driver_name()
{
	device=$1

	[ -L "$device/driver" ] || return 1
	basename "$(readlink "$device/driver")"
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

read_rail()
{
	regulator=$1
	state=$(cat "$regulator/state") || return 1
	microvolts=$(cat "$regulator/microvolts") || return 1
	users=$(cat "$regulator/num_users") || return 1

	[ "$state" = enabled ] || return 1
	case $microvolts:$users in
		*[!0-9:]*) return 1 ;;
	esac
	[ -n "$microvolts" ] && [ -n "$users" ] || return 1
	printf '%s:%s:%s\n' "$state" "$microvolts" "$users"
}

irq_snapshot()
{
	name=$1
	line=$(grep -E "[[:space:]]$name$" "$PROC_ROOT/interrupts" || true)
	lines=$(printf '%s\n' "$line" | awk 'NF { count++ } END { print count + 0 }')

	[ "$lines" -eq 1 ] || return 1
	irq=$(printf '%s\n' "$line" | awk -F: '{ gsub(/[[:space:]]/, "", $1); print $1 }')
	count=$(printf '%s\n' "${line#*:}" | awk '
		{
			total = 0
			for (i = 1; i <= NF && $i ~ /^[0-9]+$/; i++)
				total += $i
			print total
		}')
	printf '%s:%s\n' "$irq" "$count"
}

assert_no_drm_open()
{
	for drm_device in "$gpu_device"/drm/card* "$gpu_device"/drm/renderD*; do
		[ -e "$drm_device" ] || continue
		name=$(basename "$drm_device")
		for fd in "$PROC_ROOT"/[0-9]*/fd/*; do
			[ -L "$fd" ] || continue
			pid=$(basename "$(dirname "$(dirname "$fd")")")
			[ "$(readlink "$fd" 2>/dev/null || true)" != "/dev/dri/$name" ] ||
				reject "$pid has /dev/dri/$name open"
		done
	done
}

irq_hardware_matches()
{
	# Linux IRQ allocation is dynamic. Match the GIC INTID, not that index.
	linux_irq=$1
	expected_hwirq=$2
	case $linux_irq in
		''|*[!0-9]*) return 1 ;;
	esac
	irq_sysfs=$SYSFS_ROOT/kernel/irq/$linux_irq
	hwirq=$(cat "$irq_sysfs/hwirq" 2>/dev/null) || return 1
	chip=$(cat "$irq_sysfs/chip_name" 2>/dev/null) || return 1
	[ "$chip" = GICv3 ] && [ "$hwirq" = "$expected_hwirq" ]
}

echo "[g3-policy]"
cmdline=$(cat "$PROC_ROOT/cmdline") || {
	echo "cannot read the kernel command line" >&2
	exit 2
}
printf 'cmdline=%s\n' "$cmdline"
case " $cmdline " in
	*' regulator_ignore_unused '*) ;;
	*) reject "regulator_ignore_unused G1 safety fence is absent" ;;
esac
case " $cmdline " in
	*' pd_ignore_unused '*)
		reject "pd_ignore_unused would weaken the inherited G0 lifecycle gate"
		;;
esac

echo "[g3-devicetree]"
for status_spec in \
	"$SYSFS_ROOT/firmware/devicetree/base/i2c@11010000/status:I2C7" \
	"$rt5735_dt/status:RT5735" \
	"$SYSFS_ROOT/firmware/devicetree/base/syscon@13000000/status:MFGSYS" \
	"$gpu_dt/status:GPU"; do
	status_file=${status_spec%%:*}
	label=${status_spec#*:}
	if [ -r "$status_file" ]; then
		status=$(read_dt_string "$status_file")
		printf '%s_dt_status=%s\n' "$label" "$status"
		[ "$status" = okay ] || reject "$label DT status is $status, expected okay"
	else
		reject "$label DT status is unavailable"
	fi
done
if [ -r "$rt5735_dt/regulator-ramp-delay" ]; then
	ramp_delay=$(read_dt_cell "$rt5735_dt/regulator-ramp-delay")
	printf 'regulator_ramp_delay=%s\n' "$ramp_delay"
	[ "$ramp_delay" = 00007d00 ] ||
		reject "RT5735 ramp delay is 0x$ramp_delay, expected 0x00007d00"
else
	reject "RT5735 ramp delay is unavailable"
fi

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
reject_pattern="$reject_pattern|(L2|shader|tiler) power-(on|off) timeout"
reject_pattern="$reject_pattern|GPU fault|Unhandled Page fault"
identity_pattern='13040000\.gpu: mali-t880 id 0x880 major 0x1 minor 0x0 status 0x8$'
topology_pattern='shader_present=0xf l2_present=0x1$'

identity_lines=$(printf '%s\n' "$gpu_log" | grep -E "$identity_pattern" || true)
identity_count=$(printf '%s\n' "$identity_lines" |
	awk 'NF { count++ } END { print count + 0 }')
if [ "$identity_count" -ne 1 ]; then
	reject "Panfrost did not report exactly one MT6797 T880 r1p0 status-8 identity"
fi
if ! printf '%s\n' "$gpu_log" | grep -Eq "$topology_pattern"; then
	reject "Panfrost did not report the qualified shader=0xf L2=0x1 topology"
fi
if printf '%s\n' "$gpu_log" | grep -Eq "$reject_pattern"; then
	reject "kernel log contains a GPU, VGPU, MFG, reset, or devfreq failure"
fi
if [ ! -L "$gpu_device/driver" ] ||
	[ "$(driver_name "$gpu_device" 2>/dev/null || true)" != panfrost ]; then
	reject "13040000.gpu is not bound to Panfrost"
fi
rt5735_device=
for candidate in "$SYSFS_ROOT"/bus/i2c/devices/*-001c; do
	[ -d "$candidate" ] || continue
	[ -L "$candidate/of_node" ] || continue
	[ "$(readlink -f "$candidate/of_node")" = "$(readlink -f "$rt5735_dt")" ] ||
		continue
	if [ -n "$rt5735_device" ]; then
		reject "more than one I2C device maps to RT5735 regulator@1c"
		continue
	fi
	rt5735_device=$candidate
done
if [ -z "$rt5735_device" ]; then
	reject "no I2C device has RT5735 regulator@1c OF ancestry"
elif [ "$(driver_name "$rt5735_device" 2>/dev/null || true)" != \
	rt5735-regulator ]; then
	reject "RT5735 regulator@1c is not bound to rt5735-regulator"
fi
regulator_device=
if [ -n "$rt5735_device" ]; then
	for candidate in "$rt5735_device"/regulator/regulator.*; do
		[ -d "$candidate" ] || continue
		if [ -n "$regulator_device" ]; then
			reject "RT5735 exposes more than one regulator class device"
			continue
		fi
		regulator_device=$candidate
	done
fi
[ -n "$regulator_device" ] || reject "RT5735 regulator class device is absent"
if [ -n "$regulator_device" ]; then
	regulator_class=$SYSFS_ROOT/class/regulator/$(basename "$regulator_device")
	if [ ! -e "$regulator_class" ] ||
		[ "$(readlink -f "$regulator_class")" != \
		"$(readlink -f "$regulator_device")" ]; then
		reject "regulator class ancestry does not resolve to RT5735"
	fi
	regulator_name=$(cat "$regulator_device/name" 2>/dev/null || true)
	[ "$regulator_name" = vgpu ] ||
		reject "RT5735 regulator is named $regulator_name, expected vgpu"
fi

echo "[gpu-irqs]"
if [ ! -r "$PROC_ROOT/interrupts" ]; then
	reject "interrupt registration state is unavailable"
else
	for irq_spec in panfrost-job:296 panfrost-mmu:295 panfrost-gpu:294; do
		irq_name=${irq_spec%%:*}
		expected_hwirq=${irq_spec#*:}
		if snapshot=$(irq_snapshot "$irq_name"); then
			printf '%s=%s\n' "$irq_name" "$snapshot"
			irq=${snapshot%%:*}
			count=${snapshot#*:}
			irq_hardware_matches "$irq" "$expected_hwirq" ||
				reject "$irq_name Linux IRQ $irq does not map to GICv3 INTID $expected_hwirq"
			[ "$count" -eq 0 ] ||
				reject "$irq_name count is $count before the no-job observation"
			case $irq_name in
				panfrost-job) job_irq_before=$snapshot ;;
				panfrost-mmu) mmu_irq_before=$snapshot ;;
				panfrost-gpu) gpu_irq_before=$snapshot ;;
			esac
		else
			reject "$irq_name is absent or does not own a dedicated IRQ line"
		fi
	done
fi
assert_no_drm_open

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
power_control_before=$(cat "$gpu_device/power/control" 2>/dev/null || true)
autosuspend_before=$(cat "$gpu_device/power/autosuspend_delay_ms" 2>/dev/null || true)
[ "$power_control_before" = auto ] ||
	reject "GPU runtime-PM control is not auto"
[ "$autosuspend_before" = 50 ] ||
	reject "GPU autosuspend delay is not 50ms"

echo "[mfg-power-domains]"
if [ ! -r "$genpd_summary" ]; then
	reject "pm_genpd_summary is unavailable"
else
	for domain in mfg_async mfg mfg_core0 mfg_core1 mfg_core2 mfg_core3; do
		if line=$(find_summary_line "$domain" "$genpd_summary"); then
			printf '%s\n' "$line"
			state=$(printf '%s\n' "$line" | awk '{ print $2 }')
			case $state in
			off*) ;;
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
			case $enable_count:$prepare_count in
				1:1) ;;
				*) reject "$clock does not have exactly one Panfrost clock vote" ;;
			esac
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
	[ "$use_count" = 1 ] ||
		reject "VGPU does not have exactly one Panfrost OPP regulator vote"
else
	reject "VGPU is absent from regulator_summary"
fi

echo "[gpu-devfreq]"
devfreq_device=
opp_profile=unknown
cur_freq=unknown
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
	min_freq=$(cat "$devfreq_device/min_freq")
	max_freq=$(cat "$devfreq_device/max_freq")
	governor=$(cat "$devfreq_device/governor")
	if printf '%s\n' "$gpu_log" | grep -q 'GPU DVFS table type 12,'; then
		opp_profile=type12-fixed-low
		expected_frequencies=238000000
	else
		reject "kernel log does not identify the qualified type-12 speed bin"
		opp_profile=unknown
		expected_frequencies=
	fi
	if [ -n "$expected_frequencies" ] &&
		[ "$actual_frequencies" != "$expected_frequencies" ]; then
		reject "devfreq OPP list does not match the selected MT6797 speed bin"
	fi
	cur_freq=$(cat "$devfreq_device/cur_freq")
	[ "$cur_freq" = 238000000 ] ||
		reject "current GPU frequency is not fixed at 238000000 Hz"
	[ "$min_freq" = 238000000 ] ||
		reject "minimum GPU frequency is not fixed at 238000000 Hz"
	[ "$max_freq" = 238000000 ] ||
		reject "maximum GPU frequency is not fixed at 238000000 Hz"
	[ "$governor" = simple_ondemand ] ||
		reject "GPU devfreq governor is not simple_ondemand"
fi

expected_microvolts=
case ${opp_profile:-unknown}:$cur_freq in
	type12-fixed-low:238000000) expected_microvolts=850000 ;;
	*) reject "no evidenced RT5735 voltage for ${opp_profile:-unknown}:$cur_freq" ;;
esac

echo "[g3-stability-window]"
rail_before=unavailable
if [ -n "$regulator_device" ]; then
	rail_before=$(read_rail "$regulator_device" 2>/dev/null || true)
fi
printf 'vgpu_before=%s expected_microvolts=%s\n' \
	"$rail_before" "${expected_microvolts:-unknown}"
[ -n "$expected_microvolts" ] &&
	[ "$rail_before" = "enabled:$expected_microvolts:1" ] ||
	reject "VGPU does not exactly match the selected idle OPP with one consumer"

for clock in mfg_bg3d infra_mfg_vcg mfg_52m_sel mfg_sel; do
	line=$(find_summary_line "$clock" "$clk_summary" 2>/dev/null || true)
	case $clock in
		mfg_bg3d) mfg_bg3d_before=$line ;;
		infra_mfg_vcg) infra_mfg_vcg_before=$line ;;
		mfg_52m_sel) mfg_52m_sel_before=$line ;;
		mfg_sel) mfg_sel_before=$line ;;
	esac
done

printf 'seconds=%s\n' "$G3_OBSERVE_SECONDS"
"$SLEEP" "$G3_OBSERVE_SECONDS"
assert_no_drm_open

runtime_status_after=$(cat "$gpu_device/power/runtime_status" 2>/dev/null || true)
[ "$runtime_status_after" = suspended ] ||
	reject "GPU left runtime suspend during the no-job observation"
power_control_after=$(cat "$gpu_device/power/control" 2>/dev/null || true)
autosuspend_after=$(cat "$gpu_device/power/autosuspend_delay_ms" 2>/dev/null || true)
printf 'runtime_status_after=%s control_after=%s autosuspend_delay_ms_after=%s\n' \
	"$runtime_status_after" "$power_control_after" "$autosuspend_after"
[ "$power_control_after:$autosuspend_after" = \
	"$power_control_before:$autosuspend_before" ] ||
	reject "GPU runtime-PM policy changed during the no-job observation"

if [ -r "$PROC_ROOT/interrupts" ]; then
	for irq_name in panfrost-job panfrost-mmu panfrost-gpu; do
		after=$(irq_snapshot "$irq_name" 2>/dev/null || true)
		case $irq_name in
			panfrost-job) before=${job_irq_before:-missing} ;;
			panfrost-mmu) before=${mmu_irq_before:-missing} ;;
			panfrost-gpu) before=${gpu_irq_before:-missing} ;;
		esac
		printf '%s_after=%s\n' "$irq_name" "${after:-missing}"
		[ "$after" = "$before" ] ||
			reject "$irq_name changed during the no-job observation"
	done
fi

if [ -r "$genpd_summary" ]; then
	for domain in mfg_async mfg mfg_core0 mfg_core1 mfg_core2 mfg_core3; do
		line=$(find_summary_line "$domain" "$genpd_summary" 2>/dev/null || true)
		state=$(printf '%s\n' "$line" | awk '{ print $2 }')
		case $state in
			off*) ;;
			*) reject "$domain left the off state during observation" ;;
		esac
	done
fi

for clock in mfg_bg3d infra_mfg_vcg mfg_52m_sel mfg_sel; do
	after=$(find_summary_line "$clock" "$clk_summary" 2>/dev/null || true)
	case $clock in
		mfg_bg3d) before=$mfg_bg3d_before ;;
		infra_mfg_vcg) before=$infra_mfg_vcg_before ;;
		mfg_52m_sel) before=$mfg_52m_sel_before ;;
		mfg_sel) before=$mfg_sel_before ;;
	esac
	[ "$after" = "$before" ] ||
		reject "$clock summary changed during the no-job observation"
done

rail_after=unavailable
if [ -n "$regulator_device" ]; then
	rail_after=$(read_rail "$regulator_device" 2>/dev/null || true)
fi
printf 'vgpu_after=%s\n' "$rail_after"
[ "$rail_after" = "$rail_before" ] ||
	reject "VGPU state changed during the no-job observation"
cur_freq_after=$(cat "$devfreq_device/cur_freq" 2>/dev/null || true)
available_after=$(cat "$devfreq_device/available_frequencies" 2>/dev/null || true)
min_freq_after=$(cat "$devfreq_device/min_freq" 2>/dev/null || true)
max_freq_after=$(cat "$devfreq_device/max_freq" 2>/dev/null || true)
governor_after=$(cat "$devfreq_device/governor" 2>/dev/null || true)
printf 'devfreq_after available=%s current=%s min=%s max=%s governor=%s\n' \
	"$available_after" "$cur_freq_after" "$min_freq_after" "$max_freq_after" \
	"$governor_after"
[ "$available_after:$cur_freq_after:$min_freq_after:$max_freq_after:$governor_after" = \
	"$actual_frequencies:$cur_freq:$min_freq:$max_freq:$governor" ] ||
	reject "GPU devfreq policy changed during the no-job observation"

spm_status_after=$($DEVMEM 0x10006180 32 2>/dev/null || true)
spm_status_2nd_after=$($DEVMEM 0x10006184 32 2>/dev/null || true)
protecten_after=$($DEVMEM 0x10001220 32 2>/dev/null || true)
protectsta_after=$($DEVMEM 0x10001228 32 2>/dev/null || true)
swsysrst_after=$($DEVMEM 0x10007018 32 2>/dev/null || true)
printf 'registers_after spm=%s spm_2nd=%s protecten=%s protectsta=%s reset=%s\n' \
	"${spm_status_after:-unreadable}" "${spm_status_2nd_after:-unreadable}" \
	"${protecten_after:-unreadable}" "${protectsta_after:-unreadable}" \
	"${swsysrst_after:-unreadable}"
if [ -z "$spm_status_after" ] || [ -z "$spm_status_2nd_after" ] ||
	[ -z "$protecten_after" ] || [ -z "$protectsta_after" ] ||
	[ -z "$swsysrst_after" ]; then
		reject "MFG register readback became unavailable during observation"
else
		[ $((spm_status_after & mfg_domain_mask)) -eq 0 ] &&
			[ $((spm_status_2nd_after & mfg_domain_mask)) -eq 0 ] ||
			reject "MFG power status left paired off state during observation"
		[ $((protecten_after & mfg_protection_mask)) -eq $((mfg_protection_mask)) ] &&
			[ $((protectsta_after & mfg_protection_mask)) -eq \
			$((mfg_protection_mask)) ] ||
			reject "MFG bus protection changed during observation"
		[ $((swsysrst_after & 0x4)) -eq 0 ] ||
			reject "MFG reset asserted during observation"
fi

final_log=$($DMESG) || {
	echo "cannot reread the kernel log" >&2
	exit 2
}
if printf '%s\n' "$final_log" | grep -Eiq "$reject_pattern"; then
	reject "kernel log contains a GPU fault after the no-job observation"
fi

if [ "$failed" -ne 0 ]; then
	echo "GPU_PREFLIGHT=REJECT"
	exit 1
fi

echo "GPU_PREFLIGHT=PASS"
