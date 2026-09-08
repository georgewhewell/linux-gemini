#!/usr/bin/env python3
"""Execute actual dispatch/RPC/TX/STOP code with DMA/MMIO ownership adapters."""
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text()
common=(root/'drivers/net/wwan/mediatek/mtk_ccci_common.c').read_text()
def function(text,name):
    start=text.index(name); start=text.rfind('\n',0,start)+1
    brace=text.index('{',start); depth=1; end=brace+1
    while depth:
        depth += (text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]+'\n'
dispatch=function(source,'mt6797_ccci_dispatch_rx(')
dispatch=dispatch[:dispatch.index('\tif_id = mtk_ccci_net_rx_link')]+ '\tassert(!"post-ready outside fixture");\n'+dispatch[dispatch.index('drop:\n'):]
# Only declarations used exclusively by the deliberately excluded network tail.
for declaration in ('\tstruct mt6797_ccci_link *link;\n', '\t__be16 protocol;\n',
                    '\tint if_id;\n', '\tu8 version;\n'):
    dispatch=dispatch.replace(declaration,'')
shim=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64;
typedef uint32_t __le32; typedef uint16_t __be16; typedef uintptr_t dma_addr_t;
#include <time.h>
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define BIT(n) (1U<<(n))
#define U16_MAX UINT16_MAX
#define GFP_ATOMIC 0
#define GFP_KERNEL 1
#define xchg(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
#define EPROTO 71
#define EOPNOTSUPP 95
#define struct_size(p,m,n) (sizeof(*(p))+(n))
#define atomic_inc_return(p) (++*(p))
#define DMA_TO_DEVICE 1
#define MT6797_CCCI_QUEUE_COUNT 7
#define MT6797_CCCI_CHANNEL_COUNT 256
#define MT6797_CCCI_RING_SIZE 16
#define MTK_CCCI_HEADER_SIZE 16
#define MTK_CCCI_CONTROL_RX 0
#define MTK_CCCI_CONTROL_TX 1
#define MT6797_CCCI_ACTIVE_IRQS 0x007f007fU
#define MT6797_MD1_CLDMA_ACTIVE_QUEUES 127U
#define CLDMA_L2_TX_MASK_SET 0x428
#define CLDMA_L2_RX_MASK_SET 0xc68
#define CLDMA_UL_STOP_CMD 0x5c
#define CLDMA_SO_STOP_CMD 0x3c4
#define CLDMA_UL_RESUME_CMD 0x58
#define MT6797_CCCI_DIAG_BAD_HEADER 1
#define MT6797_CCCI_DIAG_BAD_CHANNEL 2
#define MT6797_CCCI_DIAG_BEFORE_READY 3
#define MT6797_CCCI_DIAG_BAD_SEQUENCE 4
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define atomic_read(p) __atomic_load_n(p,__ATOMIC_SEQ_CST)
#define atomic_xchg(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
#define atomic_dec(p) ((void)__atomic_sub_fetch(p,1,__ATOMIC_SEQ_CST))
#define atomic_add_unless(p,a,v) ((*(p)==(v))?0:((*(p)+=(a)),1))
#define cpu_to_le32(v) (v)
#define le32_to_cpu(v) (v)
#define MTK_CCCI_H_AST_BIT (1U<<31)
#define MTK_CCCI_H_SEQ_MASK 0x7fffU
#define MTK_CCCI_H_CHN_MASK 0xffffU
#define MTK_CCCI_H_SEQ_SHIFT 16
#define MTK_CCCI_STATUS_SEQ_SHIFT 16
#define MTK_CCCI_STATUS_ASSERT (1U<<31)
#define MTK_CCCI_STATUS_CHANNEL_MASK 0xffffU
#define MTK_CCCI_STATUS_SEQ_MASK 0x7fff0000U
#define FIELD_PREP(mask,val) ((val)<<__builtin_ctz(mask))
#define FIELD_GET(mask,val) (((val)&(mask))>>__builtin_ctz(mask))
struct mtk_ccci_header {__le32 data0,data1,status,reserved;};
struct mtk_ccci_frame {u32 data0,data1,reserved;u16 channel,sequence;bool assert_bit;};
struct sk_buff {u8 *data;size_t len;};
'''
adapter=r'''
struct mt6797_ccci_slot {void *gpd;struct sk_buff *skb;dma_addr_t skb_dma;bool mapped;};
struct mt6797_ccci_queue {struct mt6797_ccci_slot tx[16];unsigned tx_head,tx_tail,tx_used;pthread_mutex_t tx_lock;};
struct workqueue_struct {int jobs;};
struct mt6797_ccci {int failed,ready,rpc_logs,fs_logs,hs,diag,teardown,fs_pending;unsigned long fs_indices,rpc_indices;int rpc_pending,rx_rejected;bool rf_clock_present;struct mt6797_rpc_clkbuf_values rf_clock;void *dev;char *ao,*pdn;
 struct workqueue_struct *fs_wq,*rpc_wq;struct mt6797_fs_service fs_service;
 struct mt6797_ccci_queue queue[7];pthread_mutex_t tx_stop_lock;struct mt6797_rpc_eint sim_eint[2];
 int tx_sequence[256],rx_sequence[256];};
struct mt6797_ccci_link;
static struct mt6797_ccci c;static char ao[4096],pdn[4096];static u8 desc[7][16][16];static struct workqueue_struct wq,rpcwq;
static int mode,allocs,frees,maps,unmaps,publishes,resumes,stops,captures,controls;
static pthread_t stopper;static bool stopper_started;
static __thread bool stop_locked;
static inline bool mt6797_ccci_fs_index_busy(struct mt6797_ccci *,u32);
static int rx_started,rx_busy;
static bool test_bit(unsigned i,unsigned long *p){return (*p&(1UL<<i))!=0;}
static void clear_bit(unsigned i,unsigned long *p){*p&=~(1UL<<i);}
static bool test_and_set_bit(unsigned i,unsigned long *p){assert(stop_locked);bool was=(*p&(1UL<<i))!=0;*p|=1UL<<i;return was;}
/* The FS worker itself is exercised by its own actual suite; this fixture
 * checks admission, capture and the TX index-release contract only.
 */
static int jobs,job_allocs,rf_calls;static u32 rf_afc;static bool hold_rpc;static unsigned queued_rpc;static struct work_struct *held_rpc[3];
static void *kmalloc(size_t n,int flags){(void)flags;assert(!stop_locked);job_allocs++;return malloc(n);}
static void kfree(void *p){free(p);}
#define INIT_WORK(w,f) ((w)->fn=(f))
static void mt6797_ccci_fs_work(struct work_struct *w){(void)w;assert(!"worker never runs in this fixture");}
static bool queue_work(struct workqueue_struct *w,struct work_struct *job){assert(!stop_locked);if(w==&rpcwq){if(hold_rpc){assert(queued_rpc<3);held_rpc[queued_rpc++]=job;}else job->fn(job);return true;}assert(w==&wq);jobs++;
 struct mt6797_ccci_fs_work *j=container_of(job,struct mt6797_ccci_fs_work,work);assert(j->len==44&&mt6797_fs_read32(j->frame+16)==0x100e);kfree(j);return true;}
static void mt6797_rx_rejected_record(int *d,u32 ch,u32 q,const u8 *p,size_t n,int error)
{(void)ch;(void)q;(void)p;(void)n;(void)error;(*d)++;}
static int mt6797_md1_cold_owner_rf_clock(void *dev,u32 afc,u8 sw[4])
{(void)dev;rf_calls++;rf_afc=afc;if(mode==9)return -EIO;sw[0]=1;sw[1]=0;sw[2]=1;sw[3]=1;return 0;}
static void destroy_workqueue(struct workqueue_struct *w){assert(w==&rpcwq);
 for(unsigned i=0;i<queued_rpc;i++){held_rpc[i]->fn(held_rpc[i]);}queued_rpc=0;}
static bool fs_enabled=true;
static bool mt6797_md1_fs_provider_enabled(void){return fs_enabled;}
bool mt6797_fs_service_supports(u32 op){return op==0x100e||op==0x1001;}
static void lock(pthread_mutex_t *p){assert(!pthread_mutex_lock(p));if(p==&c.tx_stop_lock)stop_locked=true;}
static void unlock(pthread_mutex_t *p){if(p==&c.tx_stop_lock)stop_locked=false;assert(!pthread_mutex_unlock(p));}
#define spin_lock(p) lock(p)
#define spin_unlock(p) unlock(p)
#define spin_lock_irqsave(p,f) do{(f)=0;lock(p);}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);unlock(p);}while(0)
static struct sk_buff *alloc_skb(size_t n,int flag){(void)flag;assert(!stop_locked);if(mode==1)return NULL;
 struct sk_buff *s=malloc(sizeof(*s));s->data=malloc(n);s->len=0;allocs++;return s;}
static void skb_put_data(struct sk_buff *s,const void *d,size_t n){memcpy(s->data,d,n);s->len=n;}
static void kfree_skb(struct sk_buff *s){assert(!stop_locked);if(!s)return;free(s->data);free(s);frees++;}
static u16 mt6797_ccci_next_sequence(struct mt6797_ccci *p,u16 ch){return p->tx_sequence[ch]++;}
static void mt6797_ccci_diag_final_reply(void *d,const void *p,size_t n){(void)d;(void)p;(void)n;assert(!stop_locked);}
static void mt6797_ccci_diag_tx(void *d,bool r,unsigned h,unsigned t,unsigned u){(void)d;(void)r;(void)h;(void)t;(void)u;assert(!stop_locked);}
static void mt6797_ccci_diag_rx_save(void *d,const void *p,size_t n,unsigned q,unsigned cause,int err,unsigned ch)
{(void)d;(void)p;(void)n;(void)q;(void)cause;(void)err;(void)ch;captures++;}
static void mt6797_ccci_first_fatal(struct mt6797_ccci *p,const char *s){(void)p;(void)s;assert(!stop_locked);}
#define dev_err(...) assert(!stop_locked)
#define dev_info(...) assert(!stop_locked)
#define dev_info_ratelimited(...) assert(!stop_locked)
static void mt6797_ccci_frontend_ready(struct mt6797_ccci *p,bool ready){p->ready=ready;}
static void mt6797_md1_hs_crash(int *p,int why){(void)p;(void)why;assert(!stop_locked);}
static void mt6797_md1_cold_owner_transport_fatal(int why){(void)why;assert(!stop_locked);}
static void writel(u32 value,char *p){assert(stop_locked);
 if(p==pdn+CLDMA_UL_RESUME_CMD){assert(value==2 && !stops);resumes++;}
 else if(p==pdn+CLDMA_UL_STOP_CMD || p==pdn+CLDMA_SO_STOP_CMD)stops++;
}
static u32 readl(char *p){(void)p;assert(stop_locked);return 0;}
static dma_addr_t dma_map_single(void *dev,void *data,size_t n,int dir)
{(void)dev;(void)data;(void)n;(void)dir;assert(!stop_locked);maps++;return mode==2?0:1234;}
static bool dma_mapping_error(void *dev,dma_addr_t a){(void)dev;return !a;}
static void dma_unmap_single(void *dev,dma_addr_t a,size_t n,int dir)
{(void)dev;(void)a;(void)n;(void)dir;assert(!stop_locked);unmaps++;}
static void mt6797_ccci_quiesce(struct mt6797_ccci *,int);
static void *fail_thread(void *unused){(void)unused;mt6797_ccci_quiesce(&c,-EIO);return NULL;}
static void *rx_thread(void *unused){(void)unused;__atomic_store_n(&rx_started,1,__ATOMIC_SEQ_CST);rx_busy=mt6797_ccci_fs_index_busy(&c,4);return NULL;}
static int mt6797_md1_cldma_tgpd_prepare_tx(void *g,dma_addr_t a,size_t n)
{(void)g;(void)a;(void)n;assert(!stop_locked);if(mode==3)return -EIO;
 if(mode==5)mt6797_ccci_quiesce(&c,-EIO);
 return 0;}
static int mt6797_md1_cldma_tgpd_publish(void *g){(void)g;assert(stop_locked);if(mode==4)return -EIO;
 if(mode==6){assert(!pthread_create(&stopper,NULL,fail_thread,NULL));stopper_started=true;
  while(!atomic_read(&c.failed))sched_yield();}
 if(mode==7){rx_started=0;rx_busy=-1;assert(!pthread_create(&stopper,NULL,rx_thread,NULL));stopper_started=true;
  while(!__atomic_load_n(&rx_started,__ATOMIC_SEQ_CST))sched_yield();}
 publishes++;return 0;}
static void mt6797_ccci_control_rx(struct mt6797_ccci *p,struct sk_buff *s,const struct mtk_ccci_frame *f)
{(void)p;(void)s;(void)f;controls++;}
'''
tests=r'''
static void reset(int m){memset(&c,0,sizeof(c));c.ao=ao;c.pdn=pdn;c.dev=&c;
 assert(!pthread_mutex_init(&c.tx_stop_lock,NULL));
 for(unsigned q=0;q<7;q++){assert(!pthread_mutex_init(&c.queue[q].tx_lock,NULL));for(unsigned i=0;i<16;i++)c.queue[q].tx[i].gpd=desc[q][i];}
 memset(c.rx_sequence,0xff,sizeof(c.rx_sequence));c.sim_eint[0].present=true;c.sim_eint[0].valid=127;c.sim_eint[0].value[6]=1;
 c.tx_sequence[33]=5;c.tx_sequence[15]=9;c.fs_wq=&wq;c.rpc_wq=&rpcwq;hold_rpc=false;queued_rpc=0;rf_calls=0;rf_afc=0;fs_enabled=true;mode=m;allocs=frees=maps=unmaps=publishes=resumes=stops=captures=controls=jobs=job_allocs=0;stopper_started=false;}
static void clean(void){if(stopper_started)assert(!pthread_join(stopper,NULL));
 for(unsigned q=0;q<7;q++){for(unsigned i=0;i<16;i++)if(c.queue[q].tx[i].skb)kfree_skb(c.queue[q].tx[i].skb);
 assert(!pthread_mutex_destroy(&c.queue[q].tx_lock));}assert(!pthread_mutex_destroy(&c.tx_stop_lock));}
static struct sk_buff *request(void){int save=mode;mode=0;struct sk_buff *s=alloc_skb(68,0);mode=save;
 u8 b[68]={0};mt6797_rpc_write(b,0x12345678);mt6797_rpc_write(b+4,68);mt6797_rpc_write(b+8,0x80000020);
 mt6797_rpc_write(b+12,1);mt6797_rpc_write(b+16,0x4005);mt6797_rpc_write(b+20,3);mt6797_rpc_write(b+24,23);
 memcpy(b+28,"MD1_SIM1_HOT_PLUG_EINT",23);mt6797_rpc_write(b+52,4);mt6797_rpc_write(b+56,23);
 mt6797_rpc_write(b+60,4);mt6797_rpc_write(b+64,6);skb_put_data(s,b,68);return s;}
static struct sk_buff *fs_request(void){int save=mode;mode=0;struct sk_buff *s=alloc_skb(44,0);mode=save;
 u8 b[44]={0};mt6797_rpc_write(b+4,44);mt6797_rpc_write(b+8,0x8000000e);
 mt6797_rpc_write(b+12,4);mt6797_rpc_write(b+16,0x100e);mt6797_rpc_write(b+20,2);
 mt6797_rpc_write(b+24,8);b[28]='Z';b[30]=':';b[32]='\\';
 mt6797_rpc_write(b+36,4);mt6797_rpc_write(b+40,3);skb_put_data(s,b,44);return s;}
int main(void){
 u32 translated;assert(mt6797_rpc_eint_value(1,100000,&translated)&&translated==100);
 reset(0);mt6797_ccci_dispatch_rx(&c,request(),1);assert(publishes==1&&resumes==1&&frees==1&&!captures);
 struct sk_buff *sent=c.queue[1].tx[0].skb;assert(sent&&sent->len==40);
 assert(mt6797_rpc_read(sent->data)==0x12345678&&mt6797_rpc_read(sent->data+12)==1);
 assert(mt6797_rpc_read(sent->data+8)==0x80050021&&mt6797_rpc_read(sent->data+36)==1);clean();
 for(int m=1;m<=5;m++){reset(m);struct sk_buff *req=request();int ret=mt6797_ccci_rpc_rx(&c,req,1);
  assert(!ret&&c.failed&&!c.rpc_pending&&!c.rpc_indices&&!publishes&&!resumes&&!c.queue[1].tx_used&&!c.queue[1].tx[0].mapped&&!c.queue[1].tx[0].skb);
  assert(c.tx_sequence[33]==5);assert(unmaps==(m>=3));kfree_skb(req);clean();assert(allocs==frees);}
 reset(6);struct sk_buff *req=request();assert(!mt6797_ccci_rpc_rx(&c,req,1));kfree_skb(req);clean();
 assert(publishes==1&&resumes==1&&stops==2&&allocs==frees);
 reset(0);req=request();mt6797_rpc_write(req->data+16,0x4006);mt6797_ccci_dispatch_rx(&c,req,1);
 assert(captures==1&&!maps&&!publishes&&frees==1);clean();
 reset(0);req=request();req->len=15;mt6797_ccci_dispatch_rx(&c,req,1);assert(captures==1&&!maps);clean();
 reset(0);c.failed=1;req=request();assert(mt6797_ccci_rpc_rx(&c,req,1)==-ESHUTDOWN&&!maps);kfree_skb(req);clean();

 /* Deferred worker owns an immutable frame and excludes both live indices. */
 reset(0);hold_rpc=true;req=request();assert(!mt6797_ccci_rpc_rx(&c,req,1));
 assert(c.rpc_pending==1 && c.rpc_indices==2 && queued_rpc==1 && !maps);
 assert(mt6797_ccci_rpc_rx(&c,req,1)==-EALREADY);
 mt6797_rpc_write(req->data+12,0);assert(!mt6797_ccci_rpc_rx(&c,req,1));
 assert(c.rpc_pending==2 && c.rpc_indices==3 && queued_rpc==2);
 memset(req->data,0xff,req->len);kfree_skb(req);
 held_rpc[0]->fn(held_rpc[0]);assert(c.rpc_pending==1 && c.rpc_indices==1);
 assert(mt6797_rpc_read(c.queue[1].tx[0].skb->data+12)==1);
 held_rpc[1]->fn(held_rpc[1]);assert(!c.rpc_pending && !c.rpc_indices && publishes==2);clean();
 reset(0);c.rpc_pending=3;req=request();assert(mt6797_ccci_rpc_rx(&c,req,1)==-ENOSPC);
 assert(c.rpc_pending==3&&!c.rpc_indices);kfree_skb(req);clean();
 reset(0);hold_rpc=true;req=request();assert(!mt6797_ccci_rpc_rx(&c,req,1));kfree_skb(req);
 c.teardown=1;mt6797_ccci_rpc_destroy(&c);assert(!c.rpc_wq&&!c.rpc_pending&&!c.rpc_indices&&!maps);
 mt6797_ccci_rpc_destroy(&c);clean();assert(allocs==frees);
 /* Real codec -> provider -> actual DMA publication, including AFC failure. */
 for(unsigned scenario=0;scenario<6;scenario++) {
  reset(0);hold_rpc=true;c.rf_clock_present=true;
  c.rf_clock=(struct mt6797_rpc_clkbuf_values){{2,0,1,1},{0},{1,1,1,1}};
  req=request();req->len=36;memset(req->data,0,36);
  mt6797_rpc_write(req->data+4,36);mt6797_rpc_write(req->data+8,32);
  mt6797_rpc_write(req->data+16,0x4008);mt6797_rpc_write(req->data+20,1);
  mt6797_rpc_write(req->data+24,6);req->data[28]=4;
  mt6797_rpc_write(req->data+30,0x89abcdef);
  assert(!mt6797_ccci_rpc_rx(&c,req,1));kfree_skb(req);
  if(scenario==1)c.failed=1;
  if(scenario==2)c.teardown=1;
  if(scenario==3)mode=1; /* response allocation before provider effects */
  if(scenario==4)mode=2; /* DMA failure after AFC: terminal, no retry */
  if(scenario==5)mode=9; /* provider failure: terminal, no success TX */
  held_rpc[0]->fn(held_rpc[0]);
  assert(!c.rpc_pending&&!c.rpc_indices);
  if(!scenario){assert(rf_calls==1&&rf_afc==0x89abcdef&&publishes==1);
   sent=c.queue[1].tx[0].skb;assert(sent->len==56&&mt6797_rpc_read(sent->data+16)==0xffff4008);
   assert(sent->data[38]==2&&sent->data[42]==1&&sent->data[43]==0&&sent->data[44]==1&&sent->data[46]==1);
  } else {assert(!publishes);assert(rf_calls==(scenario>=4));}
  clean();assert(allocs==frees);
 }
 /* The actual pre-ready FS dispatch admits one immutable job per request index. */
 reset(0);mt6797_ccci_dispatch_rx(&c,fs_request(),1);
 assert(!c.ready&&jobs==1&&job_allocs==1&&!publishes&&!maps&&frees==1&&!captures&&c.fs_indices==(1UL<<4)&&c.fs_pending==1);clean();
 for(unsigned q=0;q<7;q++)if(q!=1){reset(0);mt6797_ccci_dispatch_rx(&c,fs_request(),q);
  assert(captures==1&&!jobs&&!maps&&!publishes&&frees==1&&!c.fs_indices);clean();}
 reset(0);fs_enabled=false;mt6797_ccci_dispatch_rx(&c,fs_request(),1);assert(captures==1&&!jobs);clean();
 reset(0);req=fs_request();mt6797_rpc_write(req->data+16,0x1010);mt6797_ccci_dispatch_rx(&c,req,1);
 assert(captures==1&&!jobs&&!job_allocs&&frees==1&&!c.fs_indices);clean();
 reset(0);req=fs_request();mt6797_rpc_write(req->data,1);mt6797_ccci_dispatch_rx(&c,req,1);
 assert(captures==1&&!jobs&&frees==1);clean();
 reset(0);req=fs_request();mt6797_rpc_write(req->data,0x80000000U);mt6797_ccci_dispatch_rx(&c,req,1);
 assert(captures==1&&!jobs&&frees==1);clean();
 reset(0);req=fs_request();mt6797_rpc_write(req->data+12,5);mt6797_ccci_dispatch_rx(&c,req,1);
 assert(captures==1&&!jobs&&frees==1);clean();
 reset(0);c.fs_indices=1UL<<4;mt6797_ccci_dispatch_rx(&c,fs_request(),1);
 assert(captures==1&&!jobs&&c.fs_indices==(1UL<<4)&&!c.fs_pending);clean();
 reset(0);c.failed=1;req=fs_request();assert(mt6797_ccci_fs_rx(&c,req,1)==-ESHUTDOWN&&!jobs);kfree_skb(req);clean();
 reset(0);c.teardown=1;req=fs_request();assert(mt6797_ccci_fs_rx(&c,req,1)==-ESHUTDOWN&&!jobs&&!c.fs_indices);kfree_skb(req);clean();
 reset(0);c.fs_pending=6;req=fs_request();assert(mt6797_ccci_fs_rx(&c,req,1)==-ENOSPC&&!jobs&&!c.fs_indices&&c.fs_pending==6);kfree_skb(req);clean();
 /* TX releases the FS index only when publishing a final (non-fragment) frame. */
 for(int m=0;m<=7;m++)if(m!=1)for(int fragment=0;fragment<2;fragment++){
   reset(0);u8 bytes[3584];struct mt6797_fs_reply fsr;u8 payload[8192];mt6797_fs_reply_init(&fsr,payload,sizeof(payload),4,0x1010);
   if(fragment){static u8 big[7000];assert(!mt6797_fs_reply_add(&fsr,big,sizeof(big)));}
   else assert(!mt6797_fs_reply_add_u32(&fsr,(u32)-9));
   int n=mt6797_fs_reply_frame(&fsr,0,bytes,sizeof(bytes));assert(n==(fragment?0xd94:32));
   req=alloc_skb(n,GFP_ATOMIC);skb_put_data(req,bytes,n);c.fs_indices=1UL<<4;mode=m;
   int ret=mt6797_ccci_tx(&c,1,req,15);
   if(ret){assert(c.fs_indices==(1UL<<4));kfree_skb(req);}
   else {assert(c.fs_indices==(fragment?(1UL<<4):0)&&mt6797_rpc_read(req->data+8)==0x8009000f&&mt6797_rpc_read(req->data+12)==4);
     assert(mt6797_rpc_read(req->data)==(fragment?0x80000000U:0));}
   clean();assert(allocs==frees);if(m==7)assert(rx_busy==(fragment?1:0));
 }
 puts("PASS actual pre-ready RPC/FS dispatch/TX/STOP: 40-byte RPC replies, FS admission/capture/index collision, publish-only final-frame index release with fragment retention, independent sequence, DMA ownership unwind, concurrent publish-before-STOP");
}
'''
parts=[shim,(root/'drivers/net/wwan/mediatek/mtk_mt6797_ccci_rpc.h').read_text(),
       (root/'drivers/net/wwan/mediatek/mtk_mt6797_ccci_rpc_clkbuf.h').read_text().replace('#include \"mtk_mt6797_ccci_rpc.h\"',''),
       (root/'include/linux/soc/mediatek/mt6797-md1-fs-provider.h').read_text().split('#if IS_ENABLED')[0].split('struct device;')[1],
       (root/'drivers/net/wwan/mediatek/mtk_mt6797_ccci_fs.h').read_text(),
       'struct work_struct {void (*fn)(struct work_struct *);};\n'+source[source.index('struct mt6797_ccci_fs_work {'):source.index('static inline bool mt6797_ccci_fs_index_busy(')]+source[source.index('struct mt6797_ccci_rpc_work {'):source.index('static void mt6797_ccci_rpc_work(')],adapter,
       function(common,'mtk_ccci_header_init('),function(common,'mtk_ccci_header_decode('),
       function(source,'mt6797_ccci_quiesce_from('),function(source,'mt6797_ccci_quiesce('),
       function(source,'mt6797_ccci_tx('),function(source,'mt6797_ccci_rf_clock('),function(source,'mt6797_ccci_rpc_work('),function(source,'mt6797_ccci_rpc_destroy('),function(source,'mt6797_ccci_rpc_rx('),
       function(source,'mt6797_ccci_fs_index_busy('),function(source,'mt6797_ccci_fs_index_claim('),
       function(source,'mt6797_ccci_fs_rx('),dispatch,tests]
with tempfile.TemporaryDirectory(prefix='gemini-rpc-transport-') as d:
    p=Path(d);(p/'test.c').write_text('\n'.join(parts))
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    '-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
