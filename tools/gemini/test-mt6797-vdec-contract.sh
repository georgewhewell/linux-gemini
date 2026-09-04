#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Keep MT6797 VDEC unbound until its register and firmware ABIs are implemented.

set -eu

die()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

require_text()
{
	file=$1
	text=$2
	grep -Fq -- "$text" "$file" || die "$file: missing: $text"
}

reject_text()
{
	file=$1
	text=$2
	if grep -Fq -- "$text" "$file"; then
		die "$file: unsupported contract appeared: $text"
	fi
}

check_blob()
{
	path=$1
	want=$2
	got=$(git -C "$vendor" hash-object "$path")
	[ "$got" = "$want" ] || die "vendor blob changed: $path ($got)"
}

[ "$#" -eq 1 ] || die "usage: $0 VENDOR_KERNEL_TREE"
vendor=$(CDPATH='' cd -- "$1" && pwd)
kernel=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)

[ "$(git -C "$vendor" rev-parse HEAD)" = \
	"8cfe6596a503612e3332d9c26e292a19525a7f07" ] || \
	die "vendor HEAD is not the qualified commit"
[ "$(git -C "$vendor" rev-parse 'HEAD^{tree}')" = \
	"3d7edd04f9edb6d88b93468a659f1cf11a12b8ae" ] || \
	die "vendor tree is not the qualified tree"

check_blob arch/arm64/boot/dts/mt6797.dtsi \
	6a03d1c43d4f2ae3f5e4701135386f9a825b4381
check_blob drivers/media/platform/mtk-vcodec/mtk_vcodec_drv.h \
	0e6c94d79c2db9a6366b47bc2a580de712e24cfc
check_blob drivers/media/platform/mtk-vcodec/mtk_vpuc_core.c \
	bd6272020644ae29cee17e1df595717681f448ce
check_blob drivers/media/platform/mtk-vcodec/mtk_vcodec_dec_drv.c \
	17d85f9591144bbbd86762a32c959dae3421b3f0
check_blob drivers/misc/mediatek/videocodec/mt6797/videocodec_kernel_driver.c \
	c8bc6c29b8f245417a9ad7c223908d68d36fccba
check_blob drivers/misc/mediatek/m4u/mt6797/m4u_platform.h \
	1105b8dd7fed4d92483854906cedbdc01f8d064c

dts=$kernel/arch/arm64/boot/dts/mediatek/mt6797.dtsi
binding=$kernel/Documentation/devicetree/bindings/media/mediatek,vcodec-decoder.yaml
driver=$kernel/drivers/media/platform/mediatek/vcodec/decoder/mtk_vcodec_dec_drv.c
vpu=$kernel/drivers/media/platform/mediatek/vpu/mtk_vpu.c
ports=$kernel/include/dt-bindings/memory/mt6797-larb-port.h
clock=$kernel/drivers/clk/mediatek/clk-mt6797-vdec.c
vendor_dts=$vendor/arch/arm64/boot/dts/mt6797.dtsi
vendor_vpuc=$vendor/drivers/media/platform/mtk-vcodec/mtk_vpuc_core.c
vendor_v4l2=$vendor/drivers/media/platform/mtk-vcodec/mtk_vcodec_dec_drv.c
vendor_codec=$vendor/drivers/misc/mediatek/videocodec/mt6797/videocodec_kernel_driver.c
vendor_ports=$vendor/drivers/misc/mediatek/m4u/mt6797/m4u_platform.h

# Provider resources are present and still fail closed.
require_text "$dts" 'vdecsys: syscon@16000000 {'
require_text "$dts" 'larb1: larb@16010000 {'
require_text "$dts" '<&vdecsys CLK_VDEC_CKEN>'
require_text "$dts" '<&vdecsys CLK_VDEC_LARB1_CKEN>'
require_text "$clock" 'GATE_VDEC0(CLK_VDEC_CKEN,'
require_text "$clock" 'GATE_VDEC1(CLK_VDEC_LARB1_CKEN,'
require_text "$ports" '#define M4U_PORT_VDEC_MC'
require_text "$ports" '#define M4U_PORT_VDEC_TILE'
reject_text "$dts" 'compatible = "mediatek,mt6797-vcodec-dec"'

# The executable decoder contract still requires a firmware endpoint and does
# not claim MT6797.
require_text "$binding" 'const: hwq'
require_text "$binding" 'const: hwg'
require_text "$binding" 'mediatek,vpu:'
require_text "$binding" 'mediatek,scp:'
reject_text "$binding" 'mediatek,mt6797-vcodec-dec'
require_text "$driver" '"mediatek,vpu"'
require_text "$driver" '"mediatek,scp"'
require_text "$driver" 'Could not get vdec IPI device'
reject_text "$driver" 'mediatek,mt6797-vcodec-dec'
require_text "$vpu" '"mediatek,mt8173-vpu"'
reject_text "$vpu" 'mediatek,mt6797-vpu'

# Vendor register evidence matches the common prefix but not the legacy tail.
for range in \
	'0x16020000 0x10000' \
	'0x16021000 0x800' \
	'0x16021800 0x800' \
	'0x16022000 0x1000' \
	'0x16023000 0x1000' \
	'0x16024000 0x1000' \
	'0x16025000 0x1000' \
	'0x16026800 0x800' \
	'0x16027800 0x800' \
	'0x16028000 0x1000'
do
	require_text "$vendor_dts" "reg = <$range>;"
done
reject_text "$vendor_dts" 'reg = <0x16027000'
reject_text "$vendor_dts" 'reg = <0x16028400'

# The port numbering is evidenced, while the firmware transport is not the
# mainline MT8173 VPU ABI.
require_text "$vendor_ports" 'M4U0_PORT_INIT("VDEC_MC", 0, 1, 0)'
require_text "$vendor_ports" 'M4U0_PORT_INIT("VDEC_TILE", 0, 1, 9)'
require_text "$vendor_vpuc" '#define VPU_PATH'
require_text "$vendor_vpuc" '"/dev/vpud"'
require_text "$vendor_vpuc" '#define VPUD_SET_OBJECT'
require_text "$vendor_v4l2" '"mediatek,mt2701-vcodec-dec"'
reject_text "$vendor_v4l2" 'mediatek,mt6797-vcodec-dec'
require_text "$vendor_codec" '"mediatek,mt6797-vdec_gcon"'
require_text "$vendor_codec" '"mediatek,mt6797-vdec"'

printf '%s\n' \
	'PASS: MT6797 VDEC provider resources are inventoried and activation remains closed'
