#!/bin/sh
# Read-only snapshot of AP-side state that MD1 RF reception depends on.
# Run as root on the device while the radio is on (after AT+CFUN=1), then feed
# the output to check-cellular-rf-state.py on the host.
#
# Reads only: pinctrl debugfs "pins", single MT6351 registers through the
# pwrap regmap debugfs file (one 11-byte line per dd, so exactly one PMIC read
# per register and never a read of interrupt-status/clear-on-read registers),
# and regulator sysfs attributes.  Writes nothing.
set -eu

dbg=/sys/kernel/debug
pins=
for candidate in "$dbg"/pinctrl/*10005000*/pins; do
	if [ -r "$candidate" ]; then pins=$candidate; break; fi
done
echo "# capture-cellular-rf-state v1"
echo "# pins-file ${pins:-missing}"
if [ -n "$pins" ]; then
	# Modem-owned pins from the vendor DWS: SRCLKENA, SIM, RFIC BSI, MIPI RFFE, BPI.
	grep -E '^pin (69|10[0-3]|11[23]|12[6-8]|15[5-7]|18[3-9]|19[0-9]|20[0-9]|21[0-9]) ' "$pins" || true
fi

regs=$dbg/regmap/1000d000.pwrap/registers
echo "# pmic-file $regs"
if [ -r "$regs" ]; then
	# CON0/CON1 of VTCXO24 VTCXO28 VRF18 VRF12 VMIPI, then their VOSEL registers.
	for r in 0a04 0a06 0a08 0a0a 0a46 0a48 0a68 0a6a 0a7a 0a7c 0ac6 0ac8 0b08 0b0c 0b1c; do
		# Line format is "rrrr: vvvv\n" (16-bit reg/val), register stride 2.
		dd if="$regs" bs=11 skip=$((0x$r / 2)) count=1 2>/dev/null || echo "$r: read-failed"
	done
fi

echo "# regulators"
for d in /sys/class/regulator/regulator.*; do
	[ -r "$d/name" ] || continue
	# Undescribed MT6351 LDOs keep their upper-case descriptor names.
	name=$(tr '[:upper:]' '[:lower:]' < "$d/name")
	case "$name" in
	vtcxo24|vtcxo28|vrf18|vrf12|vmipi|vsim1|vsim2|va18|vio18|vxo22)
		uv=$(cat "$d/microvolts" 2>/dev/null || echo "?")
		st=$(cat "$d/state" 2>/dev/null || echo "?")
		echo "regulator $name microvolts=$uv state=$st"
		;;
	esac
done
