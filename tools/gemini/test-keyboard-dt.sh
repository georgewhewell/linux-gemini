#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Validate the hardware/evdev boundary of the Gemini keyboard DT.
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 <mt6797-gemini-pda.dtb>" >&2
	exit 2
fi

dtb=$1
: "${FDTGET:=fdtget}"
keyboard=/keyboard-matrix

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

require_flag()
{
	if ! "$FDTGET" "$dtb" "$keyboard" "$1" >/dev/null; then
		echo "$keyboard: missing $1" >&2
		exit 1
	fi
}

require_equal "$("$FDTGET" "$dtb" "$keyboard" status)" okay \
	"keyboard status"
require_equal "$("$FDTGET" -t u "$dtb" "$keyboard" poll-interval)" 20 \
	"keyboard poll interval"
require_equal "$("$FDTGET" -t u "$dtb" "$keyboard" col-scan-delay-us)" 2 \
	"keyboard column scan delay"
require_flag gpio-activelow
require_flag drive-inactive-cols

# fdtget emits one whitespace-separated word per DT cell.
# shellcheck disable=SC2046
set -- $("$FDTGET" -t x "$dtb" "$keyboard" row-gpios)
require_equal "$#" 24 "eight three-cell row GPIO specifiers"
# shellcheck disable=SC2046
set -- $("$FDTGET" -t x "$dtb" "$keyboard" col-gpios)
require_equal "$#" 21 "seven three-cell column GPIO specifiers"

# shellcheck disable=SC2046
set -- $("$FDTGET" -t x "$dtb" "$keyboard" linux,keymap)
require_equal "$#" 52 "assigned matrix key count"

fn_count=0
for entry do
	value=$((0x$entry))
	keycode=$((value & 0xffff))

	# MATRIX_KEY(4, 3, KEY_LEFTMETA), the physical Fn contact.
	if [ "$value" -eq $((0x0403007d)) ]; then
		fn_count=$((fn_count + 1))
	fi
	if [ "$keycode" -eq $((0x1d0)) ]; then
		echo "$keyboard: KEY_FN must not replace the shipped evdev identity" >&2
		exit 1
	fi
done

require_equal "$fn_count" 1 "Fn contact encoded as KEY_LEFTMETA"

echo "Gemini keyboard DT: 8x7 matrix and KEY_LEFTMETA Fn boundary valid"
