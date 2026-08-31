#!/usr/bin/env bash
# Validate the LK-rewritten CCCI contract from an archived live FDT.
# Usage: test-mt6797-live-ccci-mblocks.sh LIVE.FDT LIVE-PROPERTIES.TXT
set -euo pipefail

fdt=$1
properties=$2

expect_node() {
	local node=$1 reg=$2 no_map=$3
	test "$(fdtget -ts "$fdt" "/reserved-memory/$node" compatible)" = \
		"mediatek,ccci"
	test "$(fdtget -tx "$fdt" "/reserved-memory/$node" reg)" = "$reg"
	if test "$no_map" = yes; then
		fdtget "$fdt" "/reserved-memory/$node" no-map >/dev/null
	else
		! fdtget "$fdt" "/reserved-memory/$node" no-map >/dev/null 2>&1
	fi
}

expect_node mblock-5-ccci "0 88000000 0 600000" yes
expect_node mblock-6-ccci "0 b4000000 0 a000000" no
expect_node mblock-7-ccci "0 be000000 0 c00000" no

ccci_nodes=0
for node in $(fdtget -l "$fdt" /reserved-memory); do
	if test "$(fdtget -ts "$fdt" "/reserved-memory/$node" compatible 2>/dev/null || :)" = \
		"mediatek,ccci"; then
		ccci_nodes=$((ccci_nodes + 1))
	fi
done
test "$ccci_nodes" = 3

# The raw archive independently records the node/property bytes collected
# from /sys/firmware/devicetree/base.
grep -Fq '===/sys/firmware/devicetree/base/reserved-memory/mblock-5-ccci' "$properties"
grep -Fq '===/sys/firmware/devicetree/base/reserved-memory/mblock-6-ccci' "$properties"
grep -Fq '===/sys/firmware/devicetree/base/reserved-memory/mblock-7-ccci' "$properties"
grep -Fq '00 00 00 00 88 00 00 00 00 00 00 00 00 60 00 00' "$properties"
grep -Fq '00 00 00 00 b4 00 00 00 00 00 00 00 0a 00 00 00' "$properties"
grep -Fq '00 00 00 00 be 00 00 00 00 00 00 00 00 c0 00 00' "$properties"
