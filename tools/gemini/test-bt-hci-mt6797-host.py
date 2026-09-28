#!/usr/bin/env python3
"""Host test for the MT6797 HCI endpoint's STP channel-0 receive path.

Compiles the production mt6797_hci_receive() and the production h4_recv_buf()
with host shims, then feeds STP payload boundaries that do not line up with
HCI packet boundaries. No hardware, STP session or HCI core is exercised.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    level = 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def block(path, start_marker, end_marker):
    source = (root / path).read_text()
    start = source.index(start_marker)
    return source[start:source.index(end_marker, start) + len(end_marker)]


driver = 'drivers/bluetooth/hci_mt6797.c'
receive = function(driver, 'static int mt6797_hci_receive(')
pkts = block(driver, 'static const struct h4_recv_pkt mt6797_hci_recv_pkts[]', '};')
bdaddr = block(driver, 'static const bdaddr_t mt6797_hci_default_bdaddr', '};')
h4_recv = function('drivers/bluetooth/hci_h4.c', 'struct sk_buff *h4_recv_buf(')
uart_h = (root / 'drivers/bluetooth/hci_uart.h').read_text()
recv_struct = re.search(r'struct h4_recv_pkt \{.*?\n\};', uart_h, re.S)[0]
recv_macros = re.findall(r'#define H4_RECV_(?:ACL|SCO|EVENT) \\\n(?:.*\\\n)*.*\n', uart_h)
assert len(recv_macros) == 3, recv_macros

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef unsigned int uint;
#define __u8 u8
#define HCI_COMMAND_PKT 0x01
#define HCI_ACLDATA_PKT 0x02
#define HCI_SCODATA_PKT 0x03
#define HCI_EVENT_PKT 0x04
#define HCI_ACL_HDR_SIZE 4
#define HCI_SCO_HDR_SIZE 3
#define HCI_EVENT_HDR_SIZE 2
#define HCI_MAX_FRAME_SIZE 1500
#define HCI_MAX_SCO_SIZE 255
#define HCI_MAX_EVENT_SIZE 260
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define ERR_PTR(e) ((void *)(long)(e))
#define PTR_ERR(p) ((long)(p))
#define IS_ERR(p) ((unsigned long)(p) >= (unsigned long)-4095)
#define GFP_ATOMIC 0
typedef struct { u8 b[6]; } bdaddr_t;
struct sk_buff { u8 type; unsigned expect, len, size; u8 *data; };
struct hci_dev_stats { unsigned err_rx, byte_rx; };
struct hci_dev { struct hci_dev_stats stat; };
struct hci_uart { struct hci_dev *hdev; u8 alignment, padding; };
struct mt6797_hci { struct hci_uart hu; struct hci_dev *hdev; struct sk_buff *rx_skb; };
#define hci_skb_pkt_type(s) ((s)->type)
#define hci_skb_expect(s) ((s)->expect)
static unsigned live_skbs;
static struct sk_buff *bt_skb_alloc(unsigned size, int gfp) {
 struct sk_buff *s = calloc(1, sizeof(*s)); (void)gfp;
 s->data = calloc(1, size); s->size = size; live_skbs++; return s;
}
static void kfree_skb(struct sk_buff *s) { if (!s) return; free(s->data); free(s); live_skbs--; }
static unsigned skb_tailroom(struct sk_buff *s) { return s->size - s->len; }
static void skb_put_data(struct sk_buff *s, const void *d, unsigned n) {
 assert(s->len + n <= s->size); memcpy(s->data + s->len, d, n); s->len += n;
}
static u16 get_unaligned_le16(const u8 *p) { return p[0] | p[1] << 8; }
static unsigned errors_logged;
#define bt_dev_err_ratelimited(h, ...) do { (void)(h); errors_logged++; } while (0)
struct frame { u8 type; unsigned len; u8 data[HCI_MAX_FRAME_SIZE]; };
static struct frame frames[16]; static unsigned nframes;
static int hci_recv_frame(struct hci_dev *h, struct sk_buff *s) {
 (void)h; assert(nframes < 16);
 frames[nframes].type = s->type; frames[nframes].len = s->len;
 memcpy(frames[nframes].data, s->data, s->len); nframes++; kfree_skb(s); return 0;
}
'''
program += recv_struct + '\n' + ''.join(recv_macros) + '\n'
program += h4_recv + '\n' + pkts + '\n' + bdaddr + '\n' + receive + '\n'
program += r'''
static struct hci_dev hdev; static struct mt6797_hci bt;
static void reset(void) {
 memset(&hdev, 0, sizeof(hdev)); memset(&bt, 0, sizeof(bt));
 bt.hdev = &hdev; bt.hu.hdev = &hdev; nframes = 0; errors_logged = 0;
}
static void feed(const u8 *d, unsigned n) { assert(mt6797_hci_receive(&bt, d, n) == 0); }
int main(void) {
 /* Command Complete for HCI_Reset split across two STP payloads. */
 const u8 reset_evt[] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
 reset();
 feed(reset_evt, 3); assert(nframes == 0 && bt.rx_skb);
 feed(reset_evt + 3, 4); assert(nframes == 1 && !bt.rx_skb);
 assert(frames[0].type == 0x04 && frames[0].len == 6);
 assert(!memcmp(frames[0].data, reset_evt + 1, 6));
 assert(hdev.stat.byte_rx == 7 && hdev.stat.err_rx == 0);

 /* One STP payload carrying an event and a zero-length ACL packet. */
 const u8 two[] = { 0x04, 0x0f, 0x04, 0x00, 0x01, 0x05, 0x04,
                    0x02, 0x01, 0x20, 0x00, 0x00 };
 reset(); feed(two, sizeof(two));
 assert(nframes == 2 && frames[0].type == 0x04 && frames[1].type == 0x02);
 assert(frames[1].len == 4 && !bt.rx_skb);

 /* ACL payload split byte-by-byte, including inside its length field. */
 const u8 acl[] = { 0x02, 0x01, 0x20, 0x03, 0x00, 0xaa, 0xbb, 0xcc };
 reset();
 for (unsigned i = 0; i < sizeof(acl); i++) feed(acl + i, 1);
 assert(nframes == 1 && frames[0].len == 7 && frames[0].data[6] == 0xcc);

 /* An invalid type is consumed (so STP ACKs it) and the stream recovers. */
 const u8 bad[] = { 0x09, 0x00 };
 reset(); feed(bad, sizeof(bad));
 assert(nframes == 0 && !bt.rx_skb && hdev.stat.err_rx == 1 && errors_logged == 1);
 feed(reset_evt, sizeof(reset_evt));
 assert(nframes == 1 && frames[0].type == 0x04);

 /* A zero-length payload is a no-op. */
 reset(); assert(mt6797_hci_receive(&bt, reset_evt, 0) == 0 && hdev.stat.byte_rx == 0);

 /* Vendor sDefaultCfg.addr 00:00:46:02:79:01 in HCI (little-endian) order. */
 const u8 vendor_msb_first[6] = { 0x00, 0x00, 0x46, 0x02, 0x79, 0x01 };
 for (unsigned i = 0; i < 6; i++)
  assert(mt6797_hci_default_bdaddr.b[i] == vendor_msb_first[5 - i]);

 assert(live_skbs == 0);
 puts("PASS mt6797 HCI: H:4 stream reassembly across STP payloads, "
      "malformed-type drop without withholding ACK, default BD address order");
 return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    src.write_text(program)
    cc = os.environ.get('CC', 'cc')
    subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-sign-compare',
                    '-fsanitize=address,undefined', '-o', exe, src], check=True)
    subprocess.run([exe], check=True)
