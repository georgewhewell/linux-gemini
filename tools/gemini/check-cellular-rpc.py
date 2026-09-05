#!/usr/bin/env python3
"""Strict offline interpretation of attributed pre-ready CCCI packet captures.

Vendor 8cfe6596: eccci/port_rpc.{c,h}, rpc_msg_handler, rpc_buffer.
No device access, responses, permission changes or service-success inference.
Raw output may contain modem filesystem/provisioning data: keep restricted.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location(
    'fatal', Path(__file__).with_name('check-cellular-first-fatal.py'))
fatal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fatal)

RPC_RX = 32
RPC_MAX_ARG_NUM = 6
RPC_MAX_BUF_SIZE = 2048
RPC_REQ_BUFFER_NUM = 2


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def packet(data, original_length, truncated, header_valid):
    require(0 <= original_length <= 3584 and len(data) <= original_length,
            'invalid captured/original length')
    require(bool(truncated) == (len(data) < original_length), 'contradictory truncation')
    result = dict(captured=len(data), length=original_length, truncated=bool(truncated),
                  sha256=hashlib.sha256(data).hexdigest())
    if not header_valid:
        result['classification'] = 'RAW_INVALID_HEADER'
        return result
    require(len(data) >= 16, 'valid header missing bytes')
    data0, declared, status, index = struct.unpack_from('<4I', data)
    channel = status & 0xffff
    result.update(header_words=[hex(x) for x in (data0, declared, status, index)],
                  channel=channel, sequence=(status >> 16) & 0x7fff,
                  asserted=bool(status >> 31))
    if channel != RPC_RX:
        result['classification'] = {14: 'RAW_FS_RX', 10: 'RAW_UART2_RX'}.get(channel, 'RAW_NON_RPC')
        return result
    require(not truncated, 'RPC capture truncated; no complete argument interpretation')
    require(declared == original_length, 'RPC declared length differs from packet')
    require(len(data) >= 24, 'RPC operation/argument count missing')
    require(index <= RPC_REQ_BUFFER_NUM, 'RPC index exceeds actual vendor upper bound')
    op, argc = struct.unpack_from('<2I', data, 16)
    require(op <= 0xffff, 'RPC request operation contains response/reserved upper bits')
    require(argc <= RPC_MAX_ARG_NUM, 'RPC argument count exceeds six')
    require(len(data) - 24 <= RPC_MAX_BUF_SIZE, 'RPC argument buffer exceeds vendor bound')
    cursor, arguments = 24, []
    for n in range(argc):
        require(cursor + 4 <= len(data), 'RPC argument length missing')
        length = struct.unpack_from('<I', data, cursor)[0]
        cursor += 4
        require(length <= RPC_MAX_BUF_SIZE, 'RPC argument length exceeds vendor bound')
        padded = (length + 3) & ~3
        require(padded <= len(data) - cursor, 'RPC argument/padding exceeds captured packet')
        body = data[cursor:cursor + length]
        arguments.append(dict(index=n, length=length, offset=cursor, hex=body.hex(),
                              padding_hex=data[cursor + length:cursor + padded].hex()))
        cursor += padded
    require(cursor == len(data), 'RPC trailing bytes after declared arguments')
    result.update(classification='COMPLETE_RPC_REQUEST', operation=hex(op), argc=argc,
                  request_index=index, arguments=arguments,
                  request_index_caveat=('vendor permits index2 but documents two slots; '
                                        'not service admission proof') if index == 2 else None)
    return result


def decode(log):
    report = fatal.decode(log)
    require('preready_rx' in report, 'no pre-ready capture format')
    packets = []
    for record in report['preready_rx']:
        data = fatal.hex_block(log, 'PREREADY_RX' + str(record['index']), 1024)
        decoded = packet(data, record['length'], record['truncated'], record['header_valid'])
        decoded.update(capture_index=record['index'], queue=record['queue'], drop=record['drop'])
        packets.append(decoded)
    return dict(evidence='OFFLINE_PACKET_INTERPRETATION', packets=packets,
                caveat='Caller establishes boot attribution; no response or modem acceptance inferred.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dmesg', type=Path)
    args = parser.parse_args()
    try:
        require(args.dmesg.stat().st_size <= 8 * 1024 * 1024, 'log exceeds 8 MiB')
        print(json.dumps(decode(args.dmesg.read_text()), indent=2))
    except (OSError, ValueError, KeyError, struct.error) as exc:
        parser.exit(1, f'CCCI_RPC_INVALID: {exc}\n')
