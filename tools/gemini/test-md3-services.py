#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual ordered MD3 service owner against bounded transport/FS suppliers."""
import os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'drivers/soc/mediatek/mt6797-md3-services.c'
SHIM=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32; typedef int64_t s64;
#define __iomem
#define BIT(n) (1U<<(n))
#define GFP_KERNEL 0
#define WQ_MEM_RECLAIM 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
#define msecs_to_jiffies(n) (n)
typedef int atomic_t;
static int atomic_read(atomic_t*p){return __atomic_load_n(p,__ATOMIC_SEQ_CST);}
static void atomic_set(atomic_t*p,int v){__atomic_store_n(p,v,__ATOMIC_SEQ_CST);}
static int atomic_cmpxchg(atomic_t*p,int o,int n){__atomic_compare_exchange_n(p,&o,n,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return o;}
struct mutex{pthread_mutex_t m;};
static void mutex_init(struct mutex*m){assert(!pthread_mutex_init(&m->m,0));}
static void mutex_lock(struct mutex*m){assert(!pthread_mutex_lock(&m->m));}
static void mutex_unlock(struct mutex*m){assert(!pthread_mutex_unlock(&m->m));}
struct device{int dummy;};struct platform_device{struct device dev;};
struct mt6797_md3_ccif{int dummy;};struct mt6797_md3_execution{int dummy;};
struct work_struct{int dummy;};struct delayed_work{struct work_struct work;};struct workqueue_struct{int dummy;};
struct mt6797_md3_lk_snapshot{bool c2k_lte_mode_present;u32 c2k_lte_mode,image_type;};
#define MT6797_FS_INDEX_COUNT 5
#define MT6797_FS_PREFIX 20
#define MT6797_FS_FRAGMENT 3456
#define MT6797_FS_MD3_REQUEST_MAX 0x40dc
#define MT6797_FS_REPLY_MAX 0x4800
#define MT6797_FS_RX 14
#define MD3_RING_FRAME_MAX 4096
#define MD3_RING_BYTES 0xb6950
struct mt6797_fs_service{bool claimed;};
struct mt6797_fs_reply{u8*payload;size_t capacity;};
static struct platform_device pdev;static struct mt6797_md3_ccif ccif;static struct mt6797_md3_execution execution;
static struct workqueue_struct wq;
static struct mt6797_md3_lk_snapshot snapshot={true,2,2};
static void*allocations[20];static unsigned allocations_used,point,fail_point,scheduled,delay,stops,releases,resets,starts,publications,tx_calls,fs_calls,claim_calls,consumed,cases;
static int flow_queue=-1,flow_error;static unsigned flow_calls;
static int lk_error,claim_error,start_error,query_error,stream_error,publish_result=1,fs_error,append_result=1,frames=1,tx_result,receive_error;
static unsigned query_mutation,tx_busy,receive_calls,receive_late,send_late;static bool receive_wdt;
static bool stream_on,running,worker_running,block_fs,fs_entered,release_fs,shutdown_entered;
static s64 now;
static pthread_t worker_thread;
static pthread_mutex_t cond_lock=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static void (*cleanup_action)(void*);static void*cleanup_data;
static void (*irq_callback)(void*,bool,void*);static void*irq_data;
static u8 queued[8][80][4096];static unsigned lengths[8][80],head[8],tail[8];
static u32 sent_channels[20];
static int tick(void){return ++point==fail_point;}
static s64 ktime_get(void){return now;}static s64 ktime_to_ms(s64 v){return v;}
static void usleep_range(unsigned a,unsigned b){assert(a==500&&b==1000);now++;}
static u32 get_unaligned_le32(const void*p){u32 v;memcpy(&v,p,4);return v;}
static void put_unaligned_le32(u32 v,void*p){memcpy(p,&v,4);}
static u32 readl(const void*p){return get_unaligned_le32(p);}static void writel(u32 v,void*p){put_unaligned_le32(v,p);}
static int sysfs_emit(char*b,const char*f,...){va_list a;va_start(a,f);int n=vsnprintf(b,4096,f,a);va_end(a);return n;}
static int mt6797_md3_lk_handoff_get(struct mt6797_md3_lk_snapshot*s){*s=snapshot;return lk_error;}
static void*devm_kmalloc(struct device*d,size_t n,int g){assert(d==&pdev.dev&&g==0);if(tick())return 0;assert(allocations_used<20);void*p=calloc(1,n);assert(p);allocations[allocations_used++]=p;return p;}
#define devm_kzalloc devm_kmalloc
#define INIT_DELAYED_WORK(w,f) ((void)(w),(void)(f))
static struct workqueue_struct*alloc_ordered_workqueue(const char*n,int f){assert(!strcmp(n,"mt6797-md3-services")&&f==1);return tick()?0:&wq;}
static void destroy_workqueue(struct workqueue_struct*w){assert(w==&wq&&!worker_running);}
static int devm_add_action_or_reset(struct device*d,void(*fn)(void*),void*p){assert(d==&pdev.dev);if(tick()){fn(p);return -ENOMEM;}cleanup_action=fn;cleanup_data=p;return 0;}
static void mod_delayed_work(struct workqueue_struct*w,struct delayed_work*d,unsigned ms){assert(w==&wq&&d);scheduled++;delay=ms;}
static void cancel_delayed_work_sync(struct delayed_work*w){assert(w);if(worker_running){assert(!pthread_join(worker_thread,0));worker_running=false;}}
static int mt6797_fs_service_init_md3(struct mt6797_fs_service*f){claim_calls++;if(claim_error)return claim_error;assert(!f->claimed);f->claimed=true;return 0;}
static void mt6797_fs_service_release(struct mt6797_fs_service*f){assert(!fs_entered||release_fs);if(f->claimed)releases++;f->claimed=false;}
static int mt6797_md3_execution_start(struct mt6797_md3_execution*e){assert(e==&execution);starts++;if(start_error)return start_error;running=true;return 0;}
static void mt6797_md3_execution_shutdown(struct mt6797_md3_execution*e){assert(e==&execution&&!stream_on);resets++;running=false;}
static int mt6797_md3_execution_query(struct mt6797_md3_execution*e,u8*q,u16*seq){assert(e==&execution&&running);const u32 words[16]={50463234,50529026,50528771,196610,0,768,3};memcpy(q,words,64);*seq=0;if(query_mutation)q[14]=2;return query_error;}
static int mt6797_md3_ccif_stream_start(struct mt6797_md3_ccif*c,void(*fn)(void*,bool,void*),void*d){assert(c==&ccif&&running);if(stream_error)return stream_error;stream_on=true;irq_callback=fn;irq_data=d;return 0;}
static void mt6797_md3_ccif_stream_stop(struct mt6797_md3_ccif*c){assert(c==&ccif);stops++;stream_on=false;irq_callback=0;irq_data=0;pthread_mutex_lock(&cond_lock);shutdown_entered=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&cond_lock);}
static int mt6797_md3_ccif_publish_runtime(struct mt6797_md3_ccif*c,const void*r,size_t n,const void*q,size_t m){assert(c==&ccif&&stream_on&&running&&n==584&&m==116);assert(get_unaligned_le32((u8*)q+8)==1);assert(((u8*)r)[0]==0);publications++;return publish_result;}
static int mt6797_md3_ccif_receive(struct mt6797_md3_ccif*c,unsigned q,void*f,size_t n){assert(c==&ccif&&stream_on&&running&&q<8&&n==4096);receive_calls++;if(receive_error)return receive_error;if(head[q]==tail[q])return 0;unsigned i=head[q]++;memcpy(f,queued[q][i],lengths[q][i]);consumed++;if(receive_late==consumed)now+=30001;if(receive_wdt)irq_callback(irq_data,true,0);return lengths[q][i];}
static int mt6797_fs_request_append_md3(u8*a,size_t*u,const void*f,size_t n){assert((get_unaligned_le32((u8*)f+8)&0xffff)==14&&n>=24);if(append_result>0){memcpy(a,f,n);*u=n;}return append_result;}
static int mt6797_fs_service_request(struct mt6797_fs_service*f,const void*a,size_t n,struct mt6797_fs_reply*r,s32*s){assert(f->claimed&&n>=24&&r->capacity==MT6797_FS_REPLY_MAX);fs_calls++;if(block_fs){pthread_mutex_lock(&cond_lock);fs_entered=true;pthread_cond_broadcast(&changed);while(!release_fs)pthread_cond_wait(&changed,&cond_lock);pthread_mutex_unlock(&cond_lock);}*s=-17;return fs_error;}
static unsigned mt6797_fs_reply_frames(const struct mt6797_fs_reply*r){assert(r->payload);return frames;}
static int mt6797_fs_reply_frame(const struct mt6797_fs_reply*r,unsigned i,void*f,size_t n){assert(i<(unsigned)frames&&n==4096);memset(f,0,24);put_unaligned_le32(15,(u8*)f+8);return 24;}
static int mt6797_md3_ccif_transmit(struct mt6797_md3_ccif*c,unsigned q,const void*f,size_t n){assert(c==&ccif&&stream_on&&((q==4&&n==24)||(q==0&&n==16)));if(q==0)assert(get_unaligned_le32(f)==UINT32_MAX&&get_unaligned_le32((u8*)f+4)==0x205&&get_unaligned_le32((u8*)f+12)==(unsigned)flow_queue);tx_calls++;if(tx_busy){tx_busy--;return -EAGAIN;}if(tx_result)return tx_result;assert(tx_calls<20);sent_channels[tx_calls-1]=get_unaligned_le32((u8*)f+8);if(send_late)now+=1001;return n;}
static int mt6797_md3_ccif_rx_resume_needed(struct mt6797_md3_ccif*c,unsigned q,unsigned*a){assert(c==&ccif&&stream_on&&running);flow_calls++;if(flow_error)return flow_error;if(flow_queue==(int)q)return 1;*a=0;return 0;}
'''
TEST=r'''
static void cleanup(void){if(cleanup_action)cleanup_action(cleanup_data);cleanup_action=0;cleanup_data=0;for(unsigned i=0;i<allocations_used;i++)free(allocations[i]);allocations_used=0;}
static void init(void){cleanup();flow_queue=-1;flow_error=0;flow_calls=0;point=fail_point=scheduled=delay=stops=releases=resets=starts=publications=tx_calls=fs_calls=claim_calls=consumed=0;lk_error=claim_error=start_error=query_error=stream_error=fs_error=tx_result=receive_error=0;publish_result=append_result=frames=1;query_mutation=tx_busy=receive_calls=receive_late=send_late=0;receive_wdt=false;stream_on=running=worker_running=block_fs=fs_entered=release_fs=shutdown_entered=false;now=0;snapshot=(struct mt6797_md3_lk_snapshot){true,2,2};memset(head,0,sizeof(head));memset(tail,0,sizeof(tail));memset(sent_channels,0,sizeof(sent_channels));}
static struct mt6797_md3_services*acquire(void){struct mt6797_md3_services*s=0;assert(!mt6797_md3_services_acquire(&pdev,&ccif,&execution,&s));assert(s&&!starts&&!claim_calls&&!publications);return s;}
static struct mt6797_md3_services*start(void){init();struct mt6797_md3_services*s=acquire();assert(!mt6797_md3_services_claim(s)&&claim_calls==1);assert(mt6797_md3_services_claim(s)==-EALREADY);assert(!mt6797_md3_services_start(s)&&s->runtime_sent&&running&&stream_on&&publications==1);return s;}
static void enqueue(unsigned q,unsigned channel,u32 seq,u32 ctrl,u32 res,unsigned len){assert(q<8&&tail[q]<80&&len>=16);u8*p=queued[q][tail[q]];memset(p,0,len);put_unaligned_le32(ctrl,p+4);put_unaligned_le32(channel|seq,p+8);put_unaligned_le32(res,p+12);if(len>=24)put_unaligned_le32(0x1001,p+16);lengths[q][tail[q]++]=len;}
static void work(struct mt6797_md3_services*s){md3_services_work(&s->work.work);cases++;}
static void failed(struct mt6797_md3_services*s,int e){assert(s->stopped&&s->error==e&&!running&&!stream_on&&!s->fs_pending&&!s->fs.claimed);unsigned r=resets;work(s);mt6797_md3_services_shutdown(s);assert(resets==r);}
static void *worker(void*p){work(p);return 0;}
static void *shutdown_worker(void*p){mt6797_md3_services_shutdown(p);return 0;}
int main(void){
 init();struct mt6797_md3_services*s=acquire();unsigned n=point;assert(mt6797_md3_services_start(s)==-ENODEV&&!starts);cleanup();
 for(unsigned i=1;i<=n;i++){init();fail_point=i;s=(void*)1;assert(mt6797_md3_services_acquire(&pdev,&ccif,&execution,&s)==-ENOMEM&&s==(void*)1&&!starts&&!claim_calls&&!publications);cases++;}
 for(unsigned i=0;i<3;i++){init();if(i==0)snapshot.c2k_lte_mode_present=false;if(i==1)snapshot.c2k_lte_mode=1;if(i==2)snapshot.image_type=12;s=0;assert(mt6797_md3_services_acquire(&pdev,&ccif,&execution,&s)==-EOPNOTSUPP&&!allocations_used);cases++;}
 init();s=acquire();claim_error=-EACCES;assert(mt6797_md3_services_claim(s)==-EACCES&&!s->fs_claimed&&!starts);cases++;
 for(unsigned i=0;i<6;i++){init();s=acquire();assert(!mt6797_md3_services_claim(s));if(i==0)start_error=-EIO;if(i==1)query_error=-EPIPE;if(i==2)query_mutation=1;if(i==3)stream_error=-EBUSY;if(i==4)publish_result=-EAGAIN;if(i==5)publish_result=0;int e=mt6797_md3_services_start(s);assert(e<0);failed(s,e);assert(mt6797_md3_services_start(s)==-EALREADY);cases++;}
 s=start();work(s);assert(!s->stopped&&!s->hs2&&delay==20);now=30000;work(s);failed(s,-ETIMEDOUT);
 s=start();enqueue(0,0,BIT(31)|(1U<<16),0,0,16);work(s);assert(s->hs2&&!s->stopped);now=100000;work(s);assert(!s->stopped);enqueue(0,0,BIT(31)|(2U<<16),0,0,16);work(s);failed(s,-EPROTO);
 s=start();enqueue(0,0,BIT(31)|(1U<<16),0,0,16);receive_late=1;work(s);failed(s,-ETIMEDOUT);assert(!s->hs2);
 s=start();for(unsigned i=0;i<70;i++)enqueue(0,0,(i<<16)|BIT(31),0x201,0,16);work(s);assert(consumed==64&&!s->stopped);work(s);assert(consumed==70&&!s->stopped);cases++;
 s=start();for(unsigned i=0;i<70;i++)enqueue(0,0,(i<<16)|BIT(31),0x201,0,16);receive_late=64;work(s);failed(s,-ETIMEDOUT);assert(consumed==64);
 s=start();enqueue(0,0,BIT(31)|(1U<<16),0x202,0,16);enqueue(0,0,BIT(31)|(3U<<16),0x205,0,16);work(s);failed(s,-EILSEQ);
 s=start();enqueue(0,0,0,0x202,0,16);enqueue(0,0,BIT(31)|(9U<<16),0x205,0,16);work(s);assert(!s->stopped&&s->rx_sequence[0]==9);cases++;
 for(unsigned i=0;i<5;i++){s=start();if(i==0)enqueue(1,0,0,0,0,16);if(i==1)enqueue(4,14,0,0,0,24);if(i==2)enqueue(0,0,0,0,0x5555ffff,16);if(i==3)enqueue(0,0,0,0,0,24);if(i==4)enqueue(4,4,0,0,5,24);work(s);assert(s->stopped&&!fs_calls);cases++;}
 s=start();enqueue(4,4,BIT(31),0,0,24);frames=3;tx_busy=2;work(s);assert(!s->stopped&&fs_calls==1&&s->fs_total==1&&s->fs_replies==1&&s->tx_fs_sequence==3&&!s->used[0]);assert(sent_channels[2]==(4U|BIT(31))&&sent_channels[3]==(4U|BIT(31)|(1U<<16))&&sent_channels[4]==(4U|BIT(31)|(2U<<16)));cases++;
 s=start();enqueue(0,0,0,0,0,16);enqueue(4,4,BIT(31),0,0,24);work(s);assert(s->hs2&&sent_channels[0]==4);cases++;

 s=start();flow_queue=4;enqueue(4,4,BIT(31),0,0,24);work(s);assert(!flow_calls&&!s->flow_resumes);enqueue(0,0,0,0,0,16);enqueue(4,4,BIT(31)|(1U<<16),0,0,24);work(s);assert(!s->stopped&&s->flow_resumes==1&&s->tx_control_sequence==1&&s->resume_attempts[4]==1&&sent_channels[2]==BIT(31));cases++;
 s=start();flow_queue=0;enqueue(0,0,0,0,0,16);send_late=1;work(s);failed(s,-ETIMEDOUT);assert(s->hs2&&s->flow_resumes==1&&s->tx_control_sequence==1&&s->resume_attempts[0]==1);cases++;
 s=start();flow_queue=0;enqueue(0,0,0,0,0,16);tx_result=-EIO;work(s);failed(s,-EIO);assert(!s->flow_resumes&&!s->tx_control_sequence&&!s->resume_attempts[0]);cases++;
 s=start();flow_error=-EPROTO;enqueue(0,0,0,0,0,16);work(s);failed(s,-EPROTO);cases++;
 /* Queue0 pressure must leave the next turn available to real FS queue4. */
 s=start();for(unsigned i=0;i<70;i++)enqueue(0,0,(i<<16)|BIT(31),0x201,0,16);enqueue(4,4,BIT(31),0,0,24);work(s);assert(!fs_calls);head[0]=tail[0]=0;for(unsigned i=64;i<134;i++)enqueue(0,0,(i<<16)|BIT(31),0x201,0,16);work(s);assert(fs_calls==1&&!s->stopped);cases++;
 s=start();append_result=0;enqueue(4,4,BIT(31),0,0,24);work(s);assert(!fs_calls&&!s->fs_total&&!s->stopped);cases++;
 s=start();fs_error=-EIO;enqueue(4,4,BIT(31),0,0,24);work(s);failed(s,-EIO);assert(s->fs_total==1&&!s->fs_replies&&!tx_calls);
 s=start();tx_result=-EIO;enqueue(4,4,BIT(31),0,0,24);work(s);failed(s,-EIO);assert(s->fs_total==1&&!s->fs_replies&&tx_calls==1&&!s->tx_fs_sequence);
 s=start();tx_busy=2000;enqueue(4,4,BIT(31),0,0,24);work(s);failed(s,-ETIMEDOUT);assert(now==1000&&!s->tx_fs_sequence&&!s->fs_replies);
 s=start();send_late=1;enqueue(4,4,BIT(31),0,0,24);work(s);failed(s,-ETIMEDOUT);assert(s->tx_fs_sequence==1&&s->fs_replies==1&&tx_calls==1);
 s=start();receive_wdt=true;enqueue(4,4,BIT(31),0,0,24);work(s);failed(s,-EIO);assert(!fs_calls&&!s->fs_total);
 s=start();receive_error=-EPROTO;work(s);failed(s,-EPROTO);
 for(unsigned i=0;i<4;i++){s=start();u8 ap[32]={0};put_unaligned_le32(i==0?BIT(12):i==1?0:i==2?BIT(7):BIT(12),ap+16);md3_services_irq(s,i==3,ap);work(s);if(i==0)assert(!s->stopped&&s->ccif_irqs==1&&get_unaligned_le32(ap+20)==BIT(12));else failed(s,i==3?-EIO:-EPROTO);cases++;}
 /* Actual worker/teardown concurrency: cancellation drains active durable FS
  * before handle release, blocks a reply once stopping, and resets only once. */
 s=start();block_fs=true;enqueue(4,4,BIT(31),0,0,24);worker_running=true;assert(!pthread_create(&worker_thread,0,worker,s));pthread_mutex_lock(&cond_lock);while(!fs_entered)pthread_cond_wait(&changed,&cond_lock);pthread_mutex_unlock(&cond_lock);
 pthread_t shut;assert(!pthread_create(&shut,0,shutdown_worker,s));pthread_mutex_lock(&cond_lock);while(!shutdown_entered)pthread_cond_wait(&changed,&cond_lock);assert(s->fs.claimed&&atomic_read(&s->stopping)&&running&&!releases);release_fs=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&cond_lock);assert(!pthread_join(shut,0));assert(!worker_running&&s->stopped&&!running&&!stream_on&&releases==1&&resets==1&&tx_calls==0);cases++;
 char text[4096];assert(mt6797_md3_services_show(s,text)>0&&strstr(text,"css_ready=0")&&strstr(text,"fs_total=1"));cleanup();printf("PASS %u actual-source MD3 services admission/frame/deadline/publication/IRQ/FS-drain cases\n",cases);
}
'''
def clean(s):return re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
def run(s,label,success):
 with tempfile.TemporaryDirectory(prefix='md3-services-') as t:
  c=Path(t)/'test.c';out=Path(t)/'test';c.write_text(SHIM+clean((ROOT/'drivers/soc/mediatek/mt6797-md3-runtime.h').read_text())+clean(s)+TEST)
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-o',str(out),str(c)],check=True)
  r=subprocess.run([str(out)],capture_output=True,timeout=20)
  if success:
   if r.returncode:raise SystemExit(r.stderr.decode())
   print(r.stdout.decode().strip())
  elif not r.returncode:raise SystemExit('negative control survived: '+label)
  else:print('REJECTED '+label)
def main():
 s=SOURCE.read_text();run(s,'actual',True)
 for name,a,b in [
  ('WDT-after-RX-ignored','if (atomic_read(&s->fatal) || atomic_read(&s->stopping)) {','if (false) {'),
  ('flow-sequence-not-consumed','s->tx_control_sequence = (s->tx_control_sequence + 1) & 0x7fff;','/* lost control sequence */'),
  ('queue0-starves-FS','queue = (s->next_queue + n) & 7;','queue = n;'),
  ('false-HS2','s->hs2 = true;', 's->hs2 = false;'),
  ('missing-terminal-reset','mt6797_md3_execution_shutdown(s->execution);','/* lost reset */'),
  ('retry-published-FS-sequence','s->tx_fs_sequence = (s->tx_fs_sequence + 1) & 0x7fff;','/* lost publication */'),
  ('lost-RX-sequence','s->rx_sequence[port] = seq;', '/* no update */'),
  ('late-HS2-admission','if (!s->hs2 && ktime_to_ms(ktime_get()) >= s->boot_deadline) {','if (false) {'),
  ('no-shutdown-fence','atomic_set(&s->stopping, 1);','/* no fence */'),
 ]:
  pattern=r'\s+'.join(re.escape(word) for word in a.split())
  changed,count=re.subn(pattern,lambda _:b,s);assert count,name;run(changed,name,False)
if __name__=='__main__':main()
