# Bounded cellular integration into the current Gemini kernel

This is source integration, not evidence of modem attach or packet data.
All six nodes in `mt6797-gemini-pda-cellular.dtso` remain disabled. The
existing control-RX lane is retained; Kconfig excludes its owners when the
full cold owner is selected. No battery, charging, GPU, thermal, CPU or
watchdog startup policy is replaced.

## Provenance and adaptations

Base: `b8b43933c6de2a82a093b567b45e80b0a51597ce` in the integration history.
The 57 paths in `cellular-source-family.files` were imported from
`b83d28018b8eff5d7aaa138d4c03bbca6e084977` in the separate cellular history.
The historical integration map records the transport extension commits and
retains their author/provenance. Only MD1/CCCI Kconfig/Makefile hunks were
added; the old mixed-history kernel was not merged wholesale.

The current watchdog API is extended with a read-only admission check for
the existing 31-second reset-only hardware mode and hardware timeout length.
This reconciles the guarantee from cellular commit
`8faeb854c00524a2a39450f9c72aaa3601e1c2a5` without replacing the watchdog
driver. Admission precedes initial EMI/MD1 writes and optional firmware
loading; the loader verification callback checks again after firmware
requests and immediately before its first `memcpy_toio`.

Three missing MT6351 voltage-register definitions are copied from vendor
`include/mt-plat/mt6797/include/mach/upmu_hw.h:338,348,358`:
VMODEM_CON9=0x063a, VMD1_CON9=0x064e, VSRAM_MD_CON9=0x0662.

PLL timeouts are fatal, never a production degraded-ready permission.
Two ordering errors in the imported adapter are corrected against vendor
`drivers/misc/mediatek/eccci/mt6797/cldma_platform.c`, `md1_pll_init`:

- Lines 699–713: after L1MCU readiness, switch L1MCU and program FLEXSEL1
  (0x2c=0x30302020) and FLEXSEL2 (0x44=0x2030), then poll DFE/IMC readiness.
- Lines 716–741: wait for all three FLEX status words before switching
  L1SYS, programming SEL0 and writing the DCM magic. The subsequent L1_A0
  BIT7 and final DCM poll/BUS switch retain vendor order.

Vendor file SHA256:
`ada25c673de99cf648955cc782fabf7ab98af1c6bf3227ca7883d5ff44792125`.
The retained September 1 capture has DFE/IMC status 0x041b041b and selectors
0x2020/0x2000 at the failed stage. That is consistent with the missing
pre-poll writes, but does not prove this correction fixes hardware startup.

## Validation and limits

`cellular-integration.config` is a build-only full profile, not activation.
It disables GNSS in that profile because baseline GNSS=m limits WWAN to m;
the CCCI lifecycle requires its providers built in. Image, modules and the
base-plus-cellular composed DT compile with the current GPU/thermal/A72
sources retained. Existing base USB ranges warnings are unrelated.

`test-cellular-integration.py` compiles the actual watchdog predicate and
PLL advance callback with host adapters. It checks exact vendor stage/write
order, every write failure, guard placement, fatal timeout and disabled/
exclusive profile wiring. `test-cellular-handoff-readiness.py` executes
actual publication/wait code using pthread adapters, including late-link
states and shutdown; it is not device-core or hardware emulation.

`cellular-tests.kunitconfig` runs the pure protocol/ownership suites through
the kernel's ARM64 QEMU KUnit runner. UML is not used: inherited generic CPU
rollback hooks lack UML implementations. No A72 code was changed to make
UML tests work. Runtime test results must be recorded separately from modem
results; a passing protocol test does not establish RF, HS1 or network data.

Local validation on this integrated source: full ARM64 Image/modules and
composed DT PASS; both host scripts PASS; ARM64 QEMU KUnit ran 141 tests,
140 passed and one framework newline-formatting test skipped. All selected
cellular suites passed, including fatal PLL timeout and complete HS1 v2
reply construction. Output directories are
`/tmp/gemini-cellular-full-arm64-WB0NIu9p` and
`/tmp/gemini-cellular-kunit-arm64-PFBl0FL0` (the latter retains `test.log`).

## Remaining hardware admission

The AP CLDMA reservation collision between supplier and consumer requires
an explicit single-owner mapping lease; bypassing resource reservations is
not acceptable. Until that is integrated, this profile must not be enabled.
The pre-ring positive-activity guard remains: idle alone does not prove
ownership or justify programming queue heads. A subsequent change requires
verified clock/reset ownership and fresh idle/head/IRQ state.

The next useful bounded boot should exercise the corrected strict PLL
sequence under the existing watchdog/rescue workflow and capture each
stage's selectors/status plus fresh CLDMA ownership state. If admission
holds, attempt real HS1/HS2 and an AT response in that same session. GO is
only a vector-release token, not HS1. Attach and packet data require their
own observed results; no SIM or subscription change is implied. Device
access remains exclusively with the root-designated hardware owner.
Battery NTC is physically disconnected and neither faked nor a power blocker.
