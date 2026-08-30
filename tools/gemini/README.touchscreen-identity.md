# Gemini NT36772 opt-in identity overlay

`mt6797-gemini-pda-touchscreen-identity.dtbo` is a deliberate, removable,
identity-only runtime gate.  The base `mt6797-gemini-pda.dtb` remains inert:
both I2C4 and `touchscreen@62` are disabled.  The read-only observer remains
the first diagnostic step.

Apply this overlay only on a recovery-capable mainline image, with no vendor
touchscreen client bound to I2C4/0x62.  It enables I2C4 and the one physical
`novatek,nt36xxx` client with `novatek,identity-only`.  Probe performs the
vendor-compatible stateful trim transaction, restores the controller through
the bounded bootloader-reset/reset-state path, emits the matched trim/chip in
the kernel log, and returns `-ENODEV`.  Consequently it does not register an
input device or request/enable an IRQ.  It never registers a second `0x01` DT
client and has no firmware update interface.

The transaction is bounded: five trim attempts, each with 35 ms + 15 ms +
10 ms protocol waits, then a reset-state poll capped at 100 iterations of
10--11 ms.  An absent or unpowered controller therefore returns an error; it
does not create an unbounded boot wait.

Known contract: vendor DWS maps `CAP_TOUCH_VDD` to MT6351 `VLDO28`, and the
mainline MT6351 provider models it as fixed 2.8 V.  The local filtered vendor
boot record identifies trim `00 00 03 72 66 03` as `NT36772` at I2C4/0x62,
with rising GPIO85/EINT8.  The overlay intentionally does not add
`vcc-supply`: this MT6351 regulator is dynamically registered without a
board-DT phandle, so the overlay cannot safely sequence the rail.  Validate
that VLDO28 is already enabled using the read-only preflight report before
applying the overlay.

GPIO68 remains untouched.  Vendor DTS/DWS establishes that it is the reset
net and provides low/high pin states, but no source proves which level is
asserted or the required pulse timing.  Do not add `reset-gpios` until a
recoverable hardware test establishes that contract.

After booting with the overlay, retain the `identity-only:` line with the trim,
then remove the overlay or reboot.  A successful log is evidence for a later,
separately reviewed full-input activation; it is not permission to change the
GPIO68 or VLDO28 contracts.  Do not use `/proc/NVTflash` or generic I2C scans.
