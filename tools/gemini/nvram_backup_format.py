# SPDX-License-Identifier: GPL-2.0-only
"""Read-only format primitives for the installed Android25 NVRAM backup.

No device access, extraction, pathname publication or implicit authority.
Feature-dependent AllMap group selection and source authority remain
mandatory before reconstructed bytes can support RESTORE.
"""
from dataclasses import dataclass, field
import struct

MAX_BYTES = 16 * 1024 * 1024
RECORD_BYTES = 140
MASK32 = (1 << 32) - 1


class FormatError(ValueError):
    """Malformed, incomplete or over-limit backup input (no payload in message)."""


class Checksum:
    """Streaming NVM_CheckData_OneFile checksum, independent of chunk size.

    libnvram.so 6b16..6b50 alternates XOR then ADD on little-endian words;
    6bda..6c02 adds the zero-extended tail and 6c80 XORs the byte length.
    This detects corruption; it does not authenticate the backup.
    """

    def __init__(self):
        self._size = 0
        self._acc = 0
        self._tail = 0
        self._used = 0
        self._add = False

    def update(self, data: bytes) -> None:
        if not isinstance(data, bytes):
            raise TypeError("checksum input must be immutable bytes")
        if len(data) > MAX_BYTES - self._size:
            raise FormatError("backup exceeds size limit")
        # Reject the entire chunk before changing state on a size error.
        self._size += len(data)
        for value in data:
            self._tail |= value << (8 * self._used)
            self._used += 1
            if self._used == 4:
                self._acc = ((self._acc + self._tail) & MASK32 if self._add
                             else self._acc ^ self._tail)
                self._add = not self._add
                self._tail = self._used = 0

    def digest(self) -> int:
        # A snapshot, not a destructive finish: later updates remain valid.
        return ((self._acc + self._tail) & MASK32) ^ self._size


def verify_checksum(allmap: bytes, allfile: bytes) -> None:
    """Check the outer integrity word, without claiming AllMap is valid."""
    if not isinstance(allmap, bytes) or not isinstance(allfile, bytes):
        raise TypeError("backup input must be immutable bytes")
    if not 12 <= len(allmap) <= MAX_BYTES:
        raise FormatError("invalid map size")
    checksum = Checksum()
    checksum.update(allfile)
    if checksum.digest() != struct.unpack_from('<I', allmap)[0]:
        raise FormatError("backup checksum mismatch")


@dataclass(frozen=True)
class Record:
    opaque: int
    offset: int
    length: int
    name: bytes


def decode_record(data: bytes, allfile_size: int) -> Record:
    """Decode one exact 140-byte record; do not authorize its pathname.

    Installed libnvram.so: record+12 is strcmp name (57f6/581e), +8 is
    byte count (59dc), +4 is AllFile seek offset (59e0..59f0). +0 remains
    opaque. Name is retained as bytes, avoiding invented encoding rules.
    Only the bounded volatile-root publisher may interpret it as a path.
    """
    if not isinstance(data, bytes):
        raise TypeError("record input must be immutable bytes")
    if type(allfile_size) is not int or not 0 <= allfile_size <= MAX_BYTES:
        raise FormatError("invalid data size")
    if len(data) != RECORD_BYTES:
        raise FormatError("invalid record size")
    opaque, offset, length = struct.unpack_from('<III', data)
    # Subtraction form also documents the required overflow-safe C check.
    if offset > allfile_size or length > allfile_size - offset:
        raise FormatError("record extent outside data")
    name_field = data[12:]
    end = name_field.find(b'\0')
    if end <= 0:
        raise FormatError("record name empty or unterminated")
    return Record(opaque, offset, length, name_field[:end])


ERASE_BYTES = 0x20000
MAP_MAGIC = 0xaabbccdd


@dataclass(frozen=True)
class Backup:
    # Avoid accidentally printing provisioning bytes in logs or tracebacks.
    allmap: bytes = field(repr=False)
    allfile: bytes = field(repr=False)
    map_block: int
    header_block: int
    data_blocks: tuple[int, ...]


def reconstruct(raw: bytes, *, layout_version: int = 0) -> Backup:
    """Reconstruct installed eMMC AllMap/AllFile, then verify the checksum.

    Immutable input is required; this routine performs no I/O. Source identity,
    feature/group selection, record validation and publication remain separate
    mandatory gates. Geometry is the installed 128-KiB erase size, two log,
    two reserved and one DM block. Only vendor layout versions 0/1 are known.
    """
    if not isinstance(raw, bytes):
        raise TypeError("backup input must be immutable bytes")
    if type(layout_version) is not int or layout_version not in (0, 1):
        raise FormatError("unsupported backup layout")
    if not raw or len(raw) > MAX_BYTES or len(raw) % ERASE_BYTES:
        raise FormatError("invalid backup geometry")
    total = len(raw) // ERASE_BYTES
    usable = total - (4 if layout_version == 0 else 0)
    map_bytes = usable - 1  # DM block, libnvram.so 6ec2.
    if map_bytes < 3 or 3 * map_bytes + 4 > ERASE_BYTES:
        raise FormatError("invalid backup geometry")

    # 6f2e..6ff0: newest candidate in physical order, three equal map copies
    # followed by magic. Invalid candidates are skipped. Once selected, a
    # corrupt payload must fail, not silently fall back to an older backup.
    selected = None
    for index in range(map_bytes - 1, -1, -1):
        start = index * ERASE_BYTES
        bitmap = raw[start:start + map_bytes]
        if (bitmap == raw[start + map_bytes:start + 2 * map_bytes]
                and bitmap == raw[start + 2 * map_bytes:start + 3 * map_bytes]
                and struct.unpack_from('<I', raw, start + 3 * map_bytes)[0]
                == MAP_MAGIC):
            selected = (index, bitmap)
            break
    if selected is None:
        raise FormatError("no valid redundant block map")
    map_block, bitmap = selected
    available = [i for i in range(map_block) if bitmap[i] == 0]
    # 713a selects the first zero for the header; 72f0 selects a subsequent
    # zero for data. Require both, including for an empty AllFile.
    if len(available) < 2:
        raise FormatError("backup has no header or data block")
    header_block = available[0]
    start = header_block * ERASE_BYTES
    map_size, file_size = struct.unpack_from('<II', raw, start)
    if not 12 <= map_size <= ERASE_BYTES - 12:
        raise FormatError("map exceeds header block")
    if file_size > MAX_BYTES:
        raise FormatError("backup data exceeds size limit")
    needed = (file_size + ERASE_BYTES - 1) // ERASE_BYTES
    if needed > len(available) - 1:
        raise FormatError("insufficient backup data blocks")
    data_blocks = tuple(available[1:1 + needed])
    allmap = raw[start + 12:start + 12 + map_size]
    allfile = b''.join(raw[i * ERASE_BYTES:(i + 1) * ERASE_BYTES]
                       for i in data_blocks)[:file_size]
    verify_checksum(allmap, allfile)
    return Backup(allmap, allfile, map_block, header_block, data_blocks)
