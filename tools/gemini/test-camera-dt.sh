#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Check both the dormant Gemini base DTB and its complete camera composition.

set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 <mt6797-gemini-pda.dtb> <mt6797-gemini-pda-camera.dtb>" >&2
	exit 2
fi

base=$1
camera=$2
: "${FDTGET:=fdtget}"

require_equal()
{
	actual=$1
	expected=$2
	description=$3

	if [ "$actual" != "$expected" ]; then
		echo "$description: got '$actual', expected '$expected'" >&2
		exit 1
	fi
}

i2c=/i2c@11014000
sensor=$i2c/camera@20
seninf=/csi@1a040000
seninf_in=$seninf/ports/port@0/endpoint
seninf_out=$seninf/ports/port@1/endpoint
camsv=/camsv@1a050000
camsv_in=$camsv/port/endpoint
larb=/larb@1a001000
pio=/pinctrl@10005000
calibration=/firmware/atag-devinfo/csi-calibration-data@64
vcama=/pwrap@1000d000/pmic/regulators/ldo-vcama
vcamd=/pwrap@1000d000/pmic/regulators/ldo-vcamd
vcamio=/pwrap@1000d000/pmic/regulators/ldo-vcamio

if "$FDTGET" "$base" "$sensor" compatible >/dev/null 2>&1; then
	echo "base board must not instantiate the camera sensor" >&2
	exit 1
fi
require_equal "$("$FDTGET" "$base" "$i2c" status)" disabled \
	"base I2C3 status"
require_equal "$("$FDTGET" "$base" "$larb" status)" disabled \
	"base LARB2 status"
require_equal "$("$FDTGET" "$base" "$seninf" status)" disabled \
	"base SENINF status"
require_equal "$("$FDTGET" "$base" "$camsv" status)" disabled \
	"base CAMSV0 status"

require_equal "$("$FDTGET" "$camera" "$sensor" compatible)" \
	siliconfile,sp5509 "front sensor compatible"
require_equal "$("$FDTGET" -t x "$camera" "$sensor" reg)" 20 \
	"front sensor address"
require_equal "$("$FDTGET" -t u "$camera" "$i2c" clock-frequency)" \
	300000 "I2C3 frequency"
require_equal "$("$FDTGET" "$camera" "$i2c" status)" okay \
	"I2C3 status"
require_equal "$("$FDTGET" "$camera" "$larb" status)" okay \
	"LARB2 status"
require_equal "$("$FDTGET" "$camera" "$seninf" status)" okay \
	"SENINF status"
require_equal "$("$FDTGET" "$camera" "$camsv" status)" okay \
	"CAMSV0 status"
require_equal "$("$FDTGET" -t u "$camera" "$sensor" orientation)" 0 \
	"front sensor orientation"
require_equal "$("$FDTGET" -t u "$camera" "$sensor" rotation)" 180 \
	"front sensor rotation"
require_equal "$("$FDTGET" -t u "$camera" "$sensor" reset-gpios | cut -d' ' -f2-)" \
	"33 1" "front sensor reset GPIO"
require_equal "$("$FDTGET" -t u "$camera" "$sensor" powerdown-gpios | cut -d' ' -f2-)" \
	"29 1" "front sensor power-down GPIO"
require_equal "$("$FDTGET" "$camera" "$sensor" clock-names)" clk \
	"front sensor clock name"
require_equal "$("$FDTGET" -t x "$camera" "$sensor" clocks)" \
	"$("$FDTGET" -t x "$camera" "$seninf" phandle)" \
	"front sensor MCLK provider"
require_equal "$("$FDTGET" -t x "$camera" "$sensor" dovdd-supply)" \
	"$("$FDTGET" -t x "$camera" "$vcamio" phandle)" \
	"front sensor DOVDD provider"
require_equal "$("$FDTGET" -t x "$camera" "$sensor" avdd-supply)" \
	"$("$FDTGET" -t x "$camera" "$vcama" phandle)" \
	"front sensor AVDD provider"
require_equal "$("$FDTGET" -t x "$camera" "$sensor" dvdd-supply)" \
	"$("$FDTGET" -t x "$camera" "$vcamd" phandle)" \
	"front sensor DVDD provider"
require_equal "$("$FDTGET" -t u "$camera" "$sensor" data-lanes 2>/dev/null || true)" \
	"" "sensor must carry lanes on its endpoint"
require_equal "$("$FDTGET" -t u "$camera" "$sensor/port/endpoint" data-lanes)" \
	"1 2" "sensor CSI data lanes"
require_equal "$("$FDTGET" -t x "$camera" "$sensor/port/endpoint" link-frequencies)" \
	"0 1a39de00 0 d1cef00 0 68e7780" "sensor CSI link frequencies"
require_equal "$("$FDTGET" -t u "$camera" "$seninf_in" data-lanes)" \
	"1 2" "SENINF CSI data lanes"

sensor_ep=$("$FDTGET" -t x "$camera" "$sensor/port/endpoint" phandle)
seninf_in_ep=$("$FDTGET" -t x "$camera" "$seninf_in" phandle)
seninf_out_ep=$("$FDTGET" -t x "$camera" "$seninf_out" phandle)
camsv_ep=$("$FDTGET" -t x "$camera" "$camsv_in" phandle)
require_equal "$("$FDTGET" -t x "$camera" "$sensor/port/endpoint" remote-endpoint)" \
	"$seninf_in_ep" "sensor to SENINF link"
require_equal "$("$FDTGET" -t x "$camera" "$seninf_in" remote-endpoint)" \
	"$sensor_ep" "SENINF to sensor link"
require_equal "$("$FDTGET" -t x "$camera" "$seninf_out" remote-endpoint)" \
	"$camsv_ep" "SENINF to CAMSV0 link"
require_equal "$("$FDTGET" -t x "$camera" "$camsv_in" remote-endpoint)" \
	"$seninf_out_ep" "CAMSV0 to SENINF link"

require_equal "$("$FDTGET" "$camera" "$seninf" nvmem-cell-names)" \
	csi-calibration-data "CSI calibration cell name"
require_equal "$("$FDTGET" -t x "$camera" "$seninf" nvmem-cells)" \
	"$("$FDTGET" -t x "$camera" "$calibration" phandle)" \
	"CSI calibration provider"
require_equal "$("$FDTGET" -t x "$camera" "$calibration" reg)" "64 14" \
	"CSI calibration extent"

camera_nodes=$("$FDTGET" -l "$camera" "$i2c" | grep '^camera@' || true)
require_equal "$camera_nodes" camera@20 "sole populated camera"

if ! "$FDTGET" -l "$camera" "$pio" | grep -qx front-camera-pins; then
	echo "front camera pinctrl state is missing" >&2
	exit 1
fi
require_equal "$("$FDTGET" -t x "$camera" \
	"$pio/front-camera-pins/pins-mclk" pinmux)" 1f01 \
	"front camera MCLK2 pinmux"
require_equal "$("$FDTGET" -t x "$camera" \
	"$pio/front-camera-pins/pins-control" pinmux)" "2100 1d00" \
	"front camera control pinmux"

echo "Gemini camera DT: PASS"
