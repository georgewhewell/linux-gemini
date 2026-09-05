# Bounded integration map: full cellular source

Historical source-map document. The completed integration's adaptations
and current safety limits are in `README.cellular-integrated.md`.

Root integration and this full-transport source have different histories.
Do not merge this repository wholesale: it carries old GPU, thermal, A72,
watchdog and diagnostic policy which must not replace current integration.

## Complete source-family snapshot

`cellular-source-family.files` lists exactly 57 MD1/CCCI source/header/build
files in this tree. It is a bounded family superset including the foundation
KUnit tests and optional old pre-release/bringup adapters, not a claim all
57 files should be enabled. Root integration already has its different
`drivers/net/wwan/mediatek/mt6797/` control-RX lane; it has none of the full
`drivers/soc/mediatek/mt6797-md1-*` foundation. Import these files at the final
handoff-fix revision, retaining origin/provenance. Do not recursively replace
the entire mediatek directory.

Shared files require selected hunks, not replacement:

- `drivers/soc/mediatek/Kconfig`: only `MTK_MT6797_MD1_*` blocks and their
  actual selects/depends. `Makefile`: only matching MD1 object/composite rules.
  The cold owner needs PRERELEASE, BOOT_STATUS_OBSERVE, LK_HANDOFF_PROBE,
  COLD_OWNER_CORE, WDT_GATE_CORE, HS_CORE, REMAP_OBSERVE and EMI_MPU; their
  firmware parser, LK layout, MPU plan and remap-attestation dependencies are
  in the listed family. The optional firmware loader object is linked by the
  cold-owner composite even when its independent runtime gate stays off.
- `drivers/net/wwan/Kconfig`: MTK_CCCI_COMMON, MTK_MT6797_CCCI and its KUnit
  block. `Makefile`: its mediatek directory rule. The family includes the new
  parent `mediatek/Makefile`; preserve root's separate old subdirectory rules.
  Full CCCI and the old `MTK_MT6797_MD1` owner must be mutually exclusive in
  the enabled build/DT profile; their hardware resources overlap. Do not
  delete the old lane merely to make symbol selection easier.
- `arch/arm64/boot/dts/mediatek/mt6797-gemini-pda.dts`: only the MD1 reserved
  memory references, lifecycle/provider and disabled CCCI nodes. Preserve
  current CPUs, clocks, GPU, thermal, watchdog, calibration and reset bindings.
  Cross-check every imported provider phandle against the resulting DT.
- The exact opt-in activation profile is
  `mt6797-gemini-pda-md1-ccci.dtso`, composed with the cold-owner foundation
  profiles. Import only required MD1 profiles/rules, not unrelated overlays.
- Add `Documentation/devicetree/bindings/net/mediatek,mt6797-ccci.yaml`,
  `Documentation/devicetree/bindings/soc/mediatek/mediatek,mt6797-md1-emi-mpu.yaml`
  and `Documentation/networking/device_drivers/wwan/mt6797-ccci.rst` plus
  relevant MAINTAINERS entries. Keep default DT nodes disabled.

## Transport extension history

The complete linear extension after `30322fe36` is:

1. `8e9429ee5` architecture documentation
2. `32c41e63d` CCCI protocol helpers
3. `9e7e71ccd` CLDMA ownership helpers
4. `ebc4e9402` CCCI binding
5. `ac65d49d4` disabled DT transport
6. `e0f620c6f` crash-safe handshake
7. `c5a5a35aa` HS1 v2 runtime replies
8. `7fa5c1fa5` below-4G coherent ring roots
9. `82854b407` IRQ-safe handshake
10. `19c24cd7b` seven-queue transport
11. `7acfb1a45` CCMNI queue correction
12. `92e7926eb` standard AT/raw-IP ports
13. `9b501ccd5` firmware loader and HS1 region13 finalization

This chain alone is insufficient without the older MD1 family foundation.
Its ancestor `2e1358e14` is a mixed W4/MD1 import, so use the bounded file map
instead of replaying that entire mixed-history commit into current root.

## Mandatory reconciliation before hardware

- Full source is missing later `8faeb854c00524a2a39450f9c72aaa3601e1c2a5`
  watchdog-before-first-MD1/EMI-write protection. Port that guarantee around
  both ordinary and optional firmware writes while preserving current root
  watchdog fixes. Its touched files are cold-owner C, SoC Kconfig, boot-candidate
  overlay, `drivers/watchdog/mtk_wdt.c`, `include/linux/mtk_wdt.h`; do not
  overwrite root's watchdog implementation with the historical file.
- Historical `18d8b4adb` permits degraded PLL continuation. Retained GO evidence
  used that diagnostic, not a proven non-degraded production startup. Resolve
  actual clock/PLL/ownership admission before enabling transport.
- The readiness fix intentionally does not remove the suspect pre-ring
  activity condition. Source-proven reset/clock leases and a fresh clean-engine
  snapshot remain prerequisites to changing that condition.
- Rebuild the complete integrated configuration and run the imported protocol,
  handshake, CLDMA, MPU/firmware and lifetime tests. The current handoff test
  and two object builds do not certify a merged image.

No hardware access, scheduling, firmware download, SIM or subscription change
is authorized by this map. The compact CLDMA reset prerequisite was separately
delivered as `e6fbf02e6280c` and is already being integrated by root.
