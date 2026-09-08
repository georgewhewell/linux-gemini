#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute preparation with synthetic disk/sysfs adapters and real sealed memfd."""
import errno
import fcntl
import hashlib
import os
from pathlib import Path
import stat
import struct
import tempfile
from contextlib import ExitStack
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import mt6797_fs_restore_register as m
from nvram_backup_format import ERASE_BYTES, MAP_MAGIC


def fixture():
    raw = bytearray(b'\xff' * m.SOURCE_BYTES)
    data = struct.pack('<III', 1, 2, 4)
    allmap = (struct.pack('<III16H', 11, 0, 0, 1, *([0]*15)) + bytes(8) +
              struct.pack('<III', 0, 0, 12) + b'/data/nvram/md/a'.ljust(128, b'\0'))
    struct.pack_into('<III', raw, 0, len(allmap), len(data), 0)
    raw[12:12+len(allmap)] = allmap
    raw[ERASE_BYTES:ERASE_BYTES+12] = data
    bitmap = bytes([0, 0] + [1]*33)
    raw[4*ERASE_BYTES:4*ERASE_BYTES+109] = bitmap*3 + struct.pack('<I', MAP_MAGIC)
    return bytes(raw)


class RegisterTests(unittest.TestCase):
    def run_case(self, case):
        raw = fixture()
        digest = hashlib.sha256(raw).hexdigest()
        opened, closed, calls, handles = [], [], [], {}
        real_open, real_close, real_fstat = os.open, os.close, os.fstat
        real_pread, real_memfd = os.pread, os.memfd_create
        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            root = Path(directory)
            source, provider = root/'source', root/'provider'
            source.write_bytes(raw); provider.touch()
            for name, value in {'dev':'179:19', 'uevent':'PARTNAME=nvram\nPARTN=19\n',
                                'size':'10240', 'provider-dev':'10:42'}.items():
                (root/name).write_text(value)
            if case == 'wrong-partition': (root/'uevent').write_text('PARTNAME=nvdata\nPARTN=19\n')
            if case == 'duplicate-partition': (root/'uevent').write_text('PARTNAME=nvram\nPARTNAME=nvram\nPARTN=19\n')
            if case == 'wrong-provider': (root/'provider-dev').write_text('10:43')
            def open_fd(path, flags, *args):
                fd = real_open(path, flags, *args)
                if str(path) in (str(source), str(provider)):
                    self.assertTrue(flags & os.O_NOFOLLOW)
                    self.assertTrue(flags & os.O_CLOEXEC)
                    if str(path) == str(source): self.assertEqual(flags & os.O_ACCMODE, os.O_RDONLY)
                    handles['source' if str(path)==str(source) else 'provider'] = fd
                    opened.append(fd)
                return fd
            def memfd(*args):
                fd = real_memfd(*args); opened.append(fd); return fd
            def close_fd(fd):
                closed.append(fd); return real_close(fd)
            def fstat_fd(fd):
                if fd == handles.get('source'):
                    return SimpleNamespace(st_mode=stat.S_IFREG if case=='regular-source' else stat.S_IFBLK, st_rdev=os.makedev(179,19))
                if fd == handles.get('provider'):
                    return SimpleNamespace(st_mode=stat.S_IFCHR, st_rdev=os.makedev(10,42))
                return real_fstat(fd)
            reads = 0
            def pread(fd, size, offset):
                nonlocal reads
                if fd != handles.get('source'): return real_pread(fd, size, offset)
                if offset == 0: reads += 1
                if case == 'eof': return b''
                if case == 'drift' and reads == 2 and offset == 0: return b'x'*size
                return real_pread(fd, min(size, 997) if case=='short-reads' else size, offset)
            def ioctl(fd, op, arg):
                if fd == handles.get('source'):
                    self.assertEqual(op, m.BLKGETSIZE64)
                    return struct.pack('=Q', m.SOURCE_BYTES-1 if case=='wrong-size' else m.SOURCE_BYTES)
                self.assertEqual(fd, handles['provider'])
                self.assertEqual(op, 0x40104df4)
                version, archive, flags, reserved = struct.unpack('=IiII', arg)
                self.assertEqual((version, flags, reserved), (1, 0, 0))
                self.assertEqual(fcntl.fcntl(archive, fcntl.F_GET_SEALS)&15, 15)
                self.assertEqual(os.pread(archive, 16, 0), b'MD1BKP1\0'+struct.pack('<II',1,168))
                self.assertEqual(os.pread(archive, 12, 156), struct.pack('<III',1,2,4))
                self.assertEqual(reads, 2)
                calls.append(op)
                if case == 'ioctl-fail': raise OSError(errno.EBUSY, 'synthetic')
                return 0
            for name, value in [('SOURCE',str(source)),('SOURCE_SYS',root),('PROVIDER',str(provider)),('PROVIDER_SYS',root/'provider-dev')]:
                stack.enter_context(patch.object(m,name,value))
            for name, value in [('open',open_fd),('close',close_fd),('fstat',fstat_fd),('pread',pread),('memfd_create',memfd)]:
                stack.enter_context(patch.object(m.os,name,value))
            stack.enter_context(patch.object(m.fcntl,'ioctl',ioctl))
            success = case in ('pass','short-reads')
            args = ('0'*64 if case=='hash' else digest, 'unknown' if case=='profile' else m.PROFILE_NAME)
            if success: m.register(*args)
            else:
                with self.assertRaises((ValueError,OSError)): m.register(*args)
            self.assertEqual(len(calls), 1 if success or case=='ioctl-fail' else 0)
            for fd in opened:
                self.assertIn(fd,closed)
                with self.assertRaises(OSError): real_fstat(fd)
            self.assertEqual(source.read_bytes(), raw)

    def test_success_short_reads_and_failure_gates(self):
        for case in ('pass','short-reads','profile','hash','eof','drift','wrong-partition',
                     'duplicate-partition','regular-source','wrong-size','wrong-provider','ioctl-fail'):
            with self.subTest(case=case): self.run_case(case)


if __name__ == '__main__': unittest.main()
