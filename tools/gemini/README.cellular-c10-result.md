# C10: real FS metadata reply, next GETATTRIBUTES request

Candidate0de069903a1cdb54b9073b351ae4789cd5fa8234fe474cae14f95434e5aec37e,
package405cdeda89b2de36b11051c75cacb4672958201f,
boot7d01a34f-14d1-4636-95d7-a7bc0403ca52. This image is consumed once.
Evidence root: handoff/cellular-c10-BYU4TT4S. Inspect only the explicitly
selected logs below; see capture-scope incident before using the archive.

Root read saved cellular-fs-prepare/result.log, cellular-oneboot/result.log,
mountinfo-mounted and dmesg-first-event.txt. The actual partition metadata
precheck passed; nvdata p6 mounted ext4 ro,norecovery,nosuid,nodev,noexec and
the directory authority was published once. This resolves C9's missing-MMC
dependency at runtime, not merely in host tests.

At90.499997 the driver logged FS GETDISKINFO0x100e, reply channel15, status0,
tx_result0,120bytes. A subsequent fully captured68-byte queue1/channel14
request has op0x1010, argc1 and a38-byte UTF16LE Z-root log pathname. Vendor
header identifies GETATTRIBUTES, a metadata operation; it is not a request to
modify the file. Do not reproduce the pathname in public status or fabricate
its existence/attributes. The next implementation needs actual read-only
metadata lookup and exact vendor response/error mapping under the retained
root, with no path escape or VFS work in NAPI.

At121.856432 the existing AP handshake timer fired, ready0; final test says
CELLULAR_FAIL no-HS2. No AT query or pass marker followed. Successful TX and
the next request establish protocol progress, not completed HS2 or connectivity.
Do not extend the timer as a substitute for the missing metadata service.

## Capture-scope incident

The owner recursively archived both requested RAM evidence directories, but
nvdata was mounted beneath cellular-fs-prepare. Consequently the archive and
its local extraction unintentionally include provision-file copies. Original
storage stayed read-only. The archive was already terminal/fsynced and extracted
when root detected the unexpected member names; no active copy remained to stop.
Neither root nor owner inspected provision-file contents. Root inspected only
the archive member listing and selected logs, then requested restricted private
permissions and no further traversal/content inspection.

Keep accidental copies private; do not attach/publish them as ordinary logs.
Future capture must explicitly exclude the mounted nvdata subtree, with a
regression fixture proving log capture without reading backing files. Do not
unmount the held authority to simplify capture. Archive SHA256 reported by owner:
98b2a1b6b428486f778ddce0714a1c3dc00ce94913414272d6ac96a6890e6742.

Original owner37348 terminated exit1 in the normal failed-recovered state at
09:11:09. Root read the completed owner RESULT. Exact rescue boot
6ad19ccb-7aec-436b-9632-73fb90ab79f4 passed USB/RJ45, all three expected hashes,
CPUs0–7/no failed units, process and lock closure. Immediate and harness pstore
console copies match SHA256
f592cdbb2cae554e0feb0cf0c18ff28927a257158fcd63b01716aeea1e99db45,
retaining C10 terminal timestamps and watchdog stop187.394111. Nothing cleared.
The accidental extraction is also restricted with go-rwx; no provision-file
inspection or further capture followed. No further hardware is authorized
by this result.
