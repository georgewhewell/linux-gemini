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
struct file { int node; loff_t f_pos; int cursor; bool directory, writable; };
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
 {"x/", 0040755, NULL, 0, -1},
 {"x/common", 0100444, "protect-f", 9, 8},
 {"y/", 0040755, NULL, 0, -1},
 {"y/common", 0100444, "protect-s-long", 14, 10},
 {"x/new", 0100644, NULL, 0, 8},
 {"y/new", 0100644, NULL, 0, 10},
};
#define NODES ((int)(sizeof(nodes) / sizeof(nodes[0])))
static int open_files, provider_available = 1, fstat_error, read_error;
static int find(const char *path) { for (int i = 0; i < NODES; i++) if (!strcmp(nodes[i].path, path)) return i; return -1; }
static void fill(int n, struct mt6797_md1_fs_attr *a) { a->mode = nodes[n].mode; a->size = nodes[n].size; a->atime = 1757260800 + n; a->mtime = 1757347200 + n; }
static int mt6797_md1_fs_provider_getattr(const char *path, struct mt6797_md1_fs_attr *a) {
 int n; assert(path && path[0] != '/' && !strstr(path, "..")); if (!provider_available) return -ENODEV;
 n = find(path); if (n < 0) return -ENOENT; fill(n, a); return 0; }
static struct file *mt6797_md1_fs_provider_open(const char *path, bool directory) {
 int n = find(path); struct file *f; if (!provider_available) return ERR_PTR(-ENODEV); if (n < 0) return ERR_PTR(-ENOENT);
 if (directory != S_ISDIR(nodes[n].mode)) return ERR_PTR(directory ? -ENOTDIR : -EISDIR);
 f = calloc(1, sizeof(*f)); f->node = n; f->directory = directory; open_files++; return f; }

static bool ram_enabled;static int ram_create_count,ram_truncate_count,root_creates[2];static char ram_data[64],root_data[2][64];
static struct file *mt6797_md1_fs_provider_open_ram(const char *path,bool writable,bool create,bool truncate){
 if(!ram_enabled)return ERR_PTR(-EROFS);
 int index=!strcmp(path,"x/new")?0:!strcmp(path,"y/new")?1:-1;
 if(index>=0){
  if(create)root_creates[index]++;
  if(truncate)nodes[12+index].size=0;
  nodes[12+index].data=root_data[index];struct file *f=mt6797_md1_fs_provider_open(path,false);f->writable=writable;return f;
 }
 assert(!strcmp(path,"big.bin"));if(create)ram_create_count++;
 if(truncate){nodes[7].size=0;ram_truncate_count++;}
 nodes[7].data=ram_data;struct file *f=mt6797_md1_fs_provider_open(path,false);f->writable=writable;return f;}
static ssize_t mt6797_md1_fs_provider_write(struct file *f,const void *data,size_t count,loff_t *pos){
 if(!ram_enabled||!f->writable)return -EROFS;
 assert(pos==&f->f_pos);
 if(f->node>=12){
  assert(f->node<=13&&*pos<64);if(count>(size_t)(64-*pos))count=64-*pos;
  memcpy(root_data[f->node-12]+*pos,data,count);*pos+=count;if((u64)*pos>nodes[f->node].size)nodes[f->node].size=*pos;return count;
 }
 assert(f->node==7);if(*pos>=64)return -ENOSPC;
 if(count>(size_t)(64-*pos))count=64-*pos;
 memcpy(ram_data+*pos,data,count);*pos+=count;if((u64)*pos>nodes[7].size)nodes[7].size=*pos;return count;}
static int mt6797_md1_fs_provider_fstat(struct file *f, struct mt6797_md1_fs_attr *a) { if (fstat_error) return fstat_error; fill(f->node, a); return 0; }
static ssize_t mt6797_md1_fs_provider_read(struct file *f, void *buf, size_t count, loff_t *pos) {
 if (read_error) return read_error;
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
static bool roots_enabled;
static int drive_path(u8 drive,const char *path,char *mapped){
 if(drive=='Z'){strcpy(mapped,path);return 0;}
 if(!roots_enabled||(drive!='X'&&drive!='Y'))return -ENODEV;
 snprintf(mapped,2048,"%c/%s",drive=='X'?'x':'y',path);return 0;
}
static int mt6797_md1_fs_provider_getattr_drive(u8 drive,const char *path,struct mt6797_md1_fs_attr *a){
 char mapped[2048];int ret=drive_path(drive,path,mapped);return ret?ret:mt6797_md1_fs_provider_getattr(mapped,a);
}
static struct file *mt6797_md1_fs_provider_open_drive(u8 drive,const char *path,bool directory){
 char mapped[2048];int ret=drive_path(drive,path,mapped);return ret?ERR_PTR(ret):mt6797_md1_fs_provider_open(mapped,directory);
}
static struct file *mt6797_md1_fs_provider_open_ram_drive(u8 drive,const char *path,bool writable,bool create,bool truncate){
 char mapped[2048];int ret=drive_path(drive,path,mapped);return ret?ERR_PTR(ret):mt6797_md1_fs_provider_open_ram(mapped,writable,create,truncate);
}
static int mt6797_md1_fs_provider_stats_drive(u8 drive,struct mt6797_md1_fs_stats *s){
 if(drive=='Z')return mt6797_md1_fs_provider_stats(s);
 if(!provider_available||!roots_enabled||(drive!='X'&&drive!='Y'))return -ENODEV;
 *s=(struct mt6797_md1_fs_stats){4096,drive=='X'?2048:3192,drive=='X'?700:900};return 0;
}
static int mutation_calls, mutation_error;
static u8 mutation_drive;
static enum mt6797_md1_fs_mutation mutation_op;
static char mutation_path[1025];
static int mt6797_md1_fs_provider_mutate_drive(u8 drive,const char *path,enum mt6797_md1_fs_mutation op){
 if(!ram_enabled)return -EROFS;
 mutation_calls++;mutation_drive=drive;mutation_op=op;strcpy(mutation_path,path);
 return mutation_error;
}

static int rename_calls, rename_error;
static u8 rename_old_drive, rename_new_drive;
static char rename_old_path[1025], rename_new_path[1025];
static int mt6797_md1_fs_provider_rename_drive(u8 old_drive,const char *old_path,u8 new_drive,const char *new_path){
 if(!ram_enabled)return -EROFS;
 rename_calls++;rename_old_drive=old_drive;rename_new_drive=new_drive;
 strcpy(rename_old_path,old_path);strcpy(rename_new_path,new_path);return rename_error;
}

static int xdelete_calls,xdelete_result;
static u32 xdelete_flags;
static u8 xdelete_drive;
static char xdelete_path[1025];
static int mt6797_md1_fs_provider_xdelete_drive(u8 drive,const char *path,u32 flags){
 if(!ram_enabled)return -EROFS;
 xdelete_calls++;xdelete_flags=flags;xdelete_drive=drive;strcpy(xdelete_path,path);return xdelete_result;
}

static int copy_calls,copy_error,copy_after_mutation,copy_after_xdelete;
static u8 copy_old_drive,copy_new_drive;
static char copy_old_path[1025],copy_new_path[1025];
static int mt6797_md1_fs_provider_copy_drive(u8 old_drive,const char *old_path,u8 new_drive,const char *new_path){
 if(!ram_enabled)return -EROFS;
 copy_calls++;copy_old_drive=old_drive;copy_new_drive=new_drive;
 strcpy(copy_old_path,old_path);strcpy(copy_new_path,new_path);
 copy_after_mutation=mutation_calls;copy_after_xdelete=xdelete_calls;
 return copy_error;
}

static int restore_calls,restore_result=-ENOENT;
static u8 restore_drive;
static char restore_path[1025];
static int mt6797_md1_fs_provider_restore_drive(u8 drive,const char *path){
 restore_calls++;restore_drive=drive;strcpy(restore_path,path);return restore_result;
}

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
 { u8 data[8] = {'1','2','3','4','5','6','7','8'}; u8 h[4], l[4]; mt6797_fs_write32(h, 2); mt6797_fs_write32(l, 8); const void *a[3] = {h, data, l}; u32 ln[3] = {4, 8, 4};
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
   u32 n = utf16("Z:\\a", pathbuf); u8 second[64]; u32 m = utf16("Z:\\b", second); u8 fl[4] = {1}; const void *a[3] = {pathbuf, second, fl}; u32 l[3] = {n, m, 4};
   assert(!run(0, MT6797_FS_RENAME, 2, a, l) && status == -18 && !run(0, MT6797_FS_MOVE, 3, a, l) && status == -18 && !run(0, MT6797_FS_RENAME, 1, a, l) && status == -2);
   u32 x = utf16("X:\\b", second); l[1] = x; assert(!run(0, MT6797_FS_RENAME, 2, a, l) && status == -19); }

 { u32 ops[] = {MT6797_FS_CREATEDIR, MT6797_FS_REMOVEDIR, MT6797_FS_DELETE};
   int errors[] = {0, -ENOENT, -EACCES, -EEXIST, -ENOTEMPTY, -ENOTDIR, -EISDIR, -EROFS, -ENOSPC};
   int expected[] = {0, -9, -16, -18, -18, -18, -18, -18, -18};
   ram_enabled=true; roots_enabled=true;
   for(unsigned i=0;i<3;i++) for(unsigned d=0;d<3;d++) {
     char name[]="Z:\\NVRAM\\new"; name[0]="ZXY"[d];
     for(unsigned e=0;e<sizeof(errors)/sizeof(*errors);e++) {
       mutation_error=errors[e];int before=mutation_calls;
       assert(!run_path(ops[i],name,0,0)&&status==expected[e]&&reply.argc==1&&arg_s32(0)==expected[e]);
       assert(mutation_calls==before+1&&mutation_drive==name[0]&&(unsigned)mutation_op==i&&!strcmp(mutation_path,"NVRAM/new"));
     }
     int before=mutation_calls;
     assert(!run_path(ops[i],name,0,1)&&status==-2&&mutation_calls==before);
     assert(!run_u32(ops[i],0,0,0,0)&&status==-2&&mutation_calls==before);
     assert(!run_path(ops[i],"Z:\\..\\escape",0,0)&&status==-16&&mutation_calls==before);
   }
   ram_enabled=false; roots_enabled=false; mutation_error=0;
   puts("PASS actual namespace service: Z/X/Y routing, exact argument counts, confinement refusals, provider failures and signed replies");
 }

 { u8 second[128]; const void *args[3]={pathbuf,second,second};u32 lens[3];
   ram_enabled=roots_enabled=true;
   int errors[]={0,-ENOENT,-EACCES,-EXDEV,-ENOTEMPTY,-EINVAL,-ENOSPC};
   int expected[]={0,-9,-16,-18,-18,-18,-18};
   for(unsigned d=0;d<3;d++)for(unsigned e=0;e<3;e++){
     char old[]="Z:\\old",new[]="Z:\\new";old[0]="ZXY"[d];new[0]="ZXY"[e];
     lens[0]=utf16(old,pathbuf);lens[1]=utf16(new,second);lens[2]=lens[1];
     for(unsigned i=0;i<sizeof(errors)/sizeof(*errors);i++){
       rename_error=errors[i];int before=rename_calls;
       assert(!run(0,MT6797_FS_RENAME,2,args,lens)&&status==expected[i]&&reply.argc==1&&arg_s32(0)==expected[i]);
       assert(rename_calls==before+1&&rename_old_drive==old[0]&&rename_new_drive==new[0]);
       assert(!strcmp(rename_old_path,"old")&&!strcmp(rename_new_path,"new"));
     }
     int before=rename_calls;
     assert(!run(0,MT6797_FS_RENAME,1,args,lens)&&status==-2&&rename_calls==before);
     assert(!run(0,MT6797_FS_RENAME,3,args,lens)&&status==-2&&rename_calls==before);
     lens[1]=utf16("Z:\\..\\bad",second);
     assert(!run(0,MT6797_FS_RENAME,2,args,lens)&&status==-16&&rename_calls==before);
   }
   rename_error=0;ram_enabled=roots_enabled=false;
   puts("PASS actual RENAME service: two independent paths, all drive pairs, argument/path refusal, errno and signed reply");
 }

 { ram_enabled=roots_enabled=true;
   for(u32 flags=4;flags<=28;flags+=4)for(unsigned d=0;d<3;d++){
     char name[]="Z:\\tree";name[0]="ZXY"[d];xdelete_result=7;int before=xdelete_calls;
     assert(!run_path(MT6797_FS_XDELETE,name,flags,1)&&status==7&&arg_s32(0)==7&&reply.argc==1);
     assert(xdelete_calls==before+1&&xdelete_flags==flags&&xdelete_drive==name[0]&&!strcmp(xdelete_path,"tree"));
   }
   u32 invalid[]={0,1,2,3,32,0x1d,0x1001c,0xffffffff};
   for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){
     int before=xdelete_calls;assert(!run_path(MT6797_FS_XDELETE,"Z:\\tree",invalid[i],1)&&status==-2&&xdelete_calls==before);
   }
   int errors[]={-ENOENT,-EACCES,-ENOTDIR,-ENOTEMPTY,-E2BIG};int results[]={-9,-16,-2,-18,-18};
   for(unsigned i=0;i<sizeof(errors)/sizeof(*errors);i++){xdelete_result=errors[i];assert(!run_path(MT6797_FS_XDELETE,"Z:\\tree",28,1)&&status==results[i]);}
   int before=xdelete_calls;
   assert(!run_path(MT6797_FS_XDELETE,"Z:\\tree",0,0)&&status==-2&&xdelete_calls==before);
   assert(!run_path(MT6797_FS_XDELETE,"Z:\\..\\escape",28,1)&&status==-16&&xdelete_calls==before);
   ram_enabled=roots_enabled=false;
   puts("PASS actual XDELETE service: seven flag combinations, all drives, deletion count, errors and malformed request refusal");
 }

 { ram_enabled=roots_enabled=true;mutation_error=0;xdelete_result=7;
   u8 second[1024],fl[4];const void *args[]={pathbuf,second,fl};u32 lens[]={0,0,4};
   u32 valid[]={1,2,3,0x10001,0x10002,0x10003};
   for(unsigned d=0;d<3;d++)for(unsigned e=0;e<3;e++)for(unsigned i=0;i<6;i++){
     char old[]="Z:\\missing-source",new[]="Z:\\missing-destination";old[0]="ZXY"[d];new[0]="ZXY"[e];
     lens[0]=utf16(old,pathbuf);lens[1]=utf16(new,second);mt6797_fs_write32(fl,valid[i]);
     int before=copy_calls,deleted=mutation_calls+ xdelete_calls;copy_error=0;
     assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==((valid[i]&1)?0:-18)&&reply.argc==1&&arg_s32(0)==status);
     assert(copy_calls==before+!!(valid[i]&1)&&mutation_calls+xdelete_calls==deleted);
     if(valid[i]&1)assert(copy_old_drive==old[0]&&copy_new_drive==new[0]&&!strcmp(copy_old_path,"missing-source")&&!strcmp(copy_new_path,"missing-destination"));
   }
   lens[0]=utf16("Z:\\big.bin",pathbuf);lens[1]=utf16("Z:\\NVRAM\\MD_A.bin",second);
   for(unsigned i=0;i<6;i++){
     mt6797_fs_write32(fl,valid[i]);int before=copy_calls,deleted=mutation_calls;
     assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==((valid[i]&1)||(valid[i]&0x10000)?0:-18));
     assert(copy_calls==before+!!(valid[i]&1)&&mutation_calls==deleted+!!(valid[i]&0x10000));
     if(valid[i]&0x10000)assert(mutation_drive=='Z'&&mutation_op==MT6797_MD1_FS_UNLINK&&!strcmp(mutation_path,"NVRAM/MD_A.bin"));
     if(valid[i]&1)assert(copy_after_mutation==mutation_calls);
   }
   lens[1]=utf16("Z:\\NVRAM",second);
   for(unsigned i=3;i<6;i++){
     mt6797_fs_write32(fl,valid[i]);int before=xdelete_calls;
     assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==((valid[i]&1)?0:7)&&arg_s32(0)==status);
     assert(xdelete_calls==before+1&&xdelete_drive=='Z'&&xdelete_flags==28&&!strcmp(xdelete_path,"NVRAM"));
     if(valid[i]&1)assert(copy_after_xdelete==xdelete_calls);
   }
   int before=copy_calls,deleted=mutation_calls+xdelete_calls;
   /* Source and destination read-only checks return the installed -45. */
   lens[0]=utf16("Z:\\ro.txt",pathbuf);lens[1]=utf16("Z:\\missing-destination",second);mt6797_fs_write32(fl,3);
   assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-45);
   lens[0]=utf16("Z:\\big.bin",pathbuf);lens[1]=utf16("Z:\\ro.txt",second);mt6797_fs_write32(fl,0x10001);
   assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-45);
   const char *old[]={"Z:\\NVRAM\\MD_A.bin","Z:\\NVRAM\\.\\MD_A.bin","Z:\\big.bin","Z:\\big.bin"};
   const char *new[]={"Z:\\NVRAM","Z:/NVRAM//MD_A.bin","Z:\\","X:\\"};
   for(unsigned i=0;i<4;i++){
     lens[0]=utf16(old[i],pathbuf);lens[1]=utf16(new[i],second);
     assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-2);
   }
   assert(copy_calls==before&&mutation_calls+xdelete_calls==deleted);
   lens[0]=utf16("Z:\\big.bin",pathbuf);lens[1]=utf16("Z:\\NVRAM",second);
   xdelete_result=-ENOSPC;assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-18&&copy_calls==before);xdelete_result=7;
   lens[1]=utf16("Z:\\NVRAM\\MD_A.bin",second);mutation_error=-EACCES;
   assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-16&&copy_calls==before);mutation_error=0;
   /* Copy failure follows successful deletion and is never hidden. */
   copy_error=-ENOENT;assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-9&&copy_calls==before+1&&copy_after_mutation==mutation_calls);copy_error=0;
   before=copy_calls;deleted=mutation_calls+xdelete_calls;
   /* Metadata failures stop before effects, unlike the stock log-and-continue. */
   provider_available=false;mt6797_fs_write32(fl,3);
   assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-18);
   mt6797_fs_write32(fl,0x10001);
   assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-18);provider_available=true;
   u32 invalid[]={0,4,0x10000,0x10004,0x20001,0xffffffff};
   for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){
     mt6797_fs_write32(fl,invalid[i]);assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-2);
   }
   mt6797_fs_write32(fl,1);
   assert(!run(0,MT6797_FS_MOVE,2,args,lens)&&status==-2);
   lens[2]=3;assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-2);lens[2]=4;
   lens[1]=utf16("Z:\\..\\bad",second);assert(!run(0,MT6797_FS_MOVE,3,args,lens)&&status==-16);
   assert(copy_calls==before&&mutation_calls+xdelete_calls==deleted);
   ram_enabled=roots_enabled=false;
   puts("PASS actual MOVE service: six flags/all drive pairs, KILL no source unlink, ordered overwrite/count, read-only -45, ancestor/root/alias guards, malformed input and effect failures");
 }
 { roots_enabled=true;
   int errors[]={0,-ENOENT,-EACCES,-EIO,-ENOSPC,-ENODEV};
   for(unsigned d=0;d<3;d++)for(unsigned e=0;e<6;e++){
     char name[]="Z:\\restore";name[0]="ZXY"[d];restore_result=errors[e];int before=restore_calls;
     assert(!run_path(MT6797_FS_RESTORE,name,0,0)&&status==(errors[e]?-18:0)&&reply.argc==1&&arg_s32(0)==status);
     assert(restore_calls==before+1&&restore_drive==name[0]&&!strcmp(restore_path,"restore"));
   }
   int before=restore_calls;
   assert(!run_path(MT6797_FS_RESTORE,"Z:\\restore",1,1)&&status==-2&&restore_calls==before);
   roots_enabled=false;
   puts("PASS RESTORE service: three drives, exact one argument, boolean vendor error mapping and signed reply");
 }
 { u32 unsupported[] = {MT6797_FS_GETFOLDERSIZE, MT6797_FS_COUNT, MT6797_FS_GETDRIVE, 0x101d, 0x101e, 0x101f, 0x1020, 0x1023, 0x1000, 0x1024, 0};
   for (unsigned i = 0; i < sizeof(unsupported) / sizeof(*unsupported); i++) { assert(!mt6797_fs_service_supports(unsupported[i]));
     assert(run_u32(unsupported[i], 1, 0, 0, 0) == -EOPNOTSUPP && status == 12345); } }
 assert(mt6797_fs_service_supports(0x1001) && mt6797_fs_service_supports(0x1021));
 { reply = (struct mt6797_fs_reply){.payload = payload, .capacity = 100}; size_t n = build(0, MT6797_FS_LOCKFAT, 0, NULL, NULL); assert(mt6797_fs_service_request(&svc, frame, n, &reply, &status) == -EMSGSIZE); }

 /* Captured C12 0x1022: composite read control includes modem pointers,
  * which must be ignored. Exercise offset, EOF, fragmentation and unwind. */
 { u8 control[40]; memset(control, 0xa5, sizeof(control));
   u32 n = utf16("Z:\\NVRAM\\MD_A.bin", pathbuf);
   const void *a[2] = {pathbuf, control}; u32 l[2] = {n, 40};
   unsigned before = svc.open_count; int files = open_files;
   mt6797_fs_write32(control, 31); mt6797_fs_write32(control + 12, 0x700);
   mt6797_fs_write32(control + 20, 5); mt6797_fs_write32(control + 24, 1);
   mt6797_fs_write32(control + 32, 36);
   assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == 0 && reply.argc == 4);
   assert(arg(0, &d) == 8 && mt6797_fs_read32(d) == 31 && !mt6797_fs_read32(d + 4));
   assert(arg_s32(1) == 10 && arg_s32(2) == 5 && arg(3, &d) == 5 && !memcmp(d, "omega", 5));
   assert(svc.open_count == before && open_files == files);
   mt6797_fs_write32(control + 20, 100);
   assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == 0 && arg_s32(2) == 0 && arg(3, &d) == 0);
   mt6797_fs_write32(control + 20, 0); mt6797_fs_write32(control + 24, 3);
   assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == -3);
   assert(arg(0, &d) == 8 && mt6797_fs_read32(d) == 3 && (s32)mt6797_fs_read32(d + 4) == -2);
   mt6797_fs_write32(control + 24, 0); mt6797_fs_write32(control + 12, 0x20000);
   assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == -1);
   assert(arg(0, &d) == 8 && !mt6797_fs_read32(d) && (s32)mt6797_fs_read32(d + 4) == -18);
   mt6797_fs_write32(control + 12, 0x100);
   l[0] = utf16("Z:\\missing", pathbuf);
   assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == -1);
   assert(arg(0, &d) == 8 && !mt6797_fs_read32(d) && (s32)mt6797_fs_read32(d + 4) == -9);
   l[0] = utf16("Z:\\big.bin", pathbuf); mt6797_fs_write32(control + 32, 5000);
   assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == 0 && arg_s32(1) == 5000);
   assert(arg(3, &d) == 5000 && !memcmp(d, big, 5000) && mt6797_fs_reply_frames(&reply) == 2);
   fstat_error = -EIO;
   assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == -2);
   assert(arg(0, &d) == 8 && mt6797_fs_read32(d) == 1 && (s32)mt6797_fs_read32(d + 4) == -18);
   fstat_error = 0; read_error = -EIO;
   assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == -4 && arg_s32(2) == 0);
   assert(arg(0, &d) == 8 && mt6797_fs_read32(d) == 7 && (s32)mt6797_fs_read32(d + 4) == -18);
   read_error = 0;
   mt6797_fs_write32(control, 0);
   assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == -7 && arg_s32(2) == 0);
   mt6797_fs_write32(control, 15);
   assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == -6);
   assert(svc.open_count == before && open_files == files);
   for (unsigned bytes = 1; bytes < 40; bytes++) {
     l[1] = bytes; assert(run(0, MT6797_FS_COMPACT_READ, 2, a, l) == -EINVAL);
   }
   l[1] = 40;
   assert(run(0, MT6797_FS_COMPACT_READ, 0, a, l) == -EINVAL);
   assert(run(0, MT6797_FS_COMPACT_READ, 1, a, l) == -EINVAL);
   mt6797_fs_write32(control + 32, MT6797_FS_READ_MAX + 1);
   assert(run(0, MT6797_FS_COMPACT_READ, 2, a, l) == -EINVAL);
   /* C14 captured flags0x13: size-only; unused read length is16384.
    * Even maximal unused length must cause no allocation/read or rejection. */
   mt6797_fs_write32(control, 0x13);
   for (unsigned i=0; i<2; i++) {
     mt6797_fs_write32(control + 32, i ? 0xffffffffU : 16384);
     assert(!run(0, MT6797_FS_COMPACT_READ, 2, a, l) && status == 0);
     assert(arg_s32(1) == 5000 && arg_s32(2) == 0 && arg(3, &d) == 0);
     assert(arg(0, &d) == 8 && mt6797_fs_read32(d) == 0x13 && !mt6797_fs_read32(d + 4));
   }
   assert(svc.open_count == before && open_files == files);
 }
 /* Handle exhaustion at the vendor's 128 usable slots, then CLOSEALL/SHUTDOWN. */

 /* Distinct X/Y fixture roots exercise actual service routing, compact OPEN,
  * returned volume identity/capacity, and directory handles without Z aliases. */
 roots_enabled=true;
 { const char *paths[]={"X:\\common","Y:\\common"};
   const char *data[]={"protect-f","protect-s-long"};
   for(int i=0;i<2;i++){
    unsigned before=svc.open_count;
    assert(!run_path(MT6797_FS_GETATTRIBUTES,paths[i],0,false)&&status==1);
    assert(!run_path(MT6797_FS_OPEN,paths[i],0x100,true)&&status>0);
    u32 handle=status;
    assert(!run_u32(MT6797_FS_READ,2,handle,30,0)&&status==0);
    assert(arg(2,&d)==strlen(data[i])&&!memcmp(d,data[i],strlen(data[i])));
    assert(!run_u32(MT6797_FS_CLOSE,1,handle,0,0)&&status==0&&svc.open_count==before);
    u8 control[40]={0};mt6797_fs_write32(control,31);mt6797_fs_write32(control+12,0x700);mt6797_fs_write32(control+32,206);
    u32 n=utf16(paths[i],pathbuf);const void *a[]={pathbuf,control};u32 l[]={n,40};
    assert(!run(0,MT6797_FS_COMPACT_READ,2,a,l)&&status==0);
    assert(arg_s32(1)==(s32)strlen(data[i])&&arg_s32(2)==(s32)strlen(data[i]));
    assert(arg(3,&d)==strlen(data[i])&&!memcmp(d,data[i],strlen(data[i]))&&svc.open_count==before);
    char root[]={i?'Y':'X',':','\\',0};
    assert(!run_path(MT6797_FS_GETDISKINFO,root,3,true)&&status==0);
    assert(arg(1,&d)==84&&d[24]==root[0]&&mt6797_fs_read32(d+56)==(i?3192:2048));
    assert(mt6797_fs_read32(d+64)==(i?900:700));
    assert(!run_u32(MT6797_FS_GETCLUSTERSIZE,1,root[0],0,0)&&status==4096);
    char pattern[]={root[0],':','\\','*',0};
    assert(!findfirst(pattern,0,0,260)&&status>0);handle=status;
    assert(!run_u32(MT6797_FS_FINDCLOSE,1,handle,0,0)&&status==0&&svc.open_count==before);
   }
   assert(!run_path(MT6797_FS_OPEN,"Z:\\common",0x100,true)&&status==-9);
   assert(!run_path(MT6797_FS_OPEN,"X:\\..\\common",0x100,true)&&status==-16);
   assert(!run_path(MT6797_FS_OPEN,"W:\\common",0x100,true)&&status==-19);
 }

 ram_enabled=true;
 { u32 handles[2];
   assert(!run_path(MT6797_FS_OPEN,"X:\\new",0x10400,true)&&status>0);handles[0]=status;
   assert(!run_path(MT6797_FS_OPEN,"Y:\\new",0x10400,true)&&status>0);handles[1]=status;
   assert(root_creates[0]==1&&root_creates[1]==1&&handles[0]!=handles[1]);
   for(int i=0;i<2;i++){
    u8 h[4],count[4];const char *content=i?"ydata":"xdata";
    mt6797_fs_write32(h,handles[i]);mt6797_fs_write32(count,5);
    const void *a[]={h,content,count};u32 l[]={4,5,4};
    assert(!run(0,MT6797_FS_WRITE,3,a,l)&&status==0&&arg_s32(1)==5);
   }
   for(int i=0;i<2;i++){
    assert(!run_u32(MT6797_FS_SEEK,3,handles[i],0,0)&&status==0);
    assert(!run_u32(MT6797_FS_READ,2,handles[i],5,0)&&status==0);
    assert(arg(2,&d)==5&&!memcmp(d,i?"ydata":"xdata",5));
    assert(!run_u32(MT6797_FS_CLOSE,1,handles[i],0,0)&&status==0);
   }
 }
 ram_enabled=false;
 roots_enabled=false;
 while (svc.open_count < 128) { assert(!run_path(MT6797_FS_OPEN, "Z:\\ro.txt", 0x100, true) && status > 0 && status < 129); }
 assert(!run_path(MT6797_FS_OPEN, "Z:\\ro.txt", 0x100, true) && status == -5 && !findfirst("Z:\\*", 0, 0, 260) && status == -5 && open_files == 128);
 assert(!run_u32(MT6797_FS_CLOSEALL, 0, 0, 0, 0) && status == 0 && reply.argc == 1 && !open_files && !svc.open_count);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\ro.txt", 0x100, true) && status == 1 && !findfirst("Z:\\*", 0, 0, 260) && status == 2);
 assert(!run_u32(MT6797_FS_SHUTDOWN, 0, 0, 0, 0) && status == 0 && reply.argc == 0 && reply.len == 4 && !open_files);
 assert(!run_path(MT6797_FS_OPEN, "Z:\\ro.txt", 0x100, true) && status == 1); mt6797_fs_service_release(&svc); assert(!open_files && !svc.open_count);

 /* Captured C17 OPEN flag words, with real service-level write/count/readback. */
 ram_enabled=true;nodes[7].size=0;
 assert(!run_path(MT6797_FS_OPEN,"Z:\\big.bin",0x20010400,true)&&status==1&&ram_create_count==1);
 {u8 h[4],length[4];mt6797_fs_write32(h,1);mt6797_fs_write32(length,5);
  const void *args[3]={h,"hello",length};u32 lens[3]={4,5,4};
  assert(!run(0,MT6797_FS_WRITE,3,args,lens)&&status==0&&reply.argc==2&&arg_s32(1)==5);
  mt6797_fs_write32(length,6);assert(!run(0,MT6797_FS_WRITE,3,args,lens)&&status==-2&&arg_s32(1)==0);
  assert(!run_u32(MT6797_FS_SEEK,3,1,0,0)&&status==0);
  assert(!run_u32(MT6797_FS_READ,2,1,5,0)&&status==0&&arg_s32(1)==5);
  assert(arg(2,&d)==5&&!memcmp(d,"hello",5));
  assert(!run_u32(MT6797_FS_SEEK,3,1,62,0)&&status==62);
  mt6797_fs_write32(length,5);assert(!run(0,MT6797_FS_WRITE,3,args,lens)&&status==0&&arg_s32(1)==2);
  assert(!run(0,MT6797_FS_WRITE,3,args,lens)&&status==-18&&arg_s32(1)==0);
 }
 assert(!run_u32(MT6797_FS_CLOSE,1,1,0,0)&&status==0&&!open_files);
 assert(!run_path(MT6797_FS_OPEN,"Z:\\big.bin",0x10400,true)&&status==1&&ram_create_count==2);
 assert(!run_u32(MT6797_FS_CLOSE,1,1,0,0)&&status==0);
 assert(!run_path(MT6797_FS_OPEN,"Z:\\big.bin",0x20000,true)&&status==1&&ram_truncate_count==1&&nodes[7].size==0);
 mt6797_fs_service_release(&svc);assert(!open_files&&!svc.open_count);ram_enabled=false;
 puts("PASS actual Z/X/Y routing: distinct compact reads, metadata, search, disk identity/capacity, simultaneous create/write/readback handles, absent and traversal refusals");
 puts("PASS actual RAM service: captured CREATE flags, write count, malformed length, seek/readback, partial/full writes, truncate and handle cleanup");
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
