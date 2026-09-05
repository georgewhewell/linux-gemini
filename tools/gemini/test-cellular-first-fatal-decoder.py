#!/usr/bin/env python3
"""Independent wire fixtures for the offline diagnostic parser."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('decoder', Path(__file__).with_name('check-cellular-first-fatal.py'))
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


def fixture():
    reply = bytearray(116)
    struct.pack_into('<4I', reply, 0, 0, 116, 0x80000001, 0x5555ffff)
    struct.pack_into('<7I', reply, 84, 1, 0x40010000, 2048, 0x40010800, 2048, 0x40000000, 0x100000)
    reply[21] = 0x10
    rows = ['MT6797_CCCI_FIRST_FATAL site=md-wdt-irq descriptor_valid=1 payload_valid=1 reply_finalized=1 runtime_len=8']
    for name, data in [('Q0_GPD', bytes(16)), ('HS1', bytes(88)), ('AP_REPLY', reply),
                       ('RUNTIME', struct.pack('<BBHI', 7, 2, 0, 0))]:
        for offset in range(0, len(data), 32):
            rows.append(f'[23.6] CCCI_{name} {offset:08x}: ' + data[offset:offset + 32].hex(' '))
    return '\n'.join(rows)


class Tests(unittest.TestCase):
    def test_preready_rx(self):
        data = struct.pack('<5I', 0, 20, 32, 0, 0x1234)
        addition = ('\nCCCI_PREREADY_RX_COUNT count=1 cap=4 bytes_per_frame=1024\n'
                    'CCCI_PREREADY_RX index=0 queue=1 drop=3 length=20 captured=20 truncated=0 '
                    'decode_error=0 header_valid=1 channel=32 payload_word0_valid=1 payload_word0=00001234\n'
                    'CCCI_PREREADY_RX0 00000000: ' + data.hex(' '))
        report = decoder.decode(fixture() + addition)
        self.assertEqual(report['preready_rx'][0]['payload_word0'], 0x1234)
        for bad in (addition.rsplit('\n', 1)[0], addition.replace('count=1', 'count=5'),
                    addition.replace('channel=32', 'channel=33'), addition.replace('00001234', '00000000'),
                    addition.replace('length=20', 'length=19'), addition + addition):
            with self.subTest(bad=bad[:100]), self.assertRaises(ValueError):
                decoder.decode(fixture() + bad)
        self.assertEqual(decoder.decode(fixture() + '\nCCCI_PREREADY_RX_COUNT count=0 cap=4 bytes_per_frame=1024')['preready_rx'], [])

    def test_wire_fields(self):
        report = decoder.decode(fixture())
        self.assertEqual(report['ap_response']['mpu_size'], 0x100000)
        self.assertEqual(report['ap_response']['feature_1'], 0x10)
        self.assertEqual(report['reply_header']['channel'], 1)
        self.assertEqual(report['runtime_records'], [dict(feature=7, support=2, reserved=0, bytes=0)])

    def test_incomplete_duplicate_corrupt(self):
        log = fixture()
        for bad in (log.rsplit('\n', 1)[0], log + '\n' + log,
                    log.replace('CCCI_HS1 00000020', 'CCCI_HS1 00000021'),
                    log.replace('runtime_len=8', 'runtime_len=2049'),
                    log.replace('07 02 00 00 00 00 00 00', '07 02 00 00 ff ff ff ff')):
            with self.subTest(bad=bad[-100:]), self.assertRaises(ValueError):
                decoder.decode(bad)

    def test_early_failure_without_payload(self):
        report = decoder.decode('MT6797_CCCI_FIRST_FATAL site=transport descriptor_valid=0 payload_valid=0 reply_finalized=0 runtime_len=0')
        self.assertNotIn('ap_response', report)


if __name__ == '__main__':
    unittest.main()
