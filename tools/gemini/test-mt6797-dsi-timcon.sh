#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Structural regression check for the Gemini-only MT6797 DSI timing quirk.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
source="$root/drivers/gpu/drm/mediatek/mtk_dsi.c"

require() {
	pattern=$1
	if ! rg -q -- "$pattern" "$source"; then
		echo "missing MT6797 DSI timing invariant: $pattern" >&2
		exit 1
	fi
}

require 'static const u32 mt6797_dsi_phy_timcon\[\]'
require '0x0a12080a'
require '0x14320f28'
require '0x0d320100'
require '0x00141208'
require '\.phy_timcon = mt6797_dsi_phy_timcon'
require 'dsi->driver_data->phy_timcon\[0\]'
require 'dsi->driver_data->phy_timcon\[3\]'
require 'bool has_timeout_snapshot'
require 'mtk_dsi_log_timeout_state'
require 'irqd_irq_disabled\(irqd\)'
require 'irqd_irq_masked\(irqd\)'
require '\.has_timeout_snapshot = true'
