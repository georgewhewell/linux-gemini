# Experimental read-only MD1 filesystem metadata authority

This source is based on C8 `b71187c5f02b8ee63225c65892a806e7f42f0a92`.
It implements a real, narrowly scoped FS GETDISKINFO service and a pre-write
cold-start gate. It is not a demonstrated HS2 fix and is not a hardware/package
authorization. The source change alone selects no backing directory; the later
installed-lineage and metadata checks below establish the real nvdata `/md`.

## Current integration and candidate boundary

Installed Android25 provenance and private metadata-only inspection have resolved
the backing question: existing `/dev/mmcblk0p6` (nvdata), existing directory
`/md`. This is not an unresolved filesystem-location blocker. The helper now
also accepts `--check-device`, sharing its partition/superblock checks without
opening mount directories or the provider and without issuing any ioctl.
It rejects both journal recovery and ext4 orphan-recovery indicators before
the separately controlled RO+noload mount; registration repeats the checks.

The separate C9 package builds that helper, one-shot prepare and a host launcher.
Root reviewed candidate SHA256
`2c2304ff4c73b670e09ac8d6d3d0897336865573ff62480a22a1f1b181f21eba`
and authorized one sole-owner observe-always-rollback run in
`handoff/cellular-c9-root-review-20260905.md`. This document does not assert
that run completed or that HS2, AT, attach or data work. The launch deadline
receipt is fixed at original transaction launch+540 seconds, conservatively
earlier than the harness's acceptance anchor; it cannot create a new window.
Registration can synchronously start the modem and is a write boundary, not
a read-only diagnostic. Never run it in rescue or retry an ambiguous result.

## Observed missing service

C8 boot `c9c9263b-149f-461a-b6c2-e2458041a3c4` progressed through twelve EINT
responses with queue-1 RX unmasked, then received a complete 44-byte channel-14
FS request on queue 1: operation `0x100e`, two arguments, UTF-16LE Z root and
flags 3. The packet SHA256 is
`4d595c27c4bf7a1a986fba329ef42390efe3b021a7537bb78f587d6550b65ead`.
The owner-restricted evidence is in
`handoff/cellular-c8-ZacNg6I4/ram-evidence/run/cellular-oneboot`.
There was no HS2 or AT result; the existing AP handshake timer fired.

The C8 kernel reached `/init` only around 48.14 seconds, after the timer at
37.89 seconds. Consequently adding a userspace responder without controlling
the initial cold start cannot satisfy this observed initialization sequence.
This patch defers cold probe until a real metadata authority is published.
It does not alter the A72/DVFSP sequence, the recovery watchdog, the one-GO
contract, or the literal existing 30-second timer from CCCI queue start.

For interpretation of the next trace, vendor eccci `port_proxy.c:486,628`
emits FS_IN after accepted pre-HS2 FS RX and FS_OUT after successful FS TX.
`ccci_fsm.c:463` toggles fs_ongoing on these events; its handshake loop at
line232 resets the timeout count while FS is ongoing. This source uses the
reviewed fixed 30-second bound instead. A timeout following a new unsupported
FS operation therefore does not itself prove rejection of the preceding
metadata response. Correlate actual reply, subsequent request and HS2 first.
This comparison is not authorization to extend the timer or invent responses.

## Vendor wire and stock mapping evidence

The published vendor header is
<https://github.com/andr3jx/MTK6577/blob/master/mediatek/source/external/ccci_fsd/ccci_fsd.h>.
It identifies GETDISKINFO `0x100e`, BASIC_INFO=1, FREE_SPACE=2,
FAT_STATISTICS=4, five FS request slots and the 84-byte FS_DiskInfo structure.
Response operation is `0xffff100e`, channel 15, with two length-prefixed
arguments: four-byte signed status and 84-byte DiskInfo. Total wire size is 120.

Actual Gemini Android25 daemon SHA256
`688e9f4d3bca872b8d6db883fa4b268e57e40e212fe4d6cdc292da3c8b836cdf`
confirms dispatcher `0xd510`, table slot `0xd580`, handler `0x621c`,
`statfs` at `0x622c`, and response construction `0x79a0`/`0xd838`.
Failure returns signed -1, not fabricated success. Its success path writes a
constant 512 into FreeClusters; this implementation deliberately returns the
actual validated statfs free-block count instead. Unsupported output fields
are zero-initialized rather than copying uninitialized vendor stack fields.

The existing stock `Gemini_x25_x27_06052019/system.img` was converted in a
private host temporary directory, without modifying the original. Read-only
debugfs extraction of `/vendor/bin/ccci_fsd` produced SHA256
`8f504cd37f3d36a07764de17cb2cafd61a6b187db502cd90128e3020b05bceed`,
identical to public Gemini Android27 commit
`300f49cfdb86ee499c958ab612c2dde38841b5c1` in
`Sailfish-on-Cosmo-Communicator/vendor_planet_geminipda`.
Stock build metadata identifies `Gemini-8.1-Planet-04162019-V2`.

Independent Android27 static audit establishes actual mapping, not strings
alone: dispatcher `0x11a18`, table base `0x11a44`, slot 13 at `0x11a78`
targets GETDISKINFO `0x13168`; `statfs` at `0x13174` uses root global
`0x26484`. FS_CCCI_GetClusterSize at `0x18388` maps V..Z using table
`0x18548={4,3,1,2,0}`, selecting that same root for Z. Startup `0x403c` /
`0x4050` / `0x4058` copies source `0x1f001` (`/vendor/nvdata/md`) into
`0x26484`; the alternative startup branch `0x4464` / `0x4470` / `0x4478`
does the same. No daemon was executed. Its mkdir/NVRAM/OTP operations are
outside this service and must not be used to initialize the board.

Stock Android27 mapping does not by itself prove the installed Gemian daemon
version or authorize any current mount. The saved accepted-rescue inventory
has `/gemian` already ext4 read-only with norecovery, and an existing
`/gemian/data/system.img`; the installed daemon lineage and exact real Z backing
directory still require read-only provenance checks before provider installation.
Neither `/gemian` generally nor an arbitrary host filesystem is an acceptable
substitute for the modem backing. Do not create directories or provision NV data.

### Subsequent installed-lineage verification

The persistent hardware owner completed one bounded read-only copy of the
existing `/gemian/data/system.img`, without mounting or changing it. The
642129920-byte host copy SHA256 is
`3e3bb6b2fa1410acd98488bd8214afba6e715fdff9a318e8186ed44cd22ce2fa`.
Source metadata matched before/after and the accepted rescue boot remained
`671e5fa7-d61d-477d-bfc5-f0465df05182`. Existing mountinfo identifies `/gemian`
as p29 ext4 read-only with norecovery.

Read-only host debugfs extraction of `/vendor/bin/ccci_fsd` produced
`688e9f4d3bca872b8d6db883fa4b268e57e40e212fe4d6cdc292da3c8b836cdf`,
independently hash-checked by root: the already analysed Android25 daemon.
Extracted build metadata says `Gemini-7.1-Planet-08102018-V1`, SDK25,
Android7.1.1. This supersedes the installed-version uncertainty above; the
Android27 stock mapping must not be substituted for this installed version.

Root independently parsed only init/fstab metadata from the installed image's
embedded `/boot/android-ramdisk.img` (gzip/newc), SHA256
`c6a3fab29ecf21fe5ba8ace12fa1f3ada2083c2cd810b24e03ae9301c3d41e1a`:

- `fstab.mt6797:16` mounts by-name nvdata at `/nvdata` as ext4.
- `init.mt6797.rc:85` invokes that fstab; line137 symlinks `/data/nvram`
  to `/nvdata`.
- `init.modem.rc:26` creates the modem directory `/data/nvram/md`; line140
  launches `/vendor/bin/ccci_fsd 0`.

Thus the installed daemon's Z root resolves to **nvdata's `/md`**, not a
directory on Gemian p29. The absent `/gemian/data/nvram/md` is expected and
does not prove absent provisioning. These Android scripts are provenance
only; their write/check/formattable behavior must not be executed.

The sole hardware owner subsequently verified the live by-partlabel nvdata
target is `/dev/mmcblk0p6`, PARTNAME nvdata, PARTN6, 33554432 bytes, ext4,
currently unmounted. Its block read-only flag remains unchanged at zero.
Existing `/md` and filesystem cleanliness still need read-only metadata checks.
No directory authority has been registered; do not initialize or substitute
modem storage. A candidate must use the exact existing directory through a
read-only, no-journal-replay mount and the one-shot FD registration interface.

### Fresh backing metadata and standalone helper

The sole hardware owner then verified the actual `nvdata` partition is
`/dev/mmcblk0p6`, PARTN6, 33554432 bytes, and copied it once to a private host
artifact for metadata-only inspection. Saved `nvdata-superblock.txt` reports
ext4 magic EF53, clean state without needs_recovery, 8192 blocks of 4096 bytes,
and last mount point `/nvdata`. `nvdata-md-stat.txt` proves `/md` already exists
as directory inode13. No provision file contents were inspected and no device
mount or modem start was performed. This establishes a genuine backing, not a
synthetic filesystem. The observed free count is evidence only, never an input
constant to the service.

`tools/gemini/mt6797-fs-register.c` now implements the standalone helper portion
of the proposed sequence. It takes only an absolute pre-established RO nvdata
mountpoint; it does not mount anything. It checks fresh kernel partition name,
number and sector count, block-device byte size, and exactly the 1024-byte ext4
superblock metadata. Dirty, error, recovery-needed, non-ext4 and invalid geometry
are refused. The mountpoint must be the actual filesystem root (inode2) on that
same device, with ext4 type and read-only mount flags. It opens only `md`, using
openat2 with no symlinks, no escape and no child mount traversal. Missing `/md`
is refused, not created. The kernel additionally enforces RO superblock state.

Exactly one REGISTER ioctl is issued with the open directory FD. EINTR or any
other result never causes an ioctl retry or cancel. All local descriptors are
closed on every path; successful publication retains the kernel reference.
The helper contains no daemon execution, mount, formatting, repair, directory
enumeration or provisioning-file reads. It is not installed in an initramfs by
this commit and must not be run on the rescue boot.

`test-cellular-fs-register-helper.py` compiles the actual helper against syscall
adapters, not a duplicate algorithm. It passed label/number/size, ext4/dirty/
needs-recovery, wrong-device/root, RW mount, missing/symlink `/md`, short
superblock-read and ambiguous ioctl fixtures, checking no more than one ioctl
and complete descriptor cleanup. The helper passed host and ARM64 GCC12 syntax
compilation. Tests invoke no actual mount, ioctl or block-device access.

## Authority and concurrency

`CONFIG_MTK_MT6797_MD1_FS_PROVIDER` is built-in-only and defaults off.
Even when compiled, `mt6797_md1_fs_provider.enable=1` is required at boot;
the parameter is read-only. Without that opt-in, existing cold behavior and
unsupported-FS capture remain unchanged. This patch changes no DT or config
fragment and installs no provider command in init.

The optional misc device is mode 0600. Registration/cancellation additionally
require CAP_SYS_ADMIN in the initial user namespace. UAPI registration version
1 accepts an already-open directory FD, zero flags and zero reserved field.
The provider validates the actual file, local ext4 superblock, read-only mount
and read-only superblock, then calls statfs once in process context and validates
its returned type and representable block counts. No modem-supplied path is
opened, and no VFS callback runs in NAPI. Cached geometry is based on real
statfs, uses 512-byte sectors and preserves real free-block counts.

State is EMPTY -> INSTALLING -> READY -> CONSUMED under one IRQ-safe lock.
Cancellation before claim is final and releases references outside the lock;
cancellation after claim is refused. Invalid first installation consumes the
registration opportunity. Claim and cancellation cannot both succeed. A claim
retains the file/mount and exact cold-device references for this boot, even if
the subsequent cold sequence fails. Stats are visible only after claim, and
the current RO flags are checked again without sleeping before each snapshot.
This is explicit privileged authority, not protection against a privileged
actor deliberately remounting or writing the backing by another route.

The early cold require gate precedes resource setup and all firmware/MPU/power
writes. A second atomic claim occurs immediately before mainline firmware load
or, for LK-loaded firmware, before initial MPU. Post-claim failures cannot
re-enter the sequence. Existing independent gates and watchdog checks remain.

Successful registration reports authority publication, not successful GO.
It requests attachment of only the single available cold-owner device. If an
ordinary deferred probe already consumed the authority, there is no additional
attach. If cancellation wins before attachment, the regular require/claim gates
refuse all writes. A concurrently started attach is serialized by device core
and still must claim; no global reprobe trigger or new workqueue is introduced.

## Transport limits

Only unfragmented complete channel-14 GETDISKINFO with exactly two bounded
arguments is accepted. Index must be 0..4; the only path is the literal UTF-16LE
Z root. Flags 1, 2 and 3 are supported; FAT statistics and unknown flags return
the vendor-defined unsupported error. Unknown operations and malformed frames
remain in the bounded pre-ready capture; they do not silently succeed.
An absent/revoked stats snapshot gives the actual defined statfs failure status.

The bounded synchronous NAPI handler copies immutable cached stats and allocates
one 120-byte reply using GFP_ATOMIC. Queue 1 and failed-state checks are required.
Existing TX handles channel-15 sequence assignment and request-index preservation;
incoming sequence is not copied. The existing TX/STOP lock serializes publication
against terminal stop, with DMA mapping and allocation outside that lock.
Successful TX owns the reply through reclaim; failed TX frees it exactly once.
Received SKBs are linear and DMA-unmapped before dispatch and released by dispatch.
The first sixteen FS result logs contain operation/status/length, not raw paths.
Sensitive request capture retains its existing bounded, owner-restricted policy.

## Validation

Use the pinned GCC12 host compiler via `CC`; default `cc` is absent on this host.
Actual-code tests:

- `test-cellular-fs-provider.py`: literal 44/120-byte wire fixture, all output
  bytes, flags/errors, short/overflow bounds, real-stat conversion, and 256
  concurrent claim/cancel races.
- `test-cellular-fs-registration.py`: actual FD/statfs/publication/claim/cancel
  implementation with reference-counted adapters; stale FD, RW flags, both type
  checks, overflow/failure, publication-to-claim/cancel races, exact-device attach
  failure, repeated registration, and retained consumed authority.
- `test-cellular-fs-start-gate.py`: actual probe first-write slices on both firmware
  paths, every post-claim stage failure, no-write deferral/cancellation, and byte-
  identical CCCI queue-start/timer tail compared with the exact C8 source.
- `test-cellular-rpc-transport.py`: actual pre-ready RPC/FS dispatch, TX and STOP,
  channel/index/independent sequence, unsupported capture, queue enforcement,
  allocation/DMA/prepare/publish unwind and concurrent publish-before-STOP.

All above passed, together with existing EINT, first-fatal, terminal, NAPI/W1C,
reset-admission and cellular-integration tests. ARM64 GCC12 object compilation
passed for provider, cold owner and CCCI in a fresh outdir with the C8 config plus
only the provider config enabled. No image was built and no device was accessed.
