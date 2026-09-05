# T9: verified one-boot Panfrost rendering

Candidate `ebee6334c0c8253a27c81faac53f9b7d16f770ce467a213a9532d5788fa694a1`,
source02c02d16e3bc22a3f7844735bc1379cc6f457913, package d84d968764281be1c26a1e66d3062da9dec360cb.
Boot `c6e1cae9-798c-4951-89f8-7b4b316f033c`.
Saved evidence: `handoff/gpu-owner-t9-9CHh5QF9/ram-evidence/run/gpu-oneboot`.
This image has been consumed once; no retry.

Initialization passed the unchanged owner collector: fixed238MHz, suspended
GPU, external rail disabled850000:0 and stable zero initial job/MMU IRQ counts.
The inherited-rail correction clears T8's admission failure without a forced
initial rail cycle. It does not remove the six-domain quiescence check.

One unchanged renderer script invocation completed exit0. Root independently
read its saved result and domain evidence:

- Mali-T880 MC4 (Panfrost), Mesa26.2.1/OpenGL ES3.1.
- Two render runs, each two draws and8192 exact-readback pixels PASS.
- Two autosuspends, all six MFG domains off after each run.
- Job IRQ counts0→8→16, MMU IRQ counts0; identical before/final boot IDs.
- No GPU/kernel error matching the reviewed complete-log scope.

Pre-AS GPULDO diagnostic records fc0/fc4=0f0f0f0f andfc8=0000000f, all reads
successful; first submit AS status/fault are zero. Independent agent review
found no GPULDO_FIRST_FAILURE, GPU_SHAREABILITY_FAULT or job timeout. An
unrelated existing da9213 legacy probe-22 is not evidence that all CPU work is
complete and is not hidden as part of the GPU result.

Root independently hashed the fsynced RAM archive:
`5d7f1527792f3599653413f45b3e671d216e6e12c82f7e602d6d499cbbe5dfd0`.
Root reran the immutable paired thermal checker: four readable physical SoC
channels, same boot, PASS. Calibration accuracy and thermal protection remain
unverified; battery temperature remains physically disconnected.

This proves the reviewed fixed-profile one-boot render/autosuspend/render
workload, not sustained-load stability, general DVFS, system suspend, display
integration or a deployed default configuration. Root authorized a guarded
same-boot pass marker only after saved evidence review. Original execution18065
and the sole owner still retain supervised exact-rescue responsibility; final
rescue/pstore/hash/network/process closure is pending at this note.
