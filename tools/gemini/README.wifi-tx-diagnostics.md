# MT6797 radio TX timing diagnostics

This source adds a read-only `/sys/class/net/<interface>/tx_diagnostics`
snapshot for the cfg80211 radio, to investigate private queue delay after
M35's fq_codel comparison. It does not change the 64-packet queue limit,
below-32 wake threshold, qdisc, firmware commands, credit ownership or
security admission. It does not implement BQL or any TX-completion reporting.

There is no hardware qualification claim. Future deployment and measurement
must use an accepted candidate boot and the existing bounded benchmark guard.
The accepted M35 kernel and the isolated WPA3/PMF work are separate.

## Snapshot and lifetime

The netdev publishes one 0444 sysfs attribute via `netdev->sysfs_groups[0]`.
Registration failure and unregister therefore use the standard netdev/device
sysfs publication/removal path. Radio/wiphy state is freed after unregister,
which withdraws the attribute and drains active sysfs readers. No extra device,
open debugfs handle, write callback, reset control, work item or firmware query
is introduced.

The show callback copies the complete counter structure plus connection state
and monotonic timestamp under `radio->lock`, then formats the copy outside
the lock. Packet queue mutations and diagnostic state transitions use that
same lock. Formatting is bounded to one sysfs page, including maximum-width
64-bit counter values. Only counts, durations and boolean state are emitted;
there are no packet contents, keys, SSIDs, addresses or packet identifiers.

Counters start at radio allocation/registration and remain cumulative across
interface stop, cancellation, reconnect and failure. `lifetime_start_ns` plus
boot/netdev identity identifies that lifetime. `connection_generation`
increments when a connect request is admitted and published to the connection
worker; it does not claim a successful association. `connected` excludes a
cancelled connection; `stopping` is the radio's unregister state. Read calls
never clear or alter counters.

## Fields and boundaries

| Field or prefix | Meaning |
|---|---|
| `version`, `snapshot_ns`, `lifetime_start_ns` | Format version 1 and monotonic nanosecond observation/lifetime clocks. |
| `queue_depth`, `queue_high_water` | Current/max accepted SKBs waiting in the private software queue; excludes the worker's current SKB and firmware buffers. |
| `queue_limit`, `queue_wake_below` | Existing queue policy: 64 and 32. Informational, not writable. |
| `enqueued`, `dequeued`, `purged` | Accepted SKBs, SKBs selected by the send worker, and queued SKBs freed during cleanup without a send attempt. |
| `rejected`, `busy` | SKBs dropped by xmit admission and NETDEV_TX_BUSY returns. A busy return retains caller ownership and leaves `skb->cb` untouched. |
| `hif_submitted`, `send_failed` | Worker sends accepted by HIF transport versus worker validation/encoding/transport failures. Neither is an airtime or firmware-completion count. |
| `send_cancelled`, `send_disconnected`, `send_timed_out` | Subsets of send_failed for -ECANCELED, -ENOTCONN and -ETIMEDOUT. Other failures are the remainder. |
| `queue_wait_*` | Accepted enqueue timestamp to worker dequeue. Includes worker scheduling and connection-lock waits while that SKB remains queued. Purged SKBs do not produce completed queue-wait samples. |
| `send_duration_*` | Dequeue through validation, encoding, lifecycle mutex, transport/credit wait, existing packet accounting and local SKB release. |
| `credit_wait_attempts`, `credit_wait_packets` | Number of waits after transport returned -EAGAIN, and number of SKBs that encountered at least one such wait. |
| `credit_wait_*` span | Wall time around each existing completion wait (including scheduling/reacquiring the diagnostic lock). These waits remain bounded by the existing 50-ms poll and one-second send deadline. |
| `credit_wait_wakeups`, `credit_wait_timeouts` | Wait returned with completion credit versus exhausted its poll timeout. A poll timeout alone is not a failed send. A wakeup does not prove a packet was transmitted. |
| `worker_duration_*` | Entire completed TX-worker invocation, including connection-lock acquisition and its packet loop; empty invocations also count. |
| `worker_active`, `send_active`, `credit_wait_active` | Incomplete worker/send/wait spans at snapshot time; each matching `_active_ns` is its current age, zero when inactive. |

Each duration span emits `_count`, `_total_ns`, `_max_ns` and `_hist`.
Histogram buckets have inclusive upper bounds of 0.1, 0.5, 1, 2, 5, 10,
20, 50 and 100 ms, followed by an unbounded >100-ms bucket. The same bounds
are published in nanoseconds as `hist_upper_ns`.

Coherent snapshot invariants, before counter wrap:

- `enqueued = dequeued + purged + queue_depth`.
- `dequeued = hif_submitted + send_failed + send_active`.
- `queue_wait_count = dequeued`.
- `send_duration_count = hif_submitted + send_failed`.
- `credit_wait_attempts = credit_wait_count + credit_wait_active`.
- `credit_wait_count = credit_wait_wakeups + credit_wait_timeouts`.
- Each histogram's bucket sum equals its corresponding sample count.

## Future interval interpretation

For a later authorized benchmark, capture boot ID, netdev identity and one
whole attribute read before/after the same three saturated uploads and one
download used for M34/M35. Preserve the matching qdisc snapshots and idle/loaded
ping/iperf outputs. Collect only the snapshot; no scan, reconnect, key query,
AP change or register/memory read is required. Repeated reads are observational.

Require the same boot, netdev, lifetime and connection generation. Reject
counter decreases/wrap; counters are unsigned 64-bit and have no reset API.
For clean completed-interval comparisons, use boundary snapshots with an empty
queue and inactive send/credit worker spans. Otherwise explicitly report the
incomplete spans, whose durations may straddle interval boundaries.

Subtract counts/totals and histogram buckets; calculate a span mean as delta
nanoseconds divided by delta count. `_max_ns` and `queue_high_water` are lifetime
maxima and cannot be subtracted to obtain interval maxima. Histogram quantiles
are bucket bounds, not exact packet percentiles. `_active_ns` is an observation,
not a cumulative counter. Do not add queue waits across different packets to
worker duration, or add credit wait to send duration: those intervals overlap.

Large queue-wait samples would directly establish private software residence.
Large credit-wait time would identify time spent in the driver's existing
credit wait. `send_duration - credit_wait` still contains validation, local
locks/scheduling and HIF writes; it is not measured firmware execution time.
Nothing here measures firmware queue occupancy, over-the-air completion,
retransmission, airtime or the AP's queue.

## Cost and validation

Storage is one fixed aggregate structure per radio plus eight bytes in the
accepted SKB's driver-owned control buffer. There is no per-packet allocation
or logging. Normal send instrumentation uses a clock at enqueue/dequeue/send
finish, two clocks per worker invocation, and two per credit wait. It adds a
short radio-lock section at dequeue, reuses the existing xmit/send-finish locks,
and records fixed ten-bucket histograms. This overhead is bounded but has not
been measured on hardware; compare throughput/timing when qualifying it.

`test-wifi-radio-tx-host.py` executes the actual xmit, send, credit-wait, worker,
purge and snapshot code with a deterministic clock, real production codec and
skb/transport fakes under ASan/UBSan. It checks histogram boundaries, independent
queue/send/credit timing, coherent reads during active waits, success, wakeup,
poll retry, deadline, cancellation, stopping, transport/allocation/linearization
failure, full-queue retry ownership, purge/reuse and maximum-width formatting.
The existing TX test entry point also runs
`test-wifi-radio-tx-diag-lifecycle-host.py`, which executes production
registration/stop/unregister with allocation, netdev and sysfs fakes. It checks
all registration failures, readable snapshots after stop, queued-send cleanup,
read-only publication and unregister-before-free ordering. The separate
connection-worker/key/link/RX/PM/admission tests preserve existing behavior.
Host tests do not simulate real IRQ/workqueue concurrency or the kernel's
sysfs active-reader barrier. ARM64 compilation and source lifecycle review
supplement them; hardware latency/overhead qualification remains future work.
