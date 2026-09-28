#!/usr/bin/env python3
"""A slow but healthy HIF pump must complete (M40 runtime HIF fault -110).

Compiles the production hif_pump.c of TREE (default: this tree) with the
expiry rule that gemini-m40 installed in hif_runtime.c:683-688 and 991-992
(ktime_after(now, start + 100 ms)) whenever the pump still offers an
expired op. Every register/FIFO access succeeds, but a fake clock advances
STALL_MS per access in place of preemption by hard and soft interrupts.
On gemini-m40 the pump returns -ETIMEDOUT, which hif_runtime.c:1006-1008
turns into the terminal fault seen on the device; with the work-bounded
pump the same sequence completes. Pass linux-gemini-m40 to reproduce.
"""
import os, re, subprocess, sys, tempfile
from pathlib import Path
tree = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
d = tree / 'drivers/net/wireless/mediatek/mt6797'
has_expiry = 'expired' in (d / 'hif_pump.h').read_text()
prog = r'''
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
#define MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH 2352U
struct mt6797_wifi_event_view { const u8 *payload; size_t payload_length, packet_length; u8 id, sequence; };
enum { MT6797_WIFI_RX_EVENT, MT6797_WIFI_RX_IEEE80211, MT6797_WIFI_RX_NATIVE_DATA, MT6797_WIFI_RX_ETHERNET };
struct mt6797_wifi_rx_view { int kind; const u8 *packet, *payload; size_t packet_length, payload_length; u8 event_id, sequence; };
static int mt6797_wifi_rx_parse(const u8 *p, size_t n, struct mt6797_wifi_rx_view *v)
{ memset(v,0,sizeof(*v)); v->kind=MT6797_WIFI_RX_EVENT; v->packet=p; v->packet_length=n; v->event_id=1; return 0; }
'''
for name in ['hif_regs.h', 'hif_pump.h', 'hif_pump.c']:
    src = (d / name).read_text()
    src = re.sub(r'^#include .*\n', '', src, flags=re.M)
    prog += src
prog += r'''
#define STALL_MS 30
#define BUDGET_MS 100
static u64 now_ms, start_ms; static unsigned rx_left = 3;
static void tick(void) { now_ms += STALL_MS; }
static int st(void *c, u32 *s) { (void)c; tick(); *s = MT6797_WIFI_WHISR_RX0_DONE | MT6797_WIFI_WHISR_TX_DONE; return 0; }
static int tx(void *c, u16 *r) { (void)c; tick(); memset(r,0,32); return 0; }
static int len(void *c, u16 *l) { (void)c; tick(); l[0] = rx_left ? 16 : 0; l[1] = 0; return 0; }
static int pkt(void *c, unsigned port, u16 n, const u8 **p) { static u8 b[16]; (void)c;(void)port;(void)n; tick(); rx_left--; *p=b; return 0; }
#ifdef HAS_EXPIRY
static bool expired(void *c) { (void)c; return now_ms > start_ms + BUDGET_MS; }
#endif
int main(void) {
 struct mt6797_wifi_hif_pump_stats s = {0}; u32 status;
 struct mt6797_wifi_hif_pump_ops ops = { .read_status=st, .read_tx_released=tx, .read_lengths=len, .read_packet=pkt,
#ifdef HAS_EXPIRY
  .expired=expired,
#endif
 };
 int ret = mt6797_wifi_hif_pump_run(&ops, &s, &status);
 printf("pump result=%d after %llu ms of healthy accesses, rx_packets=%llu\n", ret, (unsigned long long)now_ms, (unsigned long long)s.rx_packets);
#ifdef HAS_EXPIRY
 assert(ret == -ETIMEDOUT);
 puts("REPRODUCED: every access succeeded, yet the pump reported -ETIMEDOUT (-110) -> terminal fence");
#else
 assert(ret == 0 && s.rx_packets == 3);
 puts("FIXED: the same slow-but-healthy pump completes; no wall-clock fault");
#endif
 return 0;
}
'''
with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as t:
    c = Path(t) / 'r.c'; c.write_text(prog)
    cmd = [os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-fsanitize=address,undefined', str(c), '-o', str(Path(t) / 'r')]
    if has_expiry: cmd.insert(1, '-DHAS_EXPIRY')
    subprocess.run(cmd, check=True)
    subprocess.run([str(Path(t) / 'r')], check=True)
