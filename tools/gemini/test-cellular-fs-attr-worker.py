#!/usr/bin/env python3
"""Actual queue/worker/destroy with concurrent admissions and STOP adapters."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text()
def function(name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'
rpc = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_rpc.h').read_text()
rpc = rpc[rpc.index('static u32 mt6797_rpc_read('):rpc.index('static int mt6797_rpc_eint_reply(')]
codec = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_fs.h').read_text()
codec = codec[:codec.index('/* Complete, unfragmented GETDISKINFO')] + '\n#endif\n'
structure = source[source.index('struct mt6797_ccci_fs_attr_work {'):source.index('static void mt6797_ccci_fs_attr_work(')]
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
typedef uint8_t u8;typedef uint32_t u32;typedef int32_t s32;
#define GFP_ATOMIC 1
#define GFP_KERNEL 2
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define atomic_read(p) atomic_load(p)
#define atomic_inc_return(p) (atomic_fetch_add(p,1)+1)
#define atomic_dec(p) ((void)atomic_fetch_sub(p,1))
#define atomic_xchg(p,v) atomic_exchange(p,v)
#define xchg(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
#define dev_info(...) ((void)0)
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
struct sk_buff {u8 data[80];size_t len;};
struct mt6797_ccci {atomic_int failed,teardown,fs_attr_pending,fs_logs;unsigned long fs_attr_indices;struct workqueue_struct *fs_attr_wq;};
static struct mt6797_ccci c;
static struct workqueue_struct wq;
static atomic_int allocations,freed;
static int scenario,lookups,txs,skb_alloc,skb_freed,quiesced;
static struct sk_buff *owned;
static pthread_barrier_t barrier;
static bool reentered;
static struct sk_buff request(void);
static int mt6797_ccci_fs_attr_queue(struct mt6797_ccci *,const struct sk_buff *);
static void *kmalloc(size_t n,int flags){assert(flags==GFP_ATOMIC);if(scenario==1)return NULL;atomic_fetch_add(&allocations,1);return malloc(n);}
static void kfree(void *p){atomic_fetch_add(&freed,1);free(p);}
static bool queue_work(struct workqueue_struct *w,struct work_struct *job){assert(w==&wq);pthread_mutex_lock(&w->lock);assert(w->count<16);w->jobs[w->count++]=job;pthread_mutex_unlock(&w->lock);return true;}
static void destroy_workqueue(struct workqueue_struct *w){assert(w==&wq);for(unsigned i=0;i<w->count;i++)w->jobs[i]->fn(w->jobs[i]);w->count=0;}
static int mt6797_md1_fs_provider_getattr(const char *name,unsigned *mode){
 assert(!strcmp(name,"a.log"));lookups++;*mode=0100644;
 if(reentered&&lookups==2)assert(c.fs_attr_indices==1);
 if(scenario==2)atomic_store(&c.failed,1);
 return scenario==3?-ENOENT:0;
}
static struct sk_buff *alloc_skb(size_t n,int flags){assert(n==32&&flags==GFP_KERNEL);if(scenario==4)return NULL;skb_alloc++;return calloc(1,sizeof(struct sk_buff));}
static void skb_put_data(struct sk_buff *s,const void *p,size_t n){memcpy(s->data,p,n);s->len=n;}
static void kfree_skb(struct sk_buff *s){skb_freed++;free(s);}
static void mt6797_ccci_quiesce(struct mt6797_ccci *p,int reason){assert(p==&c&&reason<0);atomic_store(&p->failed,1);quiesced++;}
static int mt6797_ccci_tx(struct mt6797_ccci *p,unsigned q,struct sk_buff *s,unsigned channel){
 assert(p==&c&&q==1&&channel==15&&s->len==32);txs++;
 if(scenario==5){atomic_store(&p->failed,1);return -ESHUTDOWN;}
 if(scenario==6)return -EBUSY;
 assert(!atomic_load(&p->failed));assert(s->data[16]==16&&s->data[17]==16&&s->data[20]==1);
 assert(s->data[28]==(scenario==3?247:0));assert(!owned);
 clear_bit(s->data[12],&p->fs_attr_indices);owned=s;
 if(scenario==7){
   kfree_skb(owned);owned=NULL;scenario=0;reentered=true;
   struct sk_buff next=request();assert(!mt6797_ccci_fs_attr_queue(p,&next));
   assert(p->fs_attr_indices==1&&atomic_load(&p->fs_attr_pending)==2);
 }
 return 0;
}
'''
tests = r'''
static struct sk_buff request(void){
 struct sk_buff s={.len=48};s.data[4]=48;s.data[8]=14;s.data[11]=128;
 s.data[16]=16;s.data[17]=16;s.data[20]=1;s.data[24]=18;
 s.data[28]='Z';s.data[30]=':';s.data[32]='\\';
 const char *name="a.log";for(unsigned i=0;i<5;i++)s.data[34+i*2]=name[i];return s;
}
static void reset(int n){
 scenario=n;lookups=txs=skb_alloc=skb_freed=quiesced=0;owned=NULL;reentered=false;
 assert(!wq.count);atomic_store(&allocations,0);atomic_store(&freed,0);
 memset(&c,0,sizeof(c));c.fs_attr_wq=&wq;
}
static void *admit(void *arg){struct sk_buff s=request();int *ret=arg;s.data[12]=*ret%5;pthread_barrier_wait(&barrier);*ret=mt6797_ccci_fs_attr_queue(&c,&s);return NULL;}
int main(void){
 pthread_mutex_init(&wq.lock,NULL);
 for(int n=0;n<=7;n++){
   reset(n);struct sk_buff s=request();int ret=mt6797_ccci_fs_attr_queue(&c,&s);
   memset(&s,255,sizeof(s)); /* Received skb can be released immediately. */
   if(n==1){assert(ret==-ENOMEM&&!wq.count&&!atomic_load(&c.fs_attr_pending));continue;}
   assert(!ret&&wq.count==1&&atomic_load(&c.fs_attr_pending)==1);
   destroy_workqueue(&wq); /* Adapter drains ordinary scheduled callbacks. */
   mt6797_ccci_fs_attr_destroy(&c);assert(!c.fs_attr_wq);
   assert(!atomic_load(&c.fs_attr_pending)&&atomic_load(&allocations)==atomic_load(&freed));
   if(owned){kfree_skb(owned);owned=NULL;}
   assert(skb_alloc==skb_freed);
   if(n==2)assert(!txs&&!skb_alloc);
   if(n==4)assert(quiesced==1&&!txs);
   if(n==5)assert(txs==1&&!quiesced);
   if(n==6)assert(txs==1&&quiesced==1);
   if(n==7)assert(reentered&&lookups==2&&txs==2&&!c.fs_attr_indices);
 }
 for(int stop=0;stop<2;stop++){
   reset(0);struct sk_buff s=request();assert(!mt6797_ccci_fs_attr_queue(&c,&s));
   if(stop)atomic_store(&c.teardown,1);else atomic_store(&c.failed,1);
   mt6797_ccci_fs_attr_destroy(&c);assert(!lookups&&!txs&&!atomic_load(&c.fs_attr_pending));
   mt6797_ccci_fs_attr_destroy(&c);assert(atomic_load(&freed)==1);
 }
 reset(0);pthread_t threads[32];int returns[32];pthread_barrier_init(&barrier,NULL,32);
 for(int i=0;i<32;i++){returns[i]=i;assert(!pthread_create(&threads[i],NULL,admit,&returns[i]));}
 int accepted=0;for(int i=0;i<32;i++){pthread_join(threads[i],NULL);if(!returns[i])accepted++;else assert(returns[i]==-EALREADY);}
 assert(accepted==5&&wq.count==5&&atomic_load(&c.fs_attr_pending)==5);
 atomic_store(&c.teardown,1);mt6797_ccci_fs_attr_destroy(&c);
 assert(!lookups&&!txs&&!atomic_load(&c.fs_attr_pending)&&atomic_load(&freed)==5&&!c.fs_attr_indices);
 puts("PASS actual GETATTR queue/worker/destroy: immutable RX copy, allocation and TX ownership failures, STOP before/during lookup/before publish, drain-once, 32 concurrent admissions bounded to five");
}
'''
remove = function('mt6797_ccci_remove')
assert remove.index('napi_disable') < remove.index('mt6797_ccci_fs_attr_destroy')
assert remove.index('mt6797_ccci_quiesce') < remove.index('mt6797_ccci_fs_attr_destroy')
assert source.index('devm_add_action_or_reset(&pdev->dev, mt6797_ccci_fs_attr_destroy') > source.index('mt6797_ccci_alloc_rings(ccci)')
with tempfile.TemporaryDirectory(prefix='gemini-fs-worker-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(shim + rpc + codec + structure + function('mt6797_ccci_fs_attr_work') + function('mt6797_ccci_fs_attr_destroy') + function('mt6797_ccci_fs_attr_queue') + tests)
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=10)
