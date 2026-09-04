#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read-only MT6797 CMDQ/SMI/IOMMU provider qualification; submits no jobs.

set -eu

if [ "$#" -ne 0 ]; then
	echo "usage: $0" >&2
	exit 2
fi

: "${DEBUGFS_ROOT:=/sys/kernel/debug}"
: "${DMESG:=dmesg}"
: "${PROC_ROOT:=/proc}"
: "${SYSFS_ROOT:=/sys}"

clk_summary=$DEBUGFS_ROOT/clk/clk_summary
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
	device=$SYSFS_ROOT/bus/platform/devices/$address
	status_file=$device/power/runtime_status
	status=missing
	tries=0

	if [ ! -r "$status_file" ]; then
		reject "$address has no runtime-PM status"
		return
	fi
	while [ "$tries" -lt 5 ]; do
		status=$(cat "$status_file")
		[ "$status" = suspended ] && break
		tries=$((tries + 1))
		sleep 1
	done
	printf '%s runtime_status=%s\n' "$address" "$status"
	[ "$status" = suspended ] ||
		reject "$address did not settle in runtime suspend"
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

echo "[media-fabric-kernel-log]"
boot_log=$($DMESG) || {
	echo "cannot read the kernel log" >&2
	exit 2
}
fabric_addresses='10205000\.(iommu|m4u)|10212000\.(mailbox|gce)'
fabric_addresses="$fabric_addresses|14022000\.(smi|smi_common)"
fabric_addresses="$fabric_addresses|16010000\.larb|17001000\.larb"
fabric_log=$(printf '%s\n' "$boot_log" |
	grep -E "$fabric_addresses|mtk[_-](cmdq|iommu|smi)|M4U|CMDQ" || true)
printf '%s\n' "$fabric_log"

reject_pattern='failed to (prepare GCE clocks|initialize GCE|register ISR|register mailbox)'
reject_pattern="$reject_pattern|Failed @ IRQ|mm dts parse fail|Unable to link"
reject_pattern="$reject_pattern|Failed to get the smi_common device"
reject_pattern="$reject_pattern|translation fault|iommu fault|TLB.*fault|suspend GCE thread.*failed"
if printf '%s\n' "$fabric_log" | grep -Eiq "$reject_pattern"; then
	reject "kernel log contains a CMDQ, SMI, or IOMMU provider failure"
fi

echo "[provider-bindings]"
require_driver 10212000.mailbox mtk_cmdq
require_driver 10205000.iommu mtk-iommu
require_driver 14022000.smi mtk-smi-common
for larb in 14020000.larb 14021000.larb 16010000.larb 17001000.larb; do
	require_driver "$larb" mtk-smi-larb
done

if [ -r "$DEBUGFS_ROOT/devices_deferred" ]; then
	deferred=$(grep -E '10205000|10212000|14020000|14021000|14022000|16010000|17001000' \
		"$DEBUGFS_ROOT/devices_deferred" || true)
	if [ -n "$deferred" ]; then
		printf '%s\n' "$deferred"
		reject "a multimedia fabric provider remains deferred"
	fi
fi

echo "[runtime-pm]"
for provider in 10212000.mailbox 10205000.iommu 14022000.smi \
	14020000.larb 14021000.larb 16010000.larb 17001000.larb; do
	require_suspended "$provider"
done

echo "[provider-irqs]"
if [ ! -r "$PROC_ROOT/interrupts" ]; then
	reject "interrupt registration state is unavailable"
else
	cmdq_before=missing
	cmdq_after=missing
	m4u_before=missing
	m4u_after=missing
	if cmdq_before=$(irq_count mtk_cmdq); then
		:
	else
		reject "CMDQ SPI 152 handler mtk_cmdq is not registered"
	fi
	if m4u_before=$(irq_count 10205000.iommu); then
		:
	else
		reject "M4U SPI 156 handler 10205000.iommu is not registered"
	fi
	sleep 1
	cmdq_after=$(irq_count mtk_cmdq) || cmdq_after=missing
	m4u_after=$(irq_count 10205000.iommu) || m4u_after=missing
	printf 'mtk_cmdq before=%s after=%s\n' "$cmdq_before" "$cmdq_after"
	[ "$cmdq_before" = "$cmdq_after" ] ||
		reject "CMDQ interrupt count changed without a submitted job"
	printf '10205000.iommu before=%s after=%s\n' "$m4u_before" "$m4u_after"
	[ "$m4u_before" = "$m4u_after" ] ||
		reject "M4U interrupt count changed without a DMA consumer"
fi

echo "[provider-clocks]"
if [ ! -r "$clk_summary" ]; then
	reject "clk_summary is unavailable"
else
	for clock in infra_gce mm_smi_common mm_smi_larb0 mm_smi_larb5 \
		vdec_cken vdec_larb1_cken venc_0 venc_1; do
		if line=$(summary_line "$clock" "$clk_summary"); then
			printf '%s\n' "$line"
			enable_count=$(printf '%s\n' "$line" | awk '{ print $2 }')
			[ "$enable_count" -eq 0 ] ||
				reject "$clock remains enabled without a media consumer"
		else
			reject "$clock is absent from clk_summary"
		fi
	done
fi

echo "[media-power-domains]"
if [ ! -r "$genpd_summary" ]; then
	reject "pm_genpd_summary is unavailable"
else
	for domain in mm vdec venc; do
		if line=$(summary_line "$domain" "$genpd_summary"); then
			printf '%s\n' "$line"
			state=$(printf '%s\n' "$line" | awk '{ print $2 }')
			case $state in
			off*) ;;
			*) reject "$domain is not off after provider probe" ;;
			esac
		else
			reject "$domain is absent from pm_genpd_summary"
		fi
	done
fi

echo "[disabled-consumers]"
for consumer in 1400b000.ovl 1400d000.ovl 1400e000.ovl 1400f000.rdma \
	17002000.video-codec 17003000.jpeg-encoder 17004000.jpeg-decoder; do
	if [ -e "$SYSFS_ROOT/bus/platform/devices/$consumer" ]; then
		reject "$consumer was instantiated by the provider-only overlay"
	else
		printf '%s absent\n' "$consumer"
	fi
done

if [ "$failed" -ne 0 ]; then
	echo "MEDIA_FABRIC_PREFLIGHT=REJECT"
	exit 1
fi

echo "MEDIA_FABRIC_PREFLIGHT=PASS"
