#!/usr/bin/env python3
"""Cached-state observations only; no scans, traffic, firmware queries or writes.

Run collect on the target only when separately authorized. stdout is JSON;
compare reads saved JSON on the host. No target output file or socket pathname
is created. STATUS/BSS use an abstract local AF_UNIX datagram socket.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time
import uuid

SCHEMA = 'mt6797-pmf-cached-observation-v1'
STATUS_FIELDS = {'bssid', 'ssid', 'freq', 'pairwise_cipher', 'group_cipher',
                 'key_mgmt', 'wpa_state', 'pmf', 'mgmt_group_cipher'}
BSS_FIELDS = {'bssid', 'freq', 'flags', 'age', 'ie', 'beacon_ie'}
COUNTERS = ('rx_packets', 'rx_bytes', 'rx_dropped', 'rx_errors', 'tx_packets',
            'tx_bytes', 'tx_dropped', 'tx_errors')
BOOT_ID = '/proc/sys/kernel/random/boot_id'
FIRMWARE = '/lib/firmware/WIFI_RAM_CODE_6797'
FIRMWARE_SHA256 = 'a69383d74d829430487c39eef6b5e281b25f901595c903a632a10aa8631426dd'
LOG_MARKERS = ('HIF RX group1 register ', 'HIF startup result=', 'HIF_EVENT_PUMP_READY',
               'runtime HIF fault', 'RX rejected error=', 'RX data rejected error=',
               'EAPOL RX pairwise_message=', 'EAPOL TX pairwise_message=',
               'PM connection setup failed')


def fields(text, allowed):
    return {key: value for line in text.splitlines()
            for key, sep, value in [line.partition('=')] if sep and key in allowed}


class Local:
    def read(self, path):
        return Path(path).read_text().strip()

    def resolve(self, path):
        return str(Path(path).resolve(strict=True))

    def firmware_hash(self):
        # Disk provenance only: this does not read back firmware memory.
        return hashlib.sha256(Path(FIRMWARE).read_bytes()).hexdigest()

    def run(self, arguments):
        binary = shutil.which(arguments[0])
        if not binary:
            raise OSError('optional command unavailable: ' + arguments[0])
        result = subprocess.run([binary, *arguments[1:]], stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=5, check=False)
        return {'argv': arguments, 'returncode': result.returncode,
                'stdout': result.stdout, 'stderr': result.stderr}

    def control(self, interface, command):
        # Keep a narrow independent command gate: no arbitrary ctrl_iface verbs.
        if command != 'STATUS' and not re.fullmatch(r'BSS (?:[0-9a-f]{2}:){5}[0-9a-f]{2}', command):
            raise ValueError('unsupported control command')
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as control:
            control.settimeout(2)
            control.bind('\0gemini-pmf-observe-' + uuid.uuid4().hex)
            control.connect('/run/gemini-wifi/ctrl/' + interface)
            control.send(command.encode('ascii'))
            reply = control.recv(65536).decode('utf-8', errors='replace')
        if reply.startswith(('FAIL', 'UNKNOWN COMMAND')):
            raise OSError('cached control query unavailable')
        return reply


def collect(expected_boot_id, interface, cached_bssid=None, ops=None):
    ops = ops or Local()
    if not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9_.-]{0,14}', interface):
        raise ValueError('invalid interface name')
    expected_boot_id = str(uuid.UUID(expected_boot_id))
    if cached_bssid is not None:
        cached_bssid = cached_bssid.lower()
        if not re.fullmatch(r'(?:[0-9a-f]{2}:){5}[0-9a-f]{2}', cached_bssid):
            raise ValueError('invalid cached BSSID')
    # Fail before any control/netlink query if this is the wrong accepted boot.
    if ops.read(BOOT_ID) != expected_boot_id:
        raise ValueError('boot ID does not match the separately approved boot')
    device = '/sys/class/net/' + interface
    driver = ops.resolve(device + '/device/driver')
    if Path(driver).name != 'mt6797-wifi':
        raise ValueError('interface is not owned by mt6797-wifi')
    report = {'schema': SCHEMA, 'interface': interface, 'boot_id': expected_boot_id,
              'driver': driver, 'started_unix_ns': time.time_ns(), 'errors': {}}

    def optional(name, operation):
        try:
            value = operation()
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            report['errors'][name] = str(error)
            return None
        report[name] = value
        return value

    optional('kernel_release', lambda: ops.read('/proc/sys/kernel/osrelease'))
    optional('system', lambda: ops.resolve('/run/current-system'))
    optional('uptime_before', lambda: ops.read('/proc/uptime'))
    before = optional('status_before', lambda: fields(ops.control(interface, 'STATUS'), STATUS_FIELDS))
    for attribute in ('ifindex', 'operstate', 'carrier'):
        optional(attribute, lambda attribute=attribute: ops.read(device + '/' + attribute))
    report['counters'] = {}
    for counter in COUNTERS:
        value = optional('counter_' + counter,
                         lambda counter=counter: int(ops.read(device + '/statistics/' + counter)))
        if value is not None:
            report['counters'][counter] = value
            del report['counter_' + counter]
    optional('service', lambda: ops.run(['systemctl', 'show', 'gemini-wifi.service',
                                       '-p', 'ActiveState', '-p', 'SubState', '-p', 'MainPID', '-p', 'NRestarts']))
    optional('interface_info', lambda: ops.run(['iw', 'dev', interface, 'info']))
    optional('link', lambda: ops.run(['iw', 'dev', interface, 'link']))
    phy = optional('phy', lambda: Path(ops.resolve(device + '/phy80211')).name)
    if phy and re.fullmatch(r'phy[0-9]+', phy):
        optional('phy_info', lambda: ops.run(['iw', 'phy', phy, 'info']))
    optional('qdisc', lambda: ops.run(['tc', '-s', 'qdisc', 'show', 'dev', interface]))
    bssid = cached_bssid or (before or {}).get('bssid', '').lower()
    if re.fullmatch(r'(?:[0-9a-f]{2}:){5}[0-9a-f]{2}', bssid):
        optional('cached_bss', lambda: fields(ops.control(interface, 'BSS ' + bssid), BSS_FIELDS))
    digest = optional('firmware_file_sha256', ops.firmware_hash)
    report['firmware_file_matches_source_pin'] = digest == FIRMWARE_SHA256 if digest else None
    logs = optional('kernel_log', lambda: ops.run(['dmesg', '--color=never']))
    if logs:
        # Persist only known metadata messages, never arbitrary driver/log payloads.
        logs['stdout'] = '\n'.join(line[:512] for line in logs['stdout'].splitlines()
                                  if any(marker in line for marker in LOG_MARKERS))
    after = optional('status_after', lambda: fields(ops.control(interface, 'STATUS'), STATUS_FIELDS))
    optional('uptime_after', lambda: ops.read('/proc/uptime'))
    boot_after = optional('boot_id_after', lambda: ops.read(BOOT_ID))
    report['same_boot_at_endpoints'] = boot_after == expected_boot_id
    report['same_status_at_endpoints'] = bool(before) and before == after
    report['completed_wpa2_ccmp_at_endpoints'] = all(
        status and status.get('wpa_state') == 'COMPLETED' and
        status.get('key_mgmt') == 'WPA2-PSK' and status.get('pairwise_cipher') == 'CCMP' and
        status.get('group_cipher') == 'CCMP' for status in (before, after))
    report['pmf_firmware_qualified'] = False
    report['finished_unix_ns'] = time.time_ns()
    return report


def compare(before, after):
    if any(item.get('schema') != SCHEMA for item in (before, after)):
        raise ValueError('unsupported snapshot schema')
    identity = ('boot_id', 'interface', 'driver', 'system', 'ifindex')
    changed = [field for field in identity if before.get(field) != after.get(field)]
    missing = [field for field in identity if not before.get(field) or not after.get(field)]
    counters = {}
    for name in COUNTERS:
        first, last = before.get('counters', {}).get(name), after.get('counters', {}).get(name)
        if isinstance(first, int) and isinstance(last, int):
            counters[name] = last - first
    return {'schema': SCHEMA + '-comparison', 'changed_identity_fields': changed,
            'missing_identity_fields': missing,
            'counter_deltas': counters if not changed and not missing else {},
            'counter_resets': [name for name, delta in counters.items() if delta < 0] if not changed and not missing else [],
            'same_status_at_saved_endpoints': bool(before.get('status_after')) and
                before.get('status_after') == after.get('status_before'),
            'new_metadata_log_lines': sorted(set(after.get('kernel_log', {}).get('stdout', '').splitlines()) -
                                             set(before.get('kernel_log', {}).get('stdout', '').splitlines())),
            'pmf_firmware_qualified': False,
            'limitation': 'Endpoint comparison cannot exclude an intermediate reconnect; counters are not PMF evidence.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='operation', required=True)
    target = sub.add_parser('collect', help='read cached state on a separately authorized target')
    target.add_argument('--expected-boot-id', required=True)
    target.add_argument('--interface', default='wlan0')
    target.add_argument('--cached-bssid', help='optional BSSID already in the supplicant cache; no scan')
    host = sub.add_parser('compare', help='compare two saved JSON snapshots offline')
    host.add_argument('before', type=Path)
    host.add_argument('after', type=Path)
    args = parser.parse_args()
    try:
        if args.operation == 'collect':
            report = collect(args.expected_boot_id, args.interface, args.cached_bssid)
        else:
            report = compare(json.loads(args.before.read_text()), json.loads(args.after.read_text()))
    except (OSError, ValueError) as error:
        parser.exit(2, str(error) + '\n')
    print(json.dumps(report, indent=2, sort_keys=True))
    if args.operation == 'collect' and not (report['same_boot_at_endpoints'] and
            report['same_status_at_endpoints'] and report['completed_wpa2_ccmp_at_endpoints']):
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
