#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('rpc', Path(__file__).with_name('check-cellular-rpc.py'))
rpc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rpc)


def wire(args=(), op=0x4006, index=0, channel=32):
    payload = struct.pack('<2I', op, len(args))
    for arg in args:
        payload += struct.pack('<I', len(arg)) + arg + bytes((-len(arg)) % 4)
    return struct.pack('<4I', 0, 16 + len(payload), channel, index) + payload


class Tests(unittest.TestCase):
    def parse(self, data):
        return rpc.packet(data, len(data), False, True)

    def test_literal(self):
        data = bytes.fromhex('0000000020000000200000000100000006400000010000000400000078563412')
        r = self.parse(data)
        self.assertEqual(r['operation'], '0x4006')
        self.assertEqual(r['request_index'], 1)
        self.assertEqual(r['arguments'][0]['hex'], '78563412')
        self.assertEqual(self.parse(wire([b'a', b'bc', b'def', b'ghij', b'', b'x']))['argc'], 6)

    def test_bad_header_count_index_operation(self):
        bad = [b'123', wire(index=3), wire(op=0xffff4006), wire([b''] * 7)]
        mismatch = bytearray(wire()); mismatch[4:8] = struct.pack('<I', 0xffffffff)
        bad.append(mismatch)
        for data in bad:
            with self.subTest(data=data), self.assertRaises(ValueError): self.parse(data)
        self.assertIn('two slots', self.parse(wire(index=2))['request_index_caveat'])

    def test_length_padding_overflow(self):
        original = wire([b'abcd'])
        for length in (5, 2049, 0xffffffff):
            data = bytearray(original); data[24:28] = struct.pack('<I', length)
            with self.assertRaises(ValueError): self.parse(data)
        data = bytearray(wire([b'a'])[:-1]); data[4:8] = struct.pack('<I', len(data))
        with self.assertRaises(ValueError): self.parse(data)
        data = bytearray(wire() + bytes(4)); data[4:8] = struct.pack('<I', len(data))
        with self.assertRaises(ValueError): self.parse(data)
        with self.assertRaises(ValueError): self.parse(wire([bytes(2048)]))

    def test_truncated_raw_classification(self):
        data = wire([bytes(1100)])
        with self.assertRaises(ValueError): rpc.packet(data[:1024], len(data), True, True)
        with self.assertRaises(ValueError): rpc.packet(data[:1024], len(data), False, True)
        self.assertEqual(rpc.packet(b'x', 1, False, False)['classification'], 'RAW_INVALID_HEADER')
        self.assertEqual(self.parse(wire(channel=14))['classification'], 'RAW_FS_RX')
        self.assertEqual(self.parse(wire(channel=10))['classification'], 'RAW_UART2_RX')


if __name__ == '__main__':
    unittest.main()
