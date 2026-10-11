#!/usr/bin/env python3
"""Exercise production GNSS callback and STP retry dispatch under sanitizers."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def function(path, signature):
    source = path.read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
receive_path = Path(sys.argv[2]) if len(sys.argv) > 2 else root / 'drivers/gnss/mtk-consys.c'
receive = function(receive_path, 'static int mtk_gnss_receive(')
finish = function(root / 'drivers/soc/mediatek/mt6797-consys-stp-session.c',
                  'static int mt6797_consys_stp_finish_frame_locked(')
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
struct fifo { u8 bytes[4096]; unsigned in, out; };
static unsigned kfifo_len(struct fifo *f) { return f->in - f->out; }
static unsigned kfifo_avail(struct fifo *f) { return sizeof(f->bytes) - kfifo_len(f); }
static unsigned kfifo_in(struct fifo *f, const u8 *p, unsigned len)
{
    unsigned n = len < kfifo_avail(f) ? len : kfifo_avail(f);
    for (unsigned i = 0; i < n; i++) f->bytes[(f->in++) % sizeof(f->bytes)] = p[i];
    return n;
}
static unsigned kfifo_out(struct fifo *f, u8 *p, unsigned len)
{
    unsigned n = len < kfifo_len(f) ? len : kfifo_len(f);
    for (unsigned i = 0; i < n; i++) p[i] = f->bytes[(f->out++) % sizeof(f->bytes)];
    return n;
}
struct gnss_device { struct fifo read_fifo; };
struct mtk_gnss { struct mutex lock; bool registered, active; struct gnss_device *gdev; };
static int gnss_insert_raw(struct gnss_device *g, const u8 *p, size_t len)
{ return kfifo_in(&g->read_fifo, p, len); }
enum { MTK_STP_BT, MTK_STP_GNSS = 2, MTK_STP_WMT = 4 };
struct mt6797_consys_stp_session {
    u8 header[4], crc[2], *payload;
    unsigned payload_length, channel;
    int (*bt_receive)(void *, const u8 *, size_t);
    int (*wmt_receive)(void *, unsigned, const u8 *, size_t);
    int (*gnss_receive)(void *, const u8 *, size_t);
    void *bt_receive_context, *wmt_receive_context, *gnss_receive_context;
    struct { int (*receive)(void *, unsigned, const u8 *, size_t); void *receive_context; } ops;
    u8 expected_rxseq, txack;
    bool last_rx_ok;
    unsigned ack_count;
};
/* CRC/peer TX credit are outside this callback ownership fixture. */
static u16 crc16(u16 seed, const u8 *p, unsigned len) { (void)seed; (void)p; (void)len; return 0; }
static int mt6797_consys_stp_process_ack_locked(struct mt6797_consys_stp_session *s, u8 ack)
{ (void)s; assert(ack == 7); return 0; }
static int mt6797_consys_stp_send_ack_locked(struct mt6797_consys_stp_session *s, u8 ack)
{ assert(ack == s->txack); s->ack_count++; return 0; }
static u8 mt6797_consys_stp_sequence_next(u8 seq) { return (seq + 1) & 7; }
static bool mt6797_consys_stp_channel_supported(unsigned channel)
{ return channel == MTK_STP_BT || channel == MTK_STP_GNSS || channel == MTK_STP_WMT; }
RECEIVE
FINISH
int main(void)
{
    struct gnss_device gdev = {0};
    struct mtk_gnss gnss = {.registered = true, .active = true, .gdev = &gdev};
    u8 filler[4095] = {0}, drained[4096] = {0}, incoming[] = {0x41, 0x42, 0x43};
    struct mt6797_consys_stp_session session = {
        .header = {7}, .payload = incoming, .payload_length = sizeof(incoming),
        .channel = MTK_STP_GNSS, .gnss_receive = mtk_gnss_receive,
        .gnss_receive_context = &gnss, .txack = 7,
    };
    assert(kfifo_in(&gdev.read_fifo, filler, sizeof(filler)) == sizeof(filler));
    assert(mt6797_consys_stp_finish_frame_locked(&session) == -ENOSPC);
    assert(kfifo_len(&gdev.read_fifo) == sizeof(filler));
    assert(session.expected_rxseq == 0 && session.ack_count == 0 && !session.last_rx_ok);
    assert(kfifo_out(&gdev.read_fifo, drained, sizeof(drained)) == sizeof(filler));
    assert(memcmp(drained, filler, sizeof(filler)) == 0);
    assert(mt6797_consys_stp_finish_frame_locked(&session) == 0);
    assert(kfifo_len(&gdev.read_fifo) == sizeof(incoming));
    assert(session.expected_rxseq == 1 && session.ack_count == 1 && session.last_rx_ok);
    assert(mt6797_consys_stp_finish_frame_locked(&session) == 0);
    assert(kfifo_len(&gdev.read_fifo) == sizeof(incoming) && session.ack_count == 2);
    assert(kfifo_out(&gdev.read_fifo, drained, sizeof(drained)) == sizeof(incoming));
    assert(memcmp(drained, incoming, sizeof(incoming)) == 0);
    /* Short, exact-fit, full and maximum STP payload ownership. */
    const unsigned lengths[] = {0, 1, 3, 2048};
    u8 maximum[2048];
    memset(maximum, 0x5a, sizeof(maximum));
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        unsigned len = lengths[i];
        memset(&gdev.read_fifo, 0, sizeof(gdev.read_fifo));
        assert(mtk_gnss_receive(&gnss, maximum, len) == 0);
        assert(kfifo_len(&gdev.read_fifo) == len);
        assert(kfifo_out(&gdev.read_fifo, drained, len) == len);
        assert(!memcmp(drained, maximum, len));
        memset(&gdev.read_fifo, 0, sizeof(gdev.read_fifo));
        assert(kfifo_in(&gdev.read_fifo, filler, sizeof(filler)) == sizeof(filler));
        u8 last = 0;
        assert(kfifo_in(&gdev.read_fifo, &last, 1) == 1);
        assert(mtk_gnss_receive(&gnss, maximum, len) == (len ? -ENOSPC : 0));
        assert(kfifo_len(&gdev.read_fifo) == 4096);
        memset(&gdev.read_fifo, 0, sizeof(gdev.read_fifo));
        unsigned fill = 4096 - len;
        assert(kfifo_in(&gdev.read_fifo, drained, fill) == fill);
        assert(mtk_gnss_receive(&gnss, maximum, len) == 0);
        assert(kfifo_len(&gdev.read_fifo) == 4096);
    }
    memset(&gdev.read_fifo, 0, sizeof(gdev.read_fifo));
    gnss.active = false;
    assert(mtk_gnss_receive(&gnss, incoming, sizeof(incoming)) == 0);
    assert(kfifo_len(&gdev.read_fifo) == 0);
    assert(mtk_gnss_receive(NULL, incoming, sizeof(incoming)) == -EINVAL);
    assert(mtk_gnss_receive(&gnss, NULL, sizeof(incoming)) == -EINVAL);
    gnss.active = true;
    gnss.registered = false;
    gnss.gdev = NULL;
    assert(mtk_gnss_receive(&gnss, incoming, sizeof(incoming)) == 0);
    return 0;
}
'''.replace('RECEIVE', receive).replace('FINISH', finish)
with tempfile.TemporaryDirectory(prefix='gnss-rx-atomic-') as out:
    source, binary = Path(out) / 'test.c', Path(out) / 'test'
    source.write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-sign-compare', '-g', '-fno-pie', '-no-pie',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(source), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)])
    if result.returncode:
        sys.exit(1)
print('PASS GNSS all-or-nothing FIFO handoff and STP retry ownership')
