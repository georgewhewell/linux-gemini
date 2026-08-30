#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Static contract test for the deliberately opt-in Gemini NT36772 overlay.

set -eu

root=$(unset CDPATH; cd -- "$(dirname -- "$0")/../.." && pwd)
base=$root/arch/arm64/boot/dts/mediatek/mt6797-gemini-pda.dts
overlay=$root/arch/arm64/boot/dts/mediatek/mt6797-gemini-pda-touchscreen-identity.dtso
makefile=$root/arch/arm64/boot/dts/mediatek/Makefile
driver=$root/drivers/input/touchscreen/novatek-nt36xxx.c
pmic=$root/drivers/regulator/mt6351-regulator.c
binding=$root/Documentation/devicetree/bindings/input/touchscreen/novatek,nt36xxx.yaml

need()
{
	grep -F -- "$2" "$1" >/dev/null || {
		echo "missing $2 in $1" >&2
		exit 1
	}
}

reject_property()
{
	if grep -E -- "^[[:space:]]*$2[[:space:]]*=" "$1" >/dev/null; then
		echo "unsafe $2 in $1" >&2
		exit 1
	fi
}

need "$base" 'status = "disabled";'
test "$(grep -Fc 'status = "disabled";' "$base")" -ge 2
need "$overlay" '&i2c4 {'
need "$overlay" '&touchscreen {'
test "$(grep -Fc 'status = "okay";' "$overlay")" -eq 2
need "$overlay" 'novatek,identity-only;'
need "$overlay" 'five 0x62 reset/xdata attempts'
need "$overlay" '100-iteration'
reject_property "$overlay" 'reset-gpios'
reject_property "$overlay" 'vcc-supply'
reject_property "$overlay" 'iovcc-supply'
need "$makefile" 'mt6797-gemini-pda-touchscreen-identity.dtbo'
need "$driver" 'NVT36XXX_RESET_RETRIES'
need "$driver" 'for (i = 0; i < 100; i++)'
need "$driver" 'device_property_read_bool(dev, "novatek,identity-only")'
need "$driver" 'keeping input and IRQ quiescent'
need "$driver" 'MODULE_DESCRIPTION("Novatek legacy NT36xxx touchscreen driver")'
need "$pmic" 'MT6351_FIXED("ldo-vldo28", VLDO28'
need "$binding" 'novatek,identity-only:'
need "$binding" 'Recovery-image bring-up mode.'

echo "touchscreen identity overlay contract: PASS"
