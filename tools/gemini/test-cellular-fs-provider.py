#!/usr/bin/env python3
"""Execute the actual FS codec/state helpers; no modem or filesystem access."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
rpc = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_rpc.h').read_text()
# Only shared byte accessors: do not duplicate the wire encoding in the test.
rpc = rpc[rpc.index('static u32 mt6797_rpc_read('):rpc.index('static int mt6797_rpc_eint_reply(')]
state = (root / 'drivers/soc/mediatek/mt6797-md1-fs-state.h').read_text()
codec = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_fs.h').read_text()
shim = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64;
#define U32_MAX UINT32_MAX
#define EPROBE_DEFER 517
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#include <time.h>
struct kstatfs {int64_t f_bsize; uint64_t f_blocks,f_bfree; int64_t f_frsize;};
struct mt6797_md1_fs_stats {u32 block_size,blocks,free_blocks;};
'''
tests = r'''
/* Literal disk-info reply built exactly as the service does: status then info. */
static int disk_reply(const u8 *data,size_t len,const struct mt6797_md1_fs_stats *stats,u8 *out,size_t capacity,s32 *status){
 struct mt6797_fs_request request;struct mt6797_fs_reply reply;u8 info[84],payload[128];int ret;
 if(!data||!out||!status||capacity<120)return -EMSGSIZE;
 ret=mt6797_fs_request_parse(data,len,&request);if(ret)return ret;
 if(request.op!=0x100e)return -EOPNOTSUPP;
 *status=mt6797_fs_diskinfo(&request,stats,info);
 mt6797_fs_reply_init(&reply,payload,sizeof(payload),request.index,request.op);
 assert(!mt6797_fs_reply_add_u32(&reply,(u32)*status)&&!mt6797_fs_reply_add(&reply,info,84));
 assert(mt6797_fs_reply_frames(&reply)==1&&mt6797_fs_reply_frame(&reply,0,out,capacity)==120);
 return 0;}
static const u8 request[44]={
 0,0,0,0,44,0,0,0,14,0,0,128,0,0,0,0,14,16,0,0,2,0,0,0,
 8,0,0,0,'Z',0,':',0,'\\',0,0,0,4,0,0,0,3,0,0,0};
static struct mt6797_fs_state shared;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t barrier;
static int claimed,cancelled;
static void *claim_thread(void *unused) {
 (void)unused;pthread_barrier_wait(&barrier);pthread_mutex_lock(&lock);
 claimed=mt6797_fs_claim(&shared);pthread_mutex_unlock(&lock);return NULL;
}
static void *cancel_thread(void *unused) {
 (void)unused;pthread_barrier_wait(&barrier);pthread_mutex_lock(&lock);
 cancelled=mt6797_fs_cancel(&shared);pthread_mutex_unlock(&lock);return NULL;
}
int main(void) {
 struct mt6797_md1_fs_stats stats={4096,12345,6789}, converted;
 u8 in[48],out[120],expected[120]={0};s32 status;
 assert(!disk_reply(request,44,&stats,out,120,&status)&&status==0);
 /* Literal vendor layout: two args, status4 + FS_DiskInfo84. Real free blocks,
  * not the historical daemon's fixed512 value. No unspecified stack bytes.
  */
 expected[4]=120;expected[8]=15;expected[16]=14;expected[17]=16;expected[18]=255;expected[19]=255;
 expected[20]=2;expected[24]=4;expected[32]=84;expected[60]='Z';expected[61]=1;
 expected[85]=2;expected[88]=8;expected[92]=0x39;expected[93]=0x30;
 expected[100]=0x85;expected[101]=0x1a;
 assert(!memcmp(out,expected,120));
 for(unsigned n=0;n<44;n++)assert(disk_reply(request,n,&stats,out,120,&status)<0);
 for(unsigned n=0;n<120;n++)assert(disk_reply(request,44,&stats,out,n,&status)==-EMSGSIZE);
 const unsigned offsets[]={0,4,8,12,20,24};
 for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++){
  memcpy(in,request,44);mt6797_rpc_write(in+offsets[i],UINT32_MAX);
  assert(disk_reply(in,44,&stats,out,120,&status)<0);
 }
 memcpy(in,request,44);mt6797_rpc_write(in+16,0x1001);
 assert(disk_reply(in,44,&stats,out,120,&status)==-EOPNOTSUPP);
 memcpy(in,request,44);mt6797_rpc_write(in+36,2);
 assert(!disk_reply(in,44,&stats,out,120,&status)&&status==-2);
 memcpy(in,request,44);in[28]='X';
 assert(!disk_reply(in,44,&stats,out,120,&status)&&status==-4);
 for(unsigned n=0;n<8;n++){
  memcpy(in,request,44);mt6797_rpc_write(in+40,n);
  assert(!disk_reply(in,44,&stats,out,120,&status));
  assert(status==((n>0&&n<4)?0:-12));
  if(!status){assert(mt6797_rpc_read(out+92)==((n&1)?12345:0));
   assert(mt6797_rpc_read(out+100)==((n&2)?6789:0));}
 }
 assert(!disk_reply(request,44,NULL,out,120,&status)&&status==-1);
 assert(mt6797_rpc_read(out+28)==UINT32_MAX);
 for(unsigned i=36;i<120;i++)assert(out[i]==0);
 for(unsigned i=0;i<5;i++){memcpy(in,request,44);mt6797_rpc_write(in+12,i);
  assert(!disk_reply(in,44,&stats,out,120,&status));assert(mt6797_rpc_read(out+12)==i);}
 struct kstatfs fs={4096,12345,6789,4096};
 assert(!mt6797_fs_stats_convert(&fs,&converted)&&!memcmp(&stats,&converted,sizeof(stats)));
 const struct kstatfs bad[]={
  {0,1,0,0},{-1,1,0,0},{511,1,0,0},{513,1,0,0},{4294967296LL,1,0,0},
  {4096,0,0,0},{4096,4294967296ULL,0,0},{4096,1,2,0},{4096,1,0,512}};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
  converted=stats;assert(mt6797_fs_stats_convert(&bad[i],&converted)==-EOVERFLOW);
  assert(!memcmp(&stats,&converted,sizeof(stats)));
 }
 struct mt6797_fs_state s={0};assert(mt6797_fs_require(&s)==-EPROBE_DEFER);
 assert(!mt6797_fs_begin(&s));assert(mt6797_fs_begin(&s)==-EALREADY);
 assert(mt6797_fs_claim(&s)==-EPROBE_DEFER);assert(!mt6797_fs_publish(&s));
 assert(!mt6797_fs_claim(&s));assert(mt6797_fs_reprobe(&s)==-ECANCELED);
 assert(mt6797_fs_cancel(&s)==-EBUSY&&mt6797_fs_claim(&s)==-EALREADY);
 /* Both legitimate publication races: no attach after consumed/cancelled. */
 for(unsigned stage=MT6797_FS_EMPTY;stage<=MT6797_FS_CANCELLED;stage++){
  s=(struct mt6797_fs_state){.stage=stage};
  assert(mt6797_fs_cancel(&s)==(stage==MT6797_FS_CONSUMED?-EBUSY:0));
  assert(mt6797_fs_begin(&s)==-EALREADY);
 }
 s=(struct mt6797_fs_state){.stage=MT6797_FS_READY};
 assert(!mt6797_fs_reprobe(&s)&&mt6797_fs_reprobe(&s)==-ECANCELED);
 assert(!mt6797_fs_cancel(&s)&&mt6797_fs_claim(&s)==-EALREADY);
 for(unsigned i=0;i<256;i++){
  pthread_t a,b;shared=(struct mt6797_fs_state){.stage=MT6797_FS_READY};
  assert(!pthread_barrier_init(&barrier,NULL,2));
  assert(!pthread_create(&a,NULL,claim_thread,NULL));assert(!pthread_create(&b,NULL,cancel_thread,NULL));
  assert(!pthread_join(a,NULL)&&!pthread_join(b,NULL));assert(!pthread_barrier_destroy(&barrier));
  assert((!claimed&&cancelled==-EBUSY&&shared.stage==MT6797_FS_CONSUMED)||
         (claimed==-EALREADY&&!cancelled&&shared.stage==MT6797_FS_CANCELLED));
 }
 puts("PASS actual FS disk-info codec/state: literal44/120-byte contract, real stats, flags/errors, malformed bounds, single authority, 256 claim/cancel races");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-fs-provider-') as d:
    p=Path(d)
    (p/'test.c').write_text(shim+rpc+state+codec+tests)
    # -Wno-unused-function: the shared codec header carries every FS helper.
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    '-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
