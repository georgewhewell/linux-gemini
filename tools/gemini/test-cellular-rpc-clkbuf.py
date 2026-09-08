#!/usr/bin/env python3
"""Execute the production RF-clock codec with a counted, failing provider."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
test = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
#include "mtk_mt6797_ccci_rpc_clkbuf.h"
static unsigned calls;
static int failure, invalid;
static u32 received;
static int provide(void *context, u32 afc, struct mt6797_rpc_clkbuf_values *v)
{
 assert(context == &calls); calls++; received=afc;
 /* Deliberately distinct valid values exercise every output field. */
 *v=(struct mt6797_rpc_clkbuf_values){{2,0,1,2},{1,0,1,0},{0,1,2,3}};
 if(invalid==1) v->configuration[3]=3;
 if(invalid==2) v->software[3]=2;
 if(invalid==3) v->driving[3]=4;
 return failure;
}
/* Vendor packed UMOLY input and four-byte argument alignment, synthetic AFC. */
static const u8 request[36]={
 0x78,0x56,0x34,0x12,36,0,0,0,32,0,0x34,0x80,1,0,0,0,
 8,64,0,0,1,0,0,0,6,0,0,0,4,0,0xef,0xcd,0xab,0x89,0xaa,0xbb};
static const u8 expected[56]={
 0x78,0x56,0x34,0x12,56,0,0,0,33,0,0,0,1,0,0,0,
 8,64,255,255,2,0,0,0,4,0,0,0,0,0,0,0,18,0,0,0,
 4,0,2,0,1,2,1,0,1,0,0,0,1,0,2,0,3,0,0,0};
static u8 reply[64]; static size_t size; static s32 status;
static int run(const u8 *b,size_t n,size_t cap)
{return mt6797_rpc_clkbuf_reply(b,n,provide,&calls,reply,cap,&size,&status);}
int main(void)
{
 u8 b[80]; unsigned before;
 /* Reference the existing EINT codec too: keep strict unused warnings enabled. */
 u32 value; assert(mt6797_rpc_eint_value(0,1,&value));
 assert(mt6797_rpc_eint_reply(NULL,0,NULL,NULL,0,NULL)==-EMSGSIZE);
 memset(reply,0xa5,sizeof(reply));
 assert(!run(request,36,56)); assert(calls==1 && received==0x89abcdef);
 assert(size==56 && status==0 && !memcmp(reply,expected,56));
 for(unsigned i=56;i<64;i++) assert(reply[i]==0xa5);
 /* Every truncation, including lengths adjusted to match the shorter frame. */
 before=calls;
 for(unsigned n=0;n<36;n++) {
  memcpy(b,request,36); assert(run(b,n,56)<0);
  mt6797_rpc_write(b+4,n); assert(run(b,n,56)<0);
 }
 for(unsigned cap=0;cap<56;cap++) assert(run(request,36,cap)==-EMSGSIZE);
 assert(calls==before);
 const unsigned offsets[]={4,8,12,16,20,24};
 for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
  memcpy(b,request,36); mt6797_rpc_write(b+offsets[i],UINT32_MAX);
  assert(run(b,36,56)<0); assert(calls==before);
 }
 /* Well-framed but semantically wrong argument shape => two parameter errors. */
 for(unsigned n=5;n<=8;n++) if(n!=6) {
  memcpy(b,request,36);mt6797_rpc_write(b+24,n);
  assert(!run(b,36,56));assert(status==-2 && size==40 && calls==before);
  assert((s32)mt6797_rpc_read(reply+36)==-2);
 }
 memcpy(b,request,36);mt6797_rpc_write(b+20,0);mt6797_rpc_write(b+4,24);
 assert(!run(b,24,56));assert(status==-2 && calls==before);
 /* Extra frame bytes and overflowing argument lengths never reach provider. */
 memcpy(b,request,36);mt6797_rpc_write(b+4,40);
 assert(run(b,40,56)==-EPROTO && calls==before);
 for(unsigned n=2049;n<2053;n++) {
  memcpy(b,request,36);mt6797_rpc_write(b+24,n);
  assert(run(b,36,56)==-EMSGSIZE && calls==before);
 }
 /* Invalid count follows the vendor sentinel response without AFC effects. */
 for(unsigned n=0;n<65536;n++) if(n!=4) {
  memcpy(b,request,36);b[28]=n;b[29]=n>>8;
  assert(!run(b,36,56));assert(status==0 && size==56 && calls==before);
  assert(reply[36]==255 && reply[37]==0);
  for(unsigned i=38;i<56;i++)assert(reply[i]==0);
 }
 assert(mt6797_rpc_clkbuf_reply(request,36,NULL,NULL,reply,56,&size,&status)==-EOPNOTSUPP);
 assert(calls==before);
 failure=-EIO;assert(!run(request,36,56));
 assert(calls==before+1 && size==40 && status==-5);
 assert((s32)mt6797_rpc_read(reply+36)==-5);failure=0;
 for(invalid=1;invalid<=3;invalid++) {
  assert(!run(request,36,56));assert(size==40 && status==-5);
 }
 invalid=0;
 for(unsigned i=0;i<2;i++) {
  memcpy(b,request,36);mt6797_rpc_write(b+12,i);
  assert(!run(b,36,56));assert(mt6797_rpc_read(reply+12)==i);
 }

 /* Vendor's absent optional NFC operation returns one FS_NO_OP argument. */
 const u8 no_nfc[32]={0x78,0x56,0x34,0x12,32,0,0,0,32,0,0,128,1,0,0,0,
  10,64,0,0,1,0,0,0,1,0,0,0,1,0xaa,0xbb,0xcc};
 const u8 no_op_reply[32]={0x78,0x56,0x34,0x12,32,0,0,0,33,0,0,0,1,0,0,0,
  10,64,255,255,1,0,0,0,4,0,0,0,255,255,255,255};
 before=calls;memset(reply,0xa5,sizeof(reply));
 assert(!mt6797_rpc_unsupported_reply(no_nfc,32,reply,32,&size,&status));
 assert(size==32&&status==-1&&!memcmp(reply,no_op_reply,32)&&calls==before);
 for(unsigned i=32;i<64;i++)assert(reply[i]==0xa5);
 for(unsigned n=0;n<32;n++) {
  memcpy(b,no_nfc,32);assert(mt6797_rpc_unsupported_reply(b,n,reply,32,&size,&status)<0);
  mt6797_rpc_write(b+4,n);assert(mt6797_rpc_unsupported_reply(b,n,reply,32,&size,&status)<0);
  assert(mt6797_rpc_unsupported_reply(no_nfc,32,reply,n,&size,&status)==-EMSGSIZE);
 }
 for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
  memcpy(b,no_nfc,32);mt6797_rpc_write(b+offsets[i],UINT32_MAX);
  assert(mt6797_rpc_unsupported_reply(b,32,reply,32,&size,&status)<0);
 }
 memcpy(b,no_nfc,32);mt6797_rpc_write(b+16,0xffff400a);
 assert(mt6797_rpc_unsupported_reply(b,32,reply,32,&size,&status)==-EPROTO);
 memcpy(b,no_nfc,32);mt6797_rpc_write(b+4,36);
 assert(mt6797_rpc_unsupported_reply(b,36,reply,32,&size,&status)==-EPROTO);
 /* Unknown operations with zero or six well-framed arguments still get a reply. */
 for(unsigned n=0;n<=6;n++) {
  memset(b,0,sizeof(b));memcpy(b,no_nfc,24);mt6797_rpc_write(b+4,24+n*8);mt6797_rpc_write(b+20,n);
  for(unsigned i=0;i<n;i++)mt6797_rpc_write(b+24+i*8,4);
  assert(!mt6797_rpc_unsupported_reply(b,24+n*8,reply,32,&size,&status));
  assert(size==32&&status==-1&&calls==before);
 }
 puts("PASS unsupported RPC: literal NFC-disabled FS_NO_OP, one argument, no provider call, truncation/overflow/response-ID/padding/slot bounds");
 puts("PASS RF clock RPC: literal wire, packed AFC, provider effects only after bounds validation, all count sentinels, failure/invalid state, padding, slot identity");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-rpc-clkbuf-') as directory:
    p = Path(directory)
    (p / 'test.c').write_text(test)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-I' + str(root / 'drivers/net/wwan/mediatek'),
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
