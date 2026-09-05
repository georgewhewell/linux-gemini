# C7: EINT replies, no HS2, recovered

Candidate `6590a389cfe502e998f4185888374f7fa6e6b9a51dae43795973179e95797972`
used source `885b656b6497f1e4b9f345d902c315fc1b1b7f00` and boot
`4b4621f6-5ff9-4921-b290-5ec2e23926cd`. One installed test returned no-HS2;
no AT, acceptance publication, retry or other hardware experiment occurred.

Saved first-event and final dmesg both show nine RPC4005 replies queued with
tx_result0: six status0 and three QUERY_STRING(-11). This demonstrates that
the new service ran, not that every reply was consumed or HS2 completed.
No unsupported pre-ready packet was retained. First fatal is the AP handshake
timer at37.892873, ready0, runtime636 and Q0 submitted/reclaimed1/used0.
The latest saved IRQ has TX status0x202 and RX status0 with RX mask0xff00ff02.
That is an IRQ-time observation, not a fresh fatal-time MMIO read.

Source review found lost interrupt-mask ownership across NAPI repolls:
`mt6797_ccci_poll` consumes rx_pending into a local variable, drains RX,
then returns budget if a concurrent TX IRQ adds work. The next poll has no
old RX bits and completion unmasks only its new local bits. The old queue
can remain masked. Local TX mask bits are likewise lost across repolls.
This matches the new burst traffic and saved queue1 mask, but hardware
causality and successful startup after a fix remain unproven. The fix needs
separate persistent mask ownership, successful-completion-only rearming,
false-completion/new-poll race coverage, and serialization against STOP.
Changing the legitimate missing-SIM error responses is not justified by this
queue-state evidence.

Restricted handoff: `handoff/cellular-c7-H6VOO3S1`. Full RAM archive SHA256
`930d50112c513da218d5d8b6d406ac121e0e2d6e66155531f555653c2ff2e005`;
live DT SHA256
`f98f2cb7374d07b76fabb97fce4bc1f307a9a5b2e31b3b50b8c191afce87abbe`.
Pstore SHA256
`bb5bc3d05f94f4ad6be332f980f12b6e36b5f85d1143defe003e1e83d7300501`
matches the harness archive and contains C7 recovery marker156.186771.

Original execution55494 closed at06:37:14 with normal recovered-and-captured
state. The corrected harness captured and cleaned up without a false reset
request or manual holder cleanup. Rescue boot
`1e50b625-3eb9-4258-bea4-3dea24513f95` passed both USB/RJ45, CPUs0–7,
no failed units and exact p22/p30/p31 hashes. All owner processes are gone
and locks free. C7 is consumed; do not repeat it.
