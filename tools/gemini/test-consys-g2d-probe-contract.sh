#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Static contract test for the G2d pre-stage DT dependencies and attribution.

set -eu

root=$(unset CDPATH; cd -- "$(dirname -- "$0")/../.." && pwd)
overlay=$root/arch/arm64/boot/dts/mediatek/mt6797-gemini-consys-g2c-patch-only.dtso
driver=$root/drivers/soc/mediatek/mt6797-consys-g2d-postpatch-owner.c

need()
{
	grep -F -- "$2" "$1" >/dev/null || {
		echo "missing $2 in $1" >&2
		exit 1
	}
}

need "$overlay" 'mediatek,emi-mpu = <&consys_emi_mpu>;'
need "$overlay" 'vcn33-bt-supply = <&mt6351_vcn33_bt_reg>;'
need "$overlay" 'vcn33-wifi-supply = <&mt6351_vcn33_wifi_reg>;'
need "$driver" 'regulator_is_equal(owner->bt, owner->wifi)'
need "$driver" 'G2d probe %s failed\n'
need "$driver" '"persistent EMI claim"'
need "$driver" '"controller start or post-patch stage"'
need "$overlay" 'mediatek,experimental-g2d-gnss-probe;'
need "$overlay" 'MT6797_GPIO69__FUNC_GPIO69'
need "$driver" 'device_property_read_bool(&pdev->dev,'
need "$driver" 'READ_ONCE(enable_experimental_g2d_gnss_probe)'
need "$driver" 'g2d_gnss_on[] = { MT6797_G2D_WMT_GNSS_TYPE, 1 }'
need "$driver" 'g2d_gnss_off[] = { MT6797_G2D_WMT_GNSS_TYPE, 0 }'
need "$driver" 'MT6797_G2D_WMT_FUNC_CTRL'

echo "G2d probe dependency and attribution contract: PASS"
