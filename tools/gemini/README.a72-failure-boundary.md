# A72 isolation boundary and retained faults

The transaction latches `isolation_attempted` before dispatching the SPM big-buck
isolation-clear operation. A bus error does not prove the write had no effect.
Once latched, apply/verify failure or CPU_ON abort returns EUCLEAN, faults the
owner, and performs no transaction rollback. The live retain callback releases
only the task-owned operation mutex. It retains provider references and hardware
state; existing fault admission/removal checks prohibit another attempt/removal.
There is no automatic watchdog action in this callback: the candidate harness
must independently arrange recovery for an unsuccessful CPU_ON attempt.

Before this boundary, existing rollback remains available. Admission now requires
big-buck isolation initially asserted, so this earlier rollback is not attempted
against a cluster already exposed to its supply. The backend also refuses inverse
operations for isolation, SRAM and the later reset/guard steps if called directly.

Evidence: the 2016 MT6797 vendor import's BigSRAMLDOSet changes calibration and
control bits as well as the voltage selector:
https://github.com/freedomtan/atf-1.0-x20/blob/cd0343dab7bf6a8175352f4565f811000ec955ab/plat/mt6797/drivers/idvfs/mt_idvfs_api.c#L339
The source does not prove an electrical inverse. The retained ixoo investigation
under handoff/ixoo-sram-selector-mining-20260908.md establishes the corresponding
one-way startup policy and full-word verification requirements.

This is one stage of the startup correction, not a boot-ready candidate. The
ordinary SRAM regulator backend still has internal rollback and selector-zero
refusal. A dedicated once-only SRAM initialization operation, its ownership and
full-word completion checks, must replace its use in this plan before hardware
qualification. Pre-boundary regulator ownership/uncertain-enable handling also
needs complete review. Do not infer safe first-start from these tests alone.

Verification: test-mt6797-a72-power-core-host.py compiles the actual transaction
core and KUnit case bodies. It injects failures before and after apply effects,
and during verification, at all 12 steps; checks retained electrical state,
zero inverse calls after the boundary, latch visibility at dispatch, retry
refusal, successful commit and retained abort. It also executes the actual live
retain callback with a mutex double, including already-released and repeated
cleanup. These are host model tests, not a hardware or in-kernel execution claim.
The driver, core and KUnit translation units also compile for ARM64 with W=1.
