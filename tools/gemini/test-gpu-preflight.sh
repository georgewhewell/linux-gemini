#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Static contract for the bounded Gemini G3 no-job source gate.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
overlay=$root/arch/arm64/boot/dts/mediatek/mt6797-gemini-pda-gpu-preflight.dtso
capture=$root/tools/gemini/capture-gpu-preflight.sh

need()
{
	file=$1
	pattern=$2

	grep -Fq "$pattern" "$file" || {
		echo "missing contract in $file: $pattern" >&2
		exit 1
	}
}

reject()
{
	file=$1
	pattern=$2

	if grep -Fq "$pattern" "$file"; then
		echo "forbidden contract in $file: $pattern" >&2
		exit 1
	fi
}

test "$(grep -Fc 'opp-hz = /bits/ 64 <238000000>;' "$overlay")" -eq 1
test "$(grep -Fc 'opp-microvolt = <846700 846700 850000>;' "$overlay")" -eq 1
test "$(grep -Fc 'status = "okay";' "$overlay")" -eq 4
need "$overlay" 'regulator-ramp-delay = <32000>;'
need "$overlay" 'operating-points-v2 = <&g3_gpu_opp_table>;'
reject "$overlay" 'regulator-boot-on'
reject "$overlay" 'regulator-min-microvolt'
reject "$overlay" 'regulator-max-microvolt'

need "$capture" "identity_pattern='13040000\\.gpu: mali-t880 id 0x880 major 0x1 minor 0x0 status 0x8$'"
need "$capture" "topology_pattern='shader_present=0xf l2_present=0x1$'"
need "$capture" 'expected_frequencies=238000000'
need "$capture" 'type12-fixed-low:238000000) expected_microvolts=850000 ;;'
need "$capture" 'for irq_spec in panfrost-job:296 panfrost-mmu:295 panfrost-gpu:294; do'
need "$capture" 'reject_pattern="$reject_pattern|(L2|shader|tiler) power-(on|off) timeout"'
need "$capture" 'assert_no_drm_open'
need "$capture" 'GPU runtime-PM policy changed during the no-job observation'
need "$capture" 'GPU devfreq policy changed during the no-job observation'
need "$capture" 'GPU_PREFLIGHT=PASS'

sh -n "$capture"
echo "Gemini G3 no-job source contract: PASS"
