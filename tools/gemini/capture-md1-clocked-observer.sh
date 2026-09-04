#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Capture the bounded MT6797 MD1 clocked-ownership observation result.

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
: "${MD1_CLOCKED_OBSERVE_SECONDS:=1}"

case $MD1_CLOCKED_OBSERVE_SECONDS in
	''|*[!0-9]*)
		echo "MD1_CLOCKED_OBSERVE_SECONDS must be an integer from 1 through 10" >&2
		exit 2
		;;
esac
if [ "$MD1_CLOCKED_OBSERVE_SECONDS" -lt 1 ] ||
	[ "$MD1_CLOCKED_OBSERVE_SECONDS" -gt 10 ]; then
	echo "MD1_CLOCKED_OBSERVE_SECONDS must be an integer from 1 through 10" >&2
	exit 2
fi

md1_dt=$SYSFS_ROOT/firmware/devicetree/base/remoteproc@10014000
md1_device=$SYSFS_ROOT/bus/platform/devices/10014000.remoteproc
observer_driver=mt6797-md1-clocked-observer
observer_binding=$SYSFS_ROOT/bus/platform/drivers/$observer_driver/10014000.remoteproc
infra_cg_sta1=0x10001094
infra_clock_gate_mask=$((0x04810000))
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

read_gate_word()
{
	value=$($DEVMEM "$infra_cg_sta1" 32) || return 1
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

echo "[md1-clocked-policy]"
if [ ! -r "$md1_dt/compatible" ]; then
	reject "MD1 compatible is unavailable"
else
	compatible=$(read_dt_string "$md1_dt/compatible")
	printf 'compatible=%s\n' "$compatible"
	[ "$compatible" = mediatek,mt6797-md1-clocked-observer ] ||
		reject "MD1 is not exclusively matched to the clocked observer"
fi
if [ ! -r "$md1_dt/status" ]; then
	reject "MD1 status is unavailable"
else
	status=$(read_dt_string "$md1_dt/status")
	printf 'status=%s\n' "$status"
	[ "$status" = okay ] || reject "MD1 status is $status, expected okay"
fi

[ -e "$md1_device" ] || reject "MD1 platform device is absent"
[ -L "$observer_binding" ] || reject "clocked observer is not bound"
if [ -L "$md1_device/driver" ]; then
	driver=$(basename "$(readlink "$md1_device/driver")")
	printf 'driver=%s\n' "$driver"
	[ "$driver" = "$observer_driver" ] ||
		reject "unexpected MD1 driver $driver"
else
	reject "MD1 driver ancestry is unavailable"
fi

echo "[md1-clock-cleanup-before]"
for clock in infra_ccif_ap infra_ccif_md infra_cldma; do
	enable=$(clock_count "$clock" clk_enable_count) || enable=missing
	prepare=$(clock_count "$clock" clk_prepare_count) || prepare=missing
	printf '%s enable_count=%s prepare_count=%s\n' \
		"$clock" "$enable" "$prepare"
	[ "$enable:$prepare" = 0:0 ] ||
		reject "$clock was not returned idle by the observer"
	eval "${clock}_before=\$enable:\$prepare"
done

gate_before=$(read_gate_word) || {
	echo "cannot read infrastructure clock-gate status" >&2
	exit 2
}
printf 'infra_cg_sta1=%s gate_mask=0x%08x\n' \
	"$gate_before" "$infra_clock_gate_mask"
[ $((gate_before & infra_clock_gate_mask)) -eq "$infra_clock_gate_mask" ] ||
	reject "one or more observer clocks remain physically ungated"

"$SLEEP" "$MD1_CLOCKED_OBSERVE_SECONDS"

echo "[md1-clock-cleanup-after]"
for clock in infra_ccif_ap infra_ccif_md infra_cldma; do
	enable=$(clock_count "$clock" clk_enable_count) || enable=missing
	prepare=$(clock_count "$clock" clk_prepare_count) || prepare=missing
	printf '%s enable_count=%s prepare_count=%s\n' \
		"$clock" "$enable" "$prepare"
	eval "before=\$${clock}_before"
	[ "$enable:$prepare" = 0:0 ] ||
		reject "$clock became active after observer cleanup"
	[ "$before" = "$enable:$prepare" ] ||
		reject "$clock software counts changed after observer cleanup"
done

gate_after=$(read_gate_word) || {
	echo "cannot re-read infrastructure clock-gate status" >&2
	exit 2
}
printf 'after_%ss infra_cg_sta1=%s\n' \
	"$MD1_CLOCKED_OBSERVE_SECONDS" "$gate_after"
[ $((gate_after & infra_clock_gate_mask)) -eq "$infra_clock_gate_mask" ] ||
	reject "one or more observer clocks became physically ungated"

echo "[md1-clocked-result]"
boot_log=$($DMESG) || {
	echo "cannot read the kernel log" >&2
	exit 2
}
observer_pattern='MD1_CLOCKED_OBSERVER|clocked observation or unwind failed'
observer_pattern="$observer_pattern|clock gates did not close"
observer_pattern="$observer_pattern|failed to enable (ccif-ap|ccif-md|cldma) clock"
observer_log=$(printf '%s\n' "$boot_log" |
	grep -E "$observer_pattern" || true)
printf '%s\n' "$observer_log"
pass_marker='MD1_CLOCKED_OBSERVER state=quiescent-unconfigured result=pass clocks=unwound'
[ "$(printf '%s\n' "$observer_log" | grep -Fc "$pass_marker")" -eq 1 ] ||
	reject "expected exactly one quiescent observer pass marker"
if printf '%s\n' "$observer_log" |
	grep -Eq 'result=(reject|stop)|observation or unwind failed'; then
	reject "observer reported a stop, ownership conflict, or lifecycle failure"
fi

if [ -r "$PROC_ROOT/interrupts" ]; then
	if grep -Eq '(^|[[:space:]])10014000\.remoteproc([[:space:]]|$)' \
		"$PROC_ROOT/interrupts"; then
		reject "MD1 registered an interrupt"
	fi
else
	reject "/proc/interrupts is unavailable"
fi
if printf '%s\n' "$boot_log" |
	grep -Eq 'owner-created CLDMA|CLDMA fatal|waiting for CCCI HS1'; then
	reject "production MD1 ring or protocol activity was logged"
fi

if [ "$failed" -ne 0 ]; then
	echo "MD1_CLOCKED_OWNER_PREFLIGHT=REJECT"
	exit 1
fi

echo "MD1_CLOCKED_OWNER_PREFLIGHT=PASS"
