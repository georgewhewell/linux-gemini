#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Read-only acceptance capture for Gemini A53 scheduler topology. It also
# proves that CPU capacity and the energy model remain unasserted. It never
# changes affinity, scheduling policy, frequency, CPU state, DT, or a file.

set -eu

cpu_root=${CPU_ROOT:-/sys/devices/system/cpu}
dt_root=${DT_ROOT:-/sys/firmware/devicetree/base}
kernel_log=${KERNEL_LOG:-}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

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

cpu_dt_name()
{
	case "$1" in
	0|1|2|3) printf 'cpu@%s\n' "$1" ;;
	4) printf 'cpu@100\n' ;;
	5) printf 'cpu@101\n' ;;
	6) printf 'cpu@102\n' ;;
	7) printf 'cpu@103\n' ;;
	8) printf 'cpu@200\n' ;;
	9) printf 'cpu@201\n' ;;
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

check_dt_cpu()
{
	cpu=$1
	cluster=$2
	core=$3
	dt_name=$(cpu_dt_name "$cpu")
	cpu_dt=$dt_root/cpus/$dt_name
	map_core=$dt_root/cpus/cpu-map/cluster$cluster/core$core

	[ -d "$cpu_dt" ] || fail "CPU$cpu DT node is absent"
	[ -d "$map_core" ] || fail "CPU$cpu topology map node is absent"
	cmp -s "$map_core/cpu" "$cpu_dt/phandle" ||
		fail "CPU$cpu topology phandle is incorrect"
	[ ! -e "$cpu_dt/capacity-dmips-mhz" ] ||
		fail "CPU$cpu unexpectedly asserts capacity-dmips-mhz"
}

check_live_cpu()
{
	cpu=$1
	cluster=$2
	core=$3
	topology=$cpu_root/cpu$cpu/topology

	check_value "$topology/physical_package_id" 0 "CPU$cpu package ID"
	check_value "$topology/cluster_id" "$cluster" "CPU$cpu cluster ID"
	check_value "$topology/core_id" "$core" "CPU$cpu core ID"
	check_value "$topology/thread_siblings_list" "$cpu" \
		"CPU$cpu thread siblings"
	check_value "$topology/core_siblings_list" 0-7 "CPU$cpu package siblings"
	if [ "$cluster" -eq 0 ]; then
		cluster_cpus=0-3
	else
		cluster_cpus=4-7
	fi
	check_value "$topology/cluster_cpus_list" "$cluster_cpus" \
		"CPU$cpu cluster siblings"
}

check_kernel_log()
{
	if [ -n "$kernel_log" ]; then
		[ -r "$kernel_log" ] || fail "kernel log is unreadable: $kernel_log"
		log_text=$(sed -n '1,$p' "$kernel_log")
	else
		log_text=$(dmesg) || fail "cannot read kernel log"
	fi

	error_pattern='Failed to parse device tree CPU topology|'
	error_pattern="${error_pattern}cpu-map.*(fail|error|invalid)|arch topology borken"
	topology_errors=$(printf '%s\n' "$log_text" | grep -Ei "$error_pattern" || true)
	[ -z "$topology_errors" ] || {
		printf '%s\n' "$topology_errors" >&2
		fail "scheduler topology or capacity error found in kernel log"
	}
}

if ! dvfs_capture=$("$script_dir/mt6797-a53-dvfs-capture.sh"); then
	printf '%s\n' "$dvfs_capture" >&2
	fail "A53 DVFS prerequisite capture failed"
fi
printf '%s\n' "$dvfs_capture"

[ -d "$cpu_root" ] || fail "CPU sysfs root is absent: $cpu_root"
[ -d "$dt_root/cpus/cpu-map" ] || fail "CPU topology DT map is absent"
check_value "$cpu_root/online" 0-7 "online CPU list"
possible=$(read_one "$cpu_root/possible") || fail "possible CPU list is unreadable"
present=$(read_one "$cpu_root/present") || fail "present CPU list is unreadable"
case "$possible" in
0-7|0-9) ;;
*) fail "possible CPU list is $possible, expected 0-7 or 0-9" ;;
esac
case "$present" in
0-7|0-9) ;;
*) fail "present CPU list is $present, expected 0-7 or 0-9" ;;
esac

for cpu in 0 1 2 3; do
	check_dt_cpu "$cpu" 0 "$cpu"
	check_live_cpu "$cpu" 0 "$cpu"
done
for cpu in 4 5 6 7; do
	core=$((cpu - 4))
	check_dt_cpu "$cpu" 1 "$core"
	check_live_cpu "$cpu" 1 "$core"
done
check_dt_cpu 8 2 0
check_dt_cpu 9 2 1
check_value "$dt_root/cpus/cpu@200/status" disabled "CPU8 DT status"
check_value "$dt_root/cpus/cpu@201/status" disabled "CPU9 DT status"
[ ! -d "$cpu_root/cpu8/cpufreq" ] || fail "CPU8 cpufreq policy is live"
[ ! -d "$cpu_root/cpu9/cpufreq" ] || fail "CPU9 cpufreq policy is live"

check_kernel_log
echo "schema=gemini-mt6797-a53-scheduler-capture-v1"
echo "collector_mode=read_only"
echo "possible_cpus=$possible"
echo "present_cpus=$present"
echo "online_cpus=0-7"
echo "capacity_dmips_mhz_asserted=no"
echo "energy_model_asserted=no"
echo "a72_live=no"
echo "result=pass"
