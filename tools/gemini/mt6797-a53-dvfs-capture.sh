#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Read-only acceptance capture for the Gemini MT6797 A53 DVFS candidate.
# This script reads existing sysfs/debugfs/log state only.  It never changes a
# governor, frequency, clock, regulator, module, device-tree property or file.

set -eu

cpufreq_root=${CPUFREQ_ROOT:-/sys/devices/system/cpu/cpufreq}
regulator_summary=${REGULATOR_SUMMARY:-/sys/kernel/debug/regulator/regulator_summary}
clock_summary=${CLOCK_SUMMARY:-/sys/kernel/debug/clk/clk_summary}
kernel_log=${KERNEL_LOG:-}

mp0_expected="624000 715000 806000 897000 1014000 1118000 1222000 1274000 1339000 1391000"
mp1_expected="650000 832000 962000 1092000 1209000 1352000 1495000 1625000 1703000 1755000 1820000 1885000 1950000"

fail()
{
	echo "result=fail"
	echo "reason=$*" >&2
	exit 1
}

read_one_line()
{
	read_path=$1

	[ -r "$read_path" ] || return 1
	tr '\n' ' ' < "$read_path" | sed 's/[[:space:]][[:space:]]*/ /g; s/^ //; s/ $//'
}

normalize_numbers()
{
	tr ' ' '\n' | sed '/^$/d' | sort -n | paste -sd ' ' -
}

number_in_list()
{
	needle=$1
	haystack=$2

	case " $haystack " in
	*" $needle "*) return 0 ;;
	*) return 1 ;;
	esac
}

require_unsigned()
{
	unsigned_name=$1
	unsigned_value=$2

	case "$unsigned_value" in
	''|*[!0-9]*) fail "$unsigned_name is not an unsigned integer: $unsigned_value" ;;
	esac
}

check_policy()
{
	policy_name=$1
	expected_cpus=$2
	expected_rates=$3
	policy_path=$cpufreq_root/$policy_name

	[ -d "$policy_path" ] || fail "missing cpufreq policy: $policy_name"

	if ! affected_cpus=$(read_one_line "$policy_path/affected_cpus"); then
		fail "missing readable state: $policy_path/affected_cpus"
	fi
	affected_cpus=$(printf '%s\n' "$affected_cpus" | normalize_numbers)
	[ "$affected_cpus" = "$expected_cpus" ] ||
		fail "$policy_name affected_cpus is '$affected_cpus', expected '$expected_cpus'"

	if ! driver=$(read_one_line "$policy_path/scaling_driver"); then
		fail "missing readable state: $policy_path/scaling_driver"
	fi
	[ "$driver" = "mtk-cpufreq" ] ||
		fail "$policy_name driver is '$driver', expected 'mtk-cpufreq'"

	if ! governor=$(read_one_line "$policy_path/scaling_governor"); then
		fail "missing readable state: $policy_path/scaling_governor"
	fi
	[ -n "$governor" ] || fail "$policy_name has no governor"

	if ! available=$(read_one_line "$policy_path/scaling_available_frequencies"); then
		fail "missing readable state: $policy_path/scaling_available_frequencies"
	fi
	available=$(printf '%s\n' "$available" | normalize_numbers)
	[ "$available" = "$expected_rates" ] ||
		fail "$policy_name available rates are '$available', expected '$expected_rates'"

	if ! scaling_khz=$(read_one_line "$policy_path/scaling_cur_freq"); then
		fail "missing readable state: $policy_path/scaling_cur_freq"
	fi
	if ! actual_khz=$(read_one_line "$policy_path/cpuinfo_cur_freq"); then
		fail "missing readable state: $policy_path/cpuinfo_cur_freq"
	fi
	require_unsigned "$policy_name scaling_cur_freq" "$scaling_khz"
	require_unsigned "$policy_name cpuinfo_cur_freq" "$actual_khz"
	number_in_list "$scaling_khz" "$expected_rates" ||
		fail "$policy_name requested rate $scaling_khz is outside its OPP set"
	number_in_list "$actual_khz" "$expected_rates" ||
		fail "$policy_name actual rate $actual_khz is outside its OPP set"

	printf '%s_affected_cpus=%s\n' "$policy_name" "$affected_cpus"
	printf '%s_driver=%s\n' "$policy_name" "$driver"
	printf '%s_governor=%s\n' "$policy_name" "$governor"
	printf '%s_available_khz=%s\n' "$policy_name" "$available"
	printf '%s_requested_khz=%s\n' "$policy_name" "$scaling_khz"
	printf '%s_actual_khz=%s\n' "$policy_name" "$actual_khz"
}

clock_rate_hz()
{
	clock_name=$1

	awk -v name="$clock_name" '
		$1 == name {
			if (seen++)
				exit 2
			rate = $5
		}
		END {
			if (seen != 1)
				exit 1
			print rate
		}
	' "$clock_summary"
}

check_clock()
{
	clock_name=$1
	expected_rates=$2
	if ! rate_hz=$(clock_rate_hz "$clock_name"); then
		fail "missing or duplicate clock: $clock_name"
	fi
	require_unsigned "$clock_name clock rate" "$rate_hz"
	[ "$rate_hz" -gt 0 ] || fail "$clock_name clock rate is zero"
	[ $((rate_hz % 1000)) -eq 0 ] ||
		fail "$clock_name clock rate is not an integral kHz value: $rate_hz"
	rate_khz=$((rate_hz / 1000))
	number_in_list "$rate_khz" "$expected_rates" ||
		fail "$clock_name clock rate $rate_hz is outside its protected plan"
	printf 'clock_%s_hz=%s\n' "$clock_name" "$rate_hz"
}

regulator_voltage_mv()
{
	regulator_name=$1

	awk -v name="$regulator_name" '
		$1 == name {
			if (seen++)
				exit 2
			voltage = $6
		}
		END {
			if (seen != 1 || voltage !~ /^[0-9]+mV$/)
				exit 1
			sub(/mV$/, "", voltage)
			print voltage
		}
	' "$regulator_summary"
}

check_kernel_log()
{
	if [ -n "$kernel_log" ]; then
		[ -r "$kernel_log" ] || fail "kernel log is unreadable: $kernel_log"
		log_text=$(sed -n '1,$p' "$kernel_log")
	else
		log_text=$(dmesg) || fail "cannot read kernel log"
	fi

	log_errors=$(printf '%s\n' "$log_text" | grep -Ei \
		'(mtk-cpufreq|clk-mt6797-cpu|mt6797_(mp0|mp1)|da9214|vproc-little|vsram-proc).*(fail|error|timeout|rollback|uncertain|invalid|mismatch|fenced|EUCLEAN)|'\
'(fail|error|timeout|rollback|uncertain|invalid|mismatch|fenced|EUCLEAN).*(mtk-cpufreq|clk-mt6797-cpu|mt6797_(mp0|mp1)|da9214|vproc-little|vsram-proc)|'\
'failed to (find OPP|get (cpu|intermediate) clk|get (proc|sram) regulator|enable (vproc|vsram|cpu clk|inter clk)|get OPP-sharing|scale (up|down) voltage|re-parent cpu clock|scale cpu clock rate|scale voltage)|'\
'invalid (Vproc|Vsram|boot clock rate|boot OPP voltage)|shared (rail|boot)|over loop count|all opp items are disabled|dvfs info.*not initialized' || true)
	if [ -n "$log_errors" ]; then
		printf '%s\n' "$log_errors" >&2
		fail "relevant cpufreq, clock or rail error found in kernel log"
	fi

	echo "kernel_log_errors=none"
}

[ -d "$cpufreq_root" ] || fail "missing cpufreq root: $cpufreq_root"
[ -r "$regulator_summary" ] ||
	fail "missing regulator summary: $regulator_summary"
[ -r "$clock_summary" ] || fail "missing clock summary: $clock_summary"
[ ! -d "$cpufreq_root/policy8" ] || fail "A72 policy8 must remain absent"

echo "schema=gemini-mt6797-a53-dvfs-capture-v1"
echo "collector_mode=read_only"
echo "sysfs_debugfs_writes=no"
echo "frequency_transition_requested=no"
echo "regulator_change_requested=no"

check_policy policy0 "0 1 2 3" "$mp0_expected"
check_policy policy4 "4 5 6 7" "$mp1_expected"
check_clock mt6797_mp0 "$mp0_expected"
check_clock mt6797_mp0_armpll "$mp0_expected"
check_clock mt6797_mp1 "$mp1_expected"
check_clock mt6797_mp1_armpll "$mp1_expected"

if ! vproc_mv=$(regulator_voltage_mv vproc-little); then
	fail "missing, duplicate or invalid regulator: vproc-little"
fi
if ! vsram_mv=$(regulator_voltage_mv vsram-proc); then
	fail "missing, duplicate or invalid regulator: vsram-proc"
fi
if awk '$1 == "vproc-big" || $1 == "vsram-big" { found = 1 }
	 END { exit !found }' "$regulator_summary"; then
	fail "A72 Vproc or secure SRAM regulator must remain absent"
fi
require_unsigned "vproc-little voltage" "$vproc_mv"
require_unsigned "vsram-proc voltage" "$vsram_mv"
[ "$vproc_mv" -ge 770 ] && [ "$vproc_mv" -le 1200 ] ||
	fail "vproc-little is outside 770-1200mV: ${vproc_mv}mV"
[ "$vsram_mv" -ge 1000 ] && [ "$vsram_mv" -le 1200 ] ||
	fail "vsram-proc is outside 1000-1200mV: ${vsram_mv}mV"
[ "$vsram_mv" -ge "$vproc_mv" ] ||
	fail "vsram-proc is below vproc-little: ${vsram_mv}mV < ${vproc_mv}mV"
printf 'vproc_little_mv=%s\n' "$vproc_mv"
printf 'vsram_proc_mv=%s\n' "$vsram_mv"

check_kernel_log
echo "a72_policy_present=no"
echo "result=pass"
