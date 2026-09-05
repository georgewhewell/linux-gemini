# Supplier-owned AP CLDMA register lease

The description below records the lease-only change. The integrated reset
follow-up now requires reset completion from the same supplier epoch, returns
it with the mapping, and requires fresh clean queue/IRQ/head state before
the consumer's first register write. Post-GO IP_BUSY is observed separately
from the supplier's pre-power reset-time zero check; see
`README.cellular-integrated.md` and `README.cellular-post-go-ip-busy.md`.

Based on readiness source `b83d28018`. This fixes a deterministic reservation
collision, not PLL/reset admission or a demonstrated modem handshake. The cold
owner already uses `devm_ioremap_resource()` for AP CLDMA AO `0x10014000/0x1e00`
and PDN `0x10219000/0x1e00`. CCCI's second resource reservation necessarily
fails while the managed supplier link keeps that owner bound.

`mt6797_md1_cold_owner_acquire_cldma()` now validates both exact memory ranges,
establishes the existing managed readiness link, preserves GO/completed passive
observation/activity admission, and reserves one non-renewable consumer lease.
It drains the passive worker outside the evidence mutex before exposing the
supplier's existing mappings. The worker and its sole CLDMA reader reject
further observation after handoff. No duplicate reservation, unreserved remap,
new register write, DT enable, clock change, reset pulse or activity-gate
relaxation is introduced.

The supplier retains both memory reservations and mapping devres. Its lifetime
is held through consumer probe and unbind by the existing managed device link.
In this kernel `device_unbind_cleanup()` releases consumer devres before
`device_links_driver_cleanup()` permits supplier teardown. Consumer ring and
IRQ devres are acquired after the lease, so they are released first. Lease
cleanup clears the borrowed consumer pointer and latches terminal ownership;
it never grants access to another consumer or restarts supplier observation.
Acquisition failure after reserving a lease is also terminal. Pre-acquisition
readiness/range failures do not consume the lease.

The transport owns all AP CLDMA accesses after acquisition, including STOP,
masking and stop-status polling. Existing remove/shutdown cancels IRQ/NAPI/work
before issuing STOP; ring cleanup frees DMA only after both existing stop-status
checks succeed, retaining allocations on failure. Shutdown has no devres
release and does not prematurely revoke mappings needed by transport cleanup.
The supplier shutdown path drains its already-finished observer and disarms
stage2; it does not reclaim the engine or assert an unproven DMA stop. HS1 MPU
stage2 is a separate secure-provider operation, not access to these registers.

`test-cellular-cldma-lease.py` compiles the actual acquisition, release and
CLDMA-reader function bodies. Host adapters cover six malformed ranges, nine
readiness/lifetime failures, action allocation failure, shutdown during drain,
idempotent/wrong-owner cleanup, 32 concurrent two-consumer races and refusal of
all seven observer reads after lease. Source contracts check retained supplier
reservation, absence of transport reservation, devres/link order and a single
worker scheduling site. The existing actual handoff tests still pass. These
are not live device-core, hardware DMA or MMIO tests.

Both changed ARM64 translation units were cross-compiled independently. At
the lease-only revision, reset/clock admission and fresh idle/head/IRQ state
were still required. The current integrated follow-up supplies those checks
and replaces circular pre-ring activity admission; see
`README.cellular-integrated.md`. This historical source gap is not a current
battery, SIM or firmware-download blocker. Live handshake/data results remain
separate from source ownership tests.
