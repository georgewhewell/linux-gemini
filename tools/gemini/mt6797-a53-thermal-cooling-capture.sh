#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Read-only acceptance capture for Gemini A53 cpufreq cooling. It never writes
# a thermal mode, trip, cooling state, cpufreq policy, regulator, or clock.

set -eu

thermal_root=${THERMAL_ROOT:-/sys/class/thermal}
cpufreq_root=${CPUFREQ_ROOT:-/sys/devices/system/cpu/cpufreq}
kernel_log=${KERNEL_LOG:-}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

mp0_desc="1391000 1339000 1274000 1222000 1118000 1014000 897000 806000 715000 624000"
mp1_desc="1950000 1885000 1820000 1755000 1703000 1625000 1495000"
mp1_desc="$mp1_desc 1352000 1209000 1092000 962000 832000 650000"

fail()
{
	echo "result=fail"
	echo "reason=$*" >&2
	exit 1
}

read_one()
{
	read_path=$1

	[ -r "$read_path" ] || return 1
	value=$(sed -n '1p' "$read_path")
	[ -n "$value" ] || return 1
	printf '%s\n' "$value"
}

require_unsigned()
{
	unsigned_name=$1
	unsigned_value=$2

	case "$unsigned_value" in
	''|*[!0-9]*) fail "$unsigned_name is not unsigned: $unsigned_value" ;;
	esac
}

find_unique_type()
{
	class_glob=$1
	wanted_type=$2
	found=
	count=0

	for candidate in $class_glob; do
		[ -d "$candidate" ] || continue
		candidate_type=$(read_one "$candidate/type") || continue
		[ "$candidate_type" = "$wanted_type" ] || continue
		found=$candidate
		count=$((count + 1))
	done
	[ "$count" -eq 1 ] ||
		fail "expected one $wanted_type device, found $count"
	printf '%s\n' "$found"
}

find_trip()
{
	expected_type=$1
	expected_temp=$2
	expected_hyst=$3
	found=
	count=0

	for type_file in "$zone"/trip_point_*_type; do
		[ -r "$type_file" ] || continue
		trip_id=${type_file##*/trip_point_}
		trip_id=${trip_id%_type}
		trip_type=$(read_one "$type_file") || continue
		trip_temp=$(read_one "$zone/trip_point_${trip_id}_temp") || continue
		trip_hyst=$(read_one "$zone/trip_point_${trip_id}_hyst") || continue
		[ "$trip_type" = "$expected_type" ] || continue
		[ "$trip_temp" = "$expected_temp" ] || continue
		[ "$trip_hyst" = "$expected_hyst" ] || continue
		found=$trip_id
		count=$((count + 1))
	done
	[ "$count" -eq 1 ] ||
		fail "expected one $expected_type ${expected_temp}mC trip, found $count"
	printf '%s\n' "$found"
}

state_cap()
{
	rates=$1
	state=$2

	set -- $rates
	while [ "$state" -gt 0 ]; do
		shift
		state=$((state - 1))
	done
	[ "$#" -gt 0 ] || return 1
	printf '%s\n' "$1"
}

check_cooling_device()
{
	cpu_name=$1
	expected_max=$2
	rates=$3
	policy_name=$4
	cdev=$(find_unique_type "$thermal_root/cooling_device*" "cpufreq-$cpu_name")

	max_state=$(read_one "$cdev/max_state") ||
		fail "$cpu_name cooling maximum is unreadable"
	cur_state=$(read_one "$cdev/cur_state") ||
		fail "$cpu_name cooling state is unreadable"
	require_unsigned "$cpu_name maximum cooling state" "$max_state"
	require_unsigned "$cpu_name cooling state" "$cur_state"
	[ "$max_state" -eq "$expected_max" ] ||
		fail "$cpu_name maximum cooling state is $max_state, expected $expected_max"
	[ "$cur_state" -le "$max_state" ] ||
		fail "$cpu_name cooling state $cur_state exceeds $max_state"

	cap_khz=$(state_cap "$rates" "$cur_state") ||
		fail "$cpu_name cooling state has no frequency"
	policy_max=$(read_one "$cpufreq_root/$policy_name/scaling_max_freq") ||
		fail "$policy_name scaling_max_freq is unreadable"
	require_unsigned "$policy_name scaling maximum" "$policy_max"
	[ "$policy_max" -le "$cap_khz" ] ||
		fail "$policy_name maximum $policy_max exceeds cooling cap $cap_khz"

	printf '%s_cooling_state=%s\n' "$policy_name" "$cur_state"
	printf '%s_cooling_max_state=%s\n' "$policy_name" "$max_state"
	printf '%s_cooling_cap_khz=%s\n' "$policy_name" "$cap_khz"
}

if ! dvfs_capture=$("$script_dir/mt6797-a53-dvfs-capture.sh"); then
	printf '%s\n' "$dvfs_capture" >&2
	fail "A53 DVFS prerequisite capture failed"
fi
printf '%s\n' "$dvfs_capture"

[ -d "$thermal_root" ] || fail "thermal class is absent"
zone=$(find_unique_type "$thermal_root/thermal_zone*" soc-thermal)
mode=$(read_one "$zone/mode") || fail "thermal zone mode is unreadable"
policy=$(read_one "$zone/policy") || fail "thermal governor is unreadable"
temperature=$(read_one "$zone/temp") || fail "thermal temperature is unreadable"
[ "$mode" = "enabled" ] || fail "thermal zone mode is $mode"
[ "$policy" = "step_wise" ] || fail "thermal governor is $policy"
require_unsigned "SoC temperature" "$temperature"
[ "$temperature" -le 130000 ] ||
	fail "SoC temperature is implausibly high: ${temperature}mC"

trip_count=0
for type_file in "$zone"/trip_point_*_type; do
	[ -r "$type_file" ] || continue
	trip_count=$((trip_count + 1))
done
[ "$trip_count" -eq 3 ] || fail "expected three thermal trips, found $trip_count"
passive_trip=$(find_trip passive 85000 5000)
emergency_trip=$(find_trip passive 95000 5000)
critical_trip=$(find_trip critical 117000 0)

bindings=
binding_count=0
for trip_file in "$zone"/cdev*_trip_point; do
	[ -r "$trip_file" ] || continue
	cdev_link=${trip_file%_trip_point}
	cdev_type=$(read_one "$cdev_link/type") ||
		fail "cooling-map device type is unreadable"
	trip_id=$(read_one "$trip_file") ||
		fail "cooling-map trip is unreadable"
	bindings="${bindings}${cdev_type}:${trip_id}\n"
	binding_count=$((binding_count + 1))
done
[ "$binding_count" -eq 4 ] ||
	fail "expected four A53 cooling bindings, found $binding_count"
bindings=$(printf '%b' "$bindings" | sort)
expected_bindings=$(printf '%s\n' \
	"cpufreq-cpu0:$passive_trip" "cpufreq-cpu0:$emergency_trip" \
	"cpufreq-cpu4:$passive_trip" "cpufreq-cpu4:$emergency_trip" | sort)
[ "$bindings" = "$expected_bindings" ] ||
	fail "unexpected thermal cooling bindings: $bindings"

check_cooling_device cpu0 9 "$mp0_desc" policy0
check_cooling_device cpu4 12 "$mp1_desc" policy4

if [ -n "$kernel_log" ]; then
	[ -r "$kernel_log" ] || fail "kernel log is unreadable: $kernel_log"
	log_text=$(sed -n '1,$p' "$kernel_log")
else
	log_text=$(dmesg) || fail "cannot read kernel log"
fi
error_pattern='(mtk.*thermal|soc-thermal|cpufreq-cpu[04]).*'
error_pattern="${error_pattern}(fail|error|invalid|timeout|uncalibrated)"
error_pattern="${error_pattern}|Device not calibrated"
thermal_errors=$(printf '%s\n' "$log_text" | grep -Ei "$error_pattern" || true)
[ -z "$thermal_errors" ] || {
	printf '%s\n' "$thermal_errors" >&2
	fail "thermal, calibration, or cooling error found in kernel log"
}

echo "schema=gemini-mt6797-a53-thermal-cooling-v1"
echo "collector_mode=read_only"
echo "thermal_zone=soc-thermal"
echo "thermal_mode=$mode"
echo "thermal_policy=$policy"
echo "temperature_millic=$temperature"
echo "passive_trip_id=$passive_trip"
echo "emergency_trip_id=$emergency_trip"
echo "critical_trip_id=$critical_trip"
echo "cooling_bindings=4"
echo "a72_touched=no"
echo "result=pass"
