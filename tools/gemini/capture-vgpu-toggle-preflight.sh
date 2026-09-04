#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Bounded MT6797 VGPU enable-bit qualification; never binds the GPU.

set -eu

if [ "$#" -ne 0 ]; then
	echo "usage: $0" >&2
	exit 2
fi

: "${DEBUGFS_ROOT:=/sys/kernel/debug}"
: "${DMESG:=dmesg}"
: "${PROC_ROOT:=/proc}"
: "${SYSFS_ROOT:=/sys}"
: "${OUTPUT_DEVICE:=$SYSFS_ROOT/bus/platform/devices/vgpu-toggle}"
: "${STATE_WRITE:=}"
: "${READY_TRIES:=50}"
: "${READY_SLEEP:=0.1}"

regulator_dt=$SYSFS_ROOT/firmware/devicetree/base/i2c@11010000/regulator@1c
gpu_status=$SYSFS_ROOT/firmware/devicetree/base/gpu@13040000/status
gpu_device=$SYSFS_ROOT/bus/platform/devices/13040000.gpu
genpd_summary=$DEBUGFS_ROOT/pm_genpd/pm_genpd_summary
cleanup_needed=0

die()
{
	echo "REJECT: $*" >&2
	exit 1
}

driver_name()
{
	device=$1

	[ -L "$device/driver" ] || return 1
	basename "$(readlink "$device/driver")"
}

read_dt_string()
{
	tr -d '\000' < "$1"
}

read_rail()
{
	regulator=$1
	state=$(cat "$regulator/state") || return 1
	microvolts=$(cat "$regulator/microvolts") || return 1
	users=$(cat "$regulator/num_users") || return 1

	case $state in enabled|disabled) ;; *) return 1 ;; esac
	case $microvolts:$users in *[!0-9:]*) return 1 ;; esac
	printf '%s:%s:%s\n' "$state" "$microvolts" "$users"
}

read_vsel0()
{
	map=$1
	value=$(awk '$1 == "11:" { print $2; found = 1; exit }
		END { if (!found) exit 1 }' "$map/registers") || return 1
	case $value in
		[0-9a-fA-F][0-9a-fA-F]) ;;
		*) return 1 ;;
	esac
	printf '%s\n' "$(printf '%s' "$value" | tr 'A-F' 'a-f')"
}

write_output_state()
{
	state=$1

	if [ -n "$STATE_WRITE" ]; then
		"$STATE_WRITE" "$OUTPUT_DEVICE/state" "$state"
	else
		printf '%s\n' "$state" > "$OUTPUT_DEVICE/state"
	fi
}

wait_output_bound()
{
	tries=0
	while [ "$tries" -lt "$READY_TRIES" ]; do
		bound_driver=$(driver_name "$OUTPUT_DEVICE" 2>/dev/null || true)
		if [ -r "$OUTPUT_DEVICE/state" ] &&
		   [ "$bound_driver" = reg-userspace-consumer ]; then
			return 0
		fi
		tries=$((tries + 1))
		sleep "$READY_SLEEP"
	done
	return 1
}

read_clock_counts()
{
	clock=$1
	enable=$(cat "$DEBUGFS_ROOT/clk/$clock/clk_enable_count") || return 1
	prepare=$(cat "$DEBUGFS_ROOT/clk/$clock/clk_prepare_count") || return 1
	case $enable:$prepare in *[!0-9:]*) return 1 ;; esac
	printf '%s:%s\n' "$enable" "$prepare"
}

domains_off()
{
	[ -r "$genpd_summary" ] || return 1
	for domain in mfg_async mfg mfg_core0 mfg_core1 mfg_core2 mfg_core3; do
		state=$(awk -v name="$domain" '$1 == name { print $2; found = 1; exit }
			END { if (!found) exit 1 }' "$genpd_summary") || return 1
		case $state in off*) ;; *) return 1 ;; esac
	done
}

gpu_closed()
{
	[ "$(read_dt_string "$gpu_status")" = disabled ] || return 1
	[ ! -e "$gpu_device" ] || return 1
	[ ! -e "$SYSFS_ROOT/bus/platform/drivers/panfrost/13040000.gpu" ] || return 1
	if [ -r "$PROC_ROOT/interrupts" ]; then
		! grep -Eq 'panfrost-(job|mmu|gpu)([[:space:]]|$)' \
			"$PROC_ROOT/interrupts" || return 1
	fi
}

cleanup()
{
	rc=$?
	trap - EXIT HUP INT TERM
	if [ "$cleanup_needed" -eq 1 ]; then
		if ! write_output_state disabled; then
			echo "REJECT: cleanup disable request failed" >&2
			rc=1
		else
			cleanup_output=$(cat "$OUTPUT_DEVICE/state" 2>/dev/null || true)
			cleanup_rail=$(read_rail "$regulator_device" 2>/dev/null || true)
			cleanup_raw=$(read_vsel0 "$regmap" 2>/dev/null || true)
			if [ "$cleanup_output" != disabled ] ||
			   [ "$cleanup_rail" != "$before" ] ||
			   [ "$cleanup_raw" != "$before_raw" ]; then
				echo 'REJECT: cleanup did not restore preimage:' \
					"output=$cleanup_output rail=$cleanup_rail" \
					"VSEL0=0x$cleanup_raw" >&2
				rc=1
			fi
		fi
	fi
	exit "$rc"
}

trap cleanup EXIT
trap 'exit 124' HUP INT TERM

echo '[g2-policy]'
cmdline=$(cat "$PROC_ROOT/cmdline") || die 'cannot read the kernel command line'
case " $cmdline " in
	*' regulator_ignore_unused '*) ;;
	*) die 'regulator_ignore_unused safety fence is absent' ;;
esac
[ -r "$regulator_dt/status" ] || die 'RT5735 DT status is unavailable'
[ "$(read_dt_string "$regulator_dt/status")" = okay ] || die 'RT5735 is disabled'
gpu_closed || die 'GPU/Panfrost is not closed'
domains_off || die 'all six MFG power domains are not off'

echo '[g2-ownership]'
wait_output_bound || die 'regulator-output did not bind before readiness deadline'
[ "$(driver_name "$OUTPUT_DEVICE")" = reg-userspace-consumer ] ||
	die 'regulator-output driver is not bound'
[ "$(cat "$OUTPUT_DEVICE/state")" = disabled ] ||
	die 'regulator-output did not inherit disabled state'

rt_device=
for candidate in "$SYSFS_ROOT"/bus/i2c/devices/*; do
	[ -d "$candidate" ] || continue
	[ -L "$candidate/of_node" ] || continue
	[ "$(readlink -f "$candidate/of_node")" = "$(readlink -f "$regulator_dt")" ] ||
		continue
	[ -z "$rt_device" ] || die 'more than one RT5735 DT device exists'
	rt_device=$candidate
done
[ -n "$rt_device" ] || die 'RT5735 DT device is absent'
[ "$(driver_name "$rt_device")" = rt5735-regulator ] ||
	die 'vgpu is not provided by rt5735-regulator'
regulator_device=
for candidate in "$rt_device"/regulator/regulator.*; do
	[ -d "$candidate" ] || continue
	[ -z "$regulator_device" ] || die 'RT5735 exposes more than one regulator'
	regulator_device=$candidate
done
[ -n "$regulator_device" ] || die 'RT5735 regulator is absent'
regulator_class=$SYSFS_ROOT/class/regulator/$(basename "$regulator_device")
[ -e "$regulator_class" ] &&
	[ "$(readlink -f "$regulator_class")" = "$(readlink -f "$regulator_device")" ] ||
	die 'vgpu regulator class ancestry does not resolve to RT5735'
[ "$(cat "$regulator_device/name")" = vgpu ] || die 'RT5735 regulator is not vgpu'
provider_name=$(basename "$rt_device")
regmap=$DEBUGFS_ROOT/regmap/$provider_name
[ -r "$regmap/name" ] && [ -r "$regmap/registers" ] ||
	die 'RT5735 read-only regmap view is absent'
[ "$(cat "$regmap/name")" = rt5735-regulator ] ||
	die 'RT5735 regmap driver identity is wrong'

before=$(read_rail "$regulator_device") || die 'cannot read the initial rail state'
before_raw=$(read_vsel0 "$regmap") || die 'cannot read initial VSEL0'
printf 'before=%s vsel0=0x%s\n' "$before" "$before_raw"
[ "$before" = disabled:843750:0 ] ||
	die "initial rail is $before, expected disabled:843750:0"
[ "$before_raw" = 27 ] || die "initial VSEL0 is 0x$before_raw, expected 0x27"
i2c_clock_before=$(read_clock_counts infra_i2c_gpupm) ||
	die 'cannot read infra_i2c_gpupm counts before enable'
[ "$i2c_clock_before" = 0:0 ] ||
	die "infra_i2c_gpupm is not idle before enable: $i2c_clock_before"
ap_dma_before=$(read_clock_counts infra_ap_dma) ||
	die 'cannot read shared infra_ap_dma counts before enable'
printf 'clock_before=infra_i2c_gpupm:%s infra_ap_dma:%s\n' \
	"$i2c_clock_before" "$ap_dma_before"

echo '[g2-enable]'
# Arm cleanup before the only state-changing operation.  A partial enable or
# any later rejection therefore receives a best-effort standard disable.
cleanup_needed=1
write_output_state enabled || die 'enable request failed'
active=$(read_rail "$regulator_device") || die 'cannot read the enabled rail state'
active_raw=$(read_vsel0 "$regmap") || die 'cannot read active VSEL0'
printf 'active=%s vsel0=0x%s\n' "$active" "$active_raw"
[ "$(cat "$OUTPUT_DEVICE/state")" = enabled ] ||
	die 'regulator-output did not report enabled'
[ "$active" = enabled:843750:1 ] ||
	die "active rail is $active, expected enabled:843750:1"
[ "$active_raw" = a7 ] || die "active VSEL0 is 0x$active_raw, expected 0xa7"
gpu_closed || die 'GPU/Panfrost appeared while the rail was enabled'
domains_off || die 'an MFG power domain turned on with the rail'

echo '[g2-disable]'
write_output_state disabled || die 'disable request failed'
after=$(read_rail "$regulator_device") || die 'cannot read the restored rail state'
after_raw=$(read_vsel0 "$regmap") || die 'cannot read restored VSEL0'
printf 'after=%s vsel0=0x%s\n' "$after" "$after_raw"
[ "$(cat "$OUTPUT_DEVICE/state")" = disabled ] ||
	die 'regulator-output did not report disabled'
[ "$after" = "$before" ] || die "rail restore mismatch: $before -> $after"
[ "$after_raw" = "$before_raw" ] ||
	die "VSEL0 restore mismatch: 0x$before_raw -> 0x$after_raw"
i2c_clock_after=$(read_clock_counts infra_i2c_gpupm) ||
	die 'cannot read infra_i2c_gpupm counts after disable'
[ "$i2c_clock_after" = 0:0 ] ||
	die "infra_i2c_gpupm is not idle after disable: $i2c_clock_after"
ap_dma_after=$(read_clock_counts infra_ap_dma) ||
	die 'cannot read shared infra_ap_dma counts after disable'
[ "$ap_dma_after" = "$ap_dma_before" ] ||
	die "shared infra_ap_dma count drift: $ap_dma_before -> $ap_dma_after"
printf 'clock_after=infra_i2c_gpupm:%s infra_ap_dma:%s\n' \
	"$i2c_clock_after" "$ap_dma_after"
gpu_closed || die 'GPU/Panfrost appeared during the transaction'
domains_off || die 'an MFG power domain is on after the transaction'

boot_log=$($DMESG) || die 'cannot read the kernel log'
log_pattern='11010000\.i2c|rt5735|vgpu-toggle|reg-userspace-consumer|13040000\.gpu|panfrost'
relevant=$(printf '%s\n' "$boot_log" |
	grep -Ei "$log_pattern" || true)
printf '%s\n' "$relevant"
if printf '%s\n' "$relevant" |
	grep -Eiq '(rt5735|vgpu-toggle|reg-userspace-consumer).*(fail|error|timeout)|panfrost'; then
	die 'kernel log contains a rail transaction or Panfrost failure'
fi

cleanup_needed=0
trap - EXIT HUP INT TERM
echo "VGPU_TOGGLE_PREFLIGHT=PASS before=0x$before_raw active=0x$active_raw after=0x$after_raw"
