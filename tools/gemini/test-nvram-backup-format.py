#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic format tests only; no phone, original partition or private data."""
import struct
import unittest
from nvram_backup_format import (Checksum, FormatError, MAX_BYTES,
                                 decode_record, verify_checksum)


class FormatTests(unittest.TestCase):
    def test_literal_checksums_and_every_split(self):
        # Independently hand-calculated words: XOR1, ADD2, XOR4, ADD8 =>15.
        vectors = [(b'', 0), (b'\x01', 0), (b'\x01\x02', 0x203),
                   (b'\x01\x02\x03', 0x30202),
                   (struct.pack('<4I', 1, 2, 4, 8), 31),
                   (struct.pack('<2I', 0xffffffff, 2), 9),
                   (struct.pack('<I', 0xffffffff) + b'\x02', 4),
                   (struct.pack('<2I', 1, 2) + b'\x04\x05\x06', 0x6050c)]
        for data, expected in vectors:
            for split in range(len(data) + 1):
                c = Checksum()
                c.update(data[:split])
                c.digest()  # Mid-word snapshots must not finalize state.
                c.update(b'')
                c.update(data[split:])
                self.assertEqual(c.digest(), expected)
                self.assertEqual(c.digest(), expected)
            c = Checksum()
            for value in data:
                c.update(bytes([value]))
            self.assertEqual(c.digest(), expected)
            verify_checksum(struct.pack('<III', expected, 0x1234, 0x5678), data)
            with self.assertRaises(FormatError):
                verify_checksum(struct.pack('<III', expected ^ 1, 0, 0), data)

    def test_limit_and_rejected_update_is_atomic(self):
        c = Checksum()
        block = bytes(65536)
        for _ in range(MAX_BYTES // len(block)):
            c.update(block)
        self.assertEqual(c.digest(), MAX_BYTES)
        with self.assertRaises(FormatError):
            c.update(b'x')
        self.assertEqual(c.digest(), MAX_BYTES)
        for size in range(12):
            with self.assertRaises(FormatError):
                verify_checksum(bytes(size), b'')
        with self.assertRaises(TypeError):
            c.update(bytearray())

    @staticmethod
    def record(offset=2, length=3, name=b'/data/nvram/test'):
        return struct.pack('<III', 0xaabbccdd, offset, length) + name.ljust(128, b'\0')

    def test_record_extent_and_opaque_bytes(self):
        r = decode_record(self.record(), 5)
        self.assertEqual((r.opaque, r.offset, r.length, r.name),
                         (0xaabbccdd, 2, 3, b'/data/nvram/test'))
        self.assertEqual(decode_record(self.record(5, 0), 5).length, 0)
        self.assertEqual(decode_record(self.record(0, 0), 0).length, 0)
        self.assertEqual(decode_record(self.record(0, MAX_BYTES), MAX_BYTES).length,
                         MAX_BYTES)
        # No text transcoding or path authorization is performed here.
        self.assertEqual(decode_record(self.record(name=b'../\xff'), 5).name, b'../\xff')
        self.assertEqual(len(decode_record(self.record(name=b'a'*127), 5).name), 127)
        for offset, length in [(6, 0), (2, 4), (0xffffffff, 2), (1, 0xffffffff)]:
            with self.assertRaises(FormatError):
                decode_record(self.record(offset, length), 5)
        for size in [-1, MAX_BYTES + 1, True, 1.5]:
            with self.assertRaises(FormatError):
                decode_record(self.record(), size)
        for data in [self.record()[:-1], self.record()+b'x',
                     self.record(name=b''), self.record(name=b'a'*128)]:
            with self.assertRaises(FormatError):
                decode_record(data, 5)


if __name__ == '__main__':
    unittest.main()
