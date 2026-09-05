# MT6797 first GPU event diagnostics

R5 and R7 both failed their first real draw with GPU-level `0x00ff0388`,
unusual high fault addresses and subsequent `AS_ACTIVE` timeouts. R7 proved
neither effective AS attributes nor any job/MMU IRQ progress. Changing memory
attributes again without that evidence is not justified.

This source-only diagnostic adds no register writes, retries, attribute changes,
power transitions, reset requests, submissions or hardware-support claims.
The platform gate is the existing MT6797 vendor-init function identity.

## Records

`MT6797_GPU_DIAG` records are emitted once each for:

- First actual submission, immediately before the existing START write while
  the submit path holds its runtime-PM reference and job lock: actual io-pgtable
  format/quirks, coherency selection, expected AS configuration, readback,
  full root-table equality, slot head/config and raw interrupt/L2 state.
- First GPU error IRQ, before existing interrupt masking/clearing: captured
  fault status/IRQ, raw statuses and L2 config, up to eight present AS records
  and three present slot records. The existing fault address capture remains
  together and is not separated by diagnostic printing.
- First MMU poll timeout, before the existing reset scheduling: AS, requested
  command, before-command versus completion phase, final polled status and
  current AS/raw interrupt/L2 configuration.

At most 20 new lines and 109 register reads across the three events on T880.
These are sequential observations, not an atomic hardware snapshot. The
diagnostic does not stop existing recovery or promise that another context
cannot concurrently alter register contents. A fault/timeout before successful
submission can legitimately leave the submit record absent.

No physical page-table address or kernel pointer is printed. TRANSTAB has
only its low 12 configuration bits exposed, plus a full-value equality result
at submission. Job-chain heads are GPU virtual addresses, not kernel pointers.

## Lifetime and power

Atomic test-and-set claims in the zero-initialized device object limit each
event to one winner even across CPUs. GPU resets, runtime resumes and retries
do not clear those bits. There is no userspace rearm endpoint. The bound is
per probed device lifetime; unbinding/reprobing or unloading/reloading the
driver creates a new device lifetime and is outside the one-boot experiment.

All three callers already execute on a powered submit/MMU path or live GPU
IRQ. The additional gate refuses any suspended GPU/job/MMU component, without
consuming the event or reading registers. Runtime suspend synchronizes the GPU
IRQ before power-off, and submission owns an existing runtime-PM reference.
The diagnostic never obtains its own PM reference or touches MFG syscon/PMIC.

## Validation

`HOSTCC=<host-gcc> python3 tools/gemini/test-gpu-first-fault-diag.py` compiles
the actual diagnostic block with counted fake MMIO and the actual register
definitions. It covers powered/platform refusal, all suspended-component bits,
three event budgets, absent-unit masks, root mismatch/redaction, repeated-call
refusal and a 32-thread atomic-claim race. Source checks enforce read-only
bodies, no pointer formatting and placement before START/reset/IRQ masking.

GCC 12.2.0 arm64 cross-compilation of the complete Panfrost directory passes.
Checkpatch on the production diff: zero errors/warnings. No hardware run or
boot package is part of this result.

The deletion condition is an attributable first-failure diagnosis and a
reviewed fix; this is not intended as a permanent noisy driver interface.
