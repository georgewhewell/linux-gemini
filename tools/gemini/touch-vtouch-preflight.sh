#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Read-only touchscreen contract collector for Planet Computers Gemini vendor
# boots.
#
# This deliberately never opens /dev/i2c-*, requests a GPIO, loads a module, or
# writes sysfs/debugfs/devmem.  It only reads the live flattened DT and optional
# debugfs/sysfs reports already exposed by the running vendor kernel.

set -eu

dt_root=${DT_ROOT:-/sys/firmware/devicetree/base}
touch_node=${TOUCH_NODE:-$dt_root/soc/touch@}
touch_i2c_node=${TOUCH_I2C_NODE:-$dt_root/soc/i2c@11011000/cap_touch@62}
regulator_summary=${REGULATOR_SUMMARY:-/sys/kernel/debug/regulator/regulator_summary}
regulator_class=${REGULATOR_CLASS:-/sys/class/regulator}
gpio_debug=${GPIO_DEBUG:-/sys/kernel/debug/gpio}
pinctrl_debug=${PINCTRL_DEBUG:-/sys/kernel/debug/pinctrl}
# Optional, already-captured vendor kernel log.  It preserves an identity the
# vendor driver logged; it never causes an I2C identity transaction itself.
kernel_log=${KERNEL_LOG:-}

die()
{
	echo "touch-vtouch-preflight: $*" >&2
	exit 1
}

property_hex()
{
	hex_key=$1
	hex_property=$2

	if [ -r "$hex_property" ]; then
		printf '%s=' "$hex_key"
		od -An -v -tx1 "$hex_property" | tr -d ' \n'
		printf '\n'
	else
		printf '%s=unknown\n' "$hex_key"
	fi
}

property_string()
{
	string_key=$1
	string_property=$2

	if [ -r "$string_property" ]; then
		printf '%s=' "$string_key"
		tr '\000' ' ' < "$string_property" | tr '\n' ' '
		printf '\n'
	else
		printf '%s=unknown\n' "$string_key"
	fi
}

resolve_phandle()
{
	resolve_key=$1
	resolve_property=$2
	provider_found=false

	if [ ! -r "$resolve_property" ]; then
		printf '%s_provider=unknown\n' "$resolve_key"
		return
	fi

	# DT node names cannot contain newlines. This loop reads only DT files.
	while IFS= read -r node; do
		for phandle in phandle linux,phandle; do
			[ -r "$node/$phandle" ] || continue
			cmp -s "$resolve_property" "$node/$phandle" || continue

			provider_found=true
			printf '%s_provider=%s\n' "$resolve_key" "$node"
			property_hex "${resolve_key}_provider_phandle" "$node/$phandle"
			property_hex "${resolve_key}_provider_compatible" "$node/compatible"
			property_string "${resolve_key}_provider_status" "$node/status"
			property_string "${resolve_key}_provider_name" "$node/regulator-name"
			property_hex "${resolve_key}_provider_min_uv" "$node/regulator-min-microvolt"
			property_hex "${resolve_key}_provider_max_uv" "$node/regulator-max-microvolt"
			property_hex "${resolve_key}_provider_pins" "$node/pins"
			property_hex "${resolve_key}_provider_pinmux" "$node/pinmux"
			# A flattened node may expose both aliases with identical bytes.
			# Report that provider once, not once per phandle property name.
			break
		done
	done <<-EOF
$(find "$dt_root" -type d -print)
EOF

	if [ "$provider_found" = false ]; then
		printf '%s_provider=unresolved\n' "$resolve_key"
	fi
}

report_matches()
{
	report_key=$1
	report_path=$2
	report_pattern=$3

	if [ ! -r "$report_path" ]; then
		printf '%s=unavailable\n' "$report_key"
		return
	fi

	matches=$(grep -Ei "$report_pattern" "$report_path" || true)
	if [ -n "$matches" ]; then
		printf '%s=observed\n' "$report_key"
		printf '%s_begin\n%s\n%s_end\n' "$report_key" "$matches" "$report_key"
	else
		printf '%s=none\n' "$report_key"
	fi
}

report_vldo28_sysfs()
{
	if [ ! -d "$regulator_class" ]; then
		echo "regulator_sysfs_vldo28=unavailable"
		return
	fi

	found=false
	for node in "$regulator_class"/*; do
		[ -d "$node" ] || continue
		[ -r "$node/name" ] || continue
		name=$(tr -d '\000\n' < "$node/name")
		case "$name" in
		[Vv][Ll][Dd][Oo]28|[Ll][Dd][Oo]-[Vv][Ll][Dd][Oo]28)
			found=true
			printf 'regulator_sysfs_vldo28_name=%s\n' "$name"
			property_string regulator_sysfs_vldo28_microvolts "$node/microvolts"
			property_string regulator_sysfs_vldo28_status "$node/status"
			;;
		esac
	done

	if [ "$found" = false ]; then
		echo "regulator_sysfs_vldo28=none"
	fi
}

report_vendor_identity_log()
{
	if [ -z "$kernel_log" ]; then
		echo "vendor_identity_log=unavailable"
		return
	fi

	report_matches vendor_identity_log "$kernel_log" \
		'nvt_ts_check_chip_ver_trim|nvt_read_pid|nvt_ts_probe.*(IC FW|request irq|end)|nvt_irq_registration|nvt_local_init|tpd_driver_name'
}

[ -d "$dt_root" ] || die "missing flattened DT root: $dt_root"
[ -d "$touch_node" ] || die "missing vendor touch pseudo-node: $touch_node"
[ -d "$touch_i2c_node" ] || die "missing vendor touch I2C node: $touch_i2c_node"

echo "schema=gemini-touch-contract-evidence-v3"
echo "collector_mode=read_only"
echo "i2c_opened=no"
echo "gpio_requested=no"
echo "regulator_changed=no"
echo "sysfs_debugfs_writes=no"
echo "identity_read_by_collector=no"
echo "touch_node=$touch_node"
echo "touch_i2c_node=$touch_i2c_node"
property_hex touch_compatible "$touch_node/compatible"
property_string touch_status "$touch_node/status"
property_hex touch_i2c_compatible "$touch_i2c_node/compatible"
property_hex touch_i2c_reg "$touch_i2c_node/reg"

# Source-derived expectations, deliberately distinct from live observations.
echo "vendor_irq_expected_gpio=85"
echo "vendor_irq_expected_function=EINT8"
echo "vendor_touch_vdd_expected_provider=mt6351_vldo28"
echo "vendor_touch_vdd_expected_uv=2800000"
echo "vendor_reset_expected_gpio=68"
echo "vendor_reset_polarity=unknown"
echo "vendor_reset_timing=unknown"

property_hex vtouch_supply_phandle "$touch_node/vtouch-supply"
resolve_phandle vtouch_supply "$touch_node/vtouch-supply"

for index in 0 1 2 3 4 5; do
	property_hex "touch_pinctrl_${index}_phandle" "$touch_node/pinctrl-$index"
	resolve_phandle "touch_pinctrl_${index}" "$touch_node/pinctrl-$index"
done

report_matches regulator_summary_vldo28 "$regulator_summary" '(^|[^[:alnum:]_])[Vv][Ll][Dd][Oo]28([^[:alnum:]_]|$)'
report_vldo28_sysfs
report_matches gpio_debug_gpio68_85 "$gpio_debug" 'gpio-?(68|85)([^0-9]|$)'
echo "reset_gpio68_observation=snapshot_only_not_a_polarity_or_timing_contract"
report_vendor_identity_log
echo "identity_gate=apply_identity_overlay_only_after_vldo28_is_observed_and_vendor_client_is_unbound"

if [ -d "$pinctrl_debug" ]; then
	found=false
	for node in "$pinctrl_debug"/*; do
		[ -d "$node" ] || continue
		for report in "$node"/pinmux-pins "$node"/pins; do
			[ -r "$report" ] || continue
			found=true
			report_matches "pinctrl_$(basename "$node")_$(basename "$report")_gpio68_85" \
				"$report" 'pin ?(68|85)([^0-9]|$)|gpio-?(68|85)([^0-9]|$)'
		done
	done
	[ "$found" = true ] || echo "pinctrl_debug=unavailable"
else
	echo "pinctrl_debug=unavailable"
fi

echo "activation_gate=base_disabled_identity_overlay_requires_recovery_image_live_vldo28_and_no_vendor_client"
