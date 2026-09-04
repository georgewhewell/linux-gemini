#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Host-only fixtures for the read-only A53 DVFS/thermal evidence collector.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
collector=${COLLECTOR:-$script_dir/mt6797-a53-dvfs-thermal-preflight-capture.sh}
[ -x "$collector" ] || collector=$script_dir/a53-dvfs-thermal-preflight-capture
fixture=$(mktemp -d "${TMPDIR:-/tmp}/a53-thermal-preflight.XXXXXX")
trap 'rm -rf "$fixture"' EXIT HUP INT TERM

cpu_root=$fixture/sys/devices/system/cpu
cpufreq_root=$cpu_root/cpufreq
thermal_root=$fixture/sys/class/thermal
kernel_log=$fixture/dmesg

write_policy()
{
	policy=$cpufreq_root/$1
	mkdir -p "$policy"
	printf '%s\n' "$2" > "$policy/affected_cpus"
	printf '%s\n' mtk-cpufreq > "$policy/scaling_driver"
	printf '%s\n' "$3" > "$policy/scaling_available_frequencies"
	printf '%s\n' "$3" > "$policy/scaling_min_freq"
	printf '%s\n' "$3" > "$policy/scaling_max_freq"
	printf '%s\n' "$3" > "$policy/scaling_cur_freq"
	printf '%s\n' "$3" > "$policy/cpuinfo_cur_freq"
}

expect_failure()
{
	wanted=$1
	if CPU_ROOT=$cpu_root CPUFREQ_ROOT=$cpufreq_root \
		THERMAL_ROOT=$thermal_root KERNEL_LOG=$kernel_log \
		"$collector" > "$fixture/failure.out" 2> "$fixture/failure.err"; then
		echo "collector unexpectedly accepted: $wanted" >&2
		exit 1
	fi
	grep -Fqx result=fail "$fixture/failure.out"
	grep -Fq "$wanted" "$fixture/failure.err"
}

mkdir -p "$thermal_root/thermal_zone0" "$thermal_root/cooling_device0" \
	"$thermal_root/cooling_device1" "$cpu_root/cpu8" "$cpu_root/cpu9"
write_policy policy0 '0 1 2 3' 1391000
write_policy policy4 '4 5 6 7' 1950000
printf '%s\n' 0 > "$cpu_root/cpu8/online"
printf '%s\n' 0 > "$cpu_root/cpu9/online"

zone=$thermal_root/thermal_zone0
printf '%s\n' soc-thermal > "$zone/type"
printf '%s\n' enabled > "$zone/mode"
printf '%s\n' step_wise > "$zone/policy"
printf '%s\n' 46000 > "$zone/temp"
printf '%s\n' passive 85000 5000 | {
	read -r type
	read -r temp
	read -r hyst
	printf '%s\n' "$type" > "$zone/trip_point_0_type"
	printf '%s\n' "$temp" > "$zone/trip_point_0_temp"
	printf '%s\n' "$hyst" > "$zone/trip_point_0_hyst"
}
printf '%s\n' passive 95000 5000 | {
	read -r type
	read -r temp
	read -r hyst
	printf '%s\n' "$type" > "$zone/trip_point_1_type"
	printf '%s\n' "$temp" > "$zone/trip_point_1_temp"
	printf '%s\n' "$hyst" > "$zone/trip_point_1_hyst"
}
printf '%s\n' critical 117000 0 | {
	read -r type
	read -r temp
	read -r hyst
	printf '%s\n' "$type" > "$zone/trip_point_2_type"
	printf '%s\n' "$temp" > "$zone/trip_point_2_temp"
	printf '%s\n' "$hyst" > "$zone/trip_point_2_hyst"
}

printf '%s\n' cpufreq-cpu0 > "$thermal_root/cooling_device0/type"
printf '%s\n' 0 > "$thermal_root/cooling_device0/max_state"
printf '%s\n' 0 > "$thermal_root/cooling_device0/cur_state"
printf '%s\n' cpufreq-cpu4 > "$thermal_root/cooling_device1/type"
printf '%s\n' 0 > "$thermal_root/cooling_device1/max_state"
printf '%s\n' 0 > "$thermal_root/cooling_device1/cur_state"
ln -s ../cooling_device0 "$zone/cdev0"
ln -s ../cooling_device0 "$zone/cdev1"
ln -s ../cooling_device1 "$zone/cdev2"
ln -s ../cooling_device1 "$zone/cdev3"
printf '%s\n' 0 > "$zone/cdev0_trip_point"
printf '%s\n' 1 > "$zone/cdev1_trip_point"
printf '%s\n' 0 > "$zone/cdev2_trip_point"
printf '%s\n' 1 > "$zone/cdev3_trip_point"
printf '%s\n' 'mtk-thermal: calibrated sensor ready' > "$kernel_log"

CPU_ROOT=$cpu_root CPUFREQ_ROOT=$cpufreq_root \
	THERMAL_ROOT=$thermal_root KERNEL_LOG=$kernel_log \
	"$collector" > "$fixture/pass.out"
for marker in \
	'schema=gemini-mt6797-a53-dvfs-thermal-preflight-v1' \
	'collector_mode=read_only' \
	'frequency_transition_available=no' \
	'frequency_transition_evidence=not_exercised' \
	'cooling_action_evidence=not_exercised' \
	'critical_shutdown_evidence=dt_policy_only' \
	'calibration_gate=driver_bound_requires_valid_nvmem' \
	'cooling_devices=2' 'cooling_bindings=4' 'a72_touched=no' 'result=pass'; do
	grep -Fqx "$marker" "$fixture/pass.out"
done

printf '%s\n' active > "$zone/trip_point_3_type"
printf '%s\n' 100000 > "$zone/trip_point_3_temp"
printf '%s\n' 0 > "$zone/trip_point_3_hyst"
expect_failure 'expected three thermal trips, found 4'
rm "$zone/trip_point_3_type" "$zone/trip_point_3_temp" "$zone/trip_point_3_hyst"

printf '%s\n' 1 > "$thermal_root/cooling_device0/max_state"
expect_failure 'cpufreq-cpu0 can request a frequency change'
printf '%s\n' 0 > "$thermal_root/cooling_device0/max_state"

mkdir "$cpufreq_root/policy8"
expect_failure 'A72 cpufreq policy is present'
rmdir "$cpufreq_root/policy8"

printf '%s\n' 'mtk-thermal: required calibration data is invalid' > "$kernel_log"
expect_failure 'CPU DVFS or thermal error found in kernel log'

echo 'test-mt6797-a53-dvfs-thermal-preflight-capture: PASS'
