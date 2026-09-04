#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read-only MT6797 JPEG probe qualification; opens no V4L2 device.

set -eu

if [ "$#" -ne 0 ]; then
	echo "usage: $0" >&2
	exit 2
fi

: "${DEBUGFS_ROOT:=/sys/kernel/debug}"
: "${DMESG:=dmesg}"
: "${PROC_ROOT:=/proc}"
: "${SYSFS_ROOT:=/sys}"

genpd_summary=$DEBUGFS_ROOT/pm_genpd/pm_genpd_summary
failed=0

reject()
{
	echo "REJECT: $*" >&2
	failed=1
}

driver_name()
{
	device=$1

	[ -L "$device/driver" ] || return 1
	basename "$(readlink "$device/driver")"
}

require_driver()
{
	address=$1
	expected=$2
	device=$SYSFS_ROOT/bus/platform/devices/$address
	actual=unbound

	if [ -d "$device" ] && actual=$(driver_name "$device"); then
		:
	else
		reject "$address is absent or unbound"
	fi
	printf '%s driver=%s\n' "$address" "$actual"
	[ "$actual" = "$expected" ] ||
		reject "$address is bound to $actual, expected $expected"
}

require_suspended()
{
	address=$1
	status_file=$SYSFS_ROOT/bus/platform/devices/$address/power/runtime_status
	status=missing

	if [ -r "$status_file" ]; then
		status=$(cat "$status_file")
	else
		reject "$address has no runtime-PM status"
	fi
	printf '%s runtime_status=%s\n' "$address" "$status"
	[ "$status" = suspended ] ||
		reject "$address is not runtime suspended before any JPEG job"
}

all_runtime_suspended()
{
	for address in 10205000.iommu 14022000.smi 17001000.larb \
		17003000.jpeg-encoder 17004000.jpeg-decoder; do
		status_file=$SYSFS_ROOT/bus/platform/devices/$address/power/runtime_status
		[ -r "$status_file" ] || return 1
		[ "$(cat "$status_file")" = suspended ] || return 1
	done
}

summary_line()
{
	name=$1
	file=$2

	awk -v name="$name" '
		$1 == name { print; found = 1; exit }
		END { if (!found) exit 1 }
	' "$file"
}

irq_count()
{
	name=$1

	awk -v name="$name" '
		index($0, name) {
			sub(/^.*:/, "")
			for (i = 1; i <= NF && $i ~ /^[0-9]+$/; i++)
				sum += $i
			found = 1
		}
		END {
			if (!found)
				exit 1
			print sum + 0
		}
	' "$PROC_ROOT/interrupts"
}

require_video_node()
{
	expected_name=$1
	expected_device=$2
	match_count=0

	for video in "$SYSFS_ROOT"/class/video4linux/video*; do
		[ -e "$video" ] || continue
		[ -r "$video/name" ] || continue
		[ "$(cat "$video/name")" = "$expected_name" ] || continue
		match_count=$((match_count + 1))
		device_link=$(readlink "$video/device" 2>/dev/null || true)
		printf '%s name=%s device=%s\n' "$(basename "$video")" \
			"$expected_name" "$device_link"
		case $device_link in
		*"/$expected_device") ;;
		*) reject "$expected_name is not backed by $expected_device" ;;
		esac
	done

	[ "$match_count" -eq 1 ] ||
		reject "expected one $expected_name video node, found $match_count"
}

echo "[jpeg-kernel-log]"
boot_log=$($DMESG) || {
	echo "cannot read the kernel log" >&2
	exit 2
}
jpeg_addresses='10205000\.(iommu|m4u)|14022000\.(smi|smi_common)'
jpeg_addresses="$jpeg_addresses|17001000\.larb"
jpeg_addresses="$jpeg_addresses|17003000\.(jpeg-encoder|jpgenc)"
jpeg_addresses="$jpeg_addresses|17004000\.(jpeg-decoder|jpgdec)"
jpeg_log=$(printf '%s\n' "$boot_log" |
	grep -Ei "$jpeg_addresses|mtk[_-]?jpeg|jpeg (encoder|decoder)|M4U" || true)
printf '%s\n' "$jpeg_log"

reject_pattern='Failed to (request jpeg_irq|init clk|register v4l2 device)'
reject_pattern="$reject_pattern|mtk_jpeg_single_core_init failed"
reject_pattern="$reject_pattern|Failed @ IRQ|mm dts parse fail|Unable to link"
reject_pattern="$reject_pattern|Failed to get the smi_common device"
reject_pattern="$reject_pattern|translation fault|iommu fault|TLB.*fault"
reject_pattern="$reject_pattern|Header invalid|decode failed|Context is NULL"
reject_pattern="$reject_pattern|Jpg (Enc|Dec) occurs unknown Err"
if printf '%s\n' "$jpeg_log" | grep -Eiq "$reject_pattern"; then
	reject "kernel log contains a JPEG, SMI, or IOMMU failure"
fi

echo "[jpeg-bindings]"
require_driver 10205000.iommu mtk-iommu
require_driver 14022000.smi mtk-smi-common
require_driver 17001000.larb mtk-smi-larb
require_driver 17003000.jpeg-encoder mtk-jpeg
require_driver 17004000.jpeg-decoder mtk-jpeg

if [ -e "$SYSFS_ROOT/bus/platform/devices/10212000.mailbox" ]; then
	reject "GCE was instantiated by the JPEG-only overlay"
else
	echo "10212000.mailbox absent"
fi

if [ -r "$DEBUGFS_ROOT/devices_deferred" ]; then
	deferred=$(grep -E '10205000|14022000|17001000|17003000|17004000' \
		"$DEBUGFS_ROOT/devices_deferred" || true)
	if [ -n "$deferred" ]; then
		printf '%s\n' "$deferred"
		reject "a JPEG or DMA dependency remains deferred"
	fi
fi

echo "[jpeg-video-nodes]"
require_video_node mtk-jpeg-enc 17003000.jpeg-encoder
require_video_node mtk-jpeg-dec 17004000.jpeg-decoder

echo "[jpeg-iommu-groups]"
for consumer in 17003000.jpeg-encoder 17004000.jpeg-decoder; do
	group=$SYSFS_ROOT/bus/platform/devices/$consumer/iommu_group
	if [ -L "$group" ]; then
		printf '%s iommu_group=%s\n' "$consumer" \
			"$(basename "$(readlink "$group")")"
	else
		reject "$consumer has no IOMMU group"
	fi
done

echo "[jpeg-runtime-pm]"
# M4U's first consumer attachment ends with an asynchronous pm_runtime_put().
# Give that one shared transition a bounded interval; the other devices start
# suspended and never acquire a runtime-PM reference during probe.
tries=0
while ! all_runtime_suspended && [ "$tries" -lt 5 ]; do
	tries=$((tries + 1))
	sleep 1
done
for device in 10205000.iommu 14022000.smi 17001000.larb \
	17003000.jpeg-encoder 17004000.jpeg-decoder; do
	require_suspended "$device"
done

echo "[jpeg-irqs]"
if [ ! -r "$PROC_ROOT/interrupts" ]; then
	reject "interrupt registration state is unavailable"
else
	m4u_before=$(irq_count 10205000.iommu) || m4u_before=missing
	enc_before=$(irq_count 17003000.jpeg-encoder) || enc_before=missing
	dec_before=$(irq_count 17004000.jpeg-decoder) || dec_before=missing
	[ "$m4u_before" != missing ] ||
		reject "M4U SPI 156 handler 10205000.iommu is not registered"
	[ "$enc_before" != missing ] ||
		reject "JPEG encoder SPI 237 handler is not registered"
	[ "$dec_before" != missing ] ||
		reject "JPEG decoder SPI 239 handler is not registered"
	sleep 1
	m4u_after=$(irq_count 10205000.iommu) || m4u_after=missing
	enc_after=$(irq_count 17003000.jpeg-encoder) || enc_after=missing
	dec_after=$(irq_count 17004000.jpeg-decoder) || dec_after=missing
	for sample in \
		"10205000.iommu:$m4u_before:$m4u_after" \
		"17003000.jpeg-encoder:$enc_before:$enc_after" \
		"17004000.jpeg-decoder:$dec_before:$dec_after"; do
		name=${sample%%:*}
		counts=${sample#*:}
		before=${counts%%:*}
		after=${counts#*:}
		printf '%s before=%s after=%s\n' "$name" "$before" "$after"
		[ "$before" = "$after" ] ||
			reject "$name interrupt count changed without a JPEG job"
	done
fi

echo "[jpeg-clocks]"
for clock in mm_smi_common venc_0 venc_1 venc_2 venc_3; do
	clock_dir=$DEBUGFS_ROOT/clk/$clock
	enable_file=$clock_dir/clk_enable_count
	prepare_file=$clock_dir/clk_prepare_count
	if [ ! -r "$enable_file" ] || [ ! -r "$prepare_file" ]; then
		reject "$clock software counters are unavailable"
		continue
	fi
	enable_count=$(cat "$enable_file")
	prepare_count=$(cat "$prepare_file")
	printf '%s enable_count=%s prepare_count=%s\n' "$clock" \
		"$enable_count" "$prepare_count"
	case $enable_count:$prepare_count in
	:*|*:|*[!0-9:]*)
		reject "$clock software counters are malformed"
		continue
		;;
	esac
	[ "$enable_count" -eq 0 ] ||
		reject "$clock remains enabled before any JPEG job"
	[ "$prepare_count" -eq 0 ] ||
		reject "$clock remains prepared before any JPEG job"
done

echo "[jpeg-power-domains]"
if [ ! -r "$genpd_summary" ]; then
	reject "pm_genpd_summary is unavailable"
else
	for domain in mm venc; do
		if line=$(summary_line "$domain" "$genpd_summary"); then
			printf '%s\n' "$line"
			state=$(printf '%s\n' "$line" | awk '{ print $2 }')
			case $state in
			off*) ;;
			*) reject "$domain is not off before any JPEG job" ;;
			esac
		else
			reject "$domain is absent from pm_genpd_summary"
		fi
	done
fi

if [ "$failed" -ne 0 ]; then
	echo "JPEG_PREFLIGHT=REJECT"
	exit 1
fi

echo "JPEG_PREFLIGHT=PASS"
