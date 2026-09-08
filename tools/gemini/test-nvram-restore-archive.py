#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic archive tests with real Linux memfd seals; no phone access."""
import errno
import fcntl
import os
import struct
import unittest
from unittest.mock import patch
from nvram_backup_format import Backup, FormatError
from nvram_restore_archive import archive_bytes, sealed_archive

PROFILE = dict(md5=False, md3=True, ecci=True, header_offset=0)


def fixture(names=(b'/data/nvram/md/a', b'/protect_f/md/b', b'/protect_s/md/c',
                   b'/data/nvram/APCFG/ignored', b'/data/nvram/md/a')):
    # Three full words 1,2,4 => checksum(1+2)^4^12 = 11.
    data = struct.pack('<III', 1, 2, 4)
    counts = [len(names)] + [0]*15
    header = struct.pack('<III16H', 11, 0, 0, *counts) + bytes(8)
    rows = b''.join(struct.pack('<III', 0, (i%3)*4, 4) + n.ljust(128, b'\0')
                    for i,n in enumerate(names))
    return Backup(header+rows, data, 4, 0, (1,))


class ArchiveTests(unittest.TestCase):
    def test_literal_format_and_first_match(self):
        data = archive_bytes(fixture(), **PROFILE)
        self.assertEqual(data[:16], b'MD1BKP1\0'+struct.pack('<II',3,448))
        for i,(drive,path,value) in enumerate(((90,b'a',1),(88,b'b',2),(89,b'c',4))):
            entry = data[16+140*i:16+140*(i+1)]
            self.assertEqual(entry[:12], struct.pack('<III',drive,436+4*i,4))
            self.assertEqual(entry[12:], path+bytes(127))
            self.assertEqual(data[436+4*i:440+4*i], struct.pack('<I',value))
        with self.assertRaises(FormatError):
            archive_bytes(fixture((b'/data/nvram/APCFG/ignored',)), **PROFILE)
        with self.assertRaises(FormatError):
            archive_bytes(fixture((b'/data/nvram/md/../escape',)), **PROFILE)

    def test_expanded_payload_limit_before_fd_creation(self):
        # Overlapping valid source extents can expand beyond the archive limit.
        size = 4*1024*1024
        counts = [5]+[0]*15
        header = struct.pack('<III16H', size, 0, 0, *counts)+bytes(8)
        rows = b''.join(struct.pack('<III',0,0,size)+
                        ('/data/nvram/md/%d' % i).encode().ljust(128,b'\0')
                        for i in range(5))
        backup = Backup(header+rows,bytes(size),4,0,(1,))
        with patch('nvram_restore_archive.os.memfd_create') as create:
            with self.assertRaises(FormatError):
                sealed_archive(backup, **PROFILE)
            create.assert_not_called()

    def test_real_memfd_seals(self):
        fd = sealed_archive(fixture(), **PROFILE)
        try:
            self.assertFalse(os.get_inheritable(fd))
            self.assertEqual(os.read(fd,4096), archive_bytes(fixture(), **PROFILE))
            self.assertEqual(fcntl.fcntl(fd,fcntl.F_GET_SEALS)&15,15)
            for operation in (lambda: os.pwrite(fd,b'x',0),
                              lambda: os.ftruncate(fd,0),
                              lambda: os.ftruncate(fd,4096)):
                with self.assertRaises(OSError) as cm:
                    operation()
                self.assertEqual(cm.exception.errno, errno.EPERM)
        finally:
            os.close(fd)

    def test_short_writes_and_failure_cleanup(self):
        original_write = os.write
        with patch('nvram_restore_archive.os.write',
                   side_effect=lambda fd,data:original_write(fd,data[:3])):
            fd = sealed_archive(fixture(), **PROFILE)
        try:
            self.assertEqual(os.read(fd,4096), archive_bytes(fixture(), **PROFILE))
        finally:
            os.close(fd)
        opened = []
        create = os.memfd_create
        def track(*args):
            fd = create(*args)
            opened.append(fd)
            return fd
        for failure in ('write','seal'):
            with patch('nvram_restore_archive.os.memfd_create',side_effect=track):
                target = ('nvram_restore_archive.os.write' if failure=='write'
                          else 'nvram_restore_archive.fcntl.fcntl')
                with patch(target,side_effect=OSError(errno.ENOSPC,'synthetic')):
                    with self.assertRaises(OSError):
                        sealed_archive(fixture(), **PROFILE)
            with self.assertRaises(OSError) as cm:
                os.fstat(opened[-1])
            self.assertEqual(cm.exception.errno, errno.EBADF)


if __name__ == '__main__':
    unittest.main()
