#!/usr/bin/env python3
"""Offline boundary tests; neither this host's nor Gemini's interfaces are read."""
import copy
import importlib.util
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('observe', Path(__file__).with_name('wifi-pmf-observe.py'))
observe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(observe)
BOOT = '12345678-1234-1234-1234-123456789abc'
STATUS = ('bssid=02:11:22:33:44:55\nssid=qualification\nfreq=2412\n'
          'wpa_state=COMPLETED\nkey_mgmt=WPA2-PSK\npairwise_cipher=CCMP\ngroup_cipher=CCMP\n'
          'psk=DO-NOT-COLLECT\npassword=DO-NOT-COLLECT\n')


class Fake:
    def __init__(self):
        self.calls = []
        self.status = [STATUS, STATUS]
        self.boot = [BOOT, BOOT]
        self.driver = 'mt6797-wifi'
        self.no_optional = False

    def read(self, path):
        self.calls.append(('read', path))
        if path == observe.BOOT_ID:
            return self.boot.pop(0)
        if '/statistics/' in path:
            return '10'
        if path.endswith('/ifindex'):
            return '4'
        return 'fixture'

    def resolve(self, path):
        self.calls.append(('resolve', path))
        if path.endswith('/driver'):
            return '/sys/bus/platform/drivers/' + self.driver
        if path.endswith('/phy80211'):
            return '/sys/class/ieee80211/phy0'
        return '/nix/store/fixture-system'

    def firmware_hash(self):
        self.calls.append(('hash', observe.FIRMWARE))
        return observe.FIRMWARE_SHA256

    def run(self, argv):
        self.calls.append(('run', argv))
        if self.no_optional:
            raise OSError('not installed')
        return {'argv': argv, 'returncode': 0, 'stdout':
                '[1] unrelated private log\n[2] mt6797-wifi: HIF_EVENT_PUMP_READY\n', 'stderr': ''}

    def control(self, interface, command):
        self.calls.append(('control', interface, command))
        if command == 'STATUS':
            return self.status.pop(0)
        assert command == 'BSS 02:11:22:33:44:55'
        return 'bssid=02:11:22:33:44:55\nage=123\nie=30020100\npsk=DO-NOT-COLLECT\n'


def expect_value_error(operation):
    try:
        operation()
    except ValueError:
        return
    raise AssertionError('expected ValueError')


fake = Fake()
report = observe.collect(BOOT, 'wlan0', ops=fake)
assert report['same_boot_at_endpoints'] and report['same_status_at_endpoints']
assert report['completed_wpa2_ccmp_at_endpoints'] and not report['pmf_firmware_qualified']
assert report['firmware_file_matches_source_pin']
assert 'DO-NOT-COLLECT' not in str(report) and 'unrelated private log' not in report['kernel_log']['stdout']
assert report['cached_bss']['age'] == '123'  # Stale cache must remain distinguishable from a new scan.
commands = [call[1] for call in fake.calls if call[0] == 'run']
assert commands == [
    ['systemctl', 'show', 'gemini-wifi.service', '-p', 'ActiveState', '-p', 'SubState', '-p', 'MainPID', '-p', 'NRestarts'],
    ['iw', 'dev', 'wlan0', 'info'], ['iw', 'dev', 'wlan0', 'link'], ['iw', 'phy', 'phy0', 'info'],
    ['tc', '-s', 'qdisc', 'show', 'dev', 'wlan0'], ['dmesg', '--color=never']]
assert [call[2] for call in fake.calls if call[0] == 'control'] == ['STATUS', 'BSS 02:11:22:33:44:55', 'STATUS']
assert not any('wpa.conf' in str(call) or '/dev/mem' in str(call) for call in fake.calls)

fake = Fake(); fake.boot[0] = 'wrong-boot'
expect_value_error(lambda: observe.collect(BOOT, 'wlan0', ops=fake))
assert fake.calls == [('read', observe.BOOT_ID)]
fake = Fake(); fake.driver = 'other-driver'
expect_value_error(lambda: observe.collect(BOOT, 'wlan0', ops=fake))
assert not any(call[0] in ('run', 'control') for call in fake.calls)
for interface in ('../wlan0', 'wlan0;reboot', '-x', 'wlan0\nscan'):
    fake = Fake()
    expect_value_error(lambda: observe.collect(BOOT, interface, ops=fake))
    assert not fake.calls
fake = Fake()
expect_value_error(lambda: observe.collect(BOOT, 'wlan0', '00:11:22:33:44:55\nSCAN', fake))
assert not fake.calls
fake = Fake(); fake.status[1] = STATUS.replace('COMPLETED', 'DISCONNECTED')
changed = observe.collect(BOOT, 'wlan0', ops=fake)
assert not changed['same_status_at_endpoints'] and not changed['completed_wpa2_ccmp_at_endpoints']
fake = Fake(); fake.boot[1] = 'new-boot'
assert not observe.collect(BOOT, 'wlan0', ops=fake)['same_boot_at_endpoints']
fake = Fake(); fake.no_optional = True
missing = observe.collect(BOOT, 'wlan0', ops=fake)
assert 'phy_info' in missing['errors'] and not missing['pmf_firmware_qualified']

# The actual control transport cannot send arbitrary commands, create a socket
# file, subscribe to events, or open an IP/raw socket. Exercise it with a fake
# socket instead of contacting a daemon.
class Control:
    def __init__(self): self.operations = []
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def settimeout(self, value): self.operations.append(('timeout', value))
    def bind(self, address): assert address.startswith('\0'); self.operations.append(('bind', address))
    def connect(self, address): self.operations.append(('connect', address))
    def send(self, data): self.operations.append(('send', data))
    def recv(self, maximum): return STATUS.encode()

control = Control()
with patch.object(observe.socket, 'socket', return_value=control) as socket_factory:
    assert observe.Local().control('wlan0', 'STATUS') == STATUS
    socket_factory.assert_called_once_with(observe.socket.AF_UNIX, observe.socket.SOCK_DGRAM)
assert ('send', b'STATUS') in control.operations
assert ('connect', '/run/gemini-wifi/ctrl/wlan0') in control.operations
for command in ('SCAN', 'REASSOCIATE', 'GET_NETWORK 0 psk', 'BSS 0', 'STATUS\nRECONNECT'):
    with patch.object(observe.socket, 'socket') as socket_factory:
        expect_value_error(lambda: observe.Local().control('wlan0', command))
        socket_factory.assert_not_called()

second = copy.deepcopy(report); second['counters']['rx_packets'] += 5
comparison = observe.compare(report, second)
assert comparison['counter_deltas']['rx_packets'] == 5 and not comparison['pmf_firmware_qualified']
second['boot_id'] = 'new-boot'
assert not observe.compare(report, second)['counter_deltas']
second = copy.deepcopy(report); second['counters']['rx_packets'] = 0
assert observe.compare(report, second)['counter_resets'] == ['rx_packets']
second = copy.deepcopy(report); del second['ifindex']
assert not observe.compare(report, second)['counter_deltas']
second.pop('status_before', None); report_without_status = copy.deepcopy(report); report_without_status.pop('status_after')
assert not observe.compare(report_without_status, second)['same_status_at_saved_endpoints']
expect_value_error(lambda: observe.compare({}, report))
print('PASS: offline collector guards, cached command allowlist, abstract UNIX transport, secret-field filtering, missing interfaces, endpoint changes and conservative counter comparison; no hardware contacted')
