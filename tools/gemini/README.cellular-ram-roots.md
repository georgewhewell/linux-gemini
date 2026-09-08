# Atomic Z/X/Y RAM filesystem authority

CI2 hardware traces show failed X:/Y: compact OPEN stages followed by Z->X
and Z->Y MOVE replies with PATH_NOT_FOUND before the modem watchdog fires.
The stock daemon maps these roots to /protect_f/md and /protect_s/md.
This identifies missing roots; it does not prove the only watchdog cause.

REGISTER_RAM_ROOTS (ioctl M/f3) takes version 1, three directory FDs in fixed
Z, X, Y order, and zero flags/reserved. All three must be non-write directory
FDs on distinct writable tmpfs superblocks. Each filesystem must have finite
capacity <=64 MiB and <=4096 inodes. Distinct superblocks prohibit aliases,
ancestor roots and shared quotas. Total possible capacity is therefore 192 MiB.
One lock-protected publication installs all three after validating every FD.
The cold owner requires all roots to remain accessible before consuming them.
Failure releases all temporary references and cancels this boot's authority.
Cancellation releases every published root unless already consumed.

Legacy REGISTER and REGISTER_RAM retain their existing single-Z behavior.
The new *_drive kernel APIs take an explicit ASCII Z/X/Y plus a relative path;
unknown or unregistered roots cannot fall back to Z. Existing wrappers still
select Z. All pathname walks and RAM mutations retain the existing component,
mount, inode-type and superblock confinement checks. Read/write file handles
continue to refer to their opened root independently of subsequent requests.

This commit adds the provider interface only. The protocol decoder/service,
userspace source validator, and NixOS preparation must be connected before
X/Y requests can succeed on hardware. MOVE semantics and persistent writes
remain unfinished. There is no new image or hardware qualification here.

Device source evidence: protect1 is GPT partition8, 8 MiB; protect2 is
partition9, 12.46875 MiB. Both ext4 filesystems are clean, match their partition
geometry, and contain /md. They have identical filesystem UUIDs: never identify
these two roots by filesystem UUID alone. Source copies and metadata are private
handoff artifacts; do not commit or print modem provisioning contents.

Validation runs the actual registration/ioctl/state/root-selection code and
actual VFS functions with host adapters. It covers legacy behavior, invalid
arguments and FDs, backing type/quota refusals, duplicate/alias superblocks,
all-root reference cleanup, concurrent claim/cancel/reprobe behavior, exact
root routing, and existing traversal/mutation confinement. ARM64 W=1 object
compilation checks real kernel API compatibility; host adapters are not hardware
or full filesystem qualification.
