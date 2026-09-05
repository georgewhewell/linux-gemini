#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Reconstruct the exact upstream A72 input; never edit either source repository.
set -euo pipefail
if [[ $# != 3 ]]; then
    echo "usage: $0 <523-patch-linux-repository> <upstream-repository> <new-output-directory>" >&2
    exit 2
fi
source_repo=$(realpath "$1")
upstream_repo=$(realpath "$2")
output=$(realpath -m "$3")
[[ ! -e "$output" ]] || { echo 'output already exists' >&2; exit 2; }
source_rev=3569f9559257b
upstream_rev=b7b72311c5eecfbead4b45494e2dd2f467d559b7
mkdir -p "$output"
git -C "$source_repo" archive "$source_rev" arch/arm64/boot/dts/mediatek \
    include/dt-bindings include/uapi/linux/input-event-codes.h | tar -x -C "$output"
git -C "$upstream_repo" archive "$upstream_rev" \
    patches/v7.1.3/0482-arm64-dts-mediatek-mt6797-describe-CPU-topology.patch \
    patches/v7.1.3/0514-dt-bindings-reset-mediatek-correct-MT6797-infracfg-resets.patch \
    patches/v7.1.3/0519-arm64-dts-mediatek-add-MT6797-thermal-reset.patch \
    experiments/2026-08-21-mainline-current-tree-serviceability-control/scripts/build-serviceability-dtb.sh \
    experiments/2026-08-28-mainline-a72-admission-serviceability-restoration/scripts/build-serviceability-dtb.sh \
    experiments/2026-09-02-mainline-mt6797-cpu-map/scripts \
    experiments/2026-09-04-mt6797-thermal-serviceability-dt-repair/thermal-serviceability.dtso \
    | tar -x -C "$output"
cd "$output"
for patch in \
    0519-arm64-dts-mediatek-add-MT6797-thermal-reset \
    0514-dt-bindings-reset-mediatek-correct-MT6797-infracfg-resets \
    0482-arm64-dts-mediatek-mt6797-describe-CPU-topology; do
    git apply -R "patches/v7.1.3/$patch.patch"
done
"${HOSTCC:-cc}" -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I include -I arch/arm64/boot/dts/mediatek \
    arch/arm64/boot/dts/mediatek/mt6797-gemini-pda-a72-admission.dts \
    -o admission-package.dts
dtc -I dts -O dtb -o admission-package.dtb admission-package.dts
echo '1bd6ce2ded2e1186503cb0d9d00107964ec27abc48062b9210e1935d38d60509  admission-package.dtb' | sha256sum -c -
# Portability only: generated wrapper's /bin/bash is absent on NixOS.
wrapper=experiments/2026-08-28-mainline-a72-admission-serviceability-restoration/scripts/build-serviceability-dtb.sh
sed -i 's@/bin/bash "$derived"@bash "$derived"@' "$wrapper"
bash "$wrapper" --base-dtb admission-package.dtb --output serviceability.dtb
cp serviceability.dtb topology-serviceability.dtb
# Reuse the source transform, with exact final identity as the decisive check.
# The historical package DT (51fefc50...) is not required to recreate its map.
python3 - <<'PY'
from pathlib import Path
import runpy
scripts = Path("experiments/2026-09-02-mainline-mt6797-cpu-map/scripts")
m = runpy.run_path(str(scripts / "build-topology-serviceability-dtb.py"))
m["validate_base"](Path("serviceability.dtb"))
m["add_map"](Path("topology-serviceability.dtb"))
if m["sha256"](Path("topology-serviceability.dtb")) != m["OUTPUT_SHA256"]:
    raise SystemExit("topology output identity mismatch")
m["load_validator"](scripts / "validate-cpu-map.py")(Path("topology-serviceability.dtb"))
PY
thermal=experiments/2026-09-04-mt6797-thermal-serviceability-dt-repair/thermal-serviceability.dtso
dtc -Wno-resets_property -Wno-thermal_sensors_property -I dts -O dtb \
    -o thermal-serviceability.dtbo "$thermal"
echo 'f2d4cec4b2dec6593a148c9bcb46cc989b825d4ea12aa46c06be0d8da11dd748  thermal-serviceability.dtbo' | sha256sum -c -
fdtoverlay -i topology-serviceability.dtb -o topology-thermal.dtb thermal-serviceability.dtbo
echo 'fe24e244b74fe9b504727a6d19d590b1e03a28fc7e461d4d033d9be718757569  topology-thermal.dtb' | sha256sum -c -
sha256sum admission-package.dtb serviceability.dtb topology-serviceability.dtb topology-thermal.dtb
echo 'PASS exact upstream DT reconstruction; no device access, no A41 identity injected'
