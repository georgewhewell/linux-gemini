#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Check the composed Gemini thermal-observation DTB, not just its overlay.

set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 <mt6797-gemini-pda-thermal-hall-observe.dtb>" >&2
	exit 2
fi

dtb=$1
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

zone=/thermal-zones/soc-thermal

require_equal "$("$FDTGET" -t u "$dtb" "$zone" polling-delay)" 1000 \
	"thermal polling delay"
require_equal "$("$FDTGET" -t u "$dtb" "$zone" polling-delay-passive)" 1000 \
	"thermal passive polling delay"
require_equal "$("$FDTGET" -t x "$dtb" "$zone" thermal-sensors)" \
	"$("$FDTGET" -t x "$dtb" /thermal@1100b000 phandle)" \
	"thermal provider"
require_equal "$("$FDTGET" "$dtb" /thermal@1100b000 status)" okay \
	"thermal controller status"
require_equal "$("$FDTGET" "$dtb" /adc@11001000 status)" okay \
	"AUXADC status"

if [ -n "$("$FDTGET" -l "$dtb" "$zone")" ]; then
	echo "thermal observation zone must not add trips or cooling maps" >&2
	exit 1
fi

echo "Gemini thermal observation DT: PASS"
