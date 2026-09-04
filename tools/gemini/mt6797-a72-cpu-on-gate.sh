#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Read-only acceptance capture for the Gemini Cortex-A72 PSCI CPU_ON gate.
# It does not request CPU_ON or write sysfs, debugfs, firmware, clock, reset or
# regulator state.

set -eu

cpu_root=${CPU_ROOT:-/sys/devices/system/cpu}
dt_root=${DT_ROOT:-/sys/firmware/devicetree/base}
cmdline_path=${CMDLINE_PATH:-/proc/cmdline}
kernel_log=${KERNEL_LOG:-}
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
preflight_helper=${PREFLIGHT_HELPER:-$script_dir/mt6797-a72-resource-preflight.sh}

fail()
{
	echo "result=fail"
	echo "reason=$*" >&2
	exit 1
}

read_trimmed()
{
	trimmed_path=$1

	[ -r "$trimmed_path" ] || return 1
	tr '\000\n' '  ' < "$trimmed_path" |
		sed 's/[[:space:]][[:space:]]*/ /g; s/^ //; s/ $//'
}

cpulist_contains()
{
	target_cpu=$1
	cpulist_path=$2

	awk -F, -v target="$target_cpu" '
		{
			for (i = 1; i <= NF; i++) {
				n = split($i, range, "-")
				first = range[1] + 0
				last = n == 2 ? range[2] + 0 : first
				if (target >= first && target <= last)
					found = 1
			}
		}
		END { exit !found }
	' "$cpulist_path"
}

[ -x "$preflight_helper" ] || fail "resource preflight helper is unavailable"
if ! preflight_output=$(EXPECTED_CPU8_STATUS=okay "$preflight_helper"); then
	printf '%s\n' "$preflight_output" >&2
	fail "resource preflight did not pass"
fi
printf '%s\n' "$preflight_output" | grep -qx 'result=pass' ||
	fail "resource preflight emitted no pass result"

if ! cmdline=$(read_trimmed "$cmdline_path"); then
	fail "cannot read kernel command line"
fi
maxcpus_count=0
for argument in $cmdline; do
	case "$argument" in
	maxcpus=8) maxcpus_count=$((maxcpus_count + 1)) ;;
	maxcpus=*) fail "unexpected CPU boot limit: $argument" ;;
	esac
done
[ "$maxcpus_count" -eq 1 ] || fail "expected exactly one maxcpus=8 fence"

cpulist_contains 4 "$cpu_root/online" ||
	fail "CPU4 prerequisite is not online"

for cpu in 8 9; do
	case "$cpu" in
	8) unit=200 ;;
	9) unit=201 ;;
	esac
	cpu_path=$dt_root/cpus/cpu@$unit

	if ! method=$(read_trimmed "$cpu_path/enable-method"); then
		fail "missing CPU$cpu enable-method"
	fi
	[ "$method" = "mediatek,mt6797-psci" ] ||
		fail "CPU$cpu enable-method is $method"
	if ! status=$(read_trimmed "$cpu_path/status"); then
		fail "missing CPU$cpu DT status"
	fi
	case "$cpu:$status" in
	8:okay|9:disabled) ;;
	*) fail "CPU$cpu DT status is $status" ;;
	esac
	[ ! -e "$cpu_path/capacity-dmips-mhz" ] ||
		fail "CPU$cpu has an unqualified static capacity"
	[ ! -e "$cpu_path/operating-points-v2" ] ||
		fail "CPU$cpu has an enabled OPP contract"
	cpulist_contains "$cpu" "$cpu_root/possible" ||
		fail "CPU$cpu is not possible"
	cpulist_contains "$cpu" "$cpu_root/present" ||
		fail "CPU$cpu is not present"
	cpulist_contains "$cpu" "$cpu_root/online" &&
		fail "CPU$cpu is online"
done

if [ -n "$kernel_log" ]; then
	[ -r "$kernel_log" ] || fail "kernel log is unreadable: $kernel_log"
	log_text=$(sed -n '1,$p' "$kernel_log")
else
	log_text=$(dmesg) || fail "cannot read kernel log"
fi

gate_count=$(printf '%s\n' "$log_text" |
	grep -Fc 'cpu-on-gate-v3:' || true)
[ "$gate_count" -eq 1 ] ||
	fail "expected one CPU_ON gate transcript, found $gate_count"
gate_line=$(printf '%s\n' "$log_text" | grep -F 'cpu-on-gate-v3:')
case "$gate_line" in
*"method=mediatek,mt6797-psci"*\
*"CPU8-mpidr=0x200 CPU9-mpidr=0x201"*\
*"affinity8=off affinity9=off"*\
*"CPU4-prerequisite=required CPU8-r0p1=required"*\
*"CPU8-admission=ready CPU9=blocked"*) ;;
*) fail "CPU_ON gate transcript is incomplete" ;;
esac
printf '%s\n' "$gate_line" | grep -Eq 'PSCI=[0-9]+\.[0-9]+' ||
	fail "CPU_ON gate has no PSCI version"
printf '%s\n' "$gate_line" |
	grep -Eq 'prepare-state=[0-9]+ commit-state=[0-9]+' ||
	fail "CPU_ON gate has incomplete hotplug states"

bringup_pattern='CPU(8|9):.*(failed|stuck)|CPU(8|9).*failed to come online'
bringup_pattern="$bringup_pattern|CPU(8|9) boot rejected"
bringup_errors=$(printf '%s\n' "$log_text" |
	grep -E "$bringup_pattern" || true)
[ -z "$bringup_errors" ] || {
	printf '%s\n' "$bringup_errors" >&2
	fail "an A72 bring-up attempt reached an error path"
}

echo "schema=gemini-mt6797-a72-cpu-on-gate-v3"
echo "collector_mode=read_only"
echo "boot_cpu_limit=8"
echo "cpu8_mpidr=0x200"
echo "cpu9_mpidr=0x201"
echo "psci_affinity=off"
echo "cpu4_prerequisite=online"
echo "resource_admission=ready"
echo "cpu_on_requested=no"
echo "cpu8_gate=ready"
echo "cpu9_gate=blocked"
echo "persistent_writes=no"
echo "result=pass"
