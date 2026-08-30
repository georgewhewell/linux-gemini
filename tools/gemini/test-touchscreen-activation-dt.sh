#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Static contract test for the opt-in Gemini touchscreen activation trial.

set -eu

root=$(unset CDPATH; cd -- "$(dirname -- "$0")/../.." && pwd)
base=$root/arch/arm64/boot/dts/mediatek/mt6797-gemini-pda.dts
overlay=$root/arch/arm64/boot/dts/mediatek/mt6797-gemini-pda-touchscreen-activation.dtso
makefile=$root/arch/arm64/boot/dts/mediatek/Makefile
driver=$root/drivers/input/touchscreen/novatek-nt36xxx.c
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
need "$overlay" '/delete-property/ novatek,identity-only;'
reject_property "$overlay" 'reset-gpios'
reject_property "$overlay" 'vcc-supply'
reject_property "$overlay" 'iovcc-supply'
need "$makefile" 'mt6797-gemini-pda-touchscreen-activation.dtbo'
need "$binding" 'touchscreen-inverted-x:'
need "$binding" 'touchscreen-swapped-x-y:'
need "$driver" 'if (!ret)'
need "$driver" 'enable_irq(data->client->irq);'
need "$driver" 'Do not expose post-suspend I2C traffic'

echo "touchscreen activation overlay contract: PASS"
