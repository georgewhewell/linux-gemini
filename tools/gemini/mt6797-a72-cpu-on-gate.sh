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
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
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
if ! preflight_output=$($preflight_helper); then
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

for cpu in 8 9; do
	case "$cpu" in
	8) unit=200 ;;
	9) unit=201 ;;
	esac
	cpu_path=$dt_root/cpus/cpu@$unit

	if ! method=$(read_trimmed "$cpu_path/enable-method"); then
		fail "missing CPU$cpu enable-method"
	fi
	[ "$method" = "psci" ] || fail "CPU$cpu enable-method is $method"
	if ! status=$(read_trimmed "$cpu_path/status"); then
		fail "missing CPU$cpu DT status"
	fi
	[ "$status" = "disabled" ] || fail "CPU$cpu DT status is $status"
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
	grep -Fc 'cpu-on-gate-v1:' || true)
[ "$gate_count" -eq 1 ] ||
	fail "expected one CPU_ON gate transcript, found $gate_count"
gate_line=$(printf '%s\n' "$log_text" | grep -F 'cpu-on-gate-v1:')
case "$gate_line" in
*"CPU8-mpidr=0x200 CPU9-mpidr=0x201"*\
*"affinity8=off affinity9=off"*\
*"admission=-EOPNOTSUPP CPU_ON=blocked"*) ;;
*) fail "CPU_ON gate transcript is incomplete" ;;
esac
printf '%s\n' "$gate_line" | grep -Eq 'PSCI=[0-9]+\.[0-9]+' ||
	fail "CPU_ON gate has no PSCI version"
printf '%s\n' "$gate_line" | grep -Eq 'hp-state=[0-9]+' ||
	fail "CPU_ON gate has no hotplug state"

bringup_errors=$(printf '%s\n' "$log_text" |
	grep -E 'CPU(8|9):.*(failed|stuck)|CPU(8|9).*failed to come online' || true)
[ -z "$bringup_errors" ] || {
	printf '%s\n' "$bringup_errors" >&2
	fail "an A72 bring-up attempt reached an error path"
}

echo "schema=gemini-mt6797-a72-cpu-on-gate-v1"
echo "collector_mode=read_only"
echo "boot_cpu_limit=8"
echo "cpu8_mpidr=0x200"
echo "cpu9_mpidr=0x201"
echo "psci_affinity=off"
echo "resource_admission=EOPNOTSUPP"
echo "cpu_on_requested=no"
echo "cpu_on_gate=blocked"
echo "persistent_writes=no"
echo "result=pass"
