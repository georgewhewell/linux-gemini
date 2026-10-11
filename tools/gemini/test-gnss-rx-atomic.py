#!/usr/bin/env python3
"""Exercise production GNSS RX admission, STP dispatch and scalar attribution."""
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
session_path = root / 'drivers/soc/mediatek/mt6797-consys-stp-session.c'
crc_source = (root / 'lib/crc/crc16.c').read_text()
crc_table = crc_source[crc_source.index('static const u16 crc16_table'):]
crc_table = crc_table[:crc_table.index('};') + 2]
crc = crc_table + '\n' + function(root / 'lib/crc/crc16.c', 'u16 crc16(')
session_code = '\n'.join(function(session_path, signature) for signature in (
    'static u8 mt6797_consys_stp_header_checksum(',
    'static void mt6797_consys_stp_reset_parser(',
    'static void mt6797_consys_stp_reset_protocol(',
    'static void mt6797_consys_stp_fence_locked(',
    'static int mt6797_consys_stp_finish_frame_locked(',
    'static int mt6797_consys_stp_process_header_locked(',
    'static int mt6797_consys_stp_receive_byte_locked(',
    'static int mt6797_consys_stp_session_receive_locked(',
    'int mt6797_consys_stp_session_receive(',
    'u64 mt6797_consys_stp_session_gnss_rx_frames(',
    'void mt6797_consys_stp_session_quiesce(',
))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MTK_STP_HEADER_SIZE 4
#define MTK_STP_TRAILER_SIZE 2
#define MTK_STP_MAX_PAYLOAD 2048
#define MT6797_CONSYS_STP_WINDOW_SIZE 7
#define MT6797_CONSYS_STP_SEQUENCE_COUNT 8
#define wake_up_all(q) ((void)(q))
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
struct mtk_gnss {
    struct mutex lock;
    bool registered, active;
    struct gnss_device *gdev;
    u64 rx_admitted_frames, rx_inactive_frames, rx_rejected_frames;
};
static int gnss_insert_raw(struct gnss_device *g, const u8 *p, size_t len)
{ return kfifo_in(&g->read_fifo, p, len); }
enum { MTK_STP_BT, MTK_STP_GNSS = 2, MTK_STP_WMT = 4 };
enum {
    MT6797_CONSYS_STP_RX_SYNC, MT6797_CONSYS_STP_RX_HEADER,
    MT6797_CONSYS_STP_RX_PAYLOAD, MT6797_CONSYS_STP_RX_CRC,
    MT6797_CONSYS_STP_RX_RESYNC,
};
enum { MT6797_CONSYS_STP_BOOTSTRAP, MT6797_CONSYS_STP_ACTIVE, MT6797_CONSYS_STP_FENCED };
struct mt6797_consys_stp_session {
    struct mutex lock;
    u8 header[4], crc[2], payload[MTK_STP_MAX_PAYLOAD];
    unsigned payload_length, channel;
    int (*bt_receive)(void *, const u8 *, size_t);
    int (*wmt_receive)(void *, unsigned, const u8 *, size_t);
    int (*gnss_receive)(void *, const u8 *, size_t);
    void *bt_receive_context, *wmt_receive_context, *gnss_receive_context;
    struct { int (*receive)(void *, unsigned, const u8 *, size_t); void *receive_context; } ops;
    u8 expected_rxseq, txack;
    bool last_rx_ok;
    unsigned ack_count;
    u64 gnss_rx_frames;
    unsigned rx_state, state, header_length, crc_length, payload_position, resync_count;
    struct { bool valid; } tx[MT6797_CONSYS_STP_SEQUENCE_COUNT];
    u8 txseq, rxack, credits, retries;
    bool bootstrap_verified;
    int tx_wait;
};
/* CRC uses the exact kernel implementation; peer TX credits are a host seam. */
CRC
static int mt6797_consys_stp_process_ack_locked(struct mt6797_consys_stp_session *s, u8 ack)
{ (void)s; assert(ack == 7); return 0; }
static int mt6797_consys_stp_send_ack_locked(struct mt6797_consys_stp_session *s, u8 ack)
{ assert(s->lock.held && ack == s->txack); s->ack_count++; return 0; }
static u8 mt6797_consys_stp_sequence_next(u8 seq) { return (seq + 1) & 7; }
static bool mt6797_consys_stp_channel_supported(unsigned channel)
{ return channel == MTK_STP_BT || channel == MTK_STP_GNSS || channel == MTK_STP_WMT; }
static bool mt6797_consys_stp_session_full_state(const struct mt6797_consys_stp_session *s)
{ return s->state == MT6797_CONSYS_STP_ACTIVE || s->state == MT6797_CONSYS_STP_BOOTSTRAP; }
static const u8 mt6797_consys_stp_resync[] = {0x7f, 0x7f, 0x7f, 0x7f};
RECEIVE
SESSION
static int finish_frame(struct mt6797_consys_stp_session *s)
{
    mutex_lock(&s->lock);
    int ret = mt6797_consys_stp_finish_frame_locked(s);
    mutex_unlock(&s->lock);
    return ret;
}
static void prepare_frame(struct mt6797_consys_stp_session *s, unsigned channel,
                          u8 sequence, const u8 *payload, unsigned length)
{
    assert(length <= sizeof(s->payload));
    s->header[0] = 0x80 | (sequence << 3) | 7;
    s->header[1] = (channel << 4) | (length >> 8);
    s->header[2] = length;
    s->header[3] = mt6797_consys_stp_header_checksum(s->header);
    s->channel = channel;
    s->payload_length = length;
    memcpy(s->payload, payload, length);
    u16 check = crc16(0, payload, length);
    s->crc[0] = check;
    s->crc[1] = check >> 8;
}
static int unexpected_channel(void *context, unsigned channel, const u8 *p, size_t n)
{ (void)context; (void)channel; (void)p; (void)n; return -ENOTCONN; }
static int other_receive(void *context, const u8 *p, size_t n)
{ (void)context; assert(p && n); return 0; }
static int wmt_receive(void *context, unsigned channel, const u8 *p, size_t n)
{ assert(channel == MTK_STP_WMT); return other_receive(context, p, n); }
static void counts(struct mt6797_consys_stp_session *s, struct mtk_gnss *g,
                   u64 wire, u64 admitted, u64 inactive, u64 rejected)
{
    assert(mt6797_consys_stp_session_gnss_rx_frames(s) == wire);
    assert(g->rx_admitted_frames == admitted && g->rx_inactive_frames == inactive);
    assert(g->rx_rejected_frames == rejected);
}
int main(void)
{
    struct gnss_device gdev = {0};
    struct mtk_gnss gnss = {.registered = true, .active = true, .gdev = &gdev};
    u8 filler[4095] = {0}, drained[4096] = {0}, incoming[] = {0x41, 0x42, 0x43};
    struct mt6797_consys_stp_session session = {
        .gnss_receive = mtk_gnss_receive,
        .gnss_receive_context = &gnss, .txack = 7,
        .state = MT6797_CONSYS_STP_ACTIVE, .ops.receive = unexpected_channel,
    };
    prepare_frame(&session, MTK_STP_GNSS, 0, incoming, sizeof(incoming));
    counts(&session, &gnss, 0, 0, 0, 0);
    assert(kfifo_in(&gdev.read_fifo, filler, sizeof(filler)) == sizeof(filler));
    assert(finish_frame(&session) == -ENOSPC);
    counts(&session, &gnss, 1, 0, 0, 1);
    assert(kfifo_len(&gdev.read_fifo) == sizeof(filler));
    assert(session.expected_rxseq == 0 && session.ack_count == 0 && !session.last_rx_ok);
    assert(kfifo_out(&gdev.read_fifo, drained, sizeof(drained)) == sizeof(filler));
    assert(memcmp(drained, filler, sizeof(filler)) == 0);
    /* An out-of-order frame while a rejected frame waits is not dispatched. */
    session.header[0] = 0x80 | (2 << 3) | 7;
    assert(finish_frame(&session) == -EILSEQ);
    counts(&session, &gnss, 2, 0, 0, 1);
    assert(session.expected_rxseq == 0 && session.ack_count == 0);
    session.header[0] = 0x80 | 7;
    assert(finish_frame(&session) == 0);
    counts(&session, &gnss, 3, 1, 0, 1);
    assert(kfifo_len(&gdev.read_fifo) == sizeof(incoming));
    assert(session.expected_rxseq == 1 && session.ack_count == 1 && session.last_rx_ok);
    assert(finish_frame(&session) == 0);
    counts(&session, &gnss, 4, 1, 0, 1);
    assert(kfifo_len(&gdev.read_fifo) == sizeof(incoming) && session.ack_count == 2);
    assert(kfifo_out(&gdev.read_fifo, drained, sizeof(drained)) == sizeof(incoming));
    assert(memcmp(drained, incoming, sizeof(incoming)) == 0);
    session.header[0] = 0x80 | (3 << 3) | 7;
    assert(finish_frame(&session) == 0);
    counts(&session, &gnss, 5, 1, 0, 1);
    assert(session.expected_rxseq == 1 && session.ack_count == 3);
    session.crc[0] ^= 1;
    assert(finish_frame(&session) == -EBADMSG);
    counts(&session, &gnss, 5, 1, 0, 1);
    assert(session.expected_rxseq == 1 && session.ack_count == 3);
    session.crc[0] ^= 1;
    gnss.active = false;
    prepare_frame(&session, MTK_STP_GNSS, 1, incoming, sizeof(incoming));
    assert(finish_frame(&session) == 0);
    counts(&session, &gnss, 6, 1, 1, 1);
    assert(session.expected_rxseq == 2 && session.ack_count == 4);
    assert(kfifo_len(&gdev.read_fifo) == 0);
    assert(finish_frame(&session) == 0); /* Inactive duplicate gets only an ACK. */
    counts(&session, &gnss, 7, 1, 1, 1);
    assert(session.ack_count == 5);
    gnss.active = true;
    session.gnss_receive = NULL;
    prepare_frame(&session, MTK_STP_GNSS, 2, incoming, sizeof(incoming));
    assert(finish_frame(&session) == -ENOTCONN);
    counts(&session, &gnss, 8, 1, 1, 1);
    assert(session.expected_rxseq == 2 && session.ack_count == 5);
    session.gnss_receive = mtk_gnss_receive;
    session.bt_receive = other_receive;
    session.wmt_receive = wmt_receive;
    prepare_frame(&session, MTK_STP_BT, 2, incoming, sizeof(incoming));
    assert(finish_frame(&session) == 0);
    prepare_frame(&session, MTK_STP_WMT, 3, incoming, sizeof(incoming));
    assert(finish_frame(&session) == 0);
    counts(&session, &gnss, 8, 1, 1, 1);
    /* Exercise the actual byte parser with fragmented valid/corrupt frames. */
    mt6797_consys_stp_reset_parser(&session);
    prepare_frame(&session, MTK_STP_GNSS, 4, incoming, sizeof(incoming));
    u8 frame[MTK_STP_HEADER_SIZE + sizeof(incoming) + MTK_STP_TRAILER_SIZE];
    memcpy(frame, session.header, MTK_STP_HEADER_SIZE);
    memcpy(frame + MTK_STP_HEADER_SIZE, incoming, sizeof(incoming));
    memcpy(frame + MTK_STP_HEADER_SIZE + sizeof(incoming), session.crc, 2);
    mt6797_consys_stp_reset_parser(&session);
    for (unsigned i = 0; i < sizeof(frame) - 1; i++) {
        assert(mt6797_consys_stp_session_receive(&session, frame + i, 1) == 0);
        counts(&session, &gnss, 8, 1, 1, 1);
    }
    assert(mt6797_consys_stp_session_receive(&session, frame + sizeof(frame) - 1, 1) == 0);
    counts(&session, &gnss, 9, 2, 1, 1);
    assert(kfifo_out(&gdev.read_fifo, drained, sizeof(drained)) == sizeof(incoming));
    assert(!memcmp(drained, incoming, sizeof(incoming)));
    frame[sizeof(frame) - 1] ^= 1;
    assert(mt6797_consys_stp_session_receive(&session, frame, sizeof(frame)) == -EBADMSG);
    counts(&session, &gnss, 9, 2, 1, 1);
    assert(session.expected_rxseq == 5 && kfifo_len(&gdev.read_fifo) == 0);
    /* A zero-length task-2-labelled ACK bypasses CRC/data dispatch entirely. */
    const u8 empty_ack[] = {0x87, 0x20, 0x00, 0xa7};
    assert(mt6797_consys_stp_session_receive(&session, empty_ack, sizeof(empty_ack)) == 0);
    counts(&session, &gnss, 9, 2, 1, 1);
    /* Actual parser/protocol resets and terminal fencing preserve counters. */
    mt6797_consys_stp_reset_parser(&session);
    mt6797_consys_stp_reset_protocol(&session);
    counts(&session, &gnss, 9, 2, 1, 1);
    mt6797_consys_stp_session_quiesce(&session);
    counts(&session, &gnss, 9, 2, 1, 1);
    assert(session.state == MT6797_CONSYS_STP_FENCED);
    assert(mt6797_consys_stp_session_receive(&session, empty_ack, sizeof(empty_ack)) == -ESHUTDOWN);
    counts(&session, &gnss, 9, 2, 1, 1);
    struct mt6797_consys_stp_session fresh = {0};
    assert(mt6797_consys_stp_session_gnss_rx_frames(&fresh) == 0);
    session.gnss_rx_frames = (1ULL << 40) + 9;
    assert(mt6797_consys_stp_session_gnss_rx_frames(&session) == (1ULL << 40) + 9);
    /* Short, exact-fit, full and maximum STP payload ownership. */
    const unsigned lengths[] = {0, 1, 3, 2048};
    u8 maximum[2048];
    memset(maximum, 0x5a, sizeof(maximum));
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        unsigned len = lengths[i];
        u64 admitted = gnss.rx_admitted_frames, rejected = gnss.rx_rejected_frames;
        memset(&gdev.read_fifo, 0, sizeof(gdev.read_fifo));
        assert(mtk_gnss_receive(&gnss, maximum, len) == 0);
        assert(kfifo_len(&gdev.read_fifo) == len);
        assert(gnss.rx_admitted_frames == admitted + !!len);
        assert(kfifo_out(&gdev.read_fifo, drained, len) == len);
        assert(!memcmp(drained, maximum, len));
        memset(&gdev.read_fifo, 0, sizeof(gdev.read_fifo));
        assert(kfifo_in(&gdev.read_fifo, filler, sizeof(filler)) == sizeof(filler));
        u8 last = 0;
        assert(kfifo_in(&gdev.read_fifo, &last, 1) == 1);
        assert(mtk_gnss_receive(&gnss, maximum, len) == (len ? -ENOSPC : 0));
        assert(kfifo_len(&gdev.read_fifo) == 4096);
        assert(gnss.rx_rejected_frames == rejected + !!len);
        memset(&gdev.read_fifo, 0, sizeof(gdev.read_fifo));
        unsigned fill = 4096 - len;
        assert(kfifo_in(&gdev.read_fifo, drained, fill) == fill);
        assert(mtk_gnss_receive(&gnss, maximum, len) == 0);
        assert(kfifo_len(&gdev.read_fifo) == 4096);
        assert(gnss.rx_admitted_frames == admitted + 2 * !!len);
    }
    memset(&gdev.read_fifo, 0, sizeof(gdev.read_fifo));
    gnss.active = false;
    u64 inactive = gnss.rx_inactive_frames;
    assert(mtk_gnss_receive(&gnss, incoming, sizeof(incoming)) == 0);
    assert(kfifo_len(&gdev.read_fifo) == 0);
    assert(gnss.rx_inactive_frames == inactive + 1);
    assert(mtk_gnss_receive(NULL, incoming, sizeof(incoming)) == -EINVAL);
    assert(mtk_gnss_receive(&gnss, NULL, sizeof(incoming)) == -EINVAL);
    assert(gnss.rx_inactive_frames == inactive + 1);
    gnss.active = true;
    gnss.registered = false;
    gnss.gdev = NULL;
    assert(mtk_gnss_receive(&gnss, incoming, sizeof(incoming)) == 0);
    assert(gnss.rx_inactive_frames == inactive + 2);
    assert(mtk_gnss_receive(&gnss, NULL, 0) == 0);
    assert(gnss.rx_inactive_frames == inactive + 2);
    return 0;
}
'''.replace('CRC\n', crc + '\n').replace('RECEIVE\n', receive + '\n').replace('SESSION\n', session_code + '\n')
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
print('PASS GNSS FIFO/retry ownership and CRC-valid/admitted/inactive/rejected attribution')
