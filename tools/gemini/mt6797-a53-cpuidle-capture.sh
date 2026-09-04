#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Read-only qualification capture for the Gemini A53 PSCI cpuidle candidate.
# It observes natural idle entries; it never changes a state, governor, CPU,
# clock, regulator, device-tree property, or file.

set -eu

cpu_root=${CPU_ROOT:-/sys/devices/system/cpu}
dt_root=${DT_ROOT:-/sys/firmware/devicetree/base}
kernel_log=${KERNEL_LOG:-}
capture_seconds=${CAPTURE_SECONDS:-5}

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
	value_name=$1
	value=$2

	case "$value" in
	''|*[!0-9]*) fail "$value_name is not unsigned: $value" ;;
	esac
}

read_be32_hex()
{
	be32_path=$1

	[ -r "$be32_path" ] || return 1
	value=$(od -An -tx1 -v "$be32_path" | tr -d ' \n')
	[ "${#value}" -eq 8 ] || return 1
	printf '%s\n' "$value"
}

cpu_dt_name()
{
	case "$1" in
	0|1|2|3) printf 'cpu@%s\n' "$1" ;;
	4) printf 'cpu@100\n' ;;
	5) printf 'cpu@101\n' ;;
	6) printf 'cpu@102\n' ;;
	7) printf 'cpu@103\n' ;;
	*) return 1 ;;
	esac
}

check_value()
{
	value_path=$1
	expected=$2
	value_name=$3

	value=$(read_one "$value_path") || fail "$value_name is unreadable"
	[ "$value" = "$expected" ] ||
		fail "$value_name is $value, expected $expected"
}

check_cpu_states()
{
	cpu=$1
	idle_path=$cpu_root/cpu$cpu/cpuidle

	[ -d "$idle_path/state0" ] || fail "CPU$cpu WFI state is absent"
	[ -d "$idle_path/state1" ] || fail "CPU$cpu powerdown state is absent"
	[ ! -e "$idle_path/state2" ] || fail "CPU$cpu exposes an unqualified deeper state"
	check_value "$idle_path/state0/name" WFI "CPU$cpu state0 name"
	check_value "$idle_path/state1/name" cpu-sleep-a53 "CPU$cpu state1 name"
	check_value "$idle_path/state1/latency" 1200 "CPU$cpu wake latency"
	check_value "$idle_path/state1/residency" 1200 "CPU$cpu target residency"
	check_value "$idle_path/state1/default_status" enabled \
		"CPU$cpu state1 default status"
	check_value "$idle_path/state1/disable" 0 "CPU$cpu state1 disable"

	cpu_usage=$(read_one "$idle_path/state1/usage") ||
		fail "CPU$cpu state1 usage is unreadable"
	cpu_time=$(read_one "$idle_path/state1/time") ||
		fail "CPU$cpu state1 time is unreadable"
	cpu_rejected=$(read_one "$idle_path/state1/rejected") ||
		fail "CPU$cpu state1 rejected count is unreadable"
	require_unsigned "CPU$cpu state1 usage" "$cpu_usage"
	require_unsigned "CPU$cpu state1 time" "$cpu_time"
	require_unsigned "CPU$cpu state1 rejected count" "$cpu_rejected"
}

check_kernel_log()
{
	if [ -n "$kernel_log" ]; then
		[ -r "$kernel_log" ] || fail "kernel log is unreadable: $kernel_log"
		log_text=$(sed -n '1,$p' "$kernel_log")
	else
		log_text=$(dmesg) || fail "cannot read kernel log"
	fi

	printf '%s\n' "$log_text" | grep -Fq 'PSCIv0.2 detected in firmware.' ||
		fail "PSCI v0.2 firmware evidence is absent"
	printf '%s\n' "$log_text" | grep -Fq \
		'Using standard PSCI v0.2 function IDs' ||
		fail "standard PSCI v0.2 function-ID evidence is absent"
	error_pattern='failed to PSCI idle|Invalid PSCI power state|'
	error_pattern="${error_pattern}CPU_SUSPEND.*(fail|error|denied|invalid)|"
	error_pattern="${error_pattern}cpuidle.*(fail|error)"
	idle_errors=$(printf '%s\n' "$log_text" | grep -Ei "$error_pattern" || true)
	[ -z "$idle_errors" ] || {
		printf '%s\n' "$idle_errors" >&2
		fail "PSCI or cpuidle error found in kernel log"
	}
}

case "$capture_seconds" in
''|*[!0-9]*) fail "CAPTURE_SECONDS is not unsigned: $capture_seconds" ;;
esac
[ "$capture_seconds" -ge 1 ] && [ "$capture_seconds" -le 30 ] ||
	fail "CAPTURE_SECONDS must be between 1 and 30"

[ -d "$cpu_root" ] || fail "CPU sysfs root is absent: $cpu_root"
[ -d "$dt_root/cpus" ] || fail "CPU device-tree root is absent"
online=$(read_one "$cpu_root/online") || fail "online CPU list is unreadable"
[ "$online" = "0-7" ] || fail "online CPU list is $online, expected 0-7"

check_value "$cpu_root/cpuidle/current_driver" psci_idle "cpuidle driver"
if [ -r "$cpu_root/cpuidle/current_governor_ro" ]; then
	governor=$(read_one "$cpu_root/cpuidle/current_governor_ro") ||
		fail "cpuidle governor is unreadable"
else
	governor=$(read_one "$cpu_root/cpuidle/current_governor") ||
		fail "cpuidle governor is unreadable"
fi

method=$(tr -d '\000' < "$dt_root/psci/method") || fail "PSCI method is unreadable"
[ "$method" = smc ] || fail "PSCI method is $method, expected smc"
state_dt=$dt_root/cpus/idle-states/cpu-sleep-a53
[ -d "$state_dt" ] || fail "A53 powerdown DT state is absent"
state_count=$(find "$dt_root/cpus/idle-states" -mindepth 1 -maxdepth 1 \
	-type d | wc -l | tr -d ' ')
[ "$state_count" -eq 1 ] ||
	fail "expected one CPU idle DT state, found $state_count"
[ "$(read_be32_hex "$state_dt/arm,psci-suspend-param")" = 00010000 ] ||
	fail "A53 PSCI suspend parameter is not 0x00010000"
[ "$(read_be32_hex "$state_dt/entry-latency-us")" = 00000258 ] ||
	fail "A53 entry latency is not 600us"
[ "$(read_be32_hex "$state_dt/exit-latency-us")" = 00000258 ] ||
	fail "A53 exit latency is not 600us"
[ "$(read_be32_hex "$state_dt/min-residency-us")" = 000004b0 ] ||
	fail "A53 minimum residency is not 1200us"

before=
for cpu in 0 1 2 3 4 5 6 7; do
	dt_name=$(cpu_dt_name "$cpu")
	cmp -s "$dt_root/cpus/$dt_name/cpu-idle-states" "$state_dt/phandle" ||
		fail "CPU$cpu does not reference only the A53 powerdown state"
	check_cpu_states "$cpu"
	before="${before}${cpu} ${cpu_usage} ${cpu_time} ${cpu_rejected}\n"
done
[ ! -e "$dt_root/cpus/cpu@200/cpu-idle-states" ] ||
	fail "CPU8 unexpectedly references an idle state"
[ ! -e "$dt_root/cpus/cpu@201/cpu-idle-states" ] ||
	fail "CPU9 unexpectedly references an idle state"

sleep "$capture_seconds"

for cpu in 0 1 2 3 4 5 6 7; do
	set -- $(printf '%b' "$before" | awk -v cpu="$cpu" '$1 == cpu { print $2, $3, $4 }')
	[ "$#" -eq 3 ] || fail "CPU$cpu initial counters are missing"
	before_usage=$1
	before_time=$2
	before_rejected=$3
	check_cpu_states "$cpu"
	[ "$cpu_usage" -gt "$before_usage" ] ||
		fail "CPU$cpu did not enter core powerdown"
	[ "$cpu_time" -gt "$before_time" ] ||
		fail "CPU$cpu core powerdown time did not advance"
	[ "$cpu_rejected" -eq "$before_rejected" ] ||
		fail "CPU$cpu core powerdown rejection count changed"
	printf 'cpu%s_powerdown_entries_delta=%s\n' "$cpu" \
		"$((cpu_usage - before_usage))"
	printf 'cpu%s_powerdown_time_delta_us=%s\n' "$cpu" \
		"$((cpu_time - before_time))"
done

check_kernel_log
echo "schema=gemini-mt6797-a53-cpuidle-capture-v1"
echo "collector_mode=read_only"
echo "cpuidle_driver=psci_idle"
echo "cpuidle_governor=$governor"
echo "psci_method=smc"
echo "psci_suspend_param=0x00010000"
echo "cluster_state_present=no"
echo "a72_online=no"
echo "result=pass"
