#!/usr/bin/env python3
"""Actual FS admission/worker/send/destroy with concurrent admissions and STOP adapters."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text()
service_source = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_fs.c').read_text()
def function(text, name):
    start = text.index(name + '(')
    start = text.rfind('\n', 0, start) + 1
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'
provider = (root / 'include/linux/soc/mediatek/mt6797-md1-fs-provider.h').read_text()
provider = provider[provider.index('struct mt6797_md1_fs_stats {'):provider.index('#if IS_ENABLED')]
codec = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_fs.h').read_text()
structure = source[source.index('struct mt6797_ccci_fs_work {'):source.index('static inline bool mt6797_ccci_fs_index_busy(')]
shim = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint8_t u8;typedef uint32_t u32;typedef int32_t s32;typedef uint64_t u64;typedef int64_t s64;
#define GFP_ATOMIC 1
#define GFP_KERNEL 2
#define EMSGSIZE 90
#define EPROTO 71
#define EOPNOTSUPP 95
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define struct_size(p,m,n) (sizeof(*(p))+(n))
#define atomic_read(p) atomic_load(p)
#define atomic_inc_return(p) (atomic_fetch_add(p,1)+1)
#define atomic_dec(p) ((void)atomic_fetch_sub(p,1))
#define xchg(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
static int logs;
#define dev_info(...) (logs++)
static bool test_and_set_bit(unsigned i,unsigned long *p){return (__atomic_fetch_or(p,1UL<<i,__ATOMIC_SEQ_CST)&(1UL<<i))!=0;}
static void clear_bit(unsigned i,unsigned long *p){__atomic_fetch_and(p,~(1UL<<i),__ATOMIC_SEQ_CST);}
static bool test_bit(unsigned i,unsigned long *p){return (__atomic_load_n(p,__ATOMIC_SEQ_CST)&(1UL<<i))!=0;}
static pthread_mutex_t stop_lock=PTHREAD_MUTEX_INITIALIZER;
#define spin_lock_irqsave(p,f) do{(f)=0;pthread_mutex_lock(&stop_lock);}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);pthread_mutex_unlock(&stop_lock);}while(0)
static bool atomic_add_unless(atomic_int *p,int n,int unless){int v=atomic_load(p);while(v!=unless){if(atomic_compare_exchange_weak(p,&v,v+n))return true;}return false;}
struct work_struct {void (*fn)(struct work_struct *);};
#define INIT_WORK(w,f) ((w)->fn=(f))
struct workqueue_struct {struct work_struct *jobs[16];unsigned count;pthread_mutex_t lock;};
struct sk_buff {u8 data[3584];size_t len;};
struct file;
'''
adapters = r'''
struct mt6797_ccci {atomic_int failed,teardown,fs_pending,fs_logs;unsigned long fs_indices;struct workqueue_struct *fs_wq;struct mt6797_fs_service fs_service;};
static struct mt6797_ccci c;
static struct workqueue_struct wq;
static atomic_int allocations,freed;
static int scenario,requests,txs,skb_alloc,skb_freed,quiesced,released,fail_kmalloc_at,kmallocs;
static struct sk_buff *owned[8];
static unsigned owned_count;
static pthread_barrier_t barrier;
static bool reentered;
static struct sk_buff request(void);
static int mt6797_ccci_fs_rx(struct mt6797_ccci *,struct sk_buff *,unsigned);
static void *kmalloc(size_t n,int flags){(void)flags;kmallocs++;if(fail_kmalloc_at&&kmallocs==fail_kmalloc_at)return NULL;atomic_fetch_add(&allocations,1);return malloc(n);}
static void kfree(void *p){if(p)atomic_fetch_add(&freed,1);free(p);}
static bool queue_work(struct workqueue_struct *w,struct work_struct *job){assert(w==&wq);pthread_mutex_lock(&w->lock);assert(w->count<16);w->jobs[w->count++]=job;pthread_mutex_unlock(&w->lock);return true;}
static void destroy_workqueue(struct workqueue_struct *w){assert(w==&wq);for(unsigned i=0;i<w->count;i++)w->jobs[i]->fn(w->jobs[i]);w->count=0;}
static bool mt6797_md1_fs_provider_enabled(void){return scenario!=9;}
void mt6797_fs_service_release(struct mt6797_fs_service *s){assert(s==&c.fs_service);released++;}
/* Deterministic service adapter: scenario selects reply size or refusal. */
int mt6797_fs_service_request(struct mt6797_fs_service *s,const u8 *data,size_t len,struct mt6797_fs_reply *reply,s32 *status){
 assert(s==&c.fs_service&&reply->capacity==MT6797_FS_REPLY_MAX&&reply->payload);requests++;
 struct mt6797_fs_request req;assert(!mt6797_fs_request_parse(data,len,&req)&&req.op==0x1001);
 if(scenario==7)return -EOPNOTSUPP;
 if(reentered&&requests==2)assert(c.fs_indices==1);
 if(scenario==2)atomic_store(&c.failed,1);
 mt6797_fs_reply_init(reply,reply->payload,reply->capacity,req.index,req.op);
 if(scenario==3){static u8 big[7000];assert(!mt6797_fs_reply_add(reply,big,sizeof(big)));*status=0;return 0;}
 *status=scenario==8?-9:1;return mt6797_fs_reply_add_u32(reply,(u32)*status);
}
static struct sk_buff *alloc_skb(size_t n,int flags){assert(n==MT6797_FS_PREFIX+MT6797_FS_FRAGMENT&&flags==GFP_KERNEL);if(scenario==11)return NULL;skb_alloc++;return calloc(1,sizeof(struct sk_buff));}
static u8 *skb_tail_pointer(struct sk_buff *s){return s->data+s->len;}
static void skb_put(struct sk_buff *s,size_t n){s->len+=n;}
static void kfree_skb(struct sk_buff *s){skb_freed++;free(s);}
static void mt6797_ccci_quiesce(struct mt6797_ccci *p,int reason){assert(p==&c&&reason<0);atomic_store(&p->failed,1);quiesced++;}
static int mt6797_ccci_tx(struct mt6797_ccci *p,unsigned q,struct sk_buff *s,unsigned channel){
 assert(p==&c&&q==1&&channel==15&&s->len>=24&&mt6797_fs_read32(s->data+4)==s->len);txs++;
 if(scenario==5){atomic_store(&p->failed,1);return -ESHUTDOWN;}
 if(scenario==6)return -EBUSY;
 assert(!atomic_load(&p->failed));assert(mt6797_fs_read32(s->data+16)==0xffff1001U);
 u32 data0=mt6797_fs_read32(s->data);u32 index=mt6797_fs_read32(s->data+12);
 if(scenario==3){assert((txs<3)==((data0&0x80000000U)!=0));if(txs<3)assert(s->len==0xd94&&test_bit(index,&p->fs_indices));}
 else assert(!data0&&s->len==32&&(s32)mt6797_fs_read32(s->data+28)==(scenario==8?-9:1));
 if(!(data0&0x80000000U))clear_bit(index,&p->fs_indices);
 assert(owned_count<8);owned[owned_count++]=s;
 if(scenario==10){
   scenario=0;reentered=true;struct sk_buff next=request();assert(!mt6797_ccci_fs_rx(p,&next,1));
   assert(p->fs_indices==1&&atomic_load(&p->fs_pending)==2);
 }
 return 0;
}
'''
tests = r'''
static struct sk_buff request(void){
 /* C11 literal OPEN request for Z:\NVRAM, flags 0x900. */
 struct sk_buff s={.len=56};static const u8 c11[56]={0,0,0,0,0x38,0,0,0,0x0e,0,2,0x80,0,0,0,0,1,0x10,0,0,2,0,0,0,0x12,0,0,0,'Z',0,':',0,'\\',0,'N',0,'V',0,'R',0,'A',0,'M',0,0,0,0x36,0,4,0,0,0,0,9,0,0};
 memcpy(s.data,c11,56);return s;
}
static void reset(int n){
 scenario=n;requests=txs=skb_alloc=skb_freed=quiesced=released=fail_kmalloc_at=kmallocs=logs=0;owned_count=0;reentered=false;
 assert(!wq.count);atomic_store(&allocations,0);atomic_store(&freed,0);
 memset(&c,0,sizeof(c));c.fs_wq=&wq;
}
static void release_owned(void){for(unsigned i=0;i<owned_count;i++)kfree_skb(owned[i]);owned_count=0;}
static void *admit(void *arg){struct sk_buff s=request();int *ret=arg;mt6797_fs_write32(s.data+12,*ret%5);pthread_barrier_wait(&barrier);*ret=mt6797_ccci_fs_rx(&c,&s,1);return NULL;}
int main(void){
 pthread_mutex_init(&wq.lock,NULL);
 for(int n=0;n<=11;n++){
   if(n==9)continue;
   reset(n);if(n==1)fail_kmalloc_at=1;
   struct sk_buff s=request();int ret=mt6797_ccci_fs_rx(&c,&s,1);
   memset(&s,255,sizeof(s)); /* Received skb can be released immediately. */
   if(n==1){assert(ret==-ENOMEM&&!wq.count&&!atomic_load(&c.fs_pending)&&!c.fs_indices);continue;}
   assert(!ret&&wq.count==1&&atomic_load(&c.fs_pending)==1&&c.fs_indices==1);
   if(n==4)fail_kmalloc_at=2; /* reply payload allocation fails inside the worker */
   destroy_workqueue(&wq); /* Adapter drains ordinary scheduled callbacks. */
   mt6797_ccci_fs_destroy(&c);assert(!c.fs_wq&&released==1);
   assert(!atomic_load(&c.fs_pending)&&atomic_load(&allocations)==atomic_load(&freed)&&!c.fs_indices);
   release_owned();assert(skb_alloc==skb_freed);
   if(n==0)assert(requests==1&&txs==1&&!quiesced&&logs==1);
   if(n==2)assert(!txs&&!skb_alloc&&logs==1);
   if(n==3)assert(txs==3&&!quiesced&&logs==1);
   if(n==4)assert(quiesced==1&&!txs&&!requests);
   if(n==5)assert(txs==1&&!quiesced);
   if(n==6)assert(txs==1&&quiesced==1);
   if(n==7)assert(!txs&&!quiesced&&logs==1);
   if(n==8)assert(txs==1&&!quiesced);
   if(n==10)assert(reentered&&requests==2&&txs==2);
   if(n==11)assert(quiesced==1&&requests==1&&!txs&&!skb_alloc);
 }
 for(int stop=0;stop<2;stop++){
   reset(0);struct sk_buff s=request();assert(!mt6797_ccci_fs_rx(&c,&s,1));
   if(stop)atomic_store(&c.teardown,1);else atomic_store(&c.failed,1);
   mt6797_ccci_fs_destroy(&c);assert(!requests&&!txs&&!atomic_load(&c.fs_pending)&&released==1);
   mt6797_ccci_fs_destroy(&c);assert(atomic_load(&freed)==1&&released==1);
 }
 /* Admission refusals never claim an index or allocate. */
 reset(0);{struct sk_buff s=request();assert(mt6797_ccci_fs_rx(&c,&s,0)==-ESHUTDOWN&&!c.fs_indices);
   s=request();mt6797_fs_write32(s.data+16,0x100a);assert(mt6797_ccci_fs_rx(&c,&s,1)==-EOPNOTSUPP&&!c.fs_indices);
   s=request();mt6797_fs_write32(s.data,1);assert(mt6797_ccci_fs_rx(&c,&s,1)==-EPROTO);
   s=request();mt6797_fs_write32(s.data+4,55);assert(mt6797_ccci_fs_rx(&c,&s,1)==-EPROTO);
   s=request();mt6797_fs_write32(s.data+12,5);assert(mt6797_ccci_fs_rx(&c,&s,1)==-EPROTO);
   s=request();s.len=20;mt6797_fs_write32(s.data+4,20);assert(mt6797_ccci_fs_rx(&c,&s,1)==-EPROTO);
   scenario=9;s=request();assert(mt6797_ccci_fs_rx(&c,&s,1)==-EOPNOTSUPP);scenario=0;
   c.fs_indices=1;s=request();assert(mt6797_ccci_fs_rx(&c,&s,1)==-EALREADY&&!wq.count);c.fs_indices=0;
   atomic_store(&c.teardown,1);s=request();assert(mt6797_ccci_fs_rx(&c,&s,1)==-ESHUTDOWN&&!c.fs_indices);atomic_store(&c.teardown,0);
   c.fs_wq=NULL;s=request();assert(mt6797_ccci_fs_rx(&c,&s,1)==-ESHUTDOWN);c.fs_wq=&wq;
   assert(!kmallocs&&!atomic_load(&c.fs_pending));}
 reset(0);pthread_t threads[32];int returns[32];pthread_barrier_init(&barrier,NULL,32);
 for(int i=0;i<32;i++){returns[i]=i;assert(!pthread_create(&threads[i],NULL,admit,&returns[i]));}
 int accepted=0;for(int i=0;i<32;i++){pthread_join(threads[i],NULL);if(!returns[i])accepted++;else assert(returns[i]==-EALREADY);}
 assert(accepted==5&&wq.count==5&&atomic_load(&c.fs_pending)==5&&c.fs_indices==31);
 atomic_store(&c.teardown,1);mt6797_ccci_fs_destroy(&c);
 assert(!requests&&!txs&&!atomic_load(&c.fs_pending)&&atomic_load(&freed)==5&&!c.fs_indices);
 puts("PASS actual FS admission/worker/send/destroy: immutable RX copy, C11 frame end to end, three-frame fragmentation with final-frame index release, allocation and TX ownership failures, STOP before/during service/before publish, refusals without claims, drain-once with service release, 32 concurrent admissions bounded to five");
}
'''
remove = function(source, 'mt6797_ccci_remove')
assert remove.index('napi_disable') < remove.index('mt6797_ccci_fs_destroy')
assert remove.index('mt6797_ccci_quiesce') < remove.index('mt6797_ccci_fs_destroy')
assert source.index('devm_add_action_or_reset(&pdev->dev, mt6797_ccci_fs_destroy') > source.index('mt6797_ccci_alloc_rings(ccci)')
with tempfile.TemporaryDirectory(prefix='gemini-fs-worker-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(shim + provider + codec + adapters + structure + function(service_source, 'mt6797_fs_service_supports') +
                                 function(source, 'mt6797_ccci_fs_index_busy') + function(source, 'mt6797_ccci_fs_index_claim') +
                                 function(source, 'mt6797_ccci_fs_send') + function(source, 'mt6797_ccci_fs_work') +
                                 function(source, 'mt6797_ccci_fs_destroy') + function(source, 'mt6797_ccci_fs_rx') + tests)
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=10)
