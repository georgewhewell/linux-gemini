#!/usr/bin/env python3
"""Runtime HIF fault handling: pump latency, firmware power-off and restart gating.

Compiles the production hif_pump.c, fw_power.c, the command header encoder
from protocol.c and the runtime transaction from hif_runtime.c with host
shims, then runs the transaction KUnit cases from hif_runtime_test.c.
A fake clock stands in for preemption; register fakes stand in for WCIR and
the mailbox. No real IRQ concurrency, firmware or CONSYS behavior is claimed.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

driver = Path(__file__).resolve().parents[2] / 'drivers/net/wireless/mediatek/mt6797'


def strip(text):
    return re.sub(r'^#include .*\n', '', text, flags=re.M)


def function(text, name):
    match = re.search(r'^(?:static )?\w[\w \*]*\b' + name + r'\(.*?\n\}\n', text,
                      re.M | re.S)
    assert match, name
    return match[0]


program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) (((~0U)<<(l))&(~0U>>(31-(h))))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define check_add_overflow(a,b,d) __builtin_add_overflow(a,b,d)
#define ETIMEDOUT_ ETIMEDOUT
static void put_unaligned_le16(u16 v, void *p) { u8 *b = p; b[0] = v; b[1] = v >> 8; }
struct mt6797_wifi_event_view { const u8 *payload; size_t payload_length, packet_length; u8 id, sequence; };
enum { MT6797_WIFI_RX_EVENT, MT6797_WIFI_RX_IEEE80211, MT6797_WIFI_RX_NATIVE_DATA, MT6797_WIFI_RX_ETHERNET };
struct mt6797_wifi_rx_view { int kind; const u8 *packet, *payload; size_t packet_length, payload_length; u8 event_id, sequence; };
static int mt6797_wifi_rx_parse(const u8 *p, size_t n, struct mt6797_wifi_rx_view *v)
{ memset(v, 0, sizeof(*v)); v->kind = MT6797_WIFI_RX_EVENT; v->packet = p; v->packet_length = n; v->event_id = 1; return 0; }
/* KUnit shims */
struct kunit { int unused; };
#define KUNIT_ASSERT_EQ(t,a,b) assert((a)==(b))
#define KUNIT_EXPECT_EQ(t,a,b) assert((a)==(b))
#define KUNIT_EXPECT_NE(t,a,b) assert((a)!=(b))
#define KUNIT_ASSERT_LT(t,a,b) assert((a)<(b))
#define KUNIT_EXPECT_TRUE(t,a) assert(a)
#define KUNIT_ASSERT_TRUE(t,a) assert(a)
#define KUNIT_EXPECT_FALSE(t,a) assert(!(a))
#define KUNIT_FAIL(t,...) assert(!"KUNIT_FAIL")
'''
protocol_h = (driver / 'protocol.h').read_text()
for name in ['MT6797_WIFI_COMMAND_HEADER_LENGTH', 'MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', protocol_h, re.M)[0] + '\n'
protocol_c = (driver / 'protocol.c').read_text()
for name in ['MT6797_WIFI_COMMAND_PQ_ID', 'MT6797_WIFI_COMMAND_PACKET_TYPE']:
    program += re.search(r'^#define\s+' + name + r'\s+[^\n]+', protocol_c, re.M)[0] + '\n'
program += function(protocol_c, 'mt6797_wifi_command_init')
for name in ['hif_regs.h', 'hif_pump.h', 'hif_pump.c', 'fw_power.h', 'fw_power.c']:
    program += strip((driver / name).read_text())

# Runtime transaction types and code, without the kernel-object parts.
runtime_h = (driver / 'hif_runtime.h').read_text()
for block in [r'enum mt6797_wifi_hif_irq_mode \{.*?\};\n',
              r'struct mt6797_wifi_hif_runtime_transaction \{.*?\};\n',
              r'struct mt6797_wifi_hif_runtime_ops \{.*?\};\n']:
    program += re.search(block, runtime_h, re.S)[0]
program += '''
bool mt6797_wifi_hif_runtime_mark_terminal(int *mode);
int mt6797_wifi_hif_runtime_transaction_start(const struct mt6797_wifi_hif_runtime_ops *ops,
	struct mt6797_wifi_hif_runtime_transaction *transaction);
int mt6797_wifi_hif_runtime_transaction_stop(const struct mt6797_wifi_hif_runtime_ops *ops,
	struct mt6797_wifi_hif_runtime_transaction *transaction, bool terminal);
'''
runtime_c = (driver / 'hif_runtime.c').read_text()
program += runtime_c[runtime_c.index('static bool mt6797_wifi_hif_runtime_ops_valid('):
                     runtime_c.index('static int mt6797_wifi_hif_reg_read(')]
program += '''
static int cmpxchg_int(int *p, int old, int new) { int cur = *p; if (cur == old) *p = new; return cur; }
#define cmpxchg(p,o,n) cmpxchg_int(p,o,n)
'''
program += function(runtime_c, 'mt6797_wifi_hif_runtime_mark_terminal')

runtime_test = (driver / 'hif_runtime_test.c').read_text()
prelude = runtime_test[:runtime_test.index('static void mt6797_wifi_hif_start_stop_test(')]
program += strip(prelude)
cases = ['mt6797_wifi_hif_start_stop_test', 'mt6797_wifi_hif_program_failure_test',
         'mt6797_wifi_hif_device_enable_failure_test', 'mt6797_wifi_hif_restore_failure_test',
         'mt6797_wifi_hif_terminal_power_off_test',
         'mt6797_wifi_hif_start_failure_no_power_off_test',
         'mt6797_wifi_hif_terminal_transition_test', 'mt6797_wifi_hif_remove_after_fault_test']
for case in cases:
    program += function(runtime_test, case)

program += r'''
/* ---- Pump latency: healthy but slow accesses must not fail. ---- */
static u64 now_ms; static unsigned rx_left;
static void tick(void) { now_ms += 30; }
static int st(void *c, u32 *s) { (void)c; tick(); *s = MT6797_WIFI_WHISR_RX0_DONE | MT6797_WIFI_WHISR_TX_DONE; return 0; }
static int txr(void *c, u16 *r) { (void)c; tick(); memset(r, 0, 32); r[15] = 1; r[14] = 1; return 0; }
static int len(void *c, u16 *l) { (void)c; tick(); l[0] = rx_left ? 16 : 0; l[1] = 0; return 0; }
static int pkt(void *c, unsigned port, u16 n, const u8 **p) { static u8 b[16]; (void)c; (void)port; (void)n; tick(); rx_left--; *p = b; return 0; }
static void pump_latency(void)
{
	struct mt6797_wifi_hif_pump_stats s = {0};
	const struct mt6797_wifi_hif_pump_ops ops = { .read_status = st, .read_tx_released = txr,
		.read_lengths = len, .read_packet = pkt };
	u32 status;

	now_ms = 0; rx_left = 32;
	assert(!mt6797_wifi_hif_pump_run(&ops, &s, &status));
	/* 32 packets at 30 ms per access: > 2 s, far beyond the old 100 ms. */
	assert(s.rx_packets == 32 && s.tx_released[15] == 1 && now_ms > 2000);
	/* The work bound itself is unchanged: a 33rd packet is still refused. */
	rx_left = 40; memset(&s, 0, sizeof(s));
	assert(mt6797_wifi_hif_pump_run(&ops, &s, &status) == MT6797_WIFI_HIF_PUMP_MORE);
	assert(s.rx_pending && s.rx_packets == 32);
	assert(!mt6797_wifi_hif_pump_run(&ops, &s, &status));
	assert(!s.rx_pending && s.rx_packets == 40);
}

/* ---- Firmware power-off (GEN3 wlanAdapterStop/wlanPowerOffInt). ---- */
static struct {
	int send_ret, drain_left, ready_polls, ready_error_at, clear_ret;
	bool int_clears, ready;
	unsigned sends, drains, reads, ints, clears, sleeps;
	int order[2048]; unsigned n;
} f;
enum { SEND = 1, DRAIN, READ, INT, CLEAR };
static void rec(int e) { if (f.n < ARRAY_SIZE(f.order)) f.order[f.n++] = e; }
static int p_send(void *c) { (void)c; f.sends++; rec(SEND); return f.send_ret; }
static int p_drain(void *c, bool *pending) { (void)c; f.drains++; rec(DRAIN); *pending = f.drain_left-- > 0; return 0; }
static int p_ready(void *c, bool *ready)
{
	(void)c; f.reads++; rec(READ);
	if (f.ready_error_at && f.reads == (unsigned)f.ready_error_at) return -EIO;
	if (f.ready && f.ready_polls >= 0 && f.reads > (unsigned)f.ready_polls) f.ready = false;
	*ready = f.ready; return 0;
}
static int p_int(void *c) { (void)c; f.ints++; rec(INT); if (f.int_clears) f.ready = false; return 0; }
static int p_clear(void *c) { (void)c; f.clears++; rec(CLEAR); return f.clear_ret; }
static void p_sleep(void *c, unsigned ms) { (void)c; assert(ms == 1); f.sleeps++; }
static const struct mt6797_wifi_power_off_ops pops = { p_send, p_drain, p_ready, p_int, p_clear, p_sleep, NULL };
static void fresh(void) { memset(&f, 0, sizeof(f)); f.ready = true; f.ready_polls = -1; }

static void power_off(void)
{
	struct mt6797_wifi_power_off_result r;
	u8 cmd[MT6797_WIFI_NIC_POWER_CTRL_LENGTH + 4];

	/* CMD_ID_NIC_POWER_CTRL(ucPowerMode=1): 12-byte TC4 command. */
	memset(cmd, 0xee, sizeof(cmd));
	assert(!mt6797_wifi_power_off_command_build(cmd, MT6797_WIFI_NIC_POWER_CTRL_LENGTH, 0xf3));
	const u8 want[12] = { 12, 0, 0x00, 0x80, 0x04, 0xa0, 1, 0xf3, 1, 0, 0, 0 };
	assert(!memcmp(cmd, want, 12) && cmd[12] == 0xee);
	assert(mt6797_wifi_power_off_command_build(cmd, 11, 0) == -EMSGSIZE);

	/* Commanded power-off: drain, then READY clears on the 3rd read. */
	fresh(); f.drain_left = 2; f.ready_polls = 2;
	assert(!mt6797_wifi_power_off_run(&pops, &r));
	assert(r.commanded && !r.interrupts && f.drains == 3 && f.reads == 3 && !f.ints);
	assert(f.order[0] == SEND && f.order[1] == DRAIN);

	/* Drain is bounded even if the firmware keeps interrupting. */
	fresh(); f.drain_left = 1000; f.ready_polls = 0;
	assert(!mt6797_wifi_power_off_run(&pops, &r) && f.drains == MT6797_WIFI_POWER_OFF_DRAIN_PASSES);

	/* READY stuck for 100 polls: vendor falls back to the mailbox interrupt. */
	fresh(); f.int_clears = true;
	assert(!mt6797_wifi_power_off_run(&pops, &r));
	assert(r.commanded && r.interrupts == 1 && r.polls == MT6797_WIFI_POWER_OFF_COMMAND_POLLS);
	assert(f.clears == 1 && f.order[f.n - 1] == CLEAR);

	/* No TX page: nothing written, go straight to the interrupt method. */
	fresh(); f.send_ret = -EAGAIN; f.int_clears = true;
	assert(!mt6797_wifi_power_off_run(&pops, &r));
	assert(!r.commanded && !f.drains && r.interrupts == 1 && !r.polls);

	/* Dead firmware: bounded, mailbox always cleared, -ETIMEDOUT. */
	fresh();
	assert(mt6797_wifi_power_off_run(&pops, &r) == -ETIMEDOUT);
	assert(r.polls == MT6797_WIFI_POWER_OFF_POLLS && f.ints == f.clears &&
	       f.ints == MT6797_WIFI_POWER_OFF_POLLS - MT6797_WIFI_POWER_OFF_COMMAND_POLLS);
	assert(f.reads == MT6797_WIFI_POWER_OFF_POLLS + f.ints * MT6797_WIFI_POWER_OFF_INT_POLLS);
	/* Worst case sleeps: vendor-equal bound of roughly 8.8 s. */
	assert(f.sleeps <= 412 * 21 + 100);

	/* Transport errors are returned (caller fences), never retried. */
	fresh(); f.send_ret = -EIO;
	assert(mt6797_wifi_power_off_run(&pops, &r) == -EIO && !f.reads);
	fresh(); f.ready_error_at = 1;
	assert(mt6797_wifi_power_off_run(&pops, &r) == -EIO);
	fresh(); f.int_clears = true; f.clear_ret = -EIO;
	assert(mt6797_wifi_power_off_run(&pops, &r) == -EIO);
	assert(mt6797_wifi_power_off_run(NULL, &r) == -EINVAL);
}

int main(void)
{
	struct kunit t = {0};
'''
program += '\n'.join(f'\t{case}(&t);' for case in cases)
program += r'''
	pump_latency();
	power_off();
	puts("PASS: slow-but-healthy pump completes (no wall-clock fault), RX work bound kept; "
	     "GEN3 NIC_POWER_CTRL encoding, drain/READY/mailbox power-off order and bounds; "
	     "terminal stop with proven power-off is not fenced, unproven or failed-start stops are");
	return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-hif-recovery-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall',
                    '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(path / 'test.c'), '-o',
                    str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
