#!/usr/bin/env python3
"""Execute the actual FS codec and service against an in-memory provider.

Covers every vendor operation the service answers, its error mapping, path
and UTF-16 handling, handle numbering, reply framing and fragmentation, and
the refusal codes for write-class operations. No modem, VFS or device access.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
provider = (root / 'include/linux/soc/mediatek/mt6797-md1-fs-provider.h').read_text()
provider = provider[provider.index('struct mt6797_md1_fs_stats {'):provider.index('#if IS_ENABLED')]
codec = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_fs.h').read_text()
service = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_fs.c').read_text()
service = '\n'.join(l for l in service.splitlines() if not l.startswith('#include'))
shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint8_t u8; typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64;
typedef int64_t loff_t; typedef long ssize_t;
#define S32_MAX INT32_MAX
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define GFP_KERNEL 0
#define EMSGSIZE 90
#define EPROTO 71
#define EOPNOTSUPP 95
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((long)(p))
#define ERR_PTR(e) ((void *)(long)(e))
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
static void cond_resched(void) {}
struct file { int node; loff_t f_pos; int cursor; bool directory; };
struct timezone { int tz_minuteswest, tz_dsttime; };
static struct timezone sys_tz;
static int allocations, frees;
static void *kmalloc(size_t n, int f) { (void)f; allocations++; return malloc(n); }
static void *kzalloc(size_t n, int f) { (void)f; allocations++; return calloc(1, n); }
static void kfree(void *p) { if (p) frees++; free(p); }
static char *kstrdup(const char *s, int f) { (void)f; allocations++; return strdup(s); }
static void time64_to_tm(s64 t, int offset, struct tm *tm) { time_t v = t + offset; gmtime_r(&v, tm); }
'''
fake = r'''
/* Fixture tree: Z:\ = { FATC62610C1.log? no; NVRAM/ { MD_A.bin, MD_B.bin, sub/ }, ro.txt, big.bin } */
struct node { const char *path; unsigned mode; const char *data; u64 size; int parent; };
static char big[5000];
static struct node nodes[] = {
 {"", 0040755, NULL, 4096, -1},
 {"NVRAM", 0040755, NULL, 4096, 0},
 {"NVRAM/MD_A.bin", 0100644, "alphaomega", 10, 1},
 {"NVRAM/MD_B.bin", 0100600, "beta", 4, 1},
 {"NVRAM/sub", 0040444, NULL, 4096, 1},
 {"NVRAM/x\xc3\xa9.txt", 0100444, "utf", 3, 1},
 {"ro.txt", 0100444, "readonly", 8, 0},
 {"big.bin", 0100644, big, sizeof(big), 0},
};
#define NODES ((int)(sizeof(nodes) / sizeof(nodes[0])))
static int open_files, provider_available = 1;
static int find(const char *path) { for (int i = 0; i < NODES; i++) if (!strcmp(nodes[i].path, path)) return i; return -1; }
static void fill(int n, struct mt6797_md1_fs_attr *a) { a->mode = nodes[n].mode; a->size = nodes[n].size; a->atime = 1757260800 + n; a->mtime = 1757347200 + n; }
static int mt6797_md1_fs_provider_getattr(const char *path, struct mt6797_md1_fs_attr *a) {
 int n; assert(path && path[0] != '/' && !strstr(path, "..")); if (!provider_available) return -ENODEV;
 n = find(path); if (n < 0) return -ENOENT; fill(n, a); return 0; }
static struct file *mt6797_md1_fs_provider_open(const char *path, bool directory) {
 int n = find(path); struct file *f; if (!provider_available) return ERR_PTR(-ENODEV); if (n < 0) return ERR_PTR(-ENOENT);
 if (directory != S_ISDIR(nodes[n].mode)) return ERR_PTR(directory ? -ENOTDIR : -EISDIR);
 f = calloc(1, sizeof(*f)); f->node = n; f->directory = directory; open_files++; return f; }
static int mt6797_md1_fs_provider_fstat(struct file *f, struct mt6797_md1_fs_attr *a) { fill(f->node, a); return 0; }
static ssize_t mt6797_md1_fs_provider_read(struct file *f, void *buf, size_t count, loff_t *pos) {
 struct node *n = &nodes[f->node]; assert(!f->directory && pos == &f->f_pos);
 if (*pos >= (loff_t)n->size) return 0;
 if (count > n->size - *pos) count = n->size - *pos;
 memcpy(buf, n->data + *pos, count); *pos += count; return count; }
static loff_t mt6797_md1_fs_provider_llseek(struct file *f, loff_t offset, int whence) {
 loff_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->f_pos : (loff_t)nodes[f->node].size;
 if (base + offset < 0) return -EINVAL;
 f->f_pos = base + offset; return f->f_pos; }
static int mt6797_md1_fs_provider_next_entry(struct file *f, struct mt6797_md1_fs_entry *e) {
 assert(f->directory); for (; f->cursor < NODES; f->cursor++) if (nodes[f->cursor].parent == f->node) {
  const char *slash = strrchr(nodes[f->cursor].path, '/'); strcpy(e->name, slash ? slash + 1 : nodes[f->cursor].path);
  fill(f->cursor, &e->attr); f->cursor++; return 0; } return -ENOENT; }
static void mt6797_md1_fs_provider_close(struct file *f) { assert(f); open_files--; free(f); }
static int mt6797_md1_fs_provider_stats(struct mt6797_md1_fs_stats *s) { if (!provider_available) return -ENODEV; *s = (struct mt6797_md1_fs_stats){4096, 12345, 6789}; return 0; }
'''
tests = r'''
static u8 frame[3584], payload[MT6797_FS_REPLY_MAX];
static struct mt6797_fs_service svc;
static struct mt6797_fs_reply reply;
static s32 status;
/* Build a channel-14 request: args given as (ptr,len) pairs. */
static size_t build(u32 index, u32 op, unsigned argc, const void *const *args, const u32 *lens) {
 size_t n = 24; memset(frame, 0, sizeof(frame)); mt6797_fs_write32(frame + 8, 14 | 0x80000000U);
 mt6797_fs_write32(frame + 12, index); mt6797_fs_write32(frame + 16, op); mt6797_fs_write32(frame + 20, argc);
 for (unsigned i = 0; i < argc; i++) { mt6797_fs_write32(frame + n, lens[i]); n += 4; memcpy(frame + n, args[i], lens[i]); n += (lens[i] + 3) & ~3U; }
 mt6797_fs_write32(frame + 4, n); return n; }
static u32 utf16(const char *ascii, u8 *out) { u32 n = 0; for (; *ascii; ascii++) { out[n++] = *ascii; out[n++] = 0; } out[n++] = 0; out[n++] = 0; return n; }
static u8 pathbuf[600];
static int run(u32 index, u32 op, unsigned argc, const void *const *args, const u32 *lens) {
 size_t n = build(index, op, argc, args, lens); reply = (struct mt6797_fs_reply){.payload = payload, .capacity = sizeof(payload)};
 status = 12345; return mt6797_fs_service_request(&svc, frame, n, &reply, &status); }
static int run_path(u32 op, const char *path, u32 extra, bool with_extra) {
 u32 n = utf16(path, pathbuf); u8 e[4]; mt6797_fs_write32(e, extra); const void *a[2] = {pathbuf, e}; u32 l[2] = {n, 4};
 return run(0, op, with_extra ? 2 : 1, a, l); }
static int run_u32(u32 op, unsigned argc, u32 a0, u32 a1, u32 a2) {
 u8 b[3][4]; mt6797_fs_write32(b[0], a0); mt6797_fs_write32(b[1], a1); mt6797_fs_write32(b[2], a2);
 const void *a[3] = {b[0], b[1], b[2]}; u32 l[3] = {4, 4, 4}; return run(0, op, argc, a, l); }
static u32 arg(unsigned n, const u8 **data) { /* n-th reply argument */
 size_t o = 4; u32 len = 0; for (unsigned i = 0; i <= n; i++) { len = mt6797_fs_read32(reply.payload + o); *data = reply.payload + o + 4; o += 4 + ((len + 3) & ~3U); } return len; }
static s32 arg_s32(unsigned n) { const u8 *d; assert(arg(n, &d) == 4); return (s32)mt6797_fs_read32(d); }
static int findfirst(const char *pattern, u8 attr, u8 mask, u32 max) {
 u32 n = utf16(pattern, pathbuf); u8 m[4]; mt6797_fs_write32(m, max); const void *a[4] = {pathbuf, &attr, &mask, m}; u32 l[4] = {n, 1, 1, 4};
 return run(1, MT6797_FS_FINDFIRST, 4, a, l); }
static void check_name(const char *expect, unsigned n) {
 const u8 *d; u32 len = arg(n, &d); size_t chars = strlen(expect); assert(len == (chars ? 2 * chars + 2 : 0));
 for (size_t i = 0; i < chars; i++) assert(d[2 * i] == (u8)expect[i] && !d[2 * i + 1]);
 if (chars) assert(!d[len - 2] && !d[len - 1]); }
int main(void) {
 struct mt6797_fs_request req; const u8 *d; char path[300]; u8 buf[600]; bool fitted;
 memset(big, 'b', sizeof(big));
 /* C11 literal 56-byte OPEN request captured 2026-09-07. */
 static const u8 c11[56] = {0,0,0,0,0x38,0,0,0,0x0e,0,2,0x80,0,0,0,0,1,0x10,0,0,2,0,0,0,0x12,0,0,0,'Z',0,':',0,'\\',0,'N',0,'V',0,'R',0,'A',0,'M',0,0,0,0x36,0,4,0,0,0,0,9,0,0};
 assert(!mt6797_fs_request_parse(c11, 56, &req) && req.op == 0x1001 && req.argc == 2 && req.index == 0);
 assert(req.len[0] == 18 && req.len[1] == 4 && mt6797_fs_read32(req.arg[1]) == 0x900);
 assert(!mt6797_fs_path_decode(req.arg[0], req.len[0], path, sizeof(path)) && !strcmp(path, "NVRAM"));
 /* C10 literal 68-byte GETATTRIBUTES request. */
 static const u8 c10[68] = {0,0,0,0,0x44,0,0,0,0x0e,0,1,0x80,0,0,0,0,0x10,0x10,0,0,1,0,0,0,0x26,0,0,0,'Z',0,':',0,'\\',0,'F',0,'A',0,'T',0,'C',0,'6',0,'2',0,'6',0,'1',0,'0',0,'C',0,'1',0,'.',0,'l',0,'o',0,'g',0,0,0,0,0};
 assert(!mt6797_fs_request_parse(c10, 68, &req) && req.op == 0x1010 && req.argc == 1 && req.len[0] == 38);
 assert(!mt6797_fs_path_decode(req.arg[0], req.len[0], path, sizeof(path)) && !strcmp(path, "FATC62610C1.log"));
 for (unsigned n = 0; n < 56; n++) assert(mt6797_fs_request_parse(c11, n, &req) < 0);
 { u8 in[56]; memcpy(in, c11, 56); in[0] = 1; assert(mt6797_fs_request_parse(in, 56, &req) == -EPROTO);
   memcpy(in, c11, 56); in[8] = 15; assert(mt6797_fs_request_parse(in, 56, &req) == -EPROTO);
   memcpy(in, c11, 56); in[12] = 5; assert(mt6797_fs_request_parse(in, 56, &req) == -EPROTO);
   memcpy(in, c11, 56); in[20] = 6; assert(mt6797_fs_request_parse(in, 56, &req) == -EPROTO);
   memcpy(in, c11, 56); in[24] = 0; assert(mt6797_fs_request_parse(in, 56, &req) == -EMSGSIZE);
   memcpy(in, c11, 56); in[24] = 40; assert(mt6797_fs_request_parse(in, 56, &req) == -EMSGSIZE);
   memcpy(in, c11, 56); in[20] = 1; assert(mt6797_fs_request_parse(in, 56, &req) == -EPROTO); }
 /* UTF-16 and path rules. */
 { static const u8 pair[] = {'a',0,0x3d,0xd8,0x00,0xde,0,0}; assert(!mt6797_fs_utf16_decode(pair, 8, path, 8) && !strcmp(path, "a\xf0\x9f\x98\x80"));
   static const u8 lone[] = {0x3d,0xd8,'a',0,0,0}; assert(mt6797_fs_utf16_decode(lone, 6, path, 8) == -EILSEQ);
   static const u8 low[] = {0x00,0xdc,0,0}; assert(mt6797_fs_utf16_decode(low, 4, path, 8) == -EILSEQ);
   static const u8 nul[] = {'a',0,0,0,'b',0,0,0}; assert(mt6797_fs_utf16_decode(nul, 8, path, 8) == -EILSEQ);
   static const u8 noterm[] = {'a',0,'b',0}; assert(mt6797_fs_utf16_decode(noterm, 4, path, 8) == -EILSEQ);
   assert(mt6797_fs_utf16_decode(noterm, 3, path, 8) == -EILSEQ);
   static const u8 ok[] = {'a',0,'b',0,0,0}; assert(mt6797_fs_utf16_decode(ok, 6, path, 2) == -ENAMETOOLONG && !mt6797_fs_utf16_decode(ok, 6, path, 3)); }
 { struct { const char *in; int ret; const char *out; } cases[] = {
   {"Z:\\", 0, ""}, {"Z:", 0, ""}, {"Z:\\a\\b", 0, "a/b"}, {"Z:/a//b/", 0, "a/b"}, {"Z:\\.\\a\\.", 0, "a"},
   {"Z:\\..\\a", -EINVAL, NULL}, {"Z:\\a\\..", -EINVAL, NULL}, {"X:\\a", -ENXIO, NULL}, {"z:\\a", -ENXIO, NULL}, {"a", -ENXIO, NULL}};
   for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) { u32 n = utf16(cases[i].in, buf);
     int ret = mt6797_fs_path_decode(buf, n, path, sizeof(path)); assert(ret == cases[i].ret); if (!ret) assert(!strcmp(path, cases[i].out)); }
   char full[300] = "Z:\\"; memset(full + 3, 'n', 256); full[259] = 0;
   u32 n = utf16(full, pathbuf); assert(mt6797_fs_path_decode(pathbuf, n, path, sizeof(path)) == -ENAMETOOLONG);
   assert(mt6797_fs_path_status(-ENXIO) == -19 && mt6797_fs_path_status(-EINVAL) == -16 && mt6797_fs_path_status(-ENAMETOOLONG) == -49 && mt6797_fs_path_status(-EILSEQ) == -3); }
 /* Vendor matcher, name encoder, attributes, DOS stamps, errno mapping. */
 assert(mt6797_fs_match("*", "any") && mt6797_fs_match("*.log", "a.log") && !mt6797_fs_match("*.log", "a.bin") && mt6797_fs_match("?", "a"));
 assert(!mt6797_fs_match("?", "ab") && mt6797_fs_match("a*b*c", "axxbyyc") && !mt6797_fs_match("a*b*c", "axxbyy") && mt6797_fs_match("", "") && !mt6797_fs_match("", "a"));
 assert(mt6797_fs_match("MD_*", "MD_A.bin") && !mt6797_fs_match("md_*", "MD_A.bin") && mt6797_fs_match("**", "x") && mt6797_fs_match("a*", "a"));
 { u32 n = mt6797_fs_name_encode("a/b", 10, buf, sizeof(buf), &fitted); assert(n == 8 && fitted && buf[0] == 'a' && buf[2] == '\\' && buf[4] == 'b' && !buf[6] && !buf[7]);
   n = mt6797_fs_name_encode("abcdef", 3, buf, sizeof(buf), &fitted); assert(n == 6 && !fitted && buf[0] == 'a' && buf[2] == 'b' && !buf[4]);
   n = mt6797_fs_name_encode("abc", 4, buf, sizeof(buf), &fitted); assert(n == 8 && fitted);
   n = mt6797_fs_name_encode("abc", 3, buf, sizeof(buf), &fitted); assert(n == 6 && !fitted);
   n = mt6797_fs_name_encode("abc", 1, buf, sizeof(buf), &fitted); assert(n == 0 && !fitted);
   n = mt6797_fs_name_encode("abc", 0, buf, sizeof(buf), &fitted); assert(n == 0 && !fitted);
   n = mt6797_fs_name_encode("x\xc3\xa9", 10, buf, sizeof(buf), &fitted); assert(n == 6 && buf[2] == 0xe9 && !buf[3]);
   n = mt6797_fs_name_encode("\xf0\x9f\x98\x80", 10, buf, sizeof(buf), &fitted); assert(n == 6 && buf[0] == 0x3d && buf[1] == 0xd8 && buf[2] == 0x00 && buf[3] == 0xde);
   n = mt6797_fs_name_encode("\xf0\x9f\x98\x80", 2, buf, sizeof(buf), &fitted); assert(n == 0 && !fitted);
   n = mt6797_fs_name_encode("\xff", 10, buf, sizeof(buf), &fitted); assert(n == 4 && buf[0] == 0xff && !buf[1]);
   n = mt6797_fs_name_encode("abc", 10, buf, 4, &fitted); assert(n == 4 && buf[0] == 'a'); }
 assert(mt6797_fs_attributes(0100644) == 0 && mt6797_fs_attributes(0100444) == 1 && mt6797_fs_attributes(0040755) == 0x10 && mt6797_fs_attributes(0040555) == 0x10 && mt6797_fs_attributes(0040444) == 0x11);
 for (unsigned m = 0; m < 0100000; m++) assert(mt6797_fs_attributes(m) == (u32)(((m & 219) == 0 && (m & 292) != 0) | ((m & 0170000) == 0040000 ? 16 : 0)));
 { struct tm tm = {.tm_sec = 33, .tm_min = 7, .tm_hour = 13, .tm_mday = 8, .tm_mon = 8, .tm_year = 126};
   assert(mt6797_fs_dos_time(&tm) == ((33 & 0x1f) | (7 << 5) | (13 << 11) | (8 << 16) | (8 << 21) | (46u << 25))); }
 assert(mt6797_fs_status(0) == 0 && mt6797_fs_status(-EMFILE) == -5 && mt6797_fs_status(-EACCES) == -16 && mt6797_fs_status(-ENOENT) == -9);
 assert(mt6797_fs_status(-EIO) == -18 && mt6797_fs_status(-EROFS) == -18 && mt6797_fs_status(-ENODEV) == -18 && mt6797_fs_status(-EISDIR) == -18);
 /* Reply framing: single frame literal and vendor 0xd80 fragmentation. */
 { struct mt6797_fs_reply r; u8 p[8192], f[3584]; mt6797_fs_reply_init(&r, p, sizeof(p), 4, 0x1010); assert(!mt6797_fs_reply_add_u32(&r, (u32)-9));
   assert(mt6797_fs_reply_frames(&r) == 1 && mt6797_fs_reply_frame(&r, 0, f, sizeof(f)) == 32);
   u8 expected[32] = {0}; expected[4] = 32; expected[8] = 15; expected[12] = 4; expected[16] = 0x10; expected[17] = 0x10; expected[18] = expected[19] = 0xff;
   expected[20] = 1; expected[24] = 4; expected[28] = 247; expected[29] = expected[30] = expected[31] = 0xff; assert(!memcmp(f, expected, 32));
   assert(mt6797_fs_reply_frame(&r, 1, f, sizeof(f)) == -EINVAL && mt6797_fs_reply_frame(&r, 0, f, 31) == -EMSGSIZE);
   mt6797_fs_reply_init(&r, p, sizeof(p), 2, 0x1003); u8 data[7000]; for (unsigned i = 0; i < sizeof(data); i++) data[i] = i;
   assert(!mt6797_fs_reply_add(&r, data, 0xd80 - 8) && mt6797_fs_reply_frames(&r) == 1 && mt6797_fs_reply_frame(&r, 0, f, sizeof(f)) == 0xd94);
   mt6797_fs_reply_init(&r, p, sizeof(p), 2, 0x1003); assert(!mt6797_fs_reply_add(&r, data, 0xd80 - 7) && r.len == 0xd80 + 4 && mt6797_fs_reply_frames(&r) == 2);
   assert(mt6797_fs_reply_frame(&r, 0, f, sizeof(f)) == 0xd94 && mt6797_fs_read32(f) == 0x80000000U && mt6797_fs_read32(f + 4) == 0xd94 && mt6797_fs_read32(f + 16) == 0xffff1003U);
   assert(!memcmp(f + 20, p, 0xd80) && mt6797_fs_reply_frame(&r, 1, f, sizeof(f)) == 24 && !mt6797_fs_read32(f) && mt6797_fs_read32(f + 12) == 2 && !memcmp(f + 20, p + 0xd80, 4));
   mt6797_fs_reply_init(&r, p, sizeof(p), 2, 0x1003); assert(!mt6797_fs_reply_add(&r, data, 7000) && mt6797_fs_reply_frames(&r) == 3);
   size_t total = 0; for (unsigned n = 0; n < 3; n++) { int len = mt6797_fs_reply_frame(&r, n, f, sizeof(f)); assert(len > 20 && !memcmp(f + 20, p + total, len - 20)); total += len - 20; assert((mt6797_fs_read32(f) >> 31) == (n < 2)); }
   assert(total == r.len); for (unsigned n = 0; n < 6; n++) assert(!mt6797_fs_reply_add_u32(&r, n) || n >= 4); mt6797_fs_reply_init(&r, p, 8, 0, 0); assert(mt6797_fs_reply_add_u32(&r, 1) == -EMSGSIZE); }
 /* GETDISKINFO through the service equals the C9/C10 literal 120-byte reply. */
 { static const u8 c8[44] = {0,0,0,0,44,0,0,0,14,0,0,128,0,0,0,0,14,16,0,0,2,0,0,0,8,0,0,0,'Z',0,':',0,'\\',0,0,0,4,0,0,0,3,0,0,0};
   memcpy(frame, c8, 44); reply = (struct mt6797_fs_reply){.payload = payload, .capacity = sizeof(payload)};
   assert(!mt6797_fs_service_request(&svc, frame, 44, &reply, &status) && !status && mt6797_fs_reply_frames(&reply) == 1);
   u8 f[3584]; assert(mt6797_fs_reply_frame(&reply, 0, f, sizeof(f)) == 120);
   u8 expected[120] = {0}; expected[4] = 120; expected[8] = 15; expected[16] = 14; expected[17] = 16; expected[18] = 255; expected[19] = 255;
   expected[20] = 2; expected[24] = 4; expected[32] = 84; expected[60] = 'Z'; expected[61] = 1; expected[85] = 2; expected[88] = 8; expected[92] = 0x39; expected[93] = 0x30;
   expected[100] = 0x85; expected[101] = 0x1a; assert(!memcmp(f, expected, 120));
   memcpy(frame, c8, 44); frame[28] = 'X'; assert(!mt6797_fs_service_request(&svc, frame, 44, &reply, &status) && status == -4);
   for (unsigned n = 0; n < 8; n++) { memcpy(frame, c8, 44); mt6797_fs_write32(frame + 40, n); assert(!mt6797_fs_service_request(&svc, frame, 44, &reply, &status)); assert(status == ((n > 0 && n < 4) ? 0 : -12)); }
   provider_available = 0; memcpy(frame, c8, 44); assert(!mt6797_fs_service_request(&svc, frame, 44, &reply, &status) && status == -1); provider_available = 1;
   memcpy(frame, c8, 44); frame[20] = 1; frame[4] = 36; assert(!mt6797_fs_service_request(&svc, frame, 36, &reply, &status) && status == -2); }
 /* GETATTRIBUTES: real modes, missing file, directory, drive and escape refusals. */
 assert(!run_path(MT6797_FS_GETATTRIBUTES, "Z:\\FATC62610C1.log", 0, false) && status == -9 && reply.argc == 1 && arg_s32(0) == -9);
 assert(!run_path(MT6797_FS_GETATTRIBUTES, "Z:\\ro.txt", 0, false) && status == 1);
 assert(!run_path(MT6797_FS_GETATTRIBUTES, "Z:\\NVRAM", 0, false) && status == 0x10);
 assert(!run_path(MT6797_FS_GETATTRIBUTES, "Z:\\NVRAM\\sub", 0, false) && status == 0x11);
 assert(!run_path(MT6797_FS_GETATTRIBUTES, "Z:\\NVRAM\\MD_A.bin", 0, false) && status == 0);
 assert(!run_path(MT6797_FS_GETATTRIBUTES, "Z:\\", 0, false) && status == 0x10);
 assert(!run_path(MT6797_FS_GETATTRIBUTES, "X:\\ro.txt", 0, false) && status == -19);
 assert(!run_path(MT6797_FS_GETATTRIBUTES, "Z:\\..\\ro.txt", 0, false) && status == -16);
 assert(!run_path(MT6797_FS_GETATTRIBUTES, "Z:\\NVRAM\\..\\ro.txt", 0, false) && status == -16);
 provider_available = 0; assert(!run_path(MT6797_FS_GETATTRIBUTES, "Z:\\ro.txt", 0, false) && status == -18); provider_available = 1;
 /* OPEN: the C11 frame, handle numbering, write intent, missing, drive. */
 memcpy(frame, c11, 56); reply = (struct mt6797_fs_reply){.payload = payload, .capacity = sizeof(payload)};
 assert(!mt6797_fs_service_request(&svc, frame, 56, &reply, &status) && status == 1 && reply.argc == 1 && arg_s32(0) == 1 && open_files == 1);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\NVRAM\\MD_A.bin", 0x100, true) && status == 2);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\NVRAM\\MD_A.bin", 0, true) && status == -18);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\NVRAM\\MD_A.bin", 0x10100, true) && status == -18);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\NVRAM\\MD_A.bin", 0x20100, true) && status == -18);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\missing.bin", 0x100, true) && status == -9);
 assert(!run_path(MT6797_FS_OPEN, "X:\\ro.txt", 0x100, true) && status == -19);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\..", 0x100, true) && status == -16);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\ro.txt", 0x100, false) && status == -2);
 assert(open_files == 2 && svc.open_count == 2);
 assert(!run_u32(MT6797_FS_CLOSE, 1, 1, 0, 0) && status == 0 && open_files == 1);
 assert(!run_u32(MT6797_FS_CLOSE, 1, 1, 0, 0) && status == -10);
 assert(!run_u32(MT6797_FS_CLOSE, 1, 0, 0, 0) && status == -10 && !run_u32(MT6797_FS_CLOSE, 1, 129, 0, 0) && status == -10);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\ro.txt", 0x100, true) && status == 1); /* lowest free slot again */
 /* GETFILESIZE, SEEK, READ (including EOF and vendor fragmentation), WRITE. */
 assert(!run_u32(MT6797_FS_GETFILESIZE, 1, 2, 0, 0) && status == 0 && reply.argc == 2 && arg_s32(1) == 10);
 assert(!run_u32(MT6797_FS_GETFILESIZE, 1, 7, 0, 0) && status == -10 && reply.argc == 2 && arg_s32(0) == -10 && arg_s32(1) == 0);
 assert(!run_u32(MT6797_FS_SEEK, 3, 2, 5, 0) && status == 5 && !run_u32(MT6797_FS_SEEK, 3, 2, 2, 1) && status == 7);
 assert(!run_u32(MT6797_FS_SEEK, 3, 2, (u32)-3, 2) && status == 7 && !run_u32(MT6797_FS_SEEK, 3, 2, 0, 3) && status == -2);
 assert(!run_u32(MT6797_FS_SEEK, 3, 9, 0, 0) && status == -10 && !run_u32(MT6797_FS_SEEK, 2, 2, 0, 0) && status == -2);
 assert(!run_u32(MT6797_FS_SEEK, 3, 2, (u32)-100, 0) && status == -18);
 assert(!run_u32(MT6797_FS_SEEK, 3, 2, 0, 0) && status == 0);
 assert(!run_u32(MT6797_FS_READ, 2, 2, 5, 0) && status == 0 && reply.argc == 3 && arg_s32(1) == 5 && arg(2, &d) == 5 && !memcmp(d, "alpha", 5));
 assert(!run_u32(MT6797_FS_READ, 2, 2, 100, 0) && status == 0 && arg_s32(1) == 5 && arg(2, &d) == 5 && !memcmp(d, "omega", 5));
 assert(!run_u32(MT6797_FS_READ, 2, 2, 100, 0) && status == 0 && arg_s32(1) == 0 && arg(2, &d) == 0 && reply.len == 24);
 assert(!run_u32(MT6797_FS_READ, 2, 9, 100, 0) && status == -10 && reply.argc == 3 && arg_s32(0) == -10 && arg_s32(1) == 0 && arg(2, &d) == 0);
 assert(!run_u32(MT6797_FS_READ, 2, 2, MT6797_FS_READ_MAX + 1, 0) && status == -2 && reply.argc == 3);
 assert(!run_u32(MT6797_FS_READ, 1, 2, 0, 0) && status == -2);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\big.bin", 0x100, true) && status == 3);
 assert(!run_u32(MT6797_FS_READ, 2, 3, 5000, 0) && status == 0 && arg_s32(1) == 5000 && arg(2, &d) == 5000 && !memcmp(d, big, 5000));
 assert(reply.len == 24 + 5000 && mt6797_fs_reply_frames(&reply) == 2);
 { u8 f[3584]; assert(mt6797_fs_reply_frame(&reply, 0, f, sizeof(f)) == 0xd94 && mt6797_fs_read32(f) == 0x80000000U && mt6797_fs_read32(f + 12) == 0);
   assert(mt6797_fs_reply_frame(&reply, 1, f, sizeof(f)) == 20 + 5024 - 0xd80 && !mt6797_fs_read32(f)); }
 assert(!run_u32(MT6797_FS_READ, 2, 3, MT6797_FS_READ_MAX, 0) && status == 0 && arg_s32(1) == 0);
 { u8 data[8] = "12345678"; u8 h[4], l[4]; mt6797_fs_write32(h, 2); mt6797_fs_write32(l, 8); const void *a[3] = {h, data, l}; u32 ln[3] = {4, 8, 4};
   assert(!run(0, MT6797_FS_WRITE, 3, a, ln) && status == -18 && reply.argc == 2 && arg_s32(1) == 0);
   mt6797_fs_write32(h, 9); assert(!run(0, MT6797_FS_WRITE, 3, a, ln) && status == -10);
   assert(!run(0, MT6797_FS_WRITE, 2, a, ln) && status == -2); }
 /* Search handles: pattern, attribute filter, name copy bound, exhaustion. */
 assert(!findfirst("Z:\\NVRAM\\*", 0, 0x10, 260) && status == 4 && reply.argc == 3 && open_files == 4);
 { u32 len = arg(1, &d); assert(len == 52 && d[11] == 0 && d[12] == 1 && mt6797_fs_read32(d + 28) == 10 && mt6797_fs_read32(d + 14) && mt6797_fs_read32(d + 22));
   for (unsigned i = 32; i < 52; i++) assert(!d[i]);
   assert(!mt6797_fs_read32(d + 18) && !d[26] && !d[27]); }
 check_name("MD_A.bin", 2);
 assert(!run_u32(MT6797_FS_FINDNEXT, 2, 4, 260, 0) && status == 0 && reply.argc == 3); check_name("MD_B.bin", 2);
 assert(!run_u32(MT6797_FS_FINDNEXT, 2, 4, 260, 0) && status == 0); check_name("x\xe9.txt", 2);
 { arg(1, &d); assert(d[11] == 1); }
 assert(!run_u32(MT6797_FS_FINDNEXT, 2, 4, 260, 0) && status == -6 && reply.argc == 3 && arg(1, &d) == 52 && arg(2, &d) == 0);
 assert(!run_u32(MT6797_FS_FINDNEXT, 2, 4, 260, 0) && status == -6);
 assert(!run_u32(MT6797_FS_FINDNEXT, 2, 2, 260, 0) && status == -10 && !run_u32(MT6797_FS_FINDNEXT, 1, 4, 0, 0) && status == -2);
 assert(!run_u32(MT6797_FS_FINDCLOSE, 1, 4, 0, 0) && status == 0 && open_files == 3 && !run_u32(MT6797_FS_FINDCLOSE, 1, 4, 0, 0) && status == -10);
 assert(!findfirst("Z:\\NVRAM\\*", 0x10, 0, 260) && status == 4); check_name("sub", 2); { arg(1, &d); assert(d[11] == 0x11); }
 assert(!run_u32(MT6797_FS_FINDNEXT, 2, 4, 260, 0) && status == -6 && !run_u32(MT6797_FS_CLOSE, 1, 4, 0, 0) && status == 0);
 assert(!findfirst("Z:\\NVRAM\\MD_?.bin", 0, 0, 4) && status == 4); check_name("MD_", 2); { arg(1, &d); assert(d[12] == 0); }
 assert(!run_u32(MT6797_FS_FINDNEXT, 2, 4, 1, 0) && status == 0 && arg(2, &d) == 0 && (arg(1, &d), d[12] == 0));
 assert(!run_u32(MT6797_FS_FINDCLOSE, 1, 4, 0, 0) && status == 0);
 assert(!findfirst("Z:\\*.log", 0, 0, 260) && status == -6 && open_files == 3 && svc.open_count == 3 && reply.argc == 3);
 assert(!findfirst("Z:\\nowhere\\*", 0, 0, 260) && status == -9 && open_files == 3);
 assert(!findfirst("Z:\\ro.txt\\*", 0, 0, 260) && status == -18);
 assert(!findfirst("Z:\\*", 0, 0, 260) && status == 4); check_name("NVRAM", 2);
 assert(!run_u32(MT6797_FS_FINDNEXT, 2, 4, 260, 0) && status == 0); check_name("ro.txt", 2);
 assert(!run_u32(MT6797_FS_FINDNEXT, 2, 4, 260, 0) && status == 0); check_name("big.bin", 2);
 assert(!run_u32(MT6797_FS_FINDNEXT, 2, 4, 260, 0) && status == -6);
 assert(!findfirst("X:\\*", 0, 0, 260) && status == -19 && !findfirst("Z:\\..\\*", 0, 0, 260) && status == -16);
 { u32 n = utf16("Z:\\*", pathbuf); u8 z = 0; const void *a[3] = {pathbuf, &z, &z}; u32 l[3] = {n, 1, 1}; assert(!run(1, MT6797_FS_FINDFIRST, 3, a, l) && status == -2); }
 /* Remaining answered operations and refusals. */
 { u32 n = utf16("Z:\\NVRAM\\MD_B.bin", pathbuf); u8 fl[4], hint[8] = {1,2,3,4,5,6,7,8}; mt6797_fs_write32(fl, 0x100); const void *a[3] = {pathbuf, fl, hint}; u32 l[3] = {n, 4, 8};
   assert(!run(3, MT6797_FS_OPENHINT, 3, a, l) && status == 5 && reply.argc == 2 && arg(1, &d) == 8 && !memcmp(d, hint, 8) && reply.index == 3);
   /* Missing/short hints must never be echoed from absent argument slots
    * or read beyond their declared payload. Rejection has no side effects. */
   unsigned before = svc.open_count;
   for (unsigned argc = 0; argc < 3; argc++)
     assert(run(3, MT6797_FS_OPENHINT, argc, a, l) == -EINVAL && status == 12345);
   for (unsigned bytes = 1; bytes < 8; bytes++) {
     l[2] = bytes;
     assert(run(3, MT6797_FS_OPENHINT, 3, a, l) == -EINVAL && status == 12345);
   }
   assert(svc.open_count == before); }
 assert(!run_u32(MT6797_FS_LOCKFAT, 0, 0, 0, 0) && status == 0 && reply.argc == 1 && !run_u32(MT6797_FS_CLEARDISKFLAG, 1, 7, 0, 0) && status == 0);
 assert(!run_u32(MT6797_FS_SETDISKFLAG, 1, 7, 0, 0) && status == 0 && !run_u32(MT6797_FS_UNLOCKALL, 0, 0, 0, 0) && status == 1 && arg_s32(0) == 1);
 assert(!run_u32(MT6797_FS_GETCLUSTERSIZE, 1, 'Z', 0, 0) && status == 4096 && !run_u32(MT6797_FS_GETCLUSTERSIZE, 1, 'A', 0, 0) && status == -4);
 provider_available = 0; assert(!run_u32(MT6797_FS_GETCLUSTERSIZE, 1, 'Z', 0, 0) && status == -18); provider_available = 1;
 { u32 writes[] = {MT6797_FS_CREATEDIR, MT6797_FS_REMOVEDIR, MT6797_FS_DELETE, MT6797_FS_RESTORE, MT6797_FS_XDELETE};
   for (unsigned i = 0; i < 5; i++) { assert(!run_path(writes[i], "Z:\\NVRAM\\new", 4, i == 4) && status == -18 && reply.argc == 1);
     assert(!run_path(writes[i], "X:\\new", 4, i == 4) && status == -19); assert(!run_path(writes[i], "Z:\\..\\new", 4, i == 4) && status == -16); }
   u32 n = utf16("Z:\\a", pathbuf); u8 second[64]; u32 m = utf16("Z:\\b", second); u8 fl[4] = {0}; const void *a[3] = {pathbuf, second, fl}; u32 l[3] = {n, m, 4};
   assert(!run(0, MT6797_FS_RENAME, 2, a, l) && status == -18 && !run(0, MT6797_FS_MOVE, 3, a, l) && status == -18 && !run(0, MT6797_FS_RENAME, 1, a, l) && status == -2);
   u32 x = utf16("X:\\b", second); l[1] = x; assert(!run(0, MT6797_FS_RENAME, 2, a, l) && status == -19); }
 { u32 unsupported[] = {MT6797_FS_GETFOLDERSIZE, MT6797_FS_COUNT, MT6797_FS_GETDRIVE, 0x101d, 0x101e, 0x101f, 0x1020, 0x1022, 0x1023, 0x1000, 0x1024, 0};
   for (unsigned i = 0; i < sizeof(unsupported) / sizeof(*unsupported); i++) { assert(!mt6797_fs_service_supports(unsupported[i]));
     assert(run_u32(unsupported[i], 1, 0, 0, 0) == -EOPNOTSUPP && status == 12345); } }
 assert(mt6797_fs_service_supports(0x1001) && mt6797_fs_service_supports(0x1021));
 { reply = (struct mt6797_fs_reply){.payload = payload, .capacity = 100}; size_t n = build(0, MT6797_FS_LOCKFAT, 0, NULL, NULL); assert(mt6797_fs_service_request(&svc, frame, n, &reply, &status) == -EMSGSIZE); }
 /* Handle exhaustion at the vendor's 128 usable slots, then CLOSEALL/SHUTDOWN. */
 while (svc.open_count < 128) { assert(!run_path(MT6797_FS_OPEN, "Z:\\ro.txt", 0x100, true) && status > 0 && status < 129); }
 assert(!run_path(MT6797_FS_OPEN, "Z:\\ro.txt", 0x100, true) && status == -5 && !findfirst("Z:\\*", 0, 0, 260) && status == -5 && open_files == 128);
 assert(!run_u32(MT6797_FS_CLOSEALL, 0, 0, 0, 0) && status == 0 && reply.argc == 1 && !open_files && !svc.open_count);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\ro.txt", 0x100, true) && status == 1 && !findfirst("Z:\\*", 0, 0, 260) && status == 2);
 assert(!run_u32(MT6797_FS_SHUTDOWN, 0, 0, 0, 0) && status == 0 && reply.argc == 0 && reply.len == 4 && !open_files);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\ro.txt", 0x100, true) && status == 1); mt6797_fs_service_release(&svc); assert(!open_files && !svc.open_count);
 assert(allocations == frees);
 puts("PASS actual FS service: C8/C10/C11 literal frames, every answered vendor op, error mapping, UTF-16/path/pattern rules, handles 1..128, 0xd80 fragmentation, read-only refusals, leak-free");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-fs-service-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(shim + provider + codec + fake + service + tests)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
