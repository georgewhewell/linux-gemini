#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the actual HS1 worker and bounded PBM-disabled DBM initializer."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--old-source", type=Path)
args = parser.parse_args()
source = (args.old_source or root / "drivers/net/wwan/mediatek/mtk_mt6797_ccci.c").read_text()
header = (root / "drivers/net/wwan/mediatek/mtk_ccci_common.h").read_text()


def body(name):
    match = re.search(r"^static (?:int|void) " + name + r"\([^;]*?\n\{", source, re.M)
    if not match:
        raise ValueError(name)
    return source[match.start():source.index("\n}\n", match.start()) + 3]


defines = "\n".join(re.findall(r"^#define MTK_CCCI_RUNTIME_(?:OFFSET|AP_SIZE)\s+[^\n]+", header, re.M))
helper = ""
if not args.old_source:
    defines += "\n" + "\n".join(re.findall(r"^#define MT6797_CCCI_DBM_(?:SIZE|OFFSET)\s+[^\n]+", source, re.M))
    helper = body("mt6797_ccci_init_dbm")
    defines += "\n#define HAVE_DBM_HELPER 1\n"

shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint32_t __le32;
typedef uint64_t resource_size_t;
#define U32_MAX UINT32_MAX
#define cpu_to_le32(x) ((uint32_t)(x))
#define GFP_KERNEL 0
#define MTK_CCCI_RUNTIME_V2_REPLY_SIZE 116U
#define MTK_CCCI_CONTROL_TX 1U
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define xchg(p,v) ({ __auto_type old=*(p); *(p)=(v); old; })
#define atomic_read(p) (*(p))
#define atomic_set(p,v) (*(p)=(v))
#define dev_info(...) ((void)0)
struct work_struct { unsigned unused; };
struct sk_buff { size_t len; u8 data[116]; };
struct mtk_ccci_frame { u32 data0,data1,channel,reserved; };
struct mt6797_md1_hs_frame { size_t wire_size; u32 data0,data1,channel,reserved; };
enum mt6797_md1_hs_action { MT6797_MD1_HS_ACTION_HS1_V2_NEEDS_RUNTIME=1 };
struct mt6797_ccci { struct work_struct hs1_work; struct sk_buff *hs1_skb;
 int failed,teardown,hs1_pending,hs,diag,runtime_config; u8 *runtime,*smem;
 resource_size_t smem_size; void *dev; };
enum fault { NONE, NO_SKB, INITIAL_FATAL, HEADER, ALLOC, RUNTIME, HS, ACTION,
 STAGE2, FATAL_AFTER_STAGE2, REMOVE_AFTER_STAGE2, SHORT_SMEM, NULL_SMEM, TX };
static enum fault fault;
static struct mt6797_ccci owner;
static u8 memory[0x10800], runtime[2048];
static unsigned live_skb, stage2_calls, writes, runtime_writes, barriers, tx_calls, fatals;
static int fatal_error;
static struct sk_buff *alloc_skb(size_t size,int flags)
{ (void)flags; assert(size<=116); if(fault==ALLOC)return NULL;
 struct sk_buff *s=calloc(1,sizeof(*s));assert(s);live_skb++;return s; }
static void skb_put(struct sk_buff *s,size_t n){assert(s->len+n<=sizeof(s->data));s->len+=n;}
static void kfree_skb(struct sk_buff *s){if(s){assert(live_skb);live_skb--;free(s);}}
static int mtk_ccci_header_decode(const void *p,size_t size,struct mtk_ccci_frame *f)
{ assert(p&&size==88);if(fault==HEADER)return -EINVAL;
 *f=(struct mtk_ccci_frame){.reserved=0x5555ffff};return 0; }
static int mtk_ccci_runtime_v2_reply(const void *p,size_t size,const void *config,
 unsigned sequence,void *data,size_t capacity,size_t *len,void *reply,size_t reply_size,size_t *reply_len)
{ (void)config;assert(p&&size==88&&!sequence&&capacity==2048&&reply_size==116);
 if(fault==RUNTIME)return -EOPNOTSUPP;
 memset(data,0x5a,16);memset(reply,0x6b,reply_size);*len=16;*reply_len=reply_size;return 0; }
static int mt6797_md1_hs_rx(int *hs,const struct mt6797_md1_hs_frame *f,enum mt6797_md1_hs_action *action)
{ (void)hs;assert(f->wire_size==88&&f->reserved==0x5555ffff);
 if(fault==HS)return -EPROTO;
 *action=fault==ACTION?0:MT6797_MD1_HS_ACTION_HS1_V2_NEEDS_RUNTIME;return 0; }
static int mt6797_md1_cold_owner_hs1_stage2(void)
{ stage2_calls++;if(fault==FATAL_AFTER_STAGE2)owner.failed=1;
 if(fault==REMOVE_AFTER_STAGE2)owner.teardown=1;
 return fault==STAGE2?-EIO:0; }
static void memcpy_toio(void *destination,const void *data,size_t size)
{ assert(owner.smem);size_t at=(u8 *)destination-owner.smem;
 assert(at<=owner.smem_size&&size<=owner.smem_size-at);
 assert(at+size<=sizeof(memory));writes++;
 if(at==0x10000){assert(stage2_calls==1);runtime_writes++;}
 memcpy(destination,data,size); }
static void wmb(void){barriers++;}
static bool dbm_correct(void)
{ for(size_t i=0;i<56;i++){
   u8 expected=(i<8||i>=48)?0x44:0xff;
   if(memory[0xffc8+i]!=expected)return false;
 }return true; }
static void mt6797_ccci_diag_payload(int *d,const void *a,size_t na,const void *b,size_t nb,const void *c,size_t nc)
{ (void)d;assert(a&&na==88&&b&&nb==116&&c&&nc==16); }
static int mt6797_ccci_tx(struct mt6797_ccci *c,unsigned q,struct sk_buff *s,unsigned channel)
{ assert(c==&owner&&!q&&channel==1&&s->len==116);tx_calls++;
 if(!dbm_correct()){fputs("DBM_NOT_INITIALIZED_BEFORE_RESPONSE\n",stderr);exit(1);}
 assert(stage2_calls==1&&writes==2&&runtime_writes==1&&barriers==1);
 if(fault==TX)return -EIO;
 kfree_skb(s);return 0; }
static void mt6797_ccci_quiesce(struct mt6797_ccci *c,int error)
{ assert(c==&owner&&error<0);fatals++;fatal_error=error;c->failed=1; }
'''

tests = r'''
static void reset(enum fault next)
{
 assert(!live_skb);fault=next;memset(&owner,0,sizeof(owner));
 memset(memory,0xa5,sizeof(memory));memset(runtime,0xa5,sizeof(runtime));
 owner.smem=memory;owner.smem_size=sizeof(memory);owner.runtime=runtime;owner.hs1_pending=1;
 writes=runtime_writes=barriers=tx_calls=fatals=stage2_calls=0;fatal_error=0;
 if(next!=NO_SKB){owner.hs1_skb=calloc(1,sizeof(*owner.hs1_skb));
  assert(owner.hs1_skb);owner.hs1_skb->len=88;live_skb++;}
 if(next==INITIAL_FATAL)owner.failed=1;
 if(next==SHORT_SMEM)owner.smem_size=0xffff;
 if(next==NULL_SMEM)owner.smem=NULL;
}
static void unaffected(bool changed)
{
 for(size_t i=0;i<sizeof(memory);i++){
  bool dbm=i>=0xffc8&&i<0x10000,rt=i>=0x10000&&i<0x10010;
  if(!changed||(!dbm&&!rt))assert(memory[i]==0xa5);
  if(changed&&rt)assert(memory[i]==0x5a);
 }
}
int main(void)
{
 reset(NONE);mt6797_ccci_hs1_work(&owner.hs1_work);
 assert(!live_skb&&!owner.hs1_skb&&!owner.hs1_pending&&!fatals&&tx_calls==1);
 assert(dbm_correct());unaffected(true);
 const struct {enum fault fault;int error;unsigned stage2;} cases[]={
  {NO_SKB,-ENODATA,0},{INITIAL_FATAL,-ESHUTDOWN,0},{HEADER,-EINVAL,0},
  {ALLOC,-ENOMEM,0},{RUNTIME,-EOPNOTSUPP,0},{HS,-EPROTO,0},{ACTION,-EPROTO,0},
  {STAGE2,-EIO,1},{FATAL_AFTER_STAGE2,-ESHUTDOWN,1},{REMOVE_AFTER_STAGE2,-ESHUTDOWN,1},
  {SHORT_SMEM,-ENOSPC,1},{NULL_SMEM,-ENODEV,1},
 };
 for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  reset(cases[i].fault);mt6797_ccci_hs1_work(&owner.hs1_work);
  assert(!live_skb&&!owner.hs1_skb&&!owner.hs1_pending&&fatals==1);
  assert(fatal_error==cases[i].error&&stage2_calls==cases[i].stage2);
  assert(!writes&&!barriers&&!tx_calls);unaffected(false);
 }
 reset(TX);mt6797_ccci_hs1_work(&owner.hs1_work);
 assert(!live_skb&&!owner.hs1_pending&&fatals==1&&fatal_error==-EIO);
 assert(tx_calls==1&&dbm_correct());unaffected(true);
#ifdef HAVE_DBM_HELPER
 reset(NO_SKB);owner.smem_size=0x10000;
 assert(!mt6797_ccci_init_dbm(&owner));assert(writes==1&&dbm_correct());
 for(size_t i=0;i<sizeof(memory);i++)if(i<0xffc8||i>=0x10000)assert(memory[i]==0xa5);
 assert(!mt6797_ccci_init_dbm(&owner));assert(writes==2&&dbm_correct());
 const resource_size_t short_sizes[]={0,0xffc8,0xffff};
 for(size_t i=0;i<sizeof(short_sizes)/sizeof(short_sizes[0]);i++){
  reset(NO_SKB);owner.smem_size=short_sizes[i];
  assert(mt6797_ccci_init_dbm(&owner)==-ENOSPC&&!writes);unaffected(false);
 }
 reset(NO_SKB);owner.smem=NULL;
 assert(mt6797_ccci_init_dbm(&owner)==-ENODEV&&!writes);unaffected(false);
 reset(NO_SKB);owner.failed=1;
 assert(mt6797_ccci_init_dbm(&owner)==-ESHUTDOWN&&!writes);unaffected(false);
 reset(NO_SKB);owner.teardown=1;
 assert(mt6797_ccci_init_dbm(&owner)==-ESHUTDOWN&&!writes);unaffected(false);
 reset(NO_SKB);owner.smem_size=UINT64_MAX;
 assert(!mt6797_ccci_init_dbm(&owner)&&writes==1&&dbm_correct());
#endif
 puts("PASS actual HS1/DBM: exact 56-byte disabled-PBM contract before runtime TX, surrounding bytes untouched, bound/terminal guards, allocation/build/HS/stage2/TX failures and skb ownership");
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="gemini-ccci-dbm-") as temp:
    path = Path(temp)
    (path / "test.c").write_text(defines + "\n" + shim + helper + body("mt6797_ccci_hs1_work") + tests)
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
    command = shlex.split(os.environ.get("CC", "cc")) + flags + shlex.split(os.environ.get("HOSTCFLAGS", ""))
    subprocess.run(command + [str(path / "test.c"), "-o", str(path / "test")], check=True)
    print("COMPILED actual HS1 worker" + (" (original source)" if args.old_source else " and DBM helper"), flush=True)
    subprocess.run([str(path / "test")], check=True, timeout=5)
