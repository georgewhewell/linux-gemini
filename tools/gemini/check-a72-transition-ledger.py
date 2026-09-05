#!/usr/bin/env python3
"""Check saved retained bytes using the selected kernel's actual admission C.

No device access or retained writes. HOSTCC selects a native C compiler.
"""
import argparse
import binascii
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('kernel_source', type=Path)
p.add_argument('capture', type=Path, nargs='?')
args = p.parse_args()
root = args.kernel_source
implementation = (root / 'fs/pstore/gemini_transition_ledger.c').read_text()
body = implementation[implementation.index('static unsigned int gemini_transition_ledger_copy_word'):]
body = body[:body.index('static bool\ngemini_transition_ledger_sequence_valid')]
header = (root / 'fs/pstore/gemini_transition_ledger_internal.h').read_text()
header = re.sub(r'^#include.*$', '', header, flags=re.M)
public = (root / 'include/linux/gemini_transition_ledger.h').read_text()
phase = public[public.index('enum gemini_transition_ledger_phase'):].split('};', 1)[0] + '};'
shim = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t __le32;
#define U32_MAX UINT32_MAX
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#else
#define cpu_to_le32(x) __builtin_bswap32(x)
#define le32_to_cpu(x) __builtin_bswap32(x)
#endif
static u32 crc32_le(u32 crc, const u8 *bytes, size_t count) {
    while (count--) {
        crc ^= *bytes++;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    return crc;
}
'''
wrapper = r'''
static u32 read_saved(void *context, unsigned int word) {
    const u8 *p = context;
    if (word >= 21) abort();
    p += word * 4;
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static void forbidden_write(void *context, unsigned int word, u32 value) {
    (void)context; (void)word; (void)value; abort();
}
static void forbidden_sync(void *context) { (void)context; abort(); }
int check_saved(void *bytes) {
    struct gemini_transition_ledger_owner owner = {0};
    const struct gemini_transition_ledger_ops ops = {
        .read = read_saved, .write = forbidden_write, .sync = forbidden_sync,
    };
    return gemini_transition_ledger_owner_begin(&owner, &ops, bytes, 1);
}
'''

def record(generation=1, stage=10):
    prefix = struct.pack('<8I', 0x4c543747, 0x10009, 1, 0,
                         generation, 3, stage, 2)
    return prefix + struct.pack('<I', binascii.crc32(prefix))

def blob(start, size, payload=b'\0' * 72):
    return struct.pack('<3I', 0x43474244, start, size) + payload

with tempfile.TemporaryDirectory(prefix='a72-ledger-host-') as directory:
    temp = Path(directory)
    c = temp / 'ledger.c'
    c.write_text(shim + phase + header + body + wrapper)
    library = temp / 'ledger.so'
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', '-shared', '-fPIC', str(c),
                    '-o', str(library)], check=True)
    native = ctypes.CDLL(str(library)).check_saved
    native.argtypes = [ctypes.c_void_p]
    native.restype = ctypes.c_int

    def check(data):
        if len(data) not in (84, 4096):
            raise ValueError('capture must be an exact 84-byte prefix or 4096-byte slot')
        return native(ctypes.create_string_buffer(data[:84]))

    cases = [
        (b'\xff' * 84, 0), (blob(0, 0), 0),
        (blob(0, 0, b'\xa5' * 72), 0),
        (blob(130, 130), -74), (b'\0' * 84, -74),
        (blob(72, 72), -74), (blob(72, 71), -74),
        (blob(72, 72, record() + b'\0' * 36), 0),
        (blob(72, 72, record(stage=11) + b'\0' * 36), -74),
        (blob(72, 72, record() + record()), -74),
        (blob(72, 72, record() + record(generation=2)), 0),
        (blob(72, 72, record(generation=0xffffffff) + b'\0' * 36), -75),
    ]
    for data, expected in cases:
        assert check(data) == expected, (data.hex(), expected)
    print('PASS: actual kernel ledger admission; 12 header/CRC/stage/generation fixtures; no writes')
    if args.capture:
        data = args.capture.read_bytes()
        result = check(data)
        print(json.dumps({'capture_sha256': hashlib.sha256(data).hexdigest(),
                          'bytes': len(data), 'header': struct.unpack('<3I', data[:12]),
                          'owner_begin_ret': result,
                          'scope': 'saved ledger bytes only; not live CPU admission'}))
        raise SystemExit(0 if result == 0 else 3)
