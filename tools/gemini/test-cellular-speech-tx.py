#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run actual native speech-send/TX/STOP code with DMA/MMIO boundaries."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
directory = root / "drivers/net/wwan/mediatek"
source_path = directory / "mtk_mt6797_ccci.c"
common_path = directory / "mtk_ccci_common.c"
source = source_path.read_text()
common = common_path.read_text()


def function(text, name):
    start = text.index(name)
    start = text.rfind("\n", 0, start) + 1
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


shim = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
typedef u32 __le32;typedef uintptr_t dma_addr_t;
#define GFP_KERNEL 0
#define U16_MAX UINT16_MAX
#define BIT(n) (1U<<(n))
#define atomic_read(p) __atomic_load_n(p,__ATOMIC_SEQ_CST)
#define atomic_xchg(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
#define atomic_dec(p) ((void)__atomic_sub_fetch(p,1,__ATOMIC_SEQ_CST))
#define cpu_to_le32(v) (v)
#define le32_to_cpu(v) (v)
#define FIELD_PREP(mask,val) ((val)<<__builtin_ctz(mask))
#define MTK_CCCI_H_AST_BIT (1U<<31)
#define MTK_CCCI_H_SEQ_MASK 0x7fffU
#define MTK_CCCI_H_CHN_MASK 0xffffU
#define MTK_CCCI_H_SEQ_SHIFT 16
#define MTK_CCCI_STATUS_SEQ_SHIFT 16
#define MTK_CCCI_STATUS_ASSERT (1U<<31)
#define MTK_CCCI_HEADER_SIZE 16
#define MTK_CCCI_CONTROL_TX 1
#define MT6797_RPC_TX 33
#define MT6797_FS_TX 15
#define MT6797_FS_INDEX_COUNT 5
#define MT6797_CCCI_QUEUE_COUNT 7
#define MT6797_CCCI_CHANNEL_COUNT 256
#define MT6797_CCCI_RING_SIZE 16
#define MT6797_CCCI_ACTIVE_IRQS 0x007f007fU
#define MT6797_MD1_CLDMA_ACTIVE_QUEUES 127U
#define CLDMA_L2_TX_MASK_SET 0x428
#define CLDMA_L2_RX_MASK_SET 0xc68
#define CLDMA_UL_STOP_CMD 0x5c
#define CLDMA_SO_STOP_CMD 0x3c4
#define CLDMA_UL_RESUME_CMD 0x58
#define DMA_TO_DEVICE 1
struct mtk_ccci_header {__le32 data0,data1,status,reserved;};
struct sk_buff {u8 *data;size_t len;};
struct mt6797_ccci_slot {void *gpd;struct sk_buff *skb;dma_addr_t skb_dma;bool mapped;};
struct mt6797_ccci_queue {struct mt6797_ccci_slot tx[16];unsigned tx_head,tx_tail,tx_used;pthread_mutex_t tx_lock;};
struct mt6797_ccci {
 struct mt6797_ccci_queue queue[7];pthread_mutex_t tx_stop_lock;
 int tx_sequence[256],failed,ready,teardown,diag,hs;void *dev,*speech;char *pdn,*ao;
 unsigned long fs_indices,rpc_indices;
};
static struct mt6797_ccci c;static char ao[4096],pdn[4096];static u8 descriptors[7][16][16];
static int mode,allocs,frees,maps,unmaps,publishes,resumes,stops,speech_stops;
static pthread_t stopper;static bool stopper_started;static __thread bool stop_locked;
static void lock(pthread_mutex_t *p){assert(!pthread_mutex_lock(p));if(p==&c.tx_stop_lock)stop_locked=true;}
static void unlock(pthread_mutex_t *p){if(p==&c.tx_stop_lock)stop_locked=false;assert(!pthread_mutex_unlock(p));}
#define spin_lock(p) lock(p)
#define spin_unlock(p) unlock(p)
#define spin_lock_irqsave(p,f) do{(f)=0;lock(p);}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);unlock(p);}while(0)
static void clear_bit(unsigned bit,unsigned long *p){*p&=~(1UL<<bit);}
static u16 mt6797_ccci_next_sequence(struct mt6797_ccci *p,u16 channel){return __atomic_fetch_add(&p->tx_sequence[channel],1,__ATOMIC_SEQ_CST);}
static struct sk_buff *alloc_skb(size_t n,int flags){(void)flags;if(mode==1)return NULL;
 assert(!stop_locked);struct sk_buff *s=malloc(sizeof(*s));assert(s);s->data=malloc(n);assert(s->data);s->len=0;allocs++;return s;}
static void skb_put_data(struct sk_buff *s,const void *data,size_t n){memcpy(s->data,data,n);s->len=n;}
static void kfree_skb(struct sk_buff *s){if(s){assert(!stop_locked);free(s->data);free(s);frees++;}}
static dma_addr_t dma_map_single(void *dev,void *data,size_t n,int direction){(void)dev;(void)data;(void)n;(void)direction;
 assert(!stop_locked);if(mode==2)return 0;maps++;return 0x1234;}
static bool dma_mapping_error(void *dev,dma_addr_t a){(void)dev;return !a;}
static void dma_unmap_single(void *dev,dma_addr_t a,size_t n,int direction){(void)dev;(void)n;(void)direction;assert(a==0x1234&&!stop_locked);unmaps++;}
static void mt6797_ccci_quiesce(struct mt6797_ccci *,int);
static void *fail_thread(void *arg){(void)arg;mt6797_ccci_quiesce(&c,-EIO);return NULL;}
static int mt6797_md1_cldma_tgpd_prepare_tx(void *g,dma_addr_t a,size_t n){(void)g;(void)a;(void)n;
 assert(!stop_locked);if(mode==3)return -EIO;if(mode==5)mt6797_ccci_quiesce(&c,-EIO);return 0;}
static int mt6797_md1_cldma_tgpd_publish(void *g){(void)g;assert(stop_locked);if(mode==4)return -EIO;
 if(mode==6){assert(!pthread_create(&stopper,NULL,fail_thread,NULL));stopper_started=true;while(!atomic_read(&c.failed))sched_yield();}
 publishes++;return 0;}
static void writel(u32 value,char *address){assert(stop_locked);
 if(address==pdn+CLDMA_UL_RESUME_CMD){assert(value==1&&!stops);resumes++;}
 else if(address==pdn+CLDMA_UL_STOP_CMD||address==pdn+CLDMA_SO_STOP_CMD)stops++;
}
static u32 readl(char *p){(void)p;assert(stop_locked);return 0;}
static void mt6797_ccci_at_rx_close(struct mt6797_ccci *p,bool terminal){(void)p;assert(terminal);}
static void mt6797_speech_port_stop(void *port,int reason){(void)port;(void)reason;speech_stops++;}
static void mt6797_ccci_first_fatal(struct mt6797_ccci *p,const char *site){(void)p;(void)site;assert(!stop_locked);}
static void mt6797_ccci_frontend_ready(struct mt6797_ccci *p,bool ready){__atomic_store_n(&p->ready,ready,__ATOMIC_SEQ_CST);}
static void mt6797_md1_hs_crash(int *hs,int reason){(void)hs;(void)reason;assert(!stop_locked);}
static void mt6797_md1_cold_owner_transport_fatal(int reason){(void)reason;assert(!stop_locked);}
static void mt6797_ccci_at_terminal_queue(struct mt6797_ccci *p){(void)p;assert(!stop_locked);}
static void mt6797_ccci_diag_final_reply(int *diag,const u8 *data,size_t n){(void)diag;(void)data;(void)n;assert(!stop_locked);}
static void mt6797_ccci_diag_tx(int *diag,bool rx,unsigned h,unsigned t,unsigned used){(void)diag;(void)rx;(void)h;(void)t;(void)used;assert(!stop_locked);}
#define dev_err(...) assert(!stop_locked)
'''

tests = r'''
static void reset(int failure){
 memset(&c,0,sizeof(c));c.ready=1;c.ao=ao;c.pdn=pdn;c.dev=&c;c.tx_sequence[5]=17;
 assert(!pthread_mutex_init(&c.tx_stop_lock,NULL));
 for(unsigned q=0;q<7;q++){assert(!pthread_mutex_init(&c.queue[q].tx_lock,NULL));for(unsigned i=0;i<16;i++)c.queue[q].tx[i].gpd=descriptors[q][i];}
 mode=failure;allocs=frees=maps=unmaps=publishes=resumes=stops=speech_stops=0;stopper_started=false;
}
static void clean(void){
 if(stopper_started)assert(!pthread_join(stopper,NULL));
 for(unsigned q=0;q<7;q++){
  for(unsigned i=0;i<16;i++){struct mt6797_ccci_slot *s=&c.queue[q].tx[i];if(s->mapped)dma_unmap_single(c.dev,s->skb_dma,s->skb->len,DMA_TO_DEVICE);kfree_skb(s->skb);}
  assert(!pthread_mutex_destroy(&c.queue[q].tx_lock));
 }
 assert(!pthread_mutex_destroy(&c.tx_stop_lock));assert(allocs==frees&&maps==unmaps);
}
static void assert_frame(const u8 *frame,size_t n){
 struct sk_buff *sent=c.queue[0].tx[0].skb;assert(sent&&sent->len==n);
 assert(mt6797_speech_read32(sent->data)==mt6797_speech_read32(frame));
 assert(mt6797_speech_read32(sent->data+4)==mt6797_speech_read32(frame+4));
 assert(mt6797_speech_read32(sent->data+8)==0x80110005);
 assert(mt6797_speech_read32(sent->data+12)==mt6797_speech_read32(frame+12));
 if(n>16)assert(!memcmp(sent->data+16,frame+16,n-16));
 assert(c.tx_sequence[5]==18&&publishes==1&&resumes==1&&c.queue[0].tx_used==1);
}
int main(void){
 u8 frame[MT6797_SPEECH_FRAME_MAX];int n=mt6797_speech_build_power(frame,sizeof(frame),true,0x4321);assert(n==16);
 mt6797_speech_write32(frame+4,0x2f201234);mt6797_speech_write32(frame+12,0xfeedbeef);
 for(int failure=0;failure<=6;failure++){
  reset(failure);int ret=mt6797_ccci_speech_send(&c,frame,n);
  if(failure>=1&&failure<=5){assert(ret==(failure==1?-ENOMEM:failure==5?-ESHUTDOWN:-EIO));
   assert(!c.queue[0].tx_used&&!c.queue[0].tx[0].mapped&&!c.queue[0].tx[0].skb&&!publishes&&!resumes&&c.tx_sequence[5]==17);
  }else{assert(!ret);assert_frame(frame,n);}
  clean();if(failure==5||failure==6)assert(stops==2&&speech_stops==1);
 }
 reset(0);c.queue[0].tx_used=16;assert(!mt6797_ccci_speech_room(&c));
 assert(mt6797_ccci_speech_send(&c,frame,n)==-EBUSY&&c.tx_sequence[5]==17&&!publishes&&!maps&&allocs==frees);clean();
 for(int gate=0;gate<3;gate++){reset(0);if(gate==0)c.ready=0;else if(gate==1)c.failed=1;else c.teardown=1;
  assert(!mt6797_ccci_speech_room(&c)&&mt6797_ccci_speech_send(&c,frame,n)==-ESHUTDOWN&&!allocs);clean();}
 u8 parameters[MT6797_SPEECH_PARAMETERS_MAX];for(unsigned i=0;i<sizeof(parameters);i++)parameters[i]=(u8)(i*37+11);
 n=mt6797_speech_build_parameters(frame,sizeof(frame),parameters,sizeof(parameters),0x7fff);assert(n==sizeof(frame));
 reset(0);assert(mt6797_ccci_speech_room(&c));assert(!mt6797_ccci_speech_send(&c,frame,n));assert_frame(frame,n);clean();
 puts("PASS: actual speech TX preserves mailbox/data IDs, native sequence, DMA unwind and publish-before-STOP");
}
'''

parts = [shim, (directory / "mtk_mt6797_ccci_speech.h").read_text(),
         function(common, "mtk_ccci_header_init("),
         function(source, "mt6797_ccci_quiesce_from("),
         function(source, "mt6797_ccci_quiesce("),
         function(source, "mt6797_ccci_tx("),
         function(source, "mt6797_ccci_tx_has_room("),
         function(source, "mt6797_ccci_speech_send("),
         function(source, "mt6797_ccci_speech_room("), tests]
with tempfile.TemporaryDirectory(prefix="speech-native-tx-") as temporary:
    path = Path(temporary)
    (path / "fixture.c").write_text("\n".join(parts))
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-pthread", str(path / "fixture.c"), "-o", str(path / "fixture")],
                   check=True, timeout=30)
    subprocess.run([str(path / "fixture")], check=True, timeout=30)
print(json.dumps({"result": "PASS", "scope": "actual-source DMA/MMIO adapters; no hardware claim",
                  "sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in [source_path, common_path, Path(__file__)]}}, sort_keys=True))
