# MT6797 SRAM first-start operation

`mt6797_sram_first_start()` is the provider-internal one-shot operation for the
future first-CPU8 path. It is separate from the ordinary regulator transaction.
It records attempted before calling the setter with110000, waits240us, and
requires two identical complete control/calibration samples. Control low12 must
be0x8fb; calibration must have nonzero low16 and zero upper16. Control upper bits
are allowed, including the upstream-observed0x4008fb. Every error after dispatch
latches fault, preserves the original error and refuses another operation. There
is exactly one setter dispatch and no inverse/retry callback in this interface.

The built-in provider API now connects this operation to the A72 power driver.
It verifies the exclusive consumer handle, exact descriptor/driver/DT supply,
CPU8 target, CPU4 online, both A72s offline in Linux and PSCI, and actual SPM
external isolation clear. Supplier/device and provider locks protect the call;
a global LDO lock serializes ordinary operations even across duplicate provider
instances. A built-in boot latch survives reprobe and excludes all later ordinary
setters. Sysfs bind/unbind is suppressed. The ordinary regulator interface keeps
its selector-zero refusal.

Admission now requires a stable, full zero control/calibration sample and the
corresponding nominal1050mV reading. The sample is logged before admission, so a
different cold state is attributable. This cold contract matches upstream's
retained vendor first-cycle evidence; it is not yet verified on our phone.
Successful first-start is followed by another full-pair verification in the power
plan. The plan retains power on any failure after isolation was attempted.

Source evidence: MT6797 vendor import BigSRAMLDOSet and retained ixoo analysis:
https://github.com/freedomtan/atf-1.0-x20/blob/cd0343dab7bf6a8175352f4565f811000ec955ab/plat/mt6797/drivers/idvfs/mt_idvfs_api.c#L339
handoff/ixoo-sram-selector-mining-20260908.md

Run `HOSTCC=<native-cc> python3 tools/gemini/test-mt6797-sram-core-host.py`.
The runner compiles the actual ordinary and first-start functions plus the
actual KUnit cases. Tests cover stable full samples, setter/read errors,
instability in either full word, missing control bits, wrong voltage selector,
zero/invalid calibration, attempt-before-dispatch, missing backend callbacks,
and repeat refusal after success or failure. Existing ordinary-regulator tests
remain enabled, including selector-zero rejection. These are host tests with I/O
doubles, not hardware tests. Driver and KUnit objects also compile ARM64 W=1.
