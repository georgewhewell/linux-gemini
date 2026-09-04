#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Read-only acceptance capture for the one-OPP A53 DVFS/thermal preflight.
# It does not write a governor, frequency, rail, cooling state, or DT property.

set -eu

cpu_root=${CPU_ROOT:-/sys/devices/system/cpu}
cpufreq_root=${CPUFREQ_ROOT:-$cpu_root/cpufreq}
thermal_root=${THERMAL_ROOT:-/sys/class/thermal}
kernel_log=${KERNEL_LOG:-}
source_identity=${SOURCE_IDENTITY:-}

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
	value=$(tr '\n' ' ' < "$read_path" |
		sed 's/[[:space:]][[:space:]]*/ /g; s/^ //; s/ $//')
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

require_source_identity()
{
	case "$source_identity" in
	''|*[!0-9a-f]*) fail "source identity is not lowercase hexadecimal" ;;
	esac
	[ "${#source_identity}" -eq 40 ] ||
		fail "source identity is not 40 hexadecimal characters"
}

require_rate_close()
{
	rate_name=$1
	actual=$2
	expected=$3

	require_unsigned "$rate_name" "$actual"
	delta=$((actual - expected))
	[ "$delta" -ge 0 ] || delta=$((-delta))
	[ "$delta" -le $((expected / 1000)) ] ||
		fail "$rate_name $actual differs from boot OPP $expected"
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

check_policy()
{
	policy_name=$1
	expected_cpus=$2
	expected_rate=$3
	policy=$cpufreq_root/$policy_name

	[ -d "$policy" ] || fail "missing cpufreq policy $policy_name"
	[ "$(read_one "$policy/affected_cpus")" = "$expected_cpus" ] ||
		fail "$policy_name has unexpected affected CPUs"
	[ "$(read_one "$policy/scaling_driver")" = "mtk-cpufreq" ] ||
		fail "$policy_name is not owned by mtk-cpufreq"
	[ "$(read_one "$policy/scaling_available_frequencies")" = "$expected_rate" ] ||
		fail "$policy_name exposes more than its boot OPP"
	[ "$(read_one "$policy/scaling_min_freq")" = "$expected_rate" ] ||
		fail "$policy_name minimum is not its boot OPP"
	[ "$(read_one "$policy/scaling_max_freq")" = "$expected_rate" ] ||
		fail "$policy_name maximum is not its boot OPP"

	scaling_rate=$(read_one "$policy/scaling_cur_freq") ||
		fail "$policy_name requested rate is unreadable"
	actual_rate=$(read_one "$policy/cpuinfo_cur_freq") ||
		fail "$policy_name actual rate is unreadable"
	require_rate_close "$policy_name requested rate" "$scaling_rate" \
		"$expected_rate"
	require_rate_close "$policy_name actual rate" "$actual_rate" \
		"$expected_rate"

	printf '%s_boot_khz=%s\n' "$policy_name" "$actual_rate"
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
		trip=${type_file##*/trip_point_}
		trip=${trip%_type}
		[ "$(read_one "$type_file")" = "$expected_type" ] || continue
		[ "$(read_one "$zone/trip_point_${trip}_temp")" = \
			"$expected_temp" ] || continue
		[ "$(read_one "$zone/trip_point_${trip}_hyst")" = \
			"$expected_hyst" ] || continue
		found=$trip
		count=$((count + 1))
	done
	[ "$count" -eq 1 ] ||
		fail "expected one $expected_type ${expected_temp}mC trip"
	printf '%s\n' "$found"
}

check_cooling_device()
{
	type=$1
	cdev=$(find_unique_type "$thermal_root/cooling_device*" "$type")

	[ "$(read_one "$cdev/max_state")" = 0 ] ||
		fail "$type can request a frequency change"
	[ "$(read_one "$cdev/cur_state")" = 0 ] ||
		fail "$type is outside its only valid state"
}

[ -d "$cpufreq_root" ] || fail "cpufreq class is absent"
require_source_identity
kernel_release=$(uname -r)
case "$kernel_release" in
''|*[!A-Za-z0-9._+-]*) fail "kernel release is unsafe: $kernel_release" ;;
esac
[ ! -d "$cpufreq_root/policy8" ] || fail "A72 cpufreq policy is present"
check_policy policy0 "0 1 2 3" 1391000
check_policy policy4 "4 5 6 7" 1950000

for cpu in 8 9; do
	if [ -r "$cpu_root/cpu$cpu/online" ]; then
		[ "$(read_one "$cpu_root/cpu$cpu/online")" = 0 ] ||
			fail "CPU$cpu is online"
	fi
done

zone=$(find_unique_type "$thermal_root/thermal_zone*" soc-thermal)
[ "$(read_one "$zone/mode")" = enabled ] || fail "thermal zone is disabled"
[ "$(read_one "$zone/policy")" = step_wise ] ||
	fail "thermal governor is not step_wise"
temperature=$(read_one "$zone/temp") || fail "SoC temperature is unreadable"
require_unsigned "SoC temperature" "$temperature"
[ "$temperature" -le 130000 ] || fail "SoC temperature is implausible"

trip_count=0
for type_file in "$zone"/trip_point_*_type; do
	[ -r "$type_file" ] || continue
	trip_count=$((trip_count + 1))
done
[ "$trip_count" -eq 3 ] || fail "expected three thermal trips, found $trip_count"
passive_trip=$(find_trip passive 85000 5000)
emergency_trip=$(find_trip passive 95000 5000)
critical_trip=$(find_trip critical 117000 0)
check_cooling_device cpufreq-cpu0
check_cooling_device cpufreq-cpu4

bindings=
binding_count=0
for trip_file in "$zone"/cdev*_trip_point; do
	[ -r "$trip_file" ] || continue
	cdev=${trip_file%_trip_point}
	bindings="${bindings}$(read_one "$cdev/type"):$(read_one "$trip_file")\n"
	binding_count=$((binding_count + 1))
done
[ "$binding_count" -eq 4 ] || fail "expected four cooling-map bindings"
bindings=$(printf '%b' "$bindings" | sort)
expected=$(printf '%s\n' \
	"cpufreq-cpu0:$passive_trip" "cpufreq-cpu0:$emergency_trip" \
	"cpufreq-cpu4:$passive_trip" "cpufreq-cpu4:$emergency_trip" | sort)
[ "$bindings" = "$expected" ] || fail "cooling-map bindings are incorrect"

if [ -n "$kernel_log" ]; then
	[ -r "$kernel_log" ] || fail "kernel log is unreadable"
	log_text=$(sed -n '1,$p' "$kernel_log")
else
	log_text=$(dmesg) || fail "cannot read kernel log"
fi
error_pattern='(mtk-cpufreq|clk-mt6797-cpu|mtk-thermal|soc-thermal|da9214).*'
error_pattern="${error_pattern}(fail|error|timeout|rollback|uncertain)"
error_pattern="${error_pattern}|(mtk-cpufreq|clk-mt6797-cpu|mtk-thermal).*"
error_pattern="${error_pattern}(invalid|fenc|EUCLEAN)"
error_pattern="${error_pattern}|Device not calibrated|required calibration"
errors=$(printf '%s\n' "$log_text" | grep -Ei "$error_pattern" || true)
[ -z "$errors" ] || {
	printf '%s\n' "$errors" >&2
	fail "CPU DVFS or thermal error found in kernel log"
}

echo "schema=gemini-mt6797-a53-dvfs-thermal-preflight-v2"
echo "source_identity=$source_identity"
echo "kernel_release=$kernel_release"
echo "collector_mode=read_only"
echo "frequency_transition_available=no"
echo "frequency_transition_evidence=not_exercised"
echo "cooling_action_evidence=not_exercised"
echo "critical_shutdown_evidence=dt_policy_only"
echo "calibration_gate=driver_bound_requires_valid_nvmem"
echo "temperature_millic=$temperature"
echo "cooling_devices=2"
echo "cooling_bindings=4"
echo "a72_touched=no"
echo "result=pass"
