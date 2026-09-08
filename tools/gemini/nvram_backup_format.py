# SPDX-License-Identifier: GPL-2.0-only
"""Read-only format primitives for the installed Android25 NVRAM backup.

No device access, extraction, pathname publication or implicit authority.
Raw block reconstruction and feature-dependent AllMap group selection must
validate their own inputs before these primitives can support RESTORE.
"""
from dataclasses import dataclass
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
