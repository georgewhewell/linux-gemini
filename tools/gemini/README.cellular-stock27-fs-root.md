# Stock Gemini Android 27 filesystem root: static proof

Root host-only analysis, 2026-09-05. The daemon was never executed.

Artifact: `ccci_fsd` extracted from stock
`Gemini_x25_x27_06052019/system.img`, SHA256
`8f504cd37f3d36a07764de17cb2cafd61a6b187db502cd90128e3020b05bceed`.
Host analysis copy: `/tmp/gemini-stock-fsd-hTOgBCzd/ccci_fsd`.
This is not proof of the daemon or backing currently installed on the device.

The embedded `.gnu_debugdata` section was extracted with objcopy and
decompressed with xz into a new private temporary directory. Symbols identify
`main` at 0x3e7c, `FS_CCCI_GetClusterSize` at 0x18388, and the checked
snprintf wrapper at 0x140d8. All addresses below are ELF virtual addresses.

## GETDISKINFO dispatch and statfs target

- At 0x11a18 the dispatcher loads 0x1001; 0x11a20 subtracts it from the
  operation. Table base is 0x11a44.
- GETDISKINFO 0x100e selects index 13: entry 0x11a78 is 0x1724.
  Thus the handler is 0x11a44 + 0x1724 = 0x13168.
- Handler 0x13168 loads literal 0x1330c from 0x14034. The PC-relative add
  at 0x13170 yields 0x13178 + 0x1330c = 0x26484.
- `statfs@plt` is called at 0x13174 with that configured first-root buffer,
  not with an arbitrary modem-supplied path.
- Success writes BytesPerSector 512, SectorsPerCluster from block size
  shifted by nine, and total clusters from statfs. At 0x131a4 it again
  stores constant 512 into FreeClusters. Do not copy that fabricated free
  count into the mainline provider.

## Drive mapping and root initialization

`FS_CCCI_GetClusterSize` subtracts ASCII V (0x56) at 0x183a8, accepts V
through Z, and indexes table 0x18548: `{4, 3, 1, 2, 0}`. Therefore Z maps
to root index zero. Its base computation is 0x183c8 + 0xe0bc = 0x26484,
using the literal at 0x1855c; root entries are 32 bytes. This is the same
buffer used by GETDISKINFO.

Startup at 0x403c computes source 0x1f001, the initialized string
`/vendor/nvdata/md`; 0x4050 computes destination 0x26484. The call at
0x4058 uses the checked snprintf wrapper with `%s` at 0x4420 and a
32-byte destination bound. Another startup branch at 0x4464/0x4470/0x4478
copies the same source to the same destination.

This connects stock Android 27's default first modem root, Z drive index,
and GETDISKINFO statfs target. It is stronger than a strings-only match,
but does not establish all alternate startup configurations or the live
device's installed lineage.

## Remaining device authority

Before registering a directory with the new read-only provider, establish
the installed lineage and identify its actual read-only backing directory
and filesystem. This host proof does not authorize creating or formatting
nvdata, running the vendor daemon, mounting it writable, or using an
unrelated directory merely because statfs succeeds.
