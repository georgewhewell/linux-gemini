# MT6797 p22 power-sufficiency boundary

Date: 2026-09-04

Status: offline evidence and decision only.  This note does not authorize a
hardware probe, USB/I2C/PMIC access, register write, flash, boot, reset, remote
operation, or opening the device.

## Decision

No p22 transaction is eligible.  The battery connector was physically open,
but it was later reconnected by the owner and battery-backed operation was
observed.  Its **current** mechanical and electrical condition is unknown; it
is neither proved disconnected nor proved safe now.  The current accepted
Stage-A image exposes no device under `/sys/class/power_supply`, so it cannot
qualify battery presence, charge, voltage, health, temperature, or external
power.  Absence of that interface is absence of telemetry, not evidence that
the battery is absent.

Every reviewed p22 package records `power_preflight_policy=none`.  Its
watchdog and Stage-A rollback contracts limit consequences after a write; they
do not provide energy for the write, reset, recovery wait, or readback.

## Reconciled physical record

The retained thread transcript is the direct record of owner-observed physical
state:

1. At `2026-09-02T04:42:00Z`, the owner reported that the battery connector
   broke, the device was powered only from USB, and it browned out.  A queued
   p30 discriminator was stopped while scanning, before a p30 write.
2. At `2026-09-02T09:10:46Z`, the owner reported reconnecting the battery and
   that the board appeared to stay up.
3. Subsequent historical observations reported BQ USB/charging state and an
   MT6351 battery supply, then `battery present`, stable boots, and live
   current/coulomb telemetry.  These corroborate reconnection on September 2;
   they cannot establish the connector state on September 4.

Source: `/mnt/Home/src/gemini-codex-thread-digest.md:1640`, `:2197`, `:2213`,
`:2283`, and `:2409`--`:2433`.

This corrects the stale attribution in
`/mnt/Home/src/nixos-gemini/repo-cellular-reconstruction/campaign/06-cellular-modem.md`:
its statement that the connector still "must be repaired" preserved the
earlier broken-connector state and did not reconcile the later owner-reported
reconnection.  The proper present-tense finding is **unknown pending fresh
battery-side evidence**.  Physical repair is conditional, not already proved
necessary.

Historical observations are consistent with that chronology but are not
current power proof:

- `/mnt/Home/src/nixos-gemini-power-oracle-20260902/research/STOCK_POWER_ORACLE_20260902.md`
  records the stock battery supply as present/Good, 86%, 4196 mV, 20 C, with
  USB and AC offline.  It also identifies the battery gauge as the MT6351
  FGADC plus MediaTek software SOC algorithm, not the BQ charger.
- `/mnt/Home/src/ixoo-gemini-pda-mainline/experiments/2026-07-12-charger-power-recovery/results/live-charger-battery-recovery-20260714.txt`
  records an older read-only vendor snapshot: battery present/Good/91% and AC
  online.
- NixOS commits `8b6b8e7d9dcc0f648a2deb301871ce0e6a686820` and
  `0d0309858c836c915a8a754c12a1e9ffe351ec99` correctly record the earlier
  batteryless boundary and reboot chronology, but predate the reconnection.

## Why USB-only stability is not a substitute

The source-only audit at NixOS commit
`846cd1b82541aedca5264eb7303e5e0369b628f0`, file
`research/BATTERYLESS_BROWNOUT_AUDIT.md`, establishes that Linux power trimming
starts after pre-Linux load.  Display/backlight, vibrator, storage and other LK
work can occur first.  Charger current-limit fields are policy settings, not a
measurement of the source's delivered current or transient headroom.

The local LK source gives a more specific hazard.  In
`/mnt/Home/src/dguidipc-gemini-lk-android8/lk/platform/mt6797/mt_battery.c`,
the normal DLPT path disables the switch-charger power path, waits 50 ms, runs
five resistance samples that can each retry three times, and toggles PMIC
dummy loads before re-enabling the path (`:477`--`:497`, `:980`--`:1143`).
Commit `6dd23c1fffd7f4918d999f01f7059b55a0366108` adds the isolated
`GEMINI_USB_ONLY_SURVIVAL` path, keeps the power path enabled, and skips DLPT.
It does not prove an external source can sustain all earlier firmware load.
No retained measurement qualifies a specific adapter rating or ampere value,
so none is asserted here.

Consequently none of these establish battery buffering or transaction power:

- USB enumeration, USB Ethernet, SSH reachability, or long idle uptime;
- BQ25896 identity, `online`, `status`, `charge_type`, input limit, or fault
  fields;
- an inline USB meter by itself (it observes input, not battery connection);
- a single stale battery sample; or
- successful watchdog/rollback operation in an earlier run.

The BQ observer enforces the same distinction in
`/mnt/Home/src/nixos-gemini/repo-g3-p22/nix/bq2589x-preflight.sh`: it never
reads the fuel gauge and explicitly does not treat BQ status as external-power
presence.  The routing note is
`/mnt/Home/src/nixos-gemini/repo-g3-p22/research/mt6797-bq25896-routing-evidence/README.md`.

## Checks possible without opening or register writes

Offline now, only provenance and source consistency can be checked.  They
cannot change the ineligible decision.

For a later, separately authorized **read-only** check, an already-running,
hardware-proven environment may be used only if it already publishes the
actual battery power supply without loading a driver, applying an overlay, or
changing charger/PMIC state.  Read its cached/exported `present`, `health`,
`capacity`, `voltage_now`, `temp`, and `status` properties, plus the boot ID
and timestamp.  Repeat the battery-side snapshot to reject a stale or
disappearing supply.  Do not read `/dev/i2c-*`, use `i2c-tools`, use `/dev/mem`,
activate an observer, or enable/reconfigure the FGADC to obtain it.

The dormant mainline qualification code is not a drop-in solution for this
constraint:

- `drivers/power/supply/mt6351-fg-readonly.c` reads only raw FGADC control and
  instantaneous-current words; it does not establish battery presence,
  voltage, temperature, health, or capacity.
- `drivers/power/supply/mt6351-fgadc.c` initializes clock/control registers and
  uses write/latch/clear/restore transactions for coherent current/coulomb
  data.  Snapshot/restore is still a hardware write.
- LK's BATON and AUXADC routines also enable/configure PMIC fields before
  reading them.  They are not write-free observers.

An external USB power meter can supplement the battery snapshot by recording
input voltage/current and droop.  It cannot replace battery-side presence and
energy evidence.  Visual connector inspection cannot be done externally with
the device closed.

## Smallest authoritative pre-p22 contract

Use the following as one indivisible manual prerequisite immediately before
any future p22 transaction.  Any missing or ambiguous field is a stop.

1. **Battery-side identity:** from an already-running, known-good battery
   provider, record `present=1` and `health=Good`; repeat both with the same
   boot ID.  The provider must be the MT6351/vendor battery supply, not a BQ
   charger supply.
2. **Conservative stored-energy floor:** record `capacity` in `81..100` from
   that same qualified battery provider.  This is not a new electrochemical
   inference: it preserves the strict, source-pinned guarded storage-write
   contract already used by
   `/mnt/Home/src/nixos-gemini-wmt-bootstrap-coordinator-src/artifacts/power-preflight-debug-20260830/manual/candidate/experiments/2026-07-29-da921x-probe-isolation/scripts/install-boot2.sh`.
   Later one-use owner overrides at 70% and 60% were explicit exceptions and
   are not reusable precedent.
3. **Rail sanity:** record a plausible, stable battery `voltage_now` and valid
   `temp` twice.  The LK source's 3450 mV low-boot threshold is useful as a
   reject boundary, not proof of p22 headroom; at or below it is an immediate
   stop.  No tighter voltage margin is invented by this audit.
4. **Mechanical statement:** the owner confirms that the battery connector is
   still securely seated and has not again loosened or been damaged since the
   September 2 reconnection.  Software telemetry cannot prove mechanical
   integrity.
5. **Full-window redundancy:** retain a powered data path for the complete
   candidate-write, boot/timeout, reset, Stage-A rollback and readback window.
   An adapter label or enumeration is insufficient; if an external meter is
   used, retain its no-droop capture as corroboration only.  Reconfirm exact
   accepted Stage-A p22 SHA-256
   `1b5f808e494f8c91ec5feedcbce89bf018271b30b87ce3910082cd298d8817dc`
   and arm the independent rollback owner only after the power prerequisite
   passes.

This contract establishes a conservative operational precondition; it does
not claim a calibrated battery model or prove arbitrary peak-current margin.
Do not weaken it by substituting BQ identity/status or USB presence.

## User-action boundary

Accepted Stage A cannot supply the required battery-side fields, and obtaining
them by activating the dormant MT6351 paths would itself introduce hardware
writes.  Therefore software work stops here.

The least invasive future route is an owner-authorized, read-only observation
from an already-proven OS that already exposes the genuine battery supply,
without changing p22.  If no such environment can be reached safely, or if
`present`, health, capacity, voltage, or temperature fails the contract, the
next step is **user physical action**: power down, open the unit, inspect and
securely reseat or repair the battery connector, or have a qualified repair
technician do so.  No p22 transaction may precede that action and a fresh
battery-side recheck.
