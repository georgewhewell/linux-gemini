# SPDX-License-Identifier: GPL-2.0-only
"""Prepare an immutable MD1 restore authority; no device registration or I/O."""
import fcntl
import os
import struct
from nvram_backup_format import (Backup, FormatError, MAX_BYTES,
                                 parse_ecci_records, modem_target)

MAGIC = b'MD1BKP1\0'
HEADER_BYTES = 16
ENTRY_BYTES = 140
MAX_FILES = 4096
SEALS = (fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW |
         fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)


def archive_bytes(backup: Backup, *, md5: bool, md3: bool,
                  ecci: bool, header_offset: int) -> bytes:
    """Validate/map records and encode a bounded, normalized restore index.

    First eligible occurrence wins, matching installed pathname lookup.
    Offsets are absolute in the archive. No source path becomes a host path.
    The consumer must independently validate this format and its seals.
    """
    records = parse_ecci_records(backup, md5=md5, md3=md3,
                                 ecci=ecci, header_offset=header_offset)
    selected = []
    seen = set()
    payload_size = 0
    for _, record in records:
        target = modem_target(record.name)
        if target is None or target in seen:
            continue
        seen.add(target)
        selected.append((target, record))
        payload_size += record.length
    count = len(selected)
    if not count or count > MAX_FILES:
        raise FormatError("invalid restore archive file count")
    total = HEADER_BYTES + count * ENTRY_BYTES + payload_size
    if total > MAX_BYTES:
        raise FormatError("restore archive exceeds size limit")
    output = bytearray(total)
    struct.pack_into('<8sII', output, 0, MAGIC, count, total)
    data_offset = HEADER_BYTES + count * ENTRY_BYTES
    for index, ((drive, path), record) in enumerate(selected):
        # Record name is at most127 bytes before prefix stripping, so the
        # relative path always fits in128 including its terminating NUL.
        if not path or len(path) >= 128:
            raise FormatError("invalid restore archive path length")
        entry = HEADER_BYTES + index * ENTRY_BYTES
        struct.pack_into('<III', output, entry, ord(drive), data_offset, record.length)
        output[entry+12:entry+12+len(path)] = path
        output[data_offset:data_offset+record.length] = backup.allfile[
            record.offset:record.offset+record.length]
        data_offset += record.length
    return bytes(output)


def sealed_archive(backup: Backup, *, md5: bool, md3: bool,
                   ecci: bool, header_offset: int) -> int:
    """Return a caller-owned, fully sealed memfd after complete validation.

    No name is published in the filesystem. On error every acquired FD is
    closed. This does not register the archive or enable modem RESTORE.
    """
    data = archive_bytes(backup, md5=md5, md3=md3, ecci=ecci,
                         header_offset=header_offset)
    fd = os.memfd_create('gemini-md1-backup', os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    try:
        offset = 0
        while offset < len(data):
            written = os.write(fd, data[offset:offset+65536])
            if written <= 0:
                raise OSError("restore archive write made no progress")
            offset += written
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, SEALS)
        if fcntl.fcntl(fd, fcntl.F_GET_SEALS) & SEALS != SEALS:
            raise OSError("restore archive seals incomplete")
        os.lseek(fd, 0, os.SEEK_SET)
        return fd
    except BaseException:
        os.close(fd)
        raise
