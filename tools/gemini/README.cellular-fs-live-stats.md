# Live filesystem statistics

GETDISKINFO and GETCLUSTERSIZE previously read the snapshot saved during
root registration. Writable RAM roots can change after that snapshot, so
free space became stale after modem writes, truncation or deletion.

stats_drive now retains the consumed root, runs the existing statfs/root
validation in process context, and releases the reference. It preserves
local-filesystem type checks, representable geometry, and bounded tmpfs
capacity/inode limits. It rechecks backing policy after statfs and only
publishes the output on success. Registration caches are removed; registration
still validates geometry before publishing authority.

All current driver callers are in the process-context filesystem service.
No VFS callback runs while the provider spinlock is held. Queries on a root
which has lost its backing policy fail instead of reporting stale capacity.
Free-space values are point-in-time observations, not allocations/reservations.

Validation: the actual registration/provider test executes live stats calls
with instrumented statfs. It covers independent changing values for Z/X/Y,
query errors, invalid geometry, post-query policy drift, unchanged output on
failure, retained-reference cleanup and over-limit tmpfs. Service and codec
regressions pass. ARM64 W=1 provider and FS service object builds pass.

This is not real-filesystem or hardware qualification. CI8 still runs f8029;
the current namespace/live-statistics development kernel remains unbooted.
