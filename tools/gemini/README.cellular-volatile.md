# Experimental bounded RAM backing: kernel layer

C17 hardware ended after two OPEN requests withflags0x20010400/0x10400:
CREATE set, READ_ONLY clear. The legacy read-only service correctly rejects
these before accessing its provider. This change tests a real writable backend;
it does not establish that the modem will reachHS2 or qualify persistence.

The legacy REGISTER ioctl still requires read-only ext4. A distinct REGISTER_RAM
ioctl requires the root-only provider interface, enable_volatile=1, writable
TMPFS_MAGIC in both inode superblock and statfs, nonzero capacity<=64MiB and
nonzero inode limit<=4096. Writable ext4 is refused. Runtime operations check
that the retained backend still satisfies its selected policy. No new path
allows writes through the read-only ext4 registration.

OPEN/OPENHINT/composite-open can create or truncate a regular file on RAM backing.
Parent components use existing confined lookup; the final component is looked
up under the parent inode lock on the same mount, refusing mountpoints, symlinks,
foreign superblocks and special files before creation. Name lookup never gets
O_TRUNC; truncation follows validation and references the exact tmpfs path.
Writes use kernel_write, retain partial count/error semantics and have a16MiB
per-file bound in addition to the tmpfs quota. READ/SEEK/CLOSE use real file state.
Legacy read-only writes still fail. Other mutating filesystem operations retain
their existing refusal behavior; no fake success was added.

Tests execute actual registration+ioctl functions (privilege/mode/type/capacity,
publication/cancellation/refcounts), actual provider functions with VFS adapters
(create/write/readback/truncate/partial/full/quota/confinement/failure cleanup),
and actual CCCI service with provider adapters (captured create flags, write
reply counts, malformed length, seek/readback, partial/full errors, nohandleleaks).
Existing readonly service/provider/worker/registration/helper tests pass. Provider,
FS service and CCCI objects compile ARM64 W=1.

Userspace RAM mounting/seeding/registration is NOT implemented yet. No bundle
or hardware test of RAM mode exists. It must use a bounded private tmpfs seeded
from the independently verified ro,noload nvdata/md, preserve metadata-only
logging, and never copy data back to persistent storage in this experiment.
Successful volatile testing would still leave production persistence to qualify.
