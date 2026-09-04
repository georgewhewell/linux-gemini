#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read existing sensor interfaces; never enable devices or alter thermal policy.
set -u
if [ "$#" -ne 0 ]; then
	echo 'usage: sh thermal-oneboot-inventory.sh' >&2
	exit 2
fi
read_attr()
{
	[ -f "$1" ] || return 0
	printf '%s=' "$1"
	if ! cat "$1"; then
		printf 'READ_ERROR\n'
	fi
}
printf 'THERMAL_INVENTORY_BEGIN units=thermal_hwmon_millicelsius,power_supply_decicelsius,iio_driver_defined\n'
printf 'battery_temperature=hardware_disconnected owner_attested=plus_minus_only\n'
printf 'battery_reported_values=not_verified_physical_temperature\n'
read_attr /proc/sys/kernel/random/boot_id
read_attr /proc/uptime
zones=0
for zone in /sys/class/thermal/thermal_zone*; do
	[ -d "$zone" ] || continue
	zones=$((zones + 1))
	for attr in type temp mode policy trip_point_*_type trip_point_*_temp trip_point_*_hyst; do
		for path in "$zone"/$attr; do read_attr "$path"; done
	done
	for link in "$zone"/cdev[0-9]*; do
		[ -L "$link" ] || continue
		printf '%s -> %s\n' "$link" "$(readlink "$link")"
	done
done
for dev in /sys/class/thermal/cooling_device*; do
	[ -d "$dev" ] || continue
	for attr in type cur_state max_state; do read_attr "$dev/$attr"; done
done
for dev in /sys/class/hwmon/hwmon*; do
	[ -d "$dev" ] || continue
	read_attr "$dev/name"
	for path in "$dev"/temp*_input "$dev"/temp*_label "$dev"/temp*_fault; do
		read_attr "$path"
	done
done
for dev in /sys/class/power_supply/*; do
	[ -d "$dev" ] || continue
	for attr in type present temp temp_ambient; do read_attr "$dev/$attr"; done
done
# IIO temperatures may be raw/scaled, not degrees. Keep the exact attribute names.
for dev in /sys/bus/iio/devices/iio:device*; do
	[ -d "$dev" ] || continue
	read_attr "$dev/name"
	for path in "$dev"/in_temp*; do read_attr "$path"; done
done
for dev in /sys/bus/platform/devices/1100b000.thermal; do
	if [ -L "$dev/driver" ]; then
		printf 'soc_thermal_driver=%s\n' "$(readlink "$dev/driver")"
	else
		printf 'soc_thermal_driver=unbound_or_absent\n'
	fi
done
read_attr /sys/kernel/debug/devices_deferred
printf 'THERMAL_INVENTORY_END zones=%s calibration=not_independently_verified cooling_action=not_tested\n' "$zones"
