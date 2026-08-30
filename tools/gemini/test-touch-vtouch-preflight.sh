#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Fixture test for the read-only Gemini VTOUCH evidence collector.

set -eu

root=$(unset CDPATH; cd -- "$(dirname -- "$0")/../.." && pwd)
collector=$root/tools/gemini/touch-vtouch-preflight.sh
fixture=$(mktemp -d "$root/.touch-vtouch-preflight-fixture.XXXXXX")
trap 'rm -rf "$fixture"' EXIT HUP INT TERM

dt=$fixture/dt
mkdir -p "$dt/soc/touch@" "$dt/soc/i2c@11011000/cap_touch@62" \
	"$dt/pmic/regulator@7" "$dt/pio/eint@0" "$dt/pio/rstoutput0"
printf '\000\000\000\007' > "$dt/soc/touch@/vtouch-supply"
printf '\000\000\000\010' > "$dt/soc/touch@/pinctrl-1"
printf '\000\000\000\011' > "$dt/soc/touch@/pinctrl-4"
printf '\000\000\000\007' > "$dt/pmic/regulator@7/phandle"
printf '\000\000\000\010' > "$dt/pio/eint@0/linux,phandle"
printf '\000\000\000\011' > "$dt/pio/rstoutput0/phandle"
printf 'mock,pmic\000' > "$dt/pmic/regulator@7/compatible"
printf 'vtouch\000' > "$dt/pmic/regulator@7/regulator-name"
printf '\000*\271\260' > "$dt/pmic/regulator@7/regulator-min-microvolt"
printf '\000*\271\260' > "$dt/pmic/regulator@7/regulator-max-microvolt"
printf 'GPIO85__FUNC_EINT8\000' > "$dt/pio/eint@0/pins"
printf 'GPIO68__FUNC_GPIO68\000' > "$dt/pio/rstoutput0/pins"
printf 'mediatek,mt6797-touch\000' > "$dt/soc/touch@/compatible"
printf 'okay\000' > "$dt/soc/touch@/status"
printf 'mediatek,cap_touch\000' > "$dt/soc/i2c@11011000/cap_touch@62/compatible"
printf '\000\000\000b' > "$dt/soc/i2c@11011000/cap_touch@62/reg"

mkdir -p "$fixture/regulator" "$fixture/regulator-class/regulator.7" "$fixture/pinctrl/mtk-pinctrl"
printf 'vtouch 2800000uV enabled\n' > "$fixture/regulator/summary"
printf 'vtouch\n' > "$fixture/regulator-class/regulator.7/name"
printf '2800000\n' > "$fixture/regulator-class/regulator.7/microvolts"
printf 'enabled\n' > "$fixture/regulator-class/regulator.7/status"
printf 'gpio-68 (touch-reset) out lo\ngpio-85 (touch-irq) in hi\n' > "$fixture/gpio"
printf 'pin 68 (GPIO68): GPIO UNCLAIMED\npin 85 (GPIO85): GPIO UNCLAIMED\n' > "$fixture/pinctrl/mtk-pinctrl/pinmux-pins"

output=$(DT_ROOT="$dt" \
	REGULATOR_SUMMARY="$fixture/regulator/summary" \
	REGULATOR_CLASS="$fixture/regulator-class" \
	GPIO_DEBUG="$fixture/gpio" \
	PINCTRL_DEBUG="$fixture/pinctrl" \
	"$collector")

require()
{
	printf '%s\n' "$output" | grep -F -- "$1" > /dev/null || {
		echo "missing expected record: $1" >&2
		exit 1
	}
}

require 'schema=gemini-vtouch-evidence-v2'
require 'collector_mode=read_only'
require 'i2c_opened=no'
require 'vtouch_supply_phandle=00000007'
require "vtouch_supply_provider=$dt/pmic/regulator@7"
require 'touch_pinctrl_1_provider_pins=4750494f38355f5f46554e435f45494e543800'
require 'touch_pinctrl_4_provider_pins=4750494f36385f5f46554e435f4750494f363800'
require 'regulator_summary_vtouch=observed'
require 'gpio_debug_gpio68_85=observed'
require 'activation_gate=keep_touchscreen_disabled_until_live_vtouch_provider_and_reset_contract_are_reviewed'

echo "touch-vtouch-preflight fixtures: PASS"
