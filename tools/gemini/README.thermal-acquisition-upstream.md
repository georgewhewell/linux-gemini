# MT6797 thermal acquisition port

Source: ixoo/gemini-pda-mainline revision
`b7b72311c5eecfbead4b45494e2dd2f467d559b7`, patches 0514–0519 and the
reader-lifetime principle from 0539. Original experiment patches use
`Gemini Mainline Experiment <gemini-mainline@example.invalid>` as a
non-certifying author and contain no DCO sign-off. This port preserves that
provenance, not an invented upstream certification or submission-ready claim.

The upstream no-workload result records 21 valid converted samples from three
seven-slot scans. Those are six sequential banks sampling four physical MCU
sensors, not seven thermometers. That is upstream device evidence, not a run
of this integrated GPU/thermal package.

Adaptations to this tree:

- Existing SET/CLR reset offsets were already corrected. Import compact
  thermal/PWRAP IDs 0/1, descriptor bounds, and upstream reset KUnit tests.
  Preserve historical CLDMA IDs13/68 solely for existing disabled DT references;
  provider bounds reject them. No modem properties or status are changed.
- Import the ordered V4 transaction and KUnit tests from 0517/0518: clocks,
  exclusive reset, masked APMIXED change, idle, pause, all-bank preparation,
  one AUXADC channel commit, all-bank enable/release, first valid samples,
  reverse cleanup. Do not program the unresolved AUXADC power bit.
- Retain existing fuse-validation, zero-code rejection, aggregate `EAGAIN`,
  and four-channel standard hwmon. Do not import the diagnostic ledger or
  three-read observer budget as a production interface.
- Adapt 0539 using a devres group containing the zone, aggregate hwmon and
  physical-sensor hwmon; release readers before transaction close during
  failure and remove. Earlier mappings/clocks/state are outside the group.
- Add thermal reset to source DT. Keep standalone AUXADC disabled in the
  sensor-only overlay: the thermal driver owns the indirect sampling engine.

The old accepted carrier DT must be completed explicitly: thermal reset
`<3 0>`, PMIC-wrapper reset `<3 1>` (formerly `<3 0x40>`), thermal status
okay, one no-trip SoC zone referring to thermal phandle0x1b, and standalone
ADC disabled. Validate all existing clock/APMIXED/NVMEM references in the
compiled carrier rather than applying a label-based overlay blindly.

`test-thermal-acquisition.py` executes the imported twelve pure KUnit case
bodies with host assertions, including all 31 transaction failure positions;
it is not a kernel KUnit/QEMU run. It also checks actual reader-group source
ordering. Existing actual-function conversion/aggregate and hwmon tests remain
required. Full kernel/object and DT builds remain separate validation.

No IRQ thermal protection, cooling policy, CPU DVFS/admission, suspend/resume,
battery NTC, charger or PMIC die-ADC support is claimed or enabled here.
