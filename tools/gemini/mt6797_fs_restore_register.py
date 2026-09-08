#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""One-shot, read-only stock backup preparation before RAM roots/cold GO."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import resource
import stat
import struct
import sys

from nvram_backup_format import FormatError, reconstruct
from nvram_restore_archive import sealed_archive

SOURCE = '/dev/mmcblk0p19'
SOURCE_SYS = Path('/sys/class/block/mmcblk0p19')
PROVIDER = '/dev/mt6797-md1-fs-provider'
PROVIDER_SYS = Path('/sys/class/misc/mt6797-md1-fs-provider/dev')
SOURCE_BYTES = 5242880
BLKGETSIZE64 = 0x80081272
REGISTER_ARCHIVE = 0x40104df4  # _IOW('M', 0xf4, 16), ARM64 and host x86_64
PROFILE_NAME = 'gemini-stock-ecci-md3-v1'
PROFILE = dict(md5=False, md3=True, ecci=True, header_offset=0)


def source_identity(fd):
    info = os.fstat(fd)
    if not stat.S_ISBLK(info.st_mode):
        raise ValueError('source type')
    dev = f'{os.major(info.st_rdev)}:{os.minor(info.st_rdev)}'
    if (SOURCE_SYS / 'dev').read_text().strip() != dev:
        raise ValueError('source device')
    fields = (SOURCE_SYS / 'uevent').read_text().splitlines()
    for prefix, expected in (('PARTNAME=', 'PARTNAME=nvram'), ('PARTN=', 'PARTN=19')):
        if [line for line in fields if line.startswith(prefix)] != [expected]:
            raise ValueError('source partition')
    if (SOURCE_SYS / 'size').read_text().strip() != '10240':
        raise ValueError('source sectors')
    size = struct.unpack('=Q', fcntl.ioctl(fd, BLKGETSIZE64, bytes(8)))[0]
    if size != SOURCE_BYTES:
        raise ValueError('source size')


def read_source(fd):
    data = bytearray()
    while len(data) < SOURCE_BYTES:
        chunk = os.pread(fd, min(65536, SOURCE_BYTES-len(data)), len(data))
        if not chunk:
            raise OSError('short source read')
        data.extend(chunk)
    return bytes(data)


def register(expected_sha256, profile):
    # No autodetection or fallback to other profile/partition/current seed.
    if (profile != PROFILE_NAME or len(expected_sha256) != 64 or
            any(c not in '0123456789abcdef' for c in expected_sha256)):
        raise ValueError('explicit profile and source hash required')
    source = os.open(SOURCE, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    archive = provider = None
    try:
        source_identity(source)
        raw = read_source(source)
        if hashlib.sha256(raw).hexdigest() != expected_sha256:
            raise ValueError('source hash')
        backup = reconstruct(raw, layout_version=0)
        archive = sealed_archive(backup, **PROFILE)
        # Refuse publication if the source changed while preparing the archive.
        source_identity(source)
        if hashlib.sha256(read_source(source)).hexdigest() != expected_sha256:
            raise ValueError('source changed')
        del raw, backup
        provider = os.open(PROVIDER, os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)
        info = os.fstat(provider)
        dev = f'{os.major(info.st_rdev)}:{os.minor(info.st_rdev)}'
        if not stat.S_ISCHR(info.st_mode) or PROVIDER_SYS.read_text().strip() != dev:
            raise ValueError('provider identity')
        # Synchronous ioctl copies and validates bytes; it does not start modem.
        fcntl.ioctl(provider, REGISTER_ARCHIVE, struct.pack('=IiII', 1, archive, 0, 0))
    finally:
        for fd in (provider, archive, source):
            if fd is not None:
                os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, choices=[PROFILE_NAME])
    parser.add_argument('--sha256', required=True)
    args = parser.parse_args()
    try:
        # Backup bytes are transient process memory; never include in a core dump.
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        register(args.sha256, args.profile)
    except (OSError, ValueError, FormatError):
        # No traceback, decoded names, provisioning bytes, or identifiers.
        print('CELLULAR_RESTORE_ARCHIVE_FAILED', file=sys.stderr)
        return 1
    print('CELLULAR_RESTORE_ARCHIVE_REGISTERED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
