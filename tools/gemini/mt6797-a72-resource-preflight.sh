#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Read-only acceptance capture for the Gemini Cortex-A72 resource preflight.
# It never requests CPU_ON or writes sysfs, debugfs, firmware, clock, reset or
# regulator state.

set -eu

cpu_root=${CPU_ROOT:-/sys/devices/system/cpu}
cpufreq_root=${CPUFREQ_ROOT:-$cpu_root/cpufreq}
dt_root=${DT_ROOT:-/sys/firmware/devicetree/base}
driver_root=${DRIVER_ROOT:-/sys/bus/platform/drivers/mt6797-a72-preflight}
regulator_summary=${REGULATOR_SUMMARY:-/sys/kernel/debug/regulator/regulator_summary}
clock_summary=${CLOCK_SUMMARY:-/sys/kernel/debug/clk/clk_summary}
cmdline_path=${CMDLINE_PATH:-/proc/cmdline}
kernel_log=${KERNEL_LOG:-}
sram_voltages=" 600000 700000 900000 925000 950000 975000 1000000 1025000 1050000 1075000 1100000 1125000 1150000 1175000 1200000 "

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

require_unsigned()
{
	unsigned_name=$1
	unsigned_value=$2

	case "$unsigned_value" in
	''|*[!0-9]*) fail "$unsigned_name is not an unsigned integer: $unsigned_value" ;;
	esac
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

extract_field()
{
	field_line=$1
	field_name=$2

	printf '%s\n' "$field_line" | awk -v key="$field_name" '
		{
			for (i = 1; i <= NF; i++) {
				n = split($i, pair, "=")
				if (n == 2 && pair[1] == key) {
					value = pair[2]
					seen++
				}
			}
		}
		END {
			if (seen != 1)
				exit 1
			print value
		}
	'
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

[ -r "$cpu_root/online" ] || fail "missing CPU online mask"
cpulist_contains 8 "$cpu_root/online" && fail "CPU8 is online"
cpulist_contains 9 "$cpu_root/online" && fail "CPU9 is online"
[ ! -d "$cpufreq_root/policy8" ] || fail "A72 cpufreq policy is present"
[ ! -d "$cpufreq_root/policy0" ] || fail "A53 policy0 is active in read-only preflight"
[ ! -d "$cpufreq_root/policy4" ] || fail "A53 policy4 is active in read-only preflight"

if ! cpu8_status=$(read_trimmed "$dt_root/cpus/cpu@200/status"); then
	fail "missing CPU8 DT status"
fi
if ! cpu9_status=$(read_trimmed "$dt_root/cpus/cpu@201/status"); then
	fail "missing CPU9 DT status"
fi
[ "$cpu8_status" = "disabled" ] || fail "CPU8 DT status is $cpu8_status"
[ "$cpu9_status" = "disabled" ] || fail "CPU9 DT status is $cpu9_status"

if ! cmdline=$(read_trimmed "$cmdline_path"); then
	fail "cannot read kernel command line"
fi
case " $cmdline " in
*" regulator_ignore_unused "*) ;;
*) fail "regulator_ignore_unused safety fence is absent" ;;
esac

[ -d "$driver_root" ] || fail "A72 preflight driver is not registered"
bound_count=$(find "$driver_root" -maxdepth 1 -type l ! -name module | wc -l |
	tr -d '[:space:]')
[ "$bound_count" = "1" ] ||
	fail "expected one bound A72 preflight device, found $bound_count"
[ -r "$regulator_summary" ] || fail "missing regulator summary"
[ -r "$clock_summary" ] || fail "missing clock summary"

if [ -n "$kernel_log" ]; then
	[ -r "$kernel_log" ] || fail "kernel log is unreadable: $kernel_log"
	log_text=$(sed -n '1,$p' "$kernel_log")
else
	log_text=$(dmesg) || fail "cannot read kernel log"
fi

identity_count=$(printf '%s\n' "$log_text" |
	grep -Fc 'DA9214 direct-address regulators registered' || true)
[ "$identity_count" = "1" ] ||
	fail "expected one successful DA9214 identity transcript, found $identity_count"

sram_provider_count=$(printf '%s\n' "$log_text" |
	grep -Fc 'voltage read/set ready; enable, status, and disable unsupported' || true)
[ "$sram_provider_count" = "1" ] ||
	fail "secure SRAM provider did not report its fail-closed lifecycle"

preflight_count=$(printf '%s\n' "$log_text" |
	grep -Fc 'resource-preflight-v1:' || true)
[ "$preflight_count" = "1" ] ||
	fail "expected one complete A72 resource snapshot, found $preflight_count"
preflight_line=$(printf '%s\n' "$log_text" |
	grep -F 'resource-preflight-v1:')
case "$preflight_line" in
*"CPU8/9 resource-preflight-v1:"*) ;;
*) fail "preflight did not resolve CPU8/9" ;;
esac

if ! offline=$(extract_field "$preflight_line" offline); then
	fail "missing or duplicate preflight field: offline"
fi
if ! vproc_field=$(extract_field "$preflight_line" vproc); then
	fail "missing or duplicate preflight field: vproc"
fi
if ! vproc_enabled=$(extract_field "$preflight_line" vproc-enabled); then
	fail "missing or duplicate preflight field: vproc-enabled"
fi
if ! sram_field=$(extract_field "$preflight_line" sram); then
	fail "missing or duplicate preflight field: sram"
fi
if ! sram_status=$(extract_field "$preflight_line" sram-status); then
	fail "missing or duplicate preflight field: sram-status"
fi
if ! mp2_field=$(extract_field "$preflight_line" mp2); then
	fail "missing or duplicate preflight field: mp2"
fi
if ! pwrap_reset=$(extract_field "$preflight_line" pwrap-reset); then
	fail "missing or duplicate preflight field: pwrap-reset"
fi
if ! spm_mp2=$(extract_field "$preflight_line" spm-mp2); then
	fail "missing or duplicate preflight field: spm-mp2"
fi
if ! spm_buck_iso=$(extract_field "$preflight_line" spm-buck-iso); then
	fail "missing or duplicate preflight field: spm-buck-iso"
fi
if ! cpu_on=$(extract_field "$preflight_line" CPU_ON); then
	fail "missing or duplicate preflight field: CPU_ON"
fi

[ "$offline" = "1" ] || fail "driver did not fence online A72 CPUs"
[ "$cpu_on" = "denied" ] || fail "CPU_ON is not denied"

vproc_uv=${vproc_field%uV}
sram_uv=${sram_field%uV}
mp2_hz=${mp2_field%Hz}
[ "$vproc_uv" != "$vproc_field" ] || fail "vproc unit is not uV"
[ "$sram_uv" != "$sram_field" ] || fail "SRAM unit is not uV"
[ "$mp2_hz" != "$mp2_field" ] || fail "MP2 unit is not Hz"
require_unsigned vproc "$vproc_uv"
require_unsigned sram "$sram_uv"
require_unsigned mp2 "$mp2_hz"
[ "$vproc_uv" -ge 800000 ] && [ "$vproc_uv" -le 1200000 ] ||
	fail "BUCKB voltage is outside 800000-1200000uV: $vproc_uv"
[ "$vproc_enabled" = "0" ] || fail "BUCKB is unexpectedly enabled"
case "$sram_voltages" in
*" $sram_uv "*) ;;
*) fail "secure SRAM voltage is not a supported selector value: $sram_uv" ;;
esac
case "$sram_status" in
-95|0|1) ;;
*) fail "secure SRAM status result is invalid: $sram_status" ;;
esac
[ "$mp2_hz" -ge 2000000000 ] && [ "$mp2_hz" -le 2500000000 ] ||
	fail "MP2 clock is outside the observed 2.0-2.5GHz window: $mp2_hz"
[ "$pwrap_reset" = "0" ] || fail "PWRAP reset is asserted"
printf '%s\n' "$spm_mp2" | grep -Eq '^0x[0-9a-fA-F]{8}$' ||
	fail "SPM MP2 snapshot is malformed"
printf '%s\n' "$spm_buck_iso" | grep -Eq '^0x[0-9a-fA-F]{8}$' ||
	fail "SPM buck-isolation snapshot is malformed"

if ! vproc_mv=$(regulator_voltage_mv vproc-big); then
	fail "BUCKB is absent from regulator summary"
fi
if ! sram_mv=$(regulator_voltage_mv vsram-big); then
	fail "secure SRAM is absent from regulator summary"
fi
require_unsigned "BUCKB summary voltage" "$vproc_mv"
require_unsigned "SRAM summary voltage" "$sram_mv"
[ $((vproc_mv * 1000)) -eq "$vproc_uv" ] ||
	fail "BUCKB log and regulator summary disagree"
[ $((sram_mv * 1000)) -eq "$sram_uv" ] ||
	fail "SRAM log and regulator summary disagree"

if ! mp2_path_hz=$(clock_rate_hz mt6797_mp2); then
	fail "MP2 path clock is absent"
fi
if ! mp2_pll_hz=$(clock_rate_hz mt6797_mp2_armpll); then
	fail "MP2 ARMPLL clock is absent"
fi
require_unsigned "MP2 path clock" "$mp2_path_hz"
require_unsigned "MP2 ARMPLL clock" "$mp2_pll_hz"
[ "$mp2_path_hz" = "$mp2_hz" ] || fail "MP2 driver and clock summary disagree"
[ "$mp2_pll_hz" = "$mp2_path_hz" ] || fail "MP2 path is not sourced at ARMPLL rate"

log_errors=$(printf '%s\n' "$log_text" | grep -Ei \
	'(mt6797-a72|resource-preflight|DA9214|bigidvfs|vproc-big|vsram-big).*(fail|error|timeout|rollback|uncertain|invalid|mismatch|fenced|EUCLEAN)|'\
'(fail|error|timeout|rollback|uncertain|invalid|mismatch|fenced|EUCLEAN).*(mt6797-a72|resource-preflight|DA9214|bigidvfs|vproc-big|vsram-big)' || true)
[ -z "$log_errors" ] || {
	printf '%s\n' "$log_errors" >&2
	fail "A72 resource error found in kernel log"
}

echo "schema=gemini-mt6797-a72-resource-preflight-v1"
echo "collector_mode=read_only"
echo "cpu8_online=no"
echo "cpu9_online=no"
echo "cpu_on_requested=no"
echo "da9214_identity=accepted"
printf 'vproc_big_uv=%s\n' "$vproc_uv"
printf 'vsram_big_uv=%s\n' "$sram_uv"
printf 'vsram_status=%s\n' "$sram_status"
printf 'mp2_hz=%s\n' "$mp2_hz"
printf 'pwrap_reset_asserted=%s\n' "$pwrap_reset"
printf 'spm_mp2=%s\n' "$spm_mp2"
printf 'spm_buck_iso=%s\n' "$spm_buck_iso"
echo "persistent_writes=no"
echo "result=pass"
