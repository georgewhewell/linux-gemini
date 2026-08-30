#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Validate both sides of the Gemini display gate and its composed DDP graph.

set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 <mt6797-gemini-pda.dtb> <mt6797-gemini-pda-display-test.dtb>" >&2
	exit 2
fi

base=$1
dtb=$2
: "${FDTGET:=fdtget}"

require_present()
{
	if ! "$FDTGET" "$1" "$2" "$3" >/dev/null 2>&1; then
		echo "$2/$3 must be present" >&2
		exit 1
	fi
}

require_equal()
{
	actual=$("$FDTGET" "$dtb" "$1" "$2")
	if [ "$actual" != "$3" ]; then
		echo "$1/$2: got '$actual', want '$3'" >&2
		exit 1
	fi
}

require_link()
{
	left=$("$FDTGET" -t x "$dtb" "$1" remote-endpoint)
	right=$("$FDTGET" -t x "$dtb" "$2" phandle)
	if [ "$left" != "$right" ]; then
		echo "$1 does not point at $2" >&2
		exit 1
	fi
}

# The base board must expose clocks while holding back automatic DRM creation.
require_present "$base" /syscon@14000000 mediatek,drm-on-demand

# The composed DT restores the M4U dependency and leaves the unroutable OVL0
# disabled; graph construction must select OVL0_2L as the first component.
require_equal /iommu@10205000 status okay
require_equal /larb@14020000 status okay
require_equal /ovl@1400b000 status disabled
require_equal /ovl@1400d000 status okay
require_equal /dsi@1401c000 status okay
require_equal /dsi@1401c000/panel@0 compatible planet,gemini-pda-nt36672

require_link /syscon@14000000/port/endpoint@0 /ovl@1400d000/ports/port@0/endpoint@0
require_link /ovl@1400d000/ports/port@1/endpoint@0 /ovl@1400e000/ports/port@0/endpoint@0
require_link /ovl@1400e000/ports/port@1/endpoint@0 /color@14013000/ports/port@0/endpoint@0
require_link /color@14013000/ports/port@1/endpoint@0 /ccorr@14014000/ports/port@0/endpoint@0
require_link /ccorr@14014000/ports/port@1/endpoint@0 /aal@14015000/ports/port@0/endpoint@0
require_link /aal@14015000/ports/port@1/endpoint@0 /gamma@14016000/ports/port@0/endpoint@0
require_link /gamma@14016000/ports/port@1/endpoint@0 /od@14017000/ports/port@0/endpoint@0
require_link /od@14017000/ports/port@1/endpoint@0 /dither@14018000/ports/port@0/endpoint@0
require_link /dither@14018000/ports/port@1/endpoint@0 /rdma@1400f000/ports/port@0/endpoint@0
require_link /rdma@1400f000/ports/port@1/endpoint@0 /ufoe@14019000/ports/port@0/endpoint@0
require_link /ufoe@14019000/ports/port@1/endpoint@0 /dsi@1401c000/ports/port@0/endpoint@0
require_link /dsi@1401c000/ports/port@1/endpoint@0 /dsi@1401c000/panel@0/port/endpoint

echo "Gemini display boot-safe gate and composed DDP graph: PASS"
