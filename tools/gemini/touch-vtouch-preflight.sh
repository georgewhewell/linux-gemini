#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Read-only VTOUCH preflight for Planet Computers Gemini PDA vendor boots.
#
# Print to stdout and redirect to persistent storage if a record is wanted.
# This script neither loads a module nor opens an I2C device; it only reads
# the live flattened device tree and, when available, regulator debugfs.

set -eu

dt_root=${DT_ROOT:-/sys/firmware/devicetree/base}
regulator_summary=${REGULATOR_SUMMARY:-/sys/kernel/debug/regulator/regulator_summary}
touch_node=$dt_root/soc/touch@
touch_i2c_node=$dt_root/soc/i2c@11011000/cap_touch@62
vtouch_supply=$touch_node/vtouch-supply

die()
{
	echo "touch-vtouch-preflight: $*" >&2
	exit 1
}

property_hex()
{
	key=$1
	property=$2

	[ -r "$property" ] || return 0
	printf '%s=' "$key"
	od -An -v -tx1 "$property" | tr -d ' \n'
	printf '\n'
}

[ -d "$touch_node" ] || die "missing vendor touch pseudo-node: $touch_node"
[ -d "$touch_i2c_node" ] || die "missing vendor touch I2C node: $touch_i2c_node"
[ -r "$vtouch_supply" ] || die "missing vtouch-supply: $vtouch_supply"

echo "touch_node=$touch_node"
echo "touch_i2c_node=$touch_i2c_node"
property_hex touch_compatible "$touch_node/compatible"
property_hex touch_status "$touch_node/status"
property_hex touch_i2c_compatible "$touch_i2c_node/compatible"
property_hex touch_i2c_reg "$touch_i2c_node/reg"
property_hex vtouch_supply_phandle "$vtouch_supply"

provider_found=false
while IFS= read -r node; do
	for phandle in phandle linux,phandle; do
		[ -r "$node/$phandle" ] || continue
		cmp -s "$vtouch_supply" "$node/$phandle" || continue

		provider_found=true
		echo "vtouch_provider=$node"
		property_hex vtouch_provider_compatible "$node/compatible"
		property_hex vtouch_provider_status "$node/status"
		property_hex vtouch_provider_name "$node/regulator-name"
		property_hex vtouch_provider_min_uv "$node/regulator-min-microvolt"
		property_hex vtouch_provider_max_uv "$node/regulator-max-microvolt"
		done
done <<-EOF
$(find "$dt_root" -type d)
EOF

if [ "$provider_found" = false ]; then
	echo "vtouch_provider=unresolved"
fi

if [ -r "$regulator_summary" ]; then
	echo "regulator_summary_vtouch:"
	grep -i vtouch "$regulator_summary" || true
else
	echo "regulator_summary_vtouch=unavailable"
fi

echo "activation_gate=keep_touchscreen_disabled_until_vtouch_provider_is_mapped"
echo "activation_gate_detail=provider_must_be_a_supported_mainline_regulator"
