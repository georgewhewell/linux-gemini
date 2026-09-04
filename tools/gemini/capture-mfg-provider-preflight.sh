#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read-only MT6797 MFG clock-provider qualification; never binds the GPU.

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
: "${G0_OBSERVE_SECONDS:=5}"

case $G0_OBSERVE_SECONDS in
	''|*[!0-9]*)
		echo "G0_OBSERVE_SECONDS must be an integer from 1 through 60" >&2
		exit 2
		;;
esac
if [ "$G0_OBSERVE_SECONDS" -lt 1 ] || [ "$G0_OBSERVE_SECONDS" -gt 60 ]; then
	echo "G0_OBSERVE_SECONDS must be an integer from 1 through 60" >&2
	exit 2
fi

mfg_device=$SYSFS_ROOT/bus/platform/devices/13000000.syscon
gpu_device=$SYSFS_ROOT/bus/platform/devices/13040000.gpu
deferred_devices=$DEBUGFS_ROOT/devices_deferred
genpd_summary=$DEBUGFS_ROOT/pm_genpd/pm_genpd_summary
mfg_clock=$DEBUGFS_ROOT/clk/mfg_bg3d
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

driver_name()
{
	device=$1

	[ -L "$device/driver" ] || return 1
	basename "$(readlink "$device/driver")"
}

clock_count()
{
	attribute=$1
	file=$mfg_clock/$attribute
	value=

	[ -r "$file" ] || return 1
	value=$(cat "$file")
	case $value in
		''|*[!0-9]*) return 1 ;;
	esac
	printf '%s\n' "$value"
}

domain_line()
{
	name=$1

	awk -v name="$name" '
		$1 == name { print; found = 1; exit }
		END { if (!found) exit 1 }
	' "$genpd_summary"
}

echo "[g0-policy]"
cmdline=$(cat "$PROC_ROOT/cmdline") || {
	echo "cannot read the kernel command line" >&2
	exit 2
}
printf 'cmdline=%s\n' "$cmdline"
case " $cmdline " in
	*' pd_ignore_unused '*)
		reject_lifecycle "pd_ignore_unused would suppress the decisive genpd sweep"
		;;
esac
case " $cmdline " in
	*' clk_ignore_unused '*)
		printf '%s\n' 'clock_late_policy=unused-clock-disable-suppressed'
		;;
	*)
		printf '%s\n' 'clock_late_policy=unused-clock-disable-exercised'
		;;
esac

echo "[g0-registration]"
mfg_status_file=$SYSFS_ROOT/firmware/devicetree/base/syscon@13000000/status
gpu_status_file=$SYSFS_ROOT/firmware/devicetree/base/gpu@13040000/status
if [ -r "$mfg_status_file" ]; then
	mfg_status=$(read_dt_string "$mfg_status_file")
	printf 'mfg_dt_status=%s\n' "$mfg_status"
	[ "$mfg_status" = okay ] ||
		reject_registration "MFGSYS DT status is $mfg_status, expected okay"
else
	reject_registration "MFGSYS DT status is unavailable"
fi
if [ -r "$gpu_status_file" ]; then
	gpu_status=$(read_dt_string "$gpu_status_file")
	printf 'gpu_dt_status=%s\n' "$gpu_status"
	[ "$gpu_status" = disabled ] ||
		reject_registration "GPU DT status is $gpu_status, expected disabled"
else
	reject_registration "GPU DT status is unavailable"
fi

mfg_driver=unbound
if [ -d "$mfg_device" ] && mfg_driver=$(driver_name "$mfg_device"); then
	:
else
	reject_registration "13000000.syscon is absent or unbound"
fi
printf 'mfg_driver=%s\n' "$mfg_driver"
[ "$mfg_driver" = clk-mt6797-mfg ] ||
	reject_registration "MFGSYS is bound to $mfg_driver, expected clk-mt6797-mfg"

if [ -r "$deferred_devices" ]; then
	deferred=$(grep -E '(^|[[:space:]])13000000\.(syscon|clock-controller)([[:space:]]|$)' \
		"$deferred_devices" || true)
	[ -z "$deferred" ] || {
		printf '%s\n' "$deferred"
		reject_registration "MFGSYS remains on the deferred-probe list"
	}
else
	reject_registration "devices_deferred is unavailable"
fi

enable_before=$(clock_count clk_enable_count) || {
	enable_before=missing
	reject_registration "mfg_bg3d enable count is unavailable or malformed"
}
prepare_before=$(clock_count clk_prepare_count) || {
	prepare_before=missing
	reject_registration "mfg_bg3d prepare count is unavailable or malformed"
}
printf 'mfg_bg3d enable_count=%s prepare_count=%s\n' \
	"$enable_before" "$prepare_before"
[ "$enable_before" = 0 ] ||
	reject_registration "mfg_bg3d has an unexpected enable vote"
[ "$prepare_before" = 0 ] ||
	reject_registration "mfg_bg3d has an unexpected prepare vote"

if [ -e "$gpu_device" ]; then
	reject_registration "13040000.gpu exists despite the disabled GPU node"
fi
if [ -e "$SYSFS_ROOT/bus/platform/drivers/panfrost/13040000.gpu" ]; then
	reject_registration "Panfrost bound to 13040000.gpu"
fi
if [ -r "$PROC_ROOT/interrupts" ] &&
	grep -Eq 'panfrost-(job|mmu|gpu)([[:space:]]|$)' "$PROC_ROOT/interrupts"; then
	reject_registration "Panfrost registered a GPU interrupt"
fi

if [ "$registration_failed" -ne 0 ]; then
	echo "G0_REGISTRATION=REJECT"
else
	echo "G0_REGISTRATION=PASS"
fi

echo "[g0-observation]"
printf 'seconds=%s\n' "$G0_OBSERVE_SECONDS"
"$SLEEP" "$G0_OBSERVE_SECONDS"

echo "[g0-lifecycle]"
boot_log=$($DMESG) || {
	echo "cannot read the kernel log" >&2
	exit 2
}
mfg_log_pattern='13000000\.(syscon|clock-controller)|mfg|MFG|panfrost'
mfg_log_pattern="$mfg_log_pattern|clk: .*unused clocks"
mfg_log_pattern="$mfg_log_pattern|genpd: .*unused power domains"
mfg_log=$(printf '%s\n' "$boot_log" |
	grep -E "$mfg_log_pattern" || true)
printf '%s\n' "$mfg_log"

reject_pattern='Trying to register duplicate clock ID: 1'
reject_pattern="$reject_pattern|Failed to power (on|off) domain mfg(_async|_core[0-3])?"
reject_pattern="$reject_pattern|Failed to restore bus protection for domain mfg"
reject_pattern="$reject_pattern|13000000\.(syscon|clock-controller).*(probe.*failed|[Ee]rror)"
reject_pattern="$reject_pattern|panfrost.*13040000|13040000\.gpu.*panfrost"
if printf '%s\n' "$mfg_log" | grep -Eq "$reject_pattern"; then
	reject_lifecycle "kernel log contains an MFG provider, genpd or Panfrost failure"
fi
if ! printf '%s\n' "$boot_log" |
	grep -q 'genpd: Disabling unused power domains'; then
	reject_lifecycle "the late unused-domain sweep is not witnessed"
fi
case " $cmdline " in
	*' clk_ignore_unused '*)
		if ! printf '%s\n' "$boot_log" |
			grep -q 'clk: Not disabling unused clocks'; then
			reject_lifecycle "the suppressed late unused-clock sweep is not witnessed"
		fi
		;;
	*)
		if ! printf '%s\n' "$boot_log" |
			grep -q 'clk: Disabling unused clocks'; then
			reject_lifecycle "the late unused-clock sweep is not witnessed"
		fi
		;;
esac

current_driver=unbound
if [ -d "$mfg_device" ] && current_driver=$(driver_name "$mfg_device"); then
	:
fi
printf 'mfg_driver_after=%s\n' "$current_driver"
[ "$current_driver" = clk-mt6797-mfg ] ||
	reject_lifecycle "MFGSYS provider did not remain bound"

enable_after=$(clock_count clk_enable_count) || enable_after=missing
prepare_after=$(clock_count clk_prepare_count) || prepare_after=missing
printf 'mfg_bg3d_after enable_count=%s prepare_count=%s\n' \
	"$enable_after" "$prepare_after"
[ "$enable_after" = 0 ] ||
	reject_lifecycle "mfg_bg3d gained an enable vote"
[ "$prepare_after" = 0 ] ||
	reject_lifecycle "mfg_bg3d gained a prepare vote"
[ "$enable_before:$prepare_before" = "$enable_after:$prepare_after" ] ||
	reject_lifecycle "mfg_bg3d software counts changed during observation"

echo "[g0-power-domains]"
if [ ! -r "$genpd_summary" ]; then
	reject_lifecycle "pm_genpd_summary is unavailable"
else
	for domain in mfg_async mfg mfg_core0 mfg_core1 mfg_core2 mfg_core3; do
		if line=$(domain_line "$domain"); then
			printf '%s\n' "$line"
			state=$(printf '%s\n' "$line" | awk '{ print $2 }')
			case $state in
				off*) ;;
				*) reject_lifecycle "$domain is not off" ;;
			esac
		else
			reject_lifecycle "$domain is absent from pm_genpd_summary"
		fi
	done
fi

if [ -e "$gpu_device" ] ||
	[ -e "$SYSFS_ROOT/bus/platform/drivers/panfrost/13040000.gpu" ]; then
	reject_lifecycle "GPU device appeared during the observation window"
fi

if [ "$lifecycle_failed" -ne 0 ]; then
	echo "G0_LIFECYCLE=REJECT"
else
	echo "G0_LIFECYCLE=PASS"
fi

if [ "$registration_failed" -ne 0 ] || [ "$lifecycle_failed" -ne 0 ]; then
	echo "MFG_PROVIDER_PREFLIGHT=REJECT"
	exit 1
fi

echo "MFG_PROVIDER_PREFLIGHT=PASS"
