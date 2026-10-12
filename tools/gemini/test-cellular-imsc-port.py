#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual native IMSC endpoint under concurrent syscall/lifetime boundaries."""
import argparse,hashlib,json,os,subprocess,tempfile
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--negative-controls',action='store_true');args=parser.parse_args()
root=Path(__file__).resolve().parents[2];directory=root/'drivers/net/wwan/mediatek'
source=directory/'mtk_mt6797_ccci_imsc_port.c';header=directory/'mtk_mt6797_ccci_imsc_port.h'
actual='\n'.join(l for l in source.read_text().splitlines()if not l.startswith('#include'))
shim = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef unsigned __poll_t;
typedef int poll_table;
#define __user
#define THIS_MODULE NULL
#define GFP_KERNEL 0
#define MISC_DYNAMIC_MINOR 255
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define PTR_ERR(x) ((int)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x) >= (uintptr_t)-4095)
struct device { int unused; }; struct inode { int unused; };
struct file { void *private_data; int f_flags; };
struct file_operations {
 void *owner;
 int (*open)(struct inode *, struct file *);
 int (*release)(struct inode *, struct file *);
 ssize_t (*read)(struct file *, char *, size_t, loff_t *);
 ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
 __poll_t (*poll)(struct file *, poll_table *);
};
struct miscdevice { int minor; const char *name; const struct file_operations *fops;
 struct device *parent; unsigned mode; bool registered; };
static pthread_mutex_t misc_mutex = PTHREAD_MUTEX_INITIALIZER;
static int register_error;
static int misc_register(struct miscdevice *m) {
 assert(!pthread_mutex_lock(&misc_mutex));
 int ret = register_error; m->registered = !ret;
 assert(!pthread_mutex_unlock(&misc_mutex)); return ret;
}
static void misc_deregister(struct miscdevice *m) {
 assert(!pthread_mutex_lock(&misc_mutex)); m->registered = false;
 assert(!pthread_mutex_unlock(&misc_mutex));
}
static int nonseekable_open(struct inode *i, struct file *f) {
 (void)i; (void)f; return 0;
}
struct mutex { pthread_mutex_t lock; };
static void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->lock,NULL)); }
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->lock)); }
static int mutex_lock_interruptible(struct mutex *m) { mutex_lock(m); return 0; }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->lock)); }
#define spin_lock_irqsave(p,f) do { (f)=0; assert(!pthread_mutex_lock(p)); } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(f); assert(!pthread_mutex_unlock(p)); } while(0)
struct kref { unsigned count; };
static void kref_init(struct kref *r) { r->count=1; }
static void kref_get(struct kref *r) { assert(__atomic_fetch_add(&r->count,1,__ATOMIC_SEQ_CST)); }
static void kref_put(struct kref *r, void (*release)(struct kref *)) {
 unsigned old=__atomic_fetch_sub(&r->count,1,__ATOMIC_SEQ_CST); assert(old);
 if(old==1)release(r);
}
static unsigned allocations, deallocations, skbs, skb_frees;
static bool fail_alloc, fail_user_alloc;
static void *kzalloc(size_t n,int flags) {
 (void)flags; if(fail_alloc)return NULL;
 void *p=calloc(1,n); assert(p); __atomic_add_fetch(&allocations,1,__ATOMIC_SEQ_CST); return p;
}
static void kfree(void *p) {
 if(p){__atomic_add_fetch(&deallocations,1,__ATOMIC_SEQ_CST);free(p);}
}
static void *memdup_user(const void *p,size_t n) {
 if(fail_user_alloc)return ERR_PTR(-ENOMEM);
 if(!p)return ERR_PTR(-EFAULT);
 void *out=kzalloc(n,0); if(!out)return ERR_PTR(-ENOMEM); memcpy(out,p,n); return out;
}
struct sk_buff { u8 *data; unsigned len,truesize; bool nonlinear;
 struct sk_buff *next,*prev; };
struct sk_buff_head { pthread_mutex_t lock; struct sk_buff *first,*last; unsigned qlen; };
static void skb_queue_head_init(struct sk_buff_head *q) {
 assert(!pthread_mutex_init(&q->lock,NULL)); q->first=q->last=NULL;q->qlen=0;
}
static bool skb_queue_empty(const struct sk_buff_head *q) { return !READ_ONCE(q->qlen); }
static bool skb_is_nonlinear(const struct sk_buff *s) { return s->nonlinear; }
static struct sk_buff *skb_peek(struct sk_buff_head *q) { return q->first; }
static void __skb_unlink(struct sk_buff *s,struct sk_buff_head *q) {
 if(s->prev)s->prev->next=s->next;else q->first=s->next;
 if(s->next)s->next->prev=s->prev;else q->last=s->prev;
 s->next=s->prev=NULL; assert(q->qlen); __atomic_sub_fetch(&q->qlen,1,__ATOMIC_SEQ_CST);
}
static void __skb_queue_tail(struct sk_buff_head *q,struct sk_buff *s) {
 s->prev=q->last;s->next=NULL;if(q->last)q->last->next=s;else q->first=s;q->last=s;
 __atomic_add_fetch(&q->qlen,1,__ATOMIC_SEQ_CST);
}
static void __skb_queue_head(struct sk_buff_head *q,struct sk_buff *s) {
 s->next=q->first;s->prev=NULL;if(q->first)q->first->prev=s;else q->last=s;q->first=s;
 __atomic_add_fetch(&q->qlen,1,__ATOMIC_SEQ_CST);
}
static struct sk_buff *__skb_dequeue(struct sk_buff_head *q) {
 struct sk_buff *s=q->first;if(s)__skb_unlink(s,q);return s;
}
static void kfree_skb(struct sk_buff *s) {
 if(s){free(s->data);free(s);__atomic_add_fetch(&skb_frees,1,__ATOMIC_SEQ_CST);}
}
static void skb_queue_purge(struct sk_buff_head *q) {
 assert(!pthread_mutex_lock(&q->lock));struct sk_buff *s;
 while((s=__skb_dequeue(q)))kfree_skb(s);assert(!pthread_mutex_unlock(&q->lock));
}
typedef struct { pthread_mutex_t lock; pthread_cond_t condition; unsigned waiters; } wait_queue_head_t;
static void init_waitqueue_head(wait_queue_head_t *w) {
 assert(!pthread_mutex_init(&w->lock,NULL));assert(!pthread_cond_init(&w->condition,NULL));w->waiters=0;
}
static void wake_up_interruptible(wait_queue_head_t *w) {
 assert(!pthread_mutex_lock(&w->lock));assert(!pthread_cond_broadcast(&w->condition));
 assert(!pthread_mutex_unlock(&w->lock));
}
#define wait_event_interruptible(w,expression) ({ \
 assert(!pthread_mutex_lock(&(w).lock)); \
 while(!(expression)){__atomic_add_fetch(&(w).waiters,1,__ATOMIC_SEQ_CST); \
 assert(!pthread_cond_wait(&(w).condition,&(w).lock)); \
 __atomic_sub_fetch(&(w).waiters,1,__ATOMIC_SEQ_CST);} \
 assert(!pthread_mutex_unlock(&(w).lock));0; })
static void poll_wait(struct file *f,wait_queue_head_t *w,poll_table *p){(void)f;(void)w;(void)p;}
struct gate {pthread_mutex_t lock;pthread_cond_t condition;bool reached,released;};
static struct gate *copy_gate,*send_gate;
static bool copy_fault;
static void gate_init(struct gate *g) {
 memset(g,0,sizeof(*g));assert(!pthread_mutex_init(&g->lock,NULL));assert(!pthread_cond_init(&g->condition,NULL));
}
static struct timespec deadline(void) { struct timespec t;assert(!clock_gettime(CLOCK_REALTIME,&t));t.tv_sec+=5;return t; }
static void gate_pause(struct gate *g) {
 if(!g)return;assert(!pthread_mutex_lock(&g->lock));g->reached=true;
 assert(!pthread_cond_broadcast(&g->condition));
 struct timespec t=deadline();while(!g->released)assert(!pthread_cond_timedwait(&g->condition,&g->lock,&t));
 assert(!pthread_mutex_unlock(&g->lock));
}
static void gate_wait(struct gate *g) {
 assert(!pthread_mutex_lock(&g->lock));struct timespec t=deadline();
 while(!g->reached)assert(!pthread_cond_timedwait(&g->condition,&g->lock,&t));
 assert(!pthread_mutex_unlock(&g->lock));
}
static void gate_release(struct gate *g) {
 assert(!pthread_mutex_lock(&g->lock));g->released=true;assert(!pthread_cond_broadcast(&g->condition));
 assert(!pthread_mutex_unlock(&g->lock));
}
static void gate_destroy(struct gate *g) {
 assert(!pthread_cond_destroy(&g->condition));assert(!pthread_mutex_destroy(&g->lock));
}
static size_t copy_to_user(void *to,const void *from,size_t n) {
 gate_pause(copy_gate);if(copy_fault||!to)return n;memcpy(to,from,n);return 0;
}
'''
tests=r'''
struct context {struct mt6797_imsc_port *port;int result;unsigned calls;bool alive,room,busy_once,synchronous;};
static void put32(u8*p,u32 n){p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static struct sk_buff *record(unsigned n,unsigned charge){
 struct sk_buff*s=calloc(1,sizeof(*s));assert(s);s->len=n+16;s->truesize=charge;
 s->data=calloc(1,s->len);assert(s->data);__atomic_add_fetch(&skbs,1,__ATOMIC_SEQ_CST);
 put32(s->data+4,s->len);put32(s->data+8,55|(123u<<16));
 for(unsigned i=0;i<n;i++)s->data[16+i]=(u8)(i*17+3);return s;
}
static int receive(struct mt6797_imsc_port*p,unsigned n,unsigned charge){
 struct sk_buff*s=record(n,charge);int r=mt6797_imsc_port_rx(p,s);if(r)kfree_skb(s);return r;
}
static int send(void*arg,const u8*payload,size_t n){
 struct context*b=arg;assert(b->alive&&n==3456);assert(payload[0]==0x6c);
 __atomic_add_fetch(&b->calls,1,__ATOMIC_SEQ_CST);gate_pause(send_gate);
 if(b->busy_once){b->busy_once=false;b->room=true;return -EBUSY;}
 if(b->synchronous)assert(!receive(b->port,3456,8192));return b->result;
}
static bool room(void*arg){struct context*b=arg;assert(b->alive);return b->room;}
static const struct mt6797_imsc_backend backend={send,room};
static struct mt6797_imsc_port *new_port(struct context*b){
 memset(b,0,sizeof(*b));b->alive=true;b->room=true;
 struct mt6797_imsc_port*p=mt6797_imsc_port_create(NULL,&backend,b);assert(!IS_ERR(p));b->port=p;return p;
}
static int open_file(struct mt6797_imsc_port*p,struct file*f,int flags){f->private_data=&p->misc;f->f_flags=flags;return mt6797_imsc_open(NULL,f);}
static struct mt6797_imsc_port *opened(struct context*b,struct file*f){
 struct mt6797_imsc_port*p=new_port(b);mt6797_imsc_port_ready(p,true);assert(!open_file(p,f,O_NONBLOCK));return p;
}
static ssize_t write_record(struct file*f){u8 frame[3456]={0x6c};return mt6797_imsc_write(f,(char*)frame,sizeof(frame),NULL);}
static void finish(struct mt6797_imsc_port*p,struct file*f,struct context*b){
 if(f)assert(!mt6797_imsc_release(NULL,f));mt6797_imsc_port_destroy(p);b->alive=false;
}
static void test_open(void){
 struct context b;struct file f,g;struct mt6797_imsc_port*p;
 assert(PTR_ERR(mt6797_imsc_port_create(NULL,NULL,&b))==-EINVAL);
 fail_alloc=true;assert(PTR_ERR(mt6797_imsc_port_create(NULL,&backend,&b))==-ENOMEM);fail_alloc=false;
 register_error=-EIO;assert(PTR_ERR(mt6797_imsc_port_create(NULL,&backend,&b))==-EIO);register_error=0;
 p=new_port(&b);assert(!strcmp(p->misc.name,"mt6797-imsc")&&p->misc.mode==0600);
 assert(open_file(p,&f,O_NONBLOCK)==-EAGAIN);mt6797_imsc_port_ready(p,true);
 assert(!open_file(p,&f,O_NONBLOCK));assert(open_file(p,&g,O_NONBLOCK)==-EBUSY);
 assert(mt6797_imsc_poll(&f,NULL)==(EPOLLOUT|EPOLLWRNORM));
 assert(!mt6797_imsc_release(NULL,&f));assert(!p->stopped);
 assert(!open_file(p,&f,O_NONBLOCK));mt6797_imsc_port_ready(p,false);
 assert(p->stopped&&write_record(&f)==-EPIPE);mt6797_imsc_port_ready(p,true);
 assert(mt6797_imsc_poll(&f,NULL)==(EPOLLERR|EPOLLHUP));finish(p,&f,&b);
}
static void test_records(void){
 struct context b;struct file f;struct mt6797_imsc_port*p=opened(&b,&f);u8 out[4096];
 assert(mt6797_imsc_read(&f,(char*)out,sizeof(out),NULL)==-EAGAIN);
 assert(!receive(p,3456,8192));assert(p->rx_count==1);
 assert(mt6797_imsc_read(&f,(char*)out,3455,NULL)==-EMSGSIZE&&p->rx_count==1);
 copy_fault=true;assert(mt6797_imsc_read(&f,(char*)out,sizeof(out),NULL)==-EFAULT&&p->rx_count==1);copy_fault=false;
 assert(mt6797_imsc_read(&f,(char*)out,sizeof(out),NULL)==3456);
 for(unsigned i=0;i<3456;i++)assert(out[i]==(u8)(i*17+3));
 assert(!p->rx_count&&!p->rx_memory&&p->touched);
 assert(!mt6797_imsc_release(NULL,&f));assert(p->stopped&&open_file(p,&f,0)==-ECANCELED);
 finish(p,NULL,&b);
 p=new_port(&b);assert(!receive(p,64,4096));mt6797_imsc_port_ready(p,true);
 assert(!open_file(p,&f,0));assert(!mt6797_imsc_release(NULL,&f));assert(p->stopped);finish(p,NULL,&b);
}
static void test_write(void){
 struct context b;struct file f;struct mt6797_imsc_port*p=opened(&b,&f);u8 frame[3472]={0x6c};
 assert(mt6797_imsc_write(&f,(char*)frame,3472,NULL)==-EMSGSIZE&&!b.calls);
 assert(mt6797_imsc_write(&f,(char*)frame,3455,NULL)==-EMSGSIZE);
 fail_user_alloc=true;assert(write_record(&f)==-ENOMEM);fail_user_alloc=false;
 assert(mt6797_imsc_write(&f,NULL,3456,NULL)==-EFAULT);
 b.result=-ENOMEM;assert(write_record(&f)==-ENOMEM&&!p->touched);
 b.result=-EBUSY;b.room=false;assert(write_record(&f)==-EAGAIN&&!p->touched);
 assert(!(mt6797_imsc_poll(&f,NULL)&EPOLLOUT));b.result=0;b.room=true;mt6797_imsc_port_tx_wake(p);
 b.synchronous=true;assert(write_record(&f)==3456&&p->touched&&p->rx_count==1);
 finish(p,&f,&b);
 p=opened(&b,&f);b.busy_once=true;f.f_flags=0;
 assert(write_record(&f)==3456&&b.calls==2);finish(p,&f,&b);
}
static void test_bad_rx(unsigned which){
 struct context b;struct file f;struct mt6797_imsc_port*p=opened(&b,&f);
 struct sk_buff*s=record(64,4096);
 if(which==0)s->nonlinear=true;
 if(which==1)put32(s->data+8,57);
 if(which==2)put32(s->data,0xffffffff);
 if(which==3)put32(s->data+4,79);
 if(which==4)s->len=79;
 if(which==5){kfree_skb(s);s=record(3457,8192);}
 assert(mt6797_imsc_port_rx(p,s)==-EPROTO&&p->stopped);kfree_skb(s);
 assert(write_record(&f)==-EPROTO);finish(p,&f,&b);
}
struct read_job {struct file*f;ssize_t ret;u8 out[3456];};
static void *reader(void*arg){struct read_job*j=arg;j->ret=mt6797_imsc_read(j->f,(char*)j->out,sizeof(j->out),NULL);return NULL;}
struct write_job {struct file*f;ssize_t ret;};
static void *writer(void*arg){struct write_job*j=arg;j->ret=write_record(j->f);return NULL;}
static void *destroyer(void*arg){mt6797_imsc_port_destroy(arg);return NULL;}
static void wait_waiter(struct mt6797_imsc_port*p){struct timespec end=deadline(),now;while(!READ_ONCE(p->wait.waiters)){clock_gettime(CLOCK_REALTIME,&now);assert(now.tv_sec<end.tv_sec);sched_yield();}}
static void test_copy(bool stop,bool fault){
 struct context b;struct file f;struct mt6797_imsc_port*p=opened(&b,&f);struct gate g;pthread_t thread;
 gate_init(&g);copy_gate=&g;copy_fault=fault;assert(!receive(p,3456,8192));
 struct read_job j={.f=&f};assert(!pthread_create(&thread,NULL,reader,&j));gate_wait(&g);
 assert(p->rx_count==1&&p->rx_memory==8192&&skb_queue_empty(&p->rx));
 for(unsigned i=1;i<32;i++)assert(!receive(p,64,8192));
 if(stop)mt6797_imsc_port_stop(p,-EPIPE);
 gate_release(&g);assert(!pthread_join(thread,NULL));copy_gate=NULL;copy_fault=false;gate_destroy(&g);
 assert(j.ret==(stop?-EPIPE:fault?-EFAULT:3456));
 if(stop)assert(!p->rx_count&&!p->rx_memory);
 else if(fault){assert(p->rx_count==32);assert(mt6797_imsc_read(&f,(char*)j.out,sizeof(j.out),NULL)==3456);}
 finish(p,&f,&b);
}
static void test_overflow(bool memory){
 struct context b;struct file f;struct mt6797_imsc_port*p=opened(&b,&f);
 if(memory)assert(receive(p,64,MT6797_IMSC_RX_MEMORY+1)==-ENOBUFS);
 else{for(unsigned i=0;i<32;i++)assert(!receive(p,64,8192));assert(receive(p,64,8192)==-ENOBUFS);}
 assert(p->stopped&&!p->rx_count&&!p->rx_memory);finish(p,&f,&b);
}
static void test_waiters(bool writing,bool wake){
 struct context b;struct file f;struct mt6797_imsc_port*p=opened(&b,&f);pthread_t thread;
 struct write_job w={&f,0};struct read_job r={.f=&f};f.f_flags=0;
 if(writing){b.result=-EBUSY;b.room=false;assert(!pthread_create(&thread,NULL,writer,&w));}
 else assert(!pthread_create(&thread,NULL,reader,&r));
 wait_waiter(p);
 if(wake){if(writing){b.result=0;b.room=true;mt6797_imsc_port_tx_wake(p);}else assert(!receive(p,64,4096));}
 else mt6797_imsc_port_stop(p,-ESHUTDOWN);
 assert(!pthread_join(thread,NULL));assert((writing?w.ret:r.ret)==(wake?(writing?3456:64):-ESHUTDOWN));finish(p,&f,&b);
}
static void test_destroy_in_send(bool error){
 struct context b;struct file f;struct mt6797_imsc_port*p=opened(&b,&f);struct gate g;pthread_t wt,dt;
 gate_init(&g);send_gate=&g;b.result=error?-EIO:0;struct write_job j={&f,0};
 assert(!pthread_create(&wt,NULL,writer,&j));gate_wait(&g);assert(!pthread_create(&dt,NULL,destroyer,p));
 while(!READ_ONCE(p->stopped))sched_yield();gate_release(&g);
 assert(!pthread_join(wt,NULL));assert(!pthread_join(dt,NULL));send_gate=NULL;gate_destroy(&g);
 assert(j.ret==(error?-ESHUTDOWN:3456));assert(p->context==NULL);b.alive=false;
 assert(write_record(&f)==-ESHUTDOWN);assert(!mt6797_imsc_release(NULL,&f));
}
int main(void){
 test_open();test_records();test_write();for(unsigned i=0;i<6;i++)test_bad_rx(i);
 for(unsigned i=0;i<2;i++)for(unsigned j=0;j<2;j++){test_copy(i,j);test_waiters(i,j);}
 test_overflow(false);test_overflow(true);test_destroy_in_send(false);test_destroy_in_send(true);
 assert(allocations==deallocations&&skbs==skb_frees);
 puts("PASS: actual IMSC exclusive endpoint framing, bounded records, faults, close/reset, wake and concurrent lifetime");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='imsc-port-') as tmp:
 p=Path(tmp);fixture=p/'fixture.c';compiler=os.environ.get('CC','cc')
 def build(name,body):
  fixture.write_text('\n'.join([shim,header.read_text(),body,tests]));out=p/name
  subprocess.run([compiler,'-std=gnu11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',str(fixture),'-o',str(out)],check=True,timeout=30);return out
 subprocess.run([str(build('fixture',actual))],check=True,timeout=30)
 if args.negative_controls:
  mutations={
   'short-record-consumption':('if (capacity < skb->len - MT6797_IMSC_CCCI_HEADER)','if (capacity < skb->len - MT6797_IMSC_CCCI_HEADER && false)','== -EMSGSIZE'),
   'header-leak':('skb->data + MT6797_IMSC_CCCI_HEADER,','skb->data,','out[i]'),
   'owner-abandonment':('if (port->touched || port->rx_count)','if (false)','p->stopped'),
   'copy-fault-loss':('if (!copied && !port->stopped)','if (!copied && !port->stopped && false)','-EFAULT'),
   'rearm-terminal':('if (!port->stopped) {\n\t\tport->ready = ready;','if (true) {\n\t\tport->stopped = false;\n\t\tport->ready = ready;','EPOLLERR'),
  }
  for name,(old,new,diagnostic) in mutations.items():
   assert actual.count(old)==1,name
   r=subprocess.run([str(build(name,actual.replace(old,new)))],capture_output=True,text=True,timeout=30)
   assert r.returncode!=0 and ('Assertion' in r.stderr or 'AddressSanitizer' in r.stderr),(name,r.stderr)
   print('REJECTED:',name)
print(json.dumps({'result':'PASS','hardware_qualified':False,'sources':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()for p in [source,header,Path(__file__)]}}))
