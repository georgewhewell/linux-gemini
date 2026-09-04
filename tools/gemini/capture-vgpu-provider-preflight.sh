#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read-only MT6797 RT5735 qualification; never binds a GPU consumer.

set -eu

if [ "$#" -ne 0 ]; then
	echo "usage: $0" >&2
	exit 2
fi

: "${DEBUGFS_ROOT:=/sys/kernel/debug}"
: "${DMESG:=dmesg}"
: "${PROC_ROOT:=/proc}"
: "${SLEEP:=sleep}"
: "${SYSFS_ROOT:=/sys}"
: "${G1_CLEANUP_WAIT_SECONDS:=45}"

case $G1_CLEANUP_WAIT_SECONDS in
	''|*[!0-9]*)
		echo "G1_CLEANUP_WAIT_SECONDS must be an integer from 1 through 60" >&2
		exit 2
		;;
esac
if [ "$G1_CLEANUP_WAIT_SECONDS" -lt 1 ] ||
	[ "$G1_CLEANUP_WAIT_SECONDS" -gt 60 ]; then
	echo "G1_CLEANUP_WAIT_SECONDS must be an integer from 1 through 60" >&2
	exit 2
fi

i2c_dt=$SYSFS_ROOT/firmware/devicetree/base/i2c@11010000
regulator_dt=$i2c_dt/regulator@1c
mfg_status_file=$SYSFS_ROOT/firmware/devicetree/base/syscon@13000000/status
gpu_status_file=$SYSFS_ROOT/firmware/devicetree/base/gpu@13040000/status
i2c_device=$SYSFS_ROOT/bus/platform/devices/11010000.i2c
gpu_device=$SYSFS_ROOT/bus/platform/devices/13040000.gpu
deferred_devices=$DEBUGFS_ROOT/devices_deferred
cleanup_marker='regulator: Not disabling unused regulators'
registration_failed=0
lifecycle_failed=0

reject_registration()
{
	echo "REJECT(registration): $*" >&2
	registration_failed=1
}

reject_lifecycle()
{
	echo "REJECT(lifecycle): $*" >&2
	lifecycle_failed=1
}

read_dt_string()
{
	tr -d '\000' < "$1"
}

read_dt_cell_hex()
{
	od -An -tx1 -v "$1" | tr -d ' \n'
}

driver_name()
{
	device=$1

	[ -L "$device/driver" ] || return 1
	basename "$(readlink "$device/driver")"
}

clock_count()
{
	clock=$1
	attribute=$2
	file=$DEBUGFS_ROOT/clk/$clock/$attribute
	value=

	[ -r "$file" ] || return 1
	value=$(cat "$file")
	case $value in
		''|*[!0-9]*) return 1 ;;
	esac
	printf '%s\n' "$value"
}

read_rail()
{
	regulator=$1
	state=$(cat "$regulator/state") || return 1
	microvolts=$(cat "$regulator/microvolts") || return 1
	users=$(cat "$regulator/num_users") || return 1

	[ -n "$microvolts" ] && [ -n "$users" ] || return 1
	case $microvolts:$users in
		*[!0-9:]*) return 1 ;;
	esac
	printf '%s:%s:%s\n' "$state" "$microvolts" "$users"
}

print_rail()
{
	phase=$1
	sample=$2
	state=${sample%%:*}
	remainder=${sample#*:}
	microvolts=${remainder%%:*}
	users=${remainder#*:}
	enabled=0

	[ "$state" = enabled ] && enabled=1
	printf '%s enabled=%s microvolts=%s num_users=%s\n' \
		"$phase" "$enabled" "$microvolts" "$users"
}

echo "[g1-policy]"
cmdline=$(cat "$PROC_ROOT/cmdline") || {
	echo "cannot read the kernel command line" >&2
	exit 2
}
printf 'cmdline=%s\n' "$cmdline"
case " $cmdline " in
	*' regulator_ignore_unused '*) ;;
	*) reject_registration "regulator_ignore_unused safety fence is absent" ;;
esac

boot_log=$($DMESG) || {
	echo "cannot read the kernel log" >&2
	exit 2
}
if printf '%s\n' "$boot_log" | grep -Fq "$cleanup_marker"; then
	reject_registration \
		"regulator cleanup already ran; invoke G1 before the 30-second boundary"
	echo "G1_REGISTRATION=REJECT"
	echo "VGPU_PROVIDER_PREFLIGHT=REJECT"
	exit 1
fi

echo "[g1-devicetree]"
for status_spec in \
	"$i2c_dt/status:I2C7:okay" \
	"$regulator_dt/status:RT5735:okay" \
	"$mfg_status_file:MFGSYS:okay" \
	"$gpu_status_file:GPU:disabled"; do
	status_file=${status_spec%%:*}
	remainder=${status_spec#*:}
	label=${remainder%%:*}
	expected=${remainder#*:}
	if [ -r "$status_file" ]; then
		status=$(read_dt_string "$status_file")
		printf '%s_dt_status=%s\n' "$label" "$status"
		[ "$status" = "$expected" ] ||
			reject_registration \
				"$label DT status is $status, expected $expected"
	else
		reject_registration "$label DT status is unavailable"
	fi
done

if [ -e "$regulator_dt/regulator-ramp-delay" ]; then
	reject_registration "rail-only DT unexpectedly programs the ramp rate"
fi
for property_spec in \
	"regulator-min-microvolt:000927c0" \
	"regulator-max-microvolt:00154456"; do
	property=${property_spec%%:*}
	expected=${property_spec#*:}
	if [ -r "$regulator_dt/$property" ]; then
		value=$(read_dt_cell_hex "$regulator_dt/$property")
		printf '%s=0x%s\n' "$property" "$value"
		[ "$value" = "$expected" ] ||
			reject_registration \
				"$property is 0x$value, expected 0x$expected"
	else
		reject_registration "$property is unavailable"
	fi
done
for property in regulator-boot-on regulator-always-on; do
	[ ! -e "$regulator_dt/$property" ] ||
		reject_registration "$property would change rail ownership"
done

echo "[g1-registration]"
i2c_driver=unbound
if [ -d "$i2c_device" ] && i2c_driver=$(driver_name "$i2c_device"); then
	:
else
	reject_registration "11010000.i2c is absent or unbound"
fi
printf 'i2c_driver=%s\n' "$i2c_driver"
[ "$i2c_driver" = i2c-mt65xx ] ||
	reject_registration \
		"I2C7 is bound to $i2c_driver, expected i2c-mt65xx"

rt_device=
for candidate in "$SYSFS_ROOT"/bus/i2c/devices/*-001c; do
	[ -d "$candidate" ] || continue
	[ -L "$candidate/of_node" ] || continue
	[ "$(readlink -f "$candidate/of_node")" = \
		"$(readlink -f "$regulator_dt")" ] || continue
	if [ -n "$rt_device" ]; then
		reject_registration "more than one I2C device maps to regulator@1c"
		continue
	fi
	rt_device=$candidate
done
if [ -z "$rt_device" ]; then
	reject_registration "no I2C device has the regulator@1c OF ancestry"
	rt_driver=unbound
else
	rt_driver=$(driver_name "$rt_device") || rt_driver=unbound
fi
printf 'rt5735_device=%s driver=%s\n' \
	"${rt_device:-absent}" "$rt_driver"
[ "$rt_driver" = rt5735-regulator ] ||
	reject_registration \
		"RT5735 is bound to $rt_driver, expected rt5735-regulator"

regulator_device=
if [ -n "$rt_device" ]; then
	for candidate in "$rt_device"/regulator/regulator.*; do
		[ -d "$candidate" ] || continue
		if [ -n "$regulator_device" ]; then
			reject_registration "RT5735 exposes more than one regulator"
			continue
		fi
		regulator_device=$candidate
	done
fi
if [ -z "$regulator_device" ]; then
	reject_registration "RT5735 regulator class device is absent"
else
	regulator_class=$SYSFS_ROOT/class/regulator/$(basename "$regulator_device")
	if [ ! -e "$regulator_class" ] ||
		[ "$(readlink -f "$regulator_class")" != \
		"$(readlink -f "$regulator_device")" ]; then
		reject_registration "regulator class ancestry does not resolve to RT5735"
	fi
	regulator_name=$(cat "$regulator_device/name") || regulator_name=unreadable
	printf 'regulator_device=%s name=%s product_id=0x10(driver-enforced)\n' \
		"$(basename "$regulator_device")" "$regulator_name"
	[ "$regulator_name" = vgpu ] ||
		reject_registration \
			"regulator is named $regulator_name, expected vgpu"
fi

if [ -r "$deferred_devices" ]; then
	deferred=$(grep -E \
		'11010000\.i2c|rt5735|regulator@1c' "$deferred_devices" || true)
	[ -z "$deferred" ] || {
		printf '%s\n' "$deferred"
		reject_registration "I2C7 or RT5735 remains deferred"
	}
else
	reject_registration "devices_deferred is unavailable"
fi

if [ -r "$PROC_ROOT/interrupts" ]; then
	grep -Eq '(^|[[:space:]])11010000\.i2c([[:space:]]|$)' \
		"$PROC_ROOT/interrupts" ||
		reject_registration "I2C7 IRQ action 11010000.i2c is absent"
else
	reject_registration "/proc/interrupts is unavailable"
fi

if [ -e "$gpu_device" ] ||
	[ -e "$SYSFS_ROOT/bus/platform/drivers/panfrost/13040000.gpu" ]; then
	reject_registration "GPU/Panfrost exists despite the disabled GPU node"
fi
if [ -r "$PROC_ROOT/interrupts" ] &&
	grep -Eq 'panfrost-(job|mmu|gpu)([[:space:]]|$)' \
		"$PROC_ROOT/interrupts"; then
	reject_registration "Panfrost registered a GPU interrupt"
fi

if [ "$registration_failed" -ne 0 ] || [ -z "$regulator_device" ]; then
	echo "G1_REGISTRATION=REJECT"
	echo "VGPU_PROVIDER_PREFLIGHT=REJECT"
	exit 1
fi

rail_before=$(read_rail "$regulator_device") || {
	echo "G1_REGISTRATION=REJECT"
	echo "VGPU_PROVIDER_PREFLIGHT=REJECT"
	echo "REJECT(registration): rail attributes are unavailable or malformed" >&2
	exit 1
}
print_rail initial "$rail_before"
[ "$rail_before" = enabled:975000:0 ] ||
	reject_registration \
		"inherited rail is $rail_before, expected enabled:975000:0"

for clock in infra_i2c_gpupm infra_ap_dma; do
	enable=$(clock_count "$clock" clk_enable_count) || enable=missing
	prepare=$(clock_count "$clock" clk_prepare_count) || prepare=missing
	printf '%s_initial enable_count=%s prepare_count=%s\n' \
		"$clock" "$enable" "$prepare"
	[ "$enable" = 0 ] ||
		reject_registration "$clock did not return to idle after rail reads"
	case $clock in
		infra_i2c_gpupm) gpupm_before=$enable:$prepare ;;
		infra_ap_dma) apdma_before=$enable:$prepare ;;
	esac
done

if [ "$registration_failed" -ne 0 ]; then
	echo "G1_REGISTRATION=REJECT"
	echo "VGPU_PROVIDER_PREFLIGHT=REJECT"
	exit 1
fi
echo "G1_REGISTRATION=PASS"

echo "[g1-cleanup-boundary]"
elapsed=0
while [ "$elapsed" -lt "$G1_CLEANUP_WAIT_SECONDS" ]; do
	boot_log=$($DMESG) || {
		echo "cannot read the kernel log" >&2
		exit 2
	}
	if printf '%s\n' "$boot_log" | grep -Fq "$cleanup_marker"; then
		break
	fi
	"$SLEEP" 1
	elapsed=$((elapsed + 1))
done
boot_log=$($DMESG) || {
	echo "cannot read the kernel log" >&2
	exit 2
}
printf 'cleanup_wait_seconds=%s\n' "$elapsed"
if ! printf '%s\n' "$boot_log" | grep -Fq "$cleanup_marker"; then
	reject_lifecycle \
		"regulator_ignore_unused cleanup marker was not seen within the window"
fi

rail_after=$(read_rail "$regulator_device") || rail_after=unavailable
print_rail after_cleanup "$rail_after"
[ "$rail_after" = enabled:975000:0 ] ||
	reject_lifecycle \
		"rail after cleanup is $rail_after, expected enabled:975000:0"
[ "$rail_before" = "$rail_after" ] ||
	reject_lifecycle "rail state changed across the cleanup boundary"

for clock in infra_i2c_gpupm infra_ap_dma; do
	enable=$(clock_count "$clock" clk_enable_count) || enable=missing
	prepare=$(clock_count "$clock" clk_prepare_count) || prepare=missing
	printf '%s_after enable_count=%s prepare_count=%s\n' \
		"$clock" "$enable" "$prepare"
	[ "$enable" = 0 ] ||
		reject_lifecycle "$clock did not return to idle after rail reads"
	case $clock in
		infra_i2c_gpupm) before=$gpupm_before ;;
		infra_ap_dma) before=$apdma_before ;;
	esac
	[ "$before" = "$enable:$prepare" ] ||
		reject_lifecycle "$clock software counts changed across cleanup"
done

relevant_log=$(printf '%s\n' "$boot_log" |
	grep -Ei '11010000\.i2c|rt5735|regulator.*unused|13040000\.gpu|panfrost' || true)
printf '%s\n' "$relevant_log"
reject_pattern='11010000\.i2c.*(fail|error|timeout)'
reject_pattern="$reject_pattern|rt5735.*(fail|error|timeout|unexpected)"
reject_pattern="$reject_pattern|13040000\.gpu.*panfrost|panfrost.*13040000"
if printf '%s\n' "$relevant_log" | grep -Eiq "$reject_pattern"; then
	reject_lifecycle "kernel log contains an I2C7, RT5735 or Panfrost failure"
fi
if [ -e "$gpu_device" ] ||
	[ -e "$SYSFS_ROOT/bus/platform/drivers/panfrost/13040000.gpu" ]; then
	reject_lifecycle "GPU/Panfrost appeared during the cleanup window"
fi

if [ "$lifecycle_failed" -ne 0 ]; then
	echo "G1_LIFECYCLE=REJECT"
	echo "VGPU_PROVIDER_PREFLIGHT=REJECT"
	exit 1
fi

echo "G1_LIFECYCLE=PASS"
echo "VGPU_PROVIDER_PREFLIGHT=PASS"
