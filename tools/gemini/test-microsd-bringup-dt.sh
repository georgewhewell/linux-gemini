#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Validate both the dormant base DTB and the opt-in composed MicroSD DTB.
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 <mt6797-gemini-pda.dtb> <mt6797-gemini-pda-microsd-bringup.dtb>" >&2
	exit 2
fi

base=$1
dtb=$2
fdtget_bin=${FDTGET:-fdtget}
mmc=/mmc@11240000
pio=/pinctrl@10005000/microsd-mmc1-pins
regs=/pwrap@1000d000/pmic/regulators

require_prop() {
	file=$1
	path=$2
	prop=$3
	shift 3
	actual=$($fdtget_bin "$file" "$path" "$prop")
	if [ "$actual" != "$*" ]; then
		echo "$file:$path/$prop: got '$actual', want '$*'" >&2
		exit 1
	fi
}

require_flag() {
	path=$1
	prop=$2
	$fdtget_bin "$dtb" "$path" "$prop" >/dev/null
}

reject_prop() {
	path=$1
	prop=$2
	if $fdtget_bin "$dtb" "$path" "$prop" >/dev/null 2>&1; then
		echo "$dtb:$path/$prop must be absent" >&2
		exit 1
	fi
}

require_supply() {
	prop=$1
	reg_path=$2
	supply=$($fdtget_bin "$dtb" "$mmc" "$prop")
	reg_phandle=$($fdtget_bin "$dtb" "$reg_path" phandle)
	if [ "$supply" != "$reg_phandle" ]; then
		echo "$dtb:$mmc/$prop does not reference $reg_path" >&2
		exit 1
	fi
}

require_prop "$base" "$mmc" status disabled

require_prop "$dtb" "$mmc" status okay
require_prop "$dtb" "$mmc" bus-width 4
require_prop "$dtb" "$mmc" max-frequency 400000

set -- $($fdtget_bin "$dtb" "$mmc" cd-gpios)
if [ "$#" -ne 3 ] || [ "$2" != 67 ] || [ "$3" != 0 ]; then
	echo "$dtb:$mmc/cd-gpios: want <&pio 67 GPIO_ACTIVE_HIGH>, got '$*'" >&2
	exit 1
fi
reject_prop "$mmc" cd-inverted

set -- $($fdtget_bin "$dtb" "$mmc" assigned-clocks)
test "$#" -eq 2 && test "$2" -eq 18
set -- $($fdtget_bin "$dtb" "$mmc" assigned-clock-parents)
test "$#" -eq 2 && test "$2" -eq 92

require_prop "$dtb" "$regs/ldo-vmch" regulator-min-microvolt 3000000
require_prop "$dtb" "$regs/ldo-vmch" regulator-max-microvolt 3000000
require_prop "$dtb" "$regs/ldo-vmc" regulator-min-microvolt 3000000
require_prop "$dtb" "$regs/ldo-vmc" regulator-max-microvolt 3000000
require_supply vmmc-supply "$regs/ldo-vmch"
require_supply vqmmc-supply "$regs/ldo-vmc"

require_prop "$dtb" "$pio/pins-cmd-dat" pinmux \
	33025 33281 33537 33793 34049
require_prop "$dtb" "$pio/pins-cmd-dat" bias-pull-up 101
require_prop "$dtb" "$pio/pins-cmd-dat" drive-strength 8
require_flag "$pio/pins-cmd-dat" input-enable
require_flag "$pio/pins-cmd-dat" input-schmitt-enable
require_prop "$dtb" "$pio/pins-clk" pinmux 34305
require_prop "$dtb" "$pio/pins-clk" bias-pull-down 102
require_prop "$dtb" "$pio/pins-clk" drive-strength 8
require_flag "$pio/pins-clk" input-enable
require_flag "$pio/pins-clk" input-schmitt-enable
require_prop "$dtb" "$pio/pins-cd" pinmux 17153
require_flag "$pio/pins-cd" input-enable
require_flag "$pio/pins-cd" input-schmitt-enable

for prop in cap-sd-highspeed sd-uhs-sdr12 sd-uhs-sdr25 sd-uhs-sdr50 \
	    sd-uhs-sdr104 sd-uhs-ddr50; do
	reject_prop "$mmc" "$prop"
done

echo "MicroSD DT contract valid: base dormant; opt-in 4-bit, 400 kHz, fixed 3.0 V."
