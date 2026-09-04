#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read-only MT6797 MD1 off-state and Linux-ownership qualification.

set -eu

if [ "$#" -ne 0 ]; then
	echo "usage: $0" >&2
	exit 2
fi

: "${DEBUGFS_ROOT:=/sys/kernel/debug}"
: "${DEVMEM:=devmem}"
: "${DMESG:=dmesg}"
: "${PROC_ROOT:=/proc}"
: "${SLEEP:=sleep}"
: "${SYSFS_ROOT:=/sys}"
: "${MD1_OFF_OBSERVE_SECONDS:=1}"

case $MD1_OFF_OBSERVE_SECONDS in
	''|*[!0-9]*)
		echo "MD1_OFF_OBSERVE_SECONDS must be an integer from 1 through 10" >&2
		exit 2
		;;
esac
if [ "$MD1_OFF_OBSERVE_SECONDS" -lt 1 ] ||
	[ "$MD1_OFF_OBSERVE_SECONDS" -gt 10 ]; then
	echo "MD1_OFF_OBSERVE_SECONDS must be an integer from 1 through 10" >&2
	exit 2
fi

md1_dt=$SYSFS_ROOT/firmware/devicetree/base/remoteproc@10014000
md1_device=$SYSFS_ROOT/bus/platform/devices/10014000.remoteproc
md1_driver_device=$SYSFS_ROOT/bus/platform/drivers/mt6797-md1/10014000.remoteproc
spm_status_address=0x10006180
spm_status_2nd_address=0x10006184
failed=0

reject()
{
	echo "REJECT: $*" >&2
	failed=1
}

read_dt_string()
{
	tr -d '\000' < "$1"
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

read_status_word()
{
	address=$1
	value=$($DEVMEM "$address" 32) || return 1
	case $value in
		0x*) hex=${value#0x} ;;
		0X*) hex=${value#0X} ;;
		*) return 1 ;;
	esac
	case $hex in
		''|*[!0-9a-fA-F]*) return 1 ;;
	esac
	printf '%s\n' "$value"
}

read_power_pair()
{
	primary=$(read_status_word "$spm_status_address") || return 1
	secondary=$(read_status_word "$spm_status_2nd_address") || return 1
	printf '%s:%s\n' "$primary" "$secondary"
}

classify_power_pair()
{
	pair=$1
	primary=${pair%%:*}
	secondary=${pair#*:}
	primary_bit=$((primary & 1))
	secondary_bit=$((secondary & 1))

	if [ "$primary_bit" -ne "$secondary_bit" ]; then
		printf '%s\n' unstable
	elif [ "$primary_bit" -eq 0 ]; then
		printf '%s\n' off
	else
		printf '%s\n' powered
	fi
}

echo "[md1-policy]"
status_file=$md1_dt/status
if [ -r "$status_file" ]; then
	status=$(read_dt_string "$status_file")
	printf 'md1_dt_status=%s\n' "$status"
	[ "$status" = disabled ] ||
		reject "MD1 DT status is $status, expected disabled"
else
	reject "MD1 DT status is unavailable"
fi

if [ -e "$md1_device" ] || [ -e "$md1_driver_device" ]; then
	reject "MD1 platform device or driver binding exists"
fi
if [ -r "$PROC_ROOT/interrupts" ]; then
	if grep -Eq '(^|[[:space:]])10014000\.remoteproc([[:space:]]|$)' \
		"$PROC_ROOT/interrupts"; then
		reject "MD1 registered a CLDMA interrupt"
	fi
else
	reject "/proc/interrupts is unavailable"
fi

echo "[md1-clocks-before]"
for clock in infra_ccif_ap infra_ccif_md infra_cldma; do
	enable=$(clock_count "$clock" clk_enable_count) || enable=missing
	prepare=$(clock_count "$clock" clk_prepare_count) || prepare=missing
	printf '%s enable_count=%s prepare_count=%s\n' \
		"$clock" "$enable" "$prepare"
	[ "$enable:$prepare" = 0:0 ] ||
		reject "$clock is prepared or enabled before the SPM reads"
	case $clock in
		infra_ccif_ap) ccif_ap_before=$enable:$prepare ;;
		infra_ccif_md) ccif_md_before=$enable:$prepare ;;
		infra_cldma) cldma_before=$enable:$prepare ;;
	esac
done

if [ "$failed" -ne 0 ]; then
	echo "MD1_OFF_OWNERSHIP_PREFLIGHT=REJECT"
	exit 1
fi

echo "[md1-spm-status]"
pair_before=$(read_power_pair) || {
	echo "cannot read the paired MD1 SPM status words" >&2
	exit 2
}
state_before=$(classify_power_pair "$pair_before")
printf 'initial primary=%s secondary=%s md1=%s\n' \
	"${pair_before%%:*}" "${pair_before#*:}" "$state_before"
[ "$state_before" = off ] ||
	reject "paired SPM status reports MD1 $state_before, expected off"

"$SLEEP" "$MD1_OFF_OBSERVE_SECONDS"
pair_after=$(read_power_pair) || {
	echo "cannot re-read the paired MD1 SPM status words" >&2
	exit 2
}
state_after=$(classify_power_pair "$pair_after")
printf 'after_%ss primary=%s secondary=%s md1=%s\n' \
	"$MD1_OFF_OBSERVE_SECONDS" "${pair_after%%:*}" \
	"${pair_after#*:}" "$state_after"
[ "$state_after" = off ] ||
	reject "paired SPM status changed to MD1 $state_after"

echo "[md1-clocks-after]"
for clock in infra_ccif_ap infra_ccif_md infra_cldma; do
	enable=$(clock_count "$clock" clk_enable_count) || enable=missing
	prepare=$(clock_count "$clock" clk_prepare_count) || prepare=missing
	printf '%s enable_count=%s prepare_count=%s\n' \
		"$clock" "$enable" "$prepare"
	case $clock in
		infra_ccif_ap) before=$ccif_ap_before ;;
		infra_ccif_md) before=$ccif_md_before ;;
		infra_cldma) before=$cldma_before ;;
	esac
	[ "$enable:$prepare" = 0:0 ] ||
		reject "$clock is prepared or enabled after the SPM reads"
	[ "$before" = "$enable:$prepare" ] ||
		reject "$clock software counts changed during observation"
done

boot_log=$($DMESG) || {
	echo "cannot read the kernel log" >&2
	exit 2
}
md1_log=$(printf '%s\n' "$boot_log" |
	grep -E '10014000\.remoteproc|mt6797-md1|owner-created CLDMA' || true)
printf '%s\n' "$md1_log"
if [ -n "$md1_log" ]; then
	reject "MD1 driver activity occurred despite the disabled node"
fi
if [ -e "$md1_device" ] || [ -e "$md1_driver_device" ]; then
	reject "MD1 platform device or binding appeared during observation"
fi

if [ "$failed" -ne 0 ]; then
	echo "MD1_OFF_OWNERSHIP_PREFLIGHT=REJECT"
	exit 1
fi

echo "MD1_OFF_OWNERSHIP_PREFLIGHT=PASS"
