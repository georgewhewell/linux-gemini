#!/usr/bin/env python3
"""Decode bounded CCCI first-fatal logs offline; never modem acceptance proof.

Input must be the caller's already attributed candidate dmesg capture. This
decoder checks record completeness, not boot identity or hardware success.
Wire layouts: mtk_ccci_common.c and mtk_mt6797_ccci_diag.h in this tree.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def require(ok, message):
    if not ok:
        raise ValueError(message)


def hex_block(log, name, maximum):
    data = bytearray()
    for offset, text in re.findall(r'CCCI_' + name + r' ([0-9a-fA-F]+):([^\n]*)', log):
        require(int(offset, 16) == len(data), name + ' gap/duplicate/out-of-order row')
        octets = text.split()
        require(0 < len(octets) <= 32 and all(re.fullmatch(r'[0-9a-fA-F]{2}', x) for x in octets),
                name + ' malformed hex row')
        data.extend(int(x, 16) for x in octets)
        require(len(data) <= maximum, name + ' exceeds capture bound')
    return bytes(data)


def preready_rx(log):
    counts = re.findall(r'CCCI_PREREADY_RX_COUNT count=(\d+) cap=4 bytes_per_frame=1024', log)
    rows = re.findall(r'CCCI_PREREADY_RX ([^\n]+)', log)
    indices = re.findall(r'CCCI_PREREADY_RX(\d+) ', log)
    if not counts and 'CCCI_PREREADY_RX' not in log:
        return None  # Historical C4/C5 diagnostic format.
    require(len(counts) == 1, 'missing/duplicate/unsupported RX count')
    count = int(counts[0])
    require(count <= 4 and len(rows) == count, 'incomplete/excess RX records')
    require(all(int(i) < count for i in indices), 'unexpected RX data index')
    result = []
    for index, row in enumerate(rows):
        pairs = [field.split('=', 1) for field in row.split()]
        require(all(len(pair) == 2 for pair in pairs), 'malformed RX metadata')
        fields = dict(pairs)
        require(len(fields) == len(pairs), 'duplicate RX metadata')
        r = {k: int(v, 16 if k == 'payload_word0' else 10) for k, v in fields.items()}
        require(r['index'] == index and 0 <= r['queue'] < 7 and 1 <= r['drop'] <= 4,
                'invalid RX index/queue/cause')
        require(0 <= r['length'] <= 3584 and r['captured'] == min(r['length'], 1024),
                'invalid RX lengths')
        require(r['truncated'] == int(r['length'] > r['captured']), 'invalid RX truncation flag')
        data = hex_block(log, 'PREREADY_RX' + str(index), 1024)
        require(len(data) == r['captured'], 'incomplete RX packet capture')
        valid = not r['decode_error'] and r['length'] >= 16
        require(r['header_valid'] == int(valid), 'invalid RX header flag')
        require(r['payload_word0_valid'] == int(valid and r['length'] >= 20), 'invalid RX payload flag')
        if valid:
            header = struct.unpack_from('<4I', data)
            require(r['channel'] == header[2] & 0xffff, 'RX channel differs from captured header')
            r['header_words'] = [hex(word) for word in header]
        if r['payload_word0_valid']:
            require(r['payload_word0'] == struct.unpack_from('<I', data, 16)[0],
                    'RX payload word differs from captured bytes')
        r['sha256'] = hashlib.sha256(data).hexdigest()
        result.append(r)
    return result


def decode(log):
    summaries = re.findall(r'MT6797_CCCI_FIRST_FATAL ([^\n]+)', log)
    require(len(summaries) == 1, 'expected exactly one first-fatal summary')
    fields = summaries[0].split()
    require(all('=' in field for field in fields), 'malformed summary')
    summary = dict(field.split('=', 1) for field in fields)
    require(len(summary) == len(fields), 'duplicate summary field')
    for name in ('descriptor_valid', 'payload_valid', 'reply_finalized'):
        require(summary.get(name) in ('0', '1'), 'missing/invalid ' + name)
    require(summary.get('site') in ('transport', 'md-wdt-irq', 'ap-handshake-timer'),
            'unknown failure site')
    runtime_len = int(summary['runtime_len'])
    require(0 <= runtime_len <= 2048, 'runtime length out of bounds')
    require(summary['payload_valid'] == '1' or
            (runtime_len == 0 and summary['reply_finalized'] == '0'),
            'payload flags contradict saved length/finalization')
    blocks = {}
    for name in ('Q0_GPD', 'HS1', 'AP_REPLY', 'RUNTIME'):
        blocks[name] = hex_block(log, name, 2048)
    lengths = {'Q0_GPD': 16 if summary['descriptor_valid'] == '1' else 0,
               'HS1': 88 if summary['payload_valid'] == '1' else 0,
               'AP_REPLY': 116 if summary['payload_valid'] == '1' else 0,
               'RUNTIME': runtime_len if summary['payload_valid'] == '1' else 0}
    for name, size in lengths.items():
        require(len(blocks[name]) == size, name + ' incomplete or unexpected bytes')
    report = {'evidence': 'COMPLETE_DIAGNOSTIC_RECORDS', 'summary': summary,
              'blocks': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                         for name, data in blocks.items()},
              'caveat': 'Caller must establish boot attribution; queued/finalized is not delivered or accepted.'}
    rx = preready_rx(log)
    if rx is not None:
        report['preready_rx'] = rx
    if summary['payload_valid'] == '1':
        reply = blocks['AP_REPLY']
        data0, data1, status, reserved = struct.unpack_from('<4I', reply)
        report['reply_header'] = dict(data0=hex(data0), data1=data1, channel=status & 0xffff,
                                      sequence=(status >> 16) & 0x7fff, asserted=bool(status >> 31),
                                      reserved=hex(reserved), finalized=summary['reply_finalized'] == '1')
        names = ('shared_memory_support', 'ap_address', 'ap_size', 'md_address', 'md_size',
                 'mpu_start', 'mpu_size')
        report['ap_response'] = dict(zip(names, struct.unpack_from('<7I', reply, 16 + 68)))
        report['ap_response']['feature_1'] = reply[16 + 5]
        records, cursor = [], 0
        runtime = blocks['RUNTIME']
        while cursor < len(runtime):
            require(cursor + 8 <= len(runtime), 'truncated runtime record header')
            feature, support, reserved, length = struct.unpack_from('<BBHI', runtime, cursor)
            require(length <= len(runtime) - cursor - 8, 'truncated runtime record data')
            records.append(dict(feature=feature, support=support, reserved=reserved, bytes=length))
            cursor += 8 + length
        report['runtime_records'] = records
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dmesg', type=Path)
    args = parser.parse_args()
    try:
        require(args.dmesg.stat().st_size <= 8 * 1024 * 1024, 'log exceeds 8 MiB bound')
        print(json.dumps(decode(args.dmesg.read_text()), indent=2))
    except (OSError, ValueError, KeyError, struct.error) as exc:
        parser.exit(1, f'CCCI_DIAGNOSTIC_INVALID: {exc}\n')
