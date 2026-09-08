# MT6797 SRAM first-start operation

`mt6797_sram_first_start()` is the provider-internal one-shot operation for the
future first-CPU8 path. It is separate from the ordinary regulator transaction.
It records attempted before calling the setter with110000, waits240us, and
requires two identical complete control/calibration samples. Control low12 must
be0x8fb; calibration must have nonzero low16 and zero upper16. Control upper bits
are allowed, including the upstream-observed0x4008fb. Every error after dispatch
latches fault, preserves the original error and refuses another operation. There
is exactly one setter dispatch and no inverse/retry callback in this interface.

This helper is deliberately not exported or connected to live SMC callbacks yet.
The caller integration must prove exclusive provider ownership, CPU8 target,
both big cores offline and the isolation boundary crossed, under serialized
provider lifetime/locking. It must keep attempt state for the provider lifetime,
block competing ordinary setters, and arrange independently supervised reset on
fault. The host tests establish operation semantics, not those caller contracts.

The initial-state contract also needs an attributable pair of complete cold
register samples from our phone. M7 retained only the low voltage selector;
nominal1050mV is insufficient proof of calibration/control state. No cold-state
admission check was relaxed in this change. No firmware or device actions occur.

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
