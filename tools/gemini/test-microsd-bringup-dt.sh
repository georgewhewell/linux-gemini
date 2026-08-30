#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Validate the compiled, composed Gemini MicroSD bring-up DTB.
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 <mt6797-gemini-pda-microsd-bringup.dtb>" >&2
	exit 2
fi

dtb=$1
fdtget_bin=${FDTGET:-fdtget}

require_prop() {
	path=$1
	prop=$2
	shift 2
	actual=$($fdtget_bin "$dtb" "$path" "$prop")
	if [ "$actual" != "$*" ]; then
		echo "$path/$prop: got '$actual', want '$*'" >&2
		exit 1
	fi
}

require_hex_prop() {
	path=$1
	prop=$2
	want=$3
	actual=$($fdtget_bin -t x "$dtb" "$path" "$prop")
	if [ "$actual" != "$want" ]; then
		echo "$path/$prop: got '$actual', want '$want'" >&2
		exit 1
	fi
}

reject_prop() {
	path=$1
	prop=$2
	if $fdtget_bin "$dtb" "$path" "$prop" >/dev/null 2>&1; then
		echo "$path/$prop must be absent" >&2
		exit 1
	fi
}

require_gpio_cd() {
	set -- $($fdtget_bin "$dtb" "$mmc" cd-gpios)
	if [ "$#" -ne 3 ] || [ "$2" != 67 ] || [ "$3" != 0 ]; then
		echo "$mmc/cd-gpios: want <&pio 67 GPIO_ACTIVE_HIGH>, got '$*'" >&2
		exit 1
	fi
}

require_supply() {
	prop=$1
	reg_path=$2
	supply=$($fdtget_bin "$dtb" "$mmc" "$prop")
	reg_phandle=$($fdtget_bin "$dtb" "$reg_path" phandle)
	if [ "$supply" != "$reg_phandle" ]; then
		echo "$mmc/$prop does not reference $reg_path" >&2
		exit 1
	fi
}

mmc=/mmc@11240000
regs=/pwrap@1000d000/pmic/regulators
cd_pins=/pinctrl@10005000/microsd-cd-pin/pins-cd

require_prop "$mmc" status okay
require_prop "$mmc" bus-width 4
require_prop "$mmc" max-frequency 25000000
# GPIO_ACTIVE_HIGH is zero.  No cd-inverted means high is card present.
require_gpio_cd
reject_prop "$mmc" cd-inverted
require_prop "$mmc" no-1-8-v
# MT6797_GPIO67__FUNC_GPIO67, not the distinct EINT6 mux function (0x4301).
require_hex_prop "$cd_pins" pinmux 4300

require_prop "$regs/ldo-vmch" regulator-min-microvolt 3000000
require_prop "$regs/ldo-vmch" regulator-max-microvolt 3000000
require_prop "$regs/ldo-vmc" regulator-min-microvolt 3000000
require_prop "$regs/ldo-vmc" regulator-max-microvolt 3000000
reject_prop "$regs/ldo-vmch" regulator-boot-on
reject_prop "$regs/ldo-vmch" regulator-always-on
reject_prop "$regs/ldo-vmc" regulator-boot-on
reject_prop "$regs/ldo-vmc" regulator-always-on
require_supply vmmc-supply "$regs/ldo-vmch"
require_supply vqmmc-supply "$regs/ldo-vmc"

# No high-speed/1.8 V advertisement can slip into this first electrical test.
reject_prop "$mmc" cap-sd-highspeed
reject_prop "$mmc" sd-uhs-sdr12
reject_prop "$mmc" sd-uhs-sdr25
reject_prop "$mmc" sd-uhs-sdr50
reject_prop "$mmc" sd-uhs-sdr104
reject_prop "$mmc" sd-uhs-ddr50

echo "MicroSD MMC1 composed-DT contract is valid: 4-bit, 25 MHz legacy SD, fixed 3.0 V."
