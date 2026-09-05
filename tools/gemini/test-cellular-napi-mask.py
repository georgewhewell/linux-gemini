#!/usr/bin/env python3
"""Actual CCCI IRQ/poll/STOP regression for mask ownership across NAPI polls."""
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text()
def body(name):
    start=source.index(name);start=source.rfind('\n',0,start)+1
    pos=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[pos]=='{')-(source[pos]=='}');pos+=1
    # Kernel IRQ callback ABI supplies an intentionally unused IRQ number.
    return source[start:pos].replace('int irq, void *data', 'int irq __attribute__((unused)), void *data')+'\n'
shim=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32; typedef int irqreturn_t;
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define BIT(n) (1U<<(n))
#define READ_ONCE(x) (x)
#define atomic_read(p) __atomic_load_n(p,__ATOMIC_SEQ_CST)
#define atomic_xchg(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
#define atomic_or(v,p) ((void)__atomic_fetch_or(p,v,__ATOMIC_SEQ_CST))
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define MT6797_CCCI_QUEUE_COUNT 7
#define MT6797_MD1_CLDMA_GPD_HWO 1
#define MT6797_CCCI_ACTIVE_IRQS 0x007f007fU
#define MT6797_MD1_CLDMA_ACTIVE_QUEUES 127U
#define CLDMA_L2_TX_STATUS 0x410
#define CLDMA_L2_TX_MASK 0x418
#define CLDMA_L2_TX_MASK_CLEAR 0x420
#define CLDMA_L2_TX_MASK_SET 0x428
#define CLDMA_L2_RX_STATUS 0x450
#define CLDMA_L2_RX_MASK 0xc58
#define CLDMA_L2_RX_MASK_CLEAR 0xc60
#define CLDMA_L2_RX_MASK_SET 0xc68
#define CLDMA_UL_STOP_CMD 0x5c
#define CLDMA_SO_STOP_CMD 0x3c4
struct napi_struct {int unused;};
struct mt6797_md1_cldma_rgpd {unsigned gpd_flags;};
struct mt6797_ccci_slot {void *gpd;};
struct mt6797_ccci_queue {struct mt6797_ccci_slot rx[1];unsigned rx_tail;};
struct mt6797_ccci {struct napi_struct napi;u32 tx_pending,rx_pending,tx_mask_owned,rx_mask_owned;
 int failed,hs,diag;void *dev;char *ao,*pdn;pthread_mutex_t tx_stop_lock;struct mt6797_ccci_queue queue[7];};
struct mt6797_md1_cldma_irq_pending {u32 tx_done,rx_done,tx_error,rx_error;};
static struct mt6797_ccci c;static char ao[4096],pdn[4096];static struct mt6797_md1_cldma_rgpd gpd[7];
static unsigned work[7],reclaimed[7];static u32 txmask,rxmask,txstatus,rxstatus,cleared_tx,cleared_rx;
static unsigned completes,schedules,stops,scenario;static bool late_tx;
static __thread bool locked;
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!pthread_mutex_lock(p));locked=true;}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);locked=false;assert(!pthread_mutex_unlock(p));}while(0)
static void mt6797_ccci_quiesce(struct mt6797_ccci *,int);
static int mt6797_ccci_poll(struct napi_struct *,int);
static irqreturn_t mt6797_ccci_cldma_irq(int,void*);
static void mt6797_ccci_first_fatal(struct mt6797_ccci *p,const char *s){(void)p;(void)s;assert(!locked);}
#define dev_err(...) assert(!locked)
static void mt6797_ccci_frontend_ready(struct mt6797_ccci *p,bool r){(void)p;(void)r;assert(!locked);}
static void mt6797_md1_hs_crash(int *p,int r){(void)p;(void)r;assert(!locked);}
static void mt6797_md1_cold_owner_transport_fatal(int r){(void)r;assert(!locked);}
static void writel(u32 value,char *address){
 if(address==pdn+CLDMA_L2_TX_MASK_CLEAR){assert(locked&&!stops);txmask&=~value;cleared_tx|=value;}
 else if(address==ao+CLDMA_L2_RX_MASK_CLEAR){assert(locked&&!stops);rxmask&=~value;cleared_rx|=value;}
 else if(address==pdn+CLDMA_L2_TX_MASK_SET){assert(locked);txmask|=value;}
 else if(address==ao+CLDMA_L2_RX_MASK_SET){assert(locked);rxmask|=value;}
 else if(address==pdn+CLDMA_UL_STOP_CMD||address==pdn+CLDMA_SO_STOP_CMD){assert(locked);stops++;}
 else if(address==pdn+CLDMA_L2_TX_STATUS)txstatus&=~value;
 else if(address==pdn+CLDMA_L2_RX_STATUS)rxstatus&=~value;
 else assert(0);
}
static u32 readl(char *address){
 if(address==pdn+CLDMA_L2_TX_STATUS)return txstatus;
 if(address==pdn+CLDMA_L2_TX_MASK)return txmask;
 if(address==pdn+CLDMA_L2_RX_STATUS)return rxstatus;
 if(address==ao+CLDMA_L2_RX_MASK)return rxmask;
 assert(address==pdn+CLDMA_SO_STOP_CMD);return 0;
}
static void mt6797_ccci_diag_irq(void *d,u32 tx,u32 tm,u32 rx,u32 rm)
{(void)d;(void)tx;(void)tm;(void)rx;(void)rm;assert(!locked);}
static void mt6797_md1_cldma_irq_decode(u32 tx,u32 tm,u32 rx,u32 rm,struct mt6797_md1_cldma_irq_pending *p)
{p->tx_done=tx&~tm&127;p->rx_done=rx&~rm&127;p->tx_error=p->rx_error=0;}
static void napi_schedule_irqoff(struct napi_struct *n){(void)n;assert(!locked);schedules++;}
static void irq(u32 tx,u32 rx){txstatus=tx;rxstatus=rx;assert(mt6797_ccci_cldma_irq(0,&c)==IRQ_HANDLED);}
static void mt6797_ccci_tx_poll(struct mt6797_ccci *p,unsigned q){(void)p;assert(!locked);reclaimed[q]++;}
static int mt6797_ccci_rx_poll(struct mt6797_ccci *p,unsigned q,int budget){(void)p;assert(!locked);
 unsigned done=work[q]<(unsigned)budget?work[q]:(unsigned)budget;work[q]-=done;
 gpd[q].gpd_flags=work[q]?0:1;
 if(late_tx){late_tx=false;irq(2,0);}return done;
}
static void *next_poll(void *budget){mt6797_ccci_poll(&c.napi,(int)(uintptr_t)budget);return NULL;}
static bool napi_complete_done(struct napi_struct *n,int done){(void)n;(void)done;assert(!locked);completes++;
 unsigned s=scenario;scenario=0;
 if(s==1)return false; /* busy/MISSED/deferred false retains masks */
 if(s==2){irq(4,0);return false;} /* concurrent IRQ and MISSED */
 if(s==3){mt6797_ccci_quiesce(&c,-EIO);return true;} /* terminal during GRO flush */
 if(s==4||s==5){ /* completion released SCHED; another CPU claims masks */
  rxmask&=~2U;work[1]=1;gpd[1].gpd_flags=0;irq(0,2);
  pthread_t thread;assert(!pthread_create(&thread,NULL,next_poll,(void*)(uintptr_t)0));
  assert(!pthread_join(thread,NULL));return s==4;
 }
 return true;
}
'''
tests=r'''
static void reset(void){memset(&c,0,sizeof(c));assert(!pthread_mutex_init(&c.tx_stop_lock,NULL));c.ao=ao;c.pdn=pdn;
 memset(work,0,sizeof(work));memset(reclaimed,0,sizeof(reclaimed));for(unsigned q=0;q<7;q++){gpd[q].gpd_flags=1;c.queue[q].rx[0].gpd=&gpd[q];}
 txmask=rxmask=txstatus=rxstatus=cleared_tx=cleared_rx=0;completes=schedules=stops=scenario=0;late_tx=false;}
static void clean(void){assert(!pthread_mutex_destroy(&c.tx_stop_lock));}
int main(void){
 /* Exact C7: nine RX packets, late TX IRQ while polling, then TX-only repoll. */
 reset();work[1]=9;irq(0,2);late_tx=true;
 assert(mt6797_ccci_poll(&c.napi,64)==64&&completes==0&&rxmask==2&&c.rx_mask_owned==2);
 assert(mt6797_ccci_poll(&c.napi,64)==0&&completes==1&&cleared_rx==2&&cleared_tx==2&&!rxmask&&!txmask);clean();
 /* Full budget preserves old TX masks and unvisited RX work. */
 reset();work[1]=64;work[2]=5;irq(2,6);assert(mt6797_ccci_poll(&c.napi,64)==64&&!completes);
 assert(mt6797_ccci_poll(&c.napi,64)==5&&cleared_tx==2&&cleared_rx==6&&!txmask&&!rxmask);clean();
 /* Zero budget processes TX only, retains masked RX and never completes. */
 reset();work[1]=1;irq(2,2);assert(mt6797_ccci_poll(&c.napi,0)==0&&!completes&&work[1]==1);
 assert(mt6797_ccci_poll(&c.napi,64)==1&&cleared_tx==2&&cleared_rx==2);clean();
 for(unsigned s=1;s<=5;s++){
  reset();work[1]=9;irq(2,2);scenario=s;mt6797_ccci_poll(&c.napi,64);
  if(s==3){assert(stops==2&&!cleared_tx&&!cleared_rx);clean();continue;}
  if(s==4)assert(c.rx_mask_owned==2&&c.rx_pending==2&&!(cleared_rx&2));
  else assert(c.rx_mask_owned==2);
  mt6797_ccci_poll(&c.napi,64);assert(!txmask&&!rxmask&&cleared_rx==2);
  if(s==2)assert(cleared_tx==6);
  clean();
 }
 reset();work[1]=1;irq(2,2);mt6797_ccci_quiesce(&c,-EIO);mt6797_ccci_poll(&c.napi,64);
 assert(stops==2&&!cleared_tx&&!cleared_rx);clean();
 reset();c.failed=1;assert(mt6797_ccci_poll(&c.napi,0)==0&&!completes);clean();
 puts("PASS actual IRQ/NAPI/STOP: C7 nine-RX+lateTX loss, full/zero budgets, completionfalse, concurrent IRQ, deferredfalse/cross-CPU nextpoll ownership, terminal no-rearm");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-napi-mask-') as d:
    p=Path(d);(p/'test.c').write_text(shim+body('mt6797_ccci_quiesce_from(')+body('mt6797_ccci_quiesce(')+body('mt6797_ccci_poll(')+body('mt6797_ccci_cldma_irq(')+tests)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
