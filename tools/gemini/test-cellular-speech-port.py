#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the actual speech port with syscall/allocation/scheduler boundaries.

Host-only manual fixture. This proves port ownership/lifecycle behavior under
the injected boundaries; it does not claim DMA, modem or ALSA qualification.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
directory = root / "drivers/net/wwan/mediatek"
source = directory / "mtk_mt6797_ccci_speech_port.c"
headers = [directory / "mtk_mt6797_ccci_speech.h",
           directory / "mtk_mt6797_ccci_speech_port.h"]
# Remove only include directives; every function body is compiled unchanged.
actual = "\n".join(line for line in source.read_text().splitlines()
                   if not line.startswith("#include"))

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

tests = r'''
struct backend_context {
 struct mt6797_speech_port *port;unsigned calls;int result;bool room,immediate,alive,busy_once;
 u8 last[MT6797_SPEECH_FRAME_MAX];size_t bytes;
};
static struct sk_buff *record(u16 id,unsigned charge) {
 struct sk_buff *s=calloc(1,sizeof(*s));assert(s);s->len=16;s->truesize=charge;
 s->data=calloc(1,16);assert(s->data);__atomic_add_fetch(&skbs,1,__ATOMIC_SEQ_CST);
 mt6797_speech_write32(s->data,0xffffffffU);mt6797_speech_write32(s->data+4,(u32)id<<16);
 mt6797_speech_write32(s->data+8,MT6797_SPEECH_RX);return s;
}
static int receive(struct mt6797_speech_port *p,u16 id,unsigned charge) {
 struct sk_buff *s=record(id,charge);int ret=mt6797_speech_port_rx(p,s);if(ret)kfree_skb(s);return ret;
}
static int backend_send(void *arg,const u8 *frame,size_t n) {
 struct backend_context *b=arg;assert(b->alive);assert(n<=sizeof(b->last));
 memcpy(b->last,frame,n);b->bytes=n;__atomic_add_fetch(&b->calls,1,__ATOMIC_SEQ_CST);
 int result=b->result;if(b->busy_once){b->result=0;b->busy_once=false;}
 gate_pause(send_gate);
 if(b->immediate&&!result){u16 id=mt6797_speech_read32(frame+(n==16?4:12))>>16;
  if(mt6797_speech_expects_ack(id))assert(!receive(b->port,id|0x8000,512));}
 return result;
}
static bool backend_room(void *arg) { struct backend_context *b=arg;assert(b->alive);return READ_ONCE(b->room); }
static const struct mt6797_speech_backend backend={backend_send,backend_room};
static struct mt6797_speech_port *create(struct backend_context *b) {
 memset(b,0,sizeof(*b));b->alive=true;b->room=true;
 b->port=mt6797_speech_port_create(NULL,&backend,b);assert(!IS_ERR(b->port));return b->port;
}
static int open_file(struct mt6797_speech_port *p,struct file *f,int flags) {
 f->private_data=&p->misc;f->f_flags=flags;
 /* Match the Linux misc core's open/deregister serialization. */
 assert(!pthread_mutex_lock(&misc_mutex));int ret=p->misc.registered?p->misc.fops->open(NULL,f):-ENODEV;
 assert(!pthread_mutex_unlock(&misc_mutex));return ret;
}
static struct mt6797_speech_port *open_ready(struct backend_context *b,struct file *f) {
 struct mt6797_speech_port *p=create(b);mt6797_speech_port_ready(p,true);assert(!open_file(p,f,O_NONBLOCK));return p;
}
static ssize_t power(struct file *f,bool on) {
 u8 frame[16];assert(mt6797_speech_build_power(frame,sizeof(frame),on,0)==16);
 return mt6797_speech_write(f,(char *)frame,16,NULL);
}
static ssize_t no_ack(struct file *f) {
 u8 frame[16];assert(mt6797_speech_build_power(frame,sizeof(frame),true,0)==16);
 mt6797_speech_write32(frame+4,0x2f000000);assert(!mt6797_speech_expects_ack(0x2f00));
 return mt6797_speech_write(f,(char *)frame,16,NULL);
}
static void finish(struct mt6797_speech_port *p,struct file *f,struct backend_context *b) {
 if(f)assert(!mt6797_speech_release(NULL,f));mt6797_speech_port_destroy(p);b->alive=false;
 assert(READ_ONCE(allocations)==READ_ONCE(deallocations));assert(READ_ONCE(skbs)==READ_ONCE(skb_frees));
}
struct io {struct file *file;struct mt6797_speech_port *port;ssize_t ret;u8 buffer[4096];bool off;};
static void *read_thread(void *arg) {struct io *i=arg;i->ret=mt6797_speech_read(i->file,(char *)i->buffer,sizeof(i->buffer),NULL);return NULL;}
static void *write_thread(void *arg) {struct io *i=arg;i->ret=power(i->file,!i->off);return NULL;}
static void *destroy_thread(void *arg) {struct io *i=arg;mt6797_speech_port_destroy(i->port);i->ret=0;return NULL;}
static void join(pthread_t t) {struct timespec d=deadline();assert(!pthread_timedjoin_np(t,NULL,&d));}
static void wait_waiter(struct mt6797_speech_port *p) {
 struct timespec t=deadline();while(!READ_ONCE(p->wait.waiters)) {
  struct timespec now;assert(!clock_gettime(CLOCK_REALTIME,&now));assert(now.tv_sec<t.tv_sec);
  usleep(1000);
 }
}
static void test_create_open(void) {
 struct backend_context b={0};struct file f,other;struct mt6797_speech_port *p;
 assert(PTR_ERR(mt6797_speech_port_create(NULL,NULL,&b))==-EINVAL);
 fail_alloc=true;assert(PTR_ERR(mt6797_speech_port_create(NULL,&backend,&b))==-ENOMEM);fail_alloc=false;
 register_error=-EBUSY;assert(PTR_ERR(mt6797_speech_port_create(NULL,&backend,&b))==-EBUSY);register_error=0;
 p=create(&b);assert(p->misc.mode==0600&&!strcmp(p->misc.name,"mt6797-speech"));
 assert(open_file(p,&f,O_NONBLOCK)==-EAGAIN);assert(!receive(p,MT6797_SPEECH_RESET_REQUEST,512));
 mt6797_speech_port_ready(p,true);assert(!open_file(p,&f,O_NONBLOCK));assert(open_file(p,&other,0)==-EBUSY);
 u8 out[16];assert(mt6797_speech_read(&f,(char *)out,15,NULL)==-EMSGSIZE&&p->rx_count==1);
 copy_fault=true;assert(mt6797_speech_read(&f,(char *)out,16,NULL)==-EFAULT&&p->rx_count==1);copy_fault=false;
 assert(mt6797_speech_read(&f,(char *)out,16,NULL)==16&&mt6797_speech_read32(out+4)==0xaf700000);
 assert(mt6797_speech_read(&f,(char *)out,16,NULL)==-EAGAIN);assert(!b.calls);
 finish(p,&f,&b);
}
static void test_queue_bounds(void) {
 struct backend_context b;struct file f;struct mt6797_speech_port *p=open_ready(&b,&f);
 for(unsigned i=0;i<32;i++)assert(!receive(p,MT6797_SPEECH_RESET_REQUEST,512));
 assert(receive(p,MT6797_SPEECH_RESET_REQUEST,512)==-ENOBUFS&&p->stopped&&!p->rx_count&&!p->rx_memory);
 finish(p,&f,&b);p=open_ready(&b,&f);
 assert(!receive(p,MT6797_SPEECH_RESET_REQUEST,MT6797_SPEECH_RX_MEMORY));
 assert(receive(p,MT6797_SPEECH_RESET_REQUEST,1)==-ENOBUFS&&!p->rx_memory);finish(p,&f,&b);
 p=open_ready(&b,&f);assert(receive(p,MT6797_SPEECH_RESET_REQUEST,MT6797_SPEECH_RX_MEMORY+1)==-ENOBUFS);finish(p,&f,&b);
}
static void test_detached_copy(bool stop,bool fault) {
 struct backend_context b;struct file f;struct mt6797_speech_port *p=open_ready(&b,&f);
 struct gate g;gate_init(&g);copy_gate=&g;copy_fault=fault;
 for(unsigned i=0;i<31;i++)assert(!receive(p,MT6797_SPEECH_RESET_REQUEST,512));
 mt6797_speech_write32(p->rx.first->data+4,0xaf701234);
 struct io io={.file=&f};pthread_t t;assert(!pthread_create(&t,NULL,read_thread,&io));gate_wait(&g);
 assert(p->rx_count==31&&p->rx_memory==31*512&&p->rx.qlen==30);
 assert(!receive(p,MT6797_SPEECH_RESET_REQUEST,512));assert(p->rx_count==32&&p->rx.qlen==31);
 if(stop){mt6797_speech_port_stop(p,-ESHUTDOWN);assert(p->rx_count==1&&p->rx_memory==512&&!p->rx.qlen);}
 gate_release(&g);join(t);copy_gate=NULL;copy_fault=false;gate_destroy(&g);
 if(stop)assert(io.ret==-ESHUTDOWN&&!p->rx_count&&!p->rx_memory);
 else if(fault){assert(io.ret==-EFAULT&&p->rx_count==32&&p->rx.qlen==32);assert(mt6797_speech_read(&f,(char *)io.buffer,16,NULL)==16&&p->rx_count==31);assert(mt6797_speech_read32(io.buffer+4)==0xaf701234);}
 else assert(io.ret==16&&p->rx_count==31&&p->rx.qlen==31);
 finish(p,&f,&b);
}
static void test_wait_and_detach(void) {
 struct backend_context b;struct file f,other;struct mt6797_speech_port *p=open_ready(&b,&f);f.f_flags=0;
 struct io io={.file=&f};pthread_t t;assert(!pthread_create(&t,NULL,read_thread,&io));wait_waiter(p);
 mt6797_speech_port_stop(p,-EIO);join(t);assert(io.ret==-EIO);
 mt6797_speech_port_ready(p,true);assert(p->stopped&&!p->ready&&mt6797_speech_poll(&f,NULL)==(EPOLLERR|EPOLLHUP));
 assert(open_file(p,&other,0)==-EIO);unsigned freed=deallocations;mt6797_speech_port_destroy(p);b.alive=false;
 assert(deallocations==freed&&p->context==NULL&&power(&f,true)==-EIO);
 assert(mt6797_speech_read(&f,(char *)io.buffer,16,NULL)==-EIO);assert(!mt6797_speech_release(NULL,&f));
 assert(deallocations==allocations&&skbs==skb_frees);
}
static void test_ack_order(bool immediate) {
 struct backend_context b;struct file f,other;struct mt6797_speech_port *p=open_ready(&b,&f);b.immediate=immediate;
 assert(power(&f,true)==16);
 if(!immediate){assert(p->waiting_id==MT6797_SPEECH_ON&&!p->speech_active);assert(power(&f,false)==-EAGAIN);
  assert(no_ack(&f)==-EAGAIN&&b.calls==1);assert(!receive(p,0xaf20,512));}
 assert(!p->waiting_id&&p->speech_active);assert(power(&f,false)==16);
 if(!immediate)assert(!receive(p,0xaf21,512));
 assert(!p->waiting_id&&!p->speech_active);assert(!mt6797_speech_release(NULL,&f));
 assert(!open_file(p,&other,O_NONBLOCK));assert(!p->rx_count);finish(p,&other,&b);
}
static void test_pending_close(bool acked) {
 struct backend_context b;struct file f,other;struct mt6797_speech_port *p=open_ready(&b,&f);
 assert(power(&f,true)==16);if(acked)assert(!receive(p,0xaf20,512));assert(!mt6797_speech_release(NULL,&f));
 assert(p->stopped&&p->error==-ECANCELED&&open_file(p,&other,0)==-ECANCELED);
 assert(receive(p,acked?0xaf21:0xaf20,512)==-ECANCELED&&!p->rx_count);finish(p,NULL,&b);
}
static void test_bad_rx(unsigned mode) {
 struct backend_context b;struct file f;struct mt6797_speech_port *p=open_ready(&b,&f);assert(power(&f,true)==16);
 struct sk_buff *s=record(mode==0?0xaf21:0xaf70,512);int expected;
 if(mode==0)expected=-EPROTO;else if(mode==1)expected=-EPIPE;
 else if(mode==2){s->len=15;expected=-EMSGSIZE;}
 else if(mode==3){mt6797_speech_write32(s->data+8,5);expected=-EPROTO;}
 else{s->nonlinear=true;expected=-EPROTO;}
 assert(mt6797_speech_port_rx(p,s)==expected&&p->stopped&&p->error==expected);kfree_skb(s);
 assert(power(&f,false)==expected&&b.calls==1);finish(p,&f,&b);
}
static void test_tx_errors(void) {
 struct backend_context b;struct file f;struct mt6797_speech_port *p=open_ready(&b,&f);
 fail_user_alloc=true;assert(power(&f,true)==-ENOMEM&&!b.calls);fail_user_alloc=false;
 b.result=-ENOMEM;assert(power(&f,true)==-ENOMEM&&!p->waiting_id&&p->tx_ready&&!p->speech_active);
 b.result=-EBUSY;b.room=false;assert(power(&f,true)==-EAGAIN&&!p->waiting_id&&!p->tx_ready);
 b.result=0;b.room=true;mt6797_speech_port_tx_wake(p);b.immediate=true;
 assert(power(&f,true)==16&&p->speech_active);assert(power(&f,false)==16&&!p->speech_active);finish(p,&f,&b);
}
static void test_pending_writer(void) {
 struct backend_context b;struct file f;struct mt6797_speech_port *p=open_ready(&b,&f);assert(power(&f,true)==16);f.f_flags=0;
 struct io io={.file=&f,.off=true};pthread_t t;assert(!pthread_create(&t,NULL,write_thread,&io));wait_waiter(p);
 assert(b.calls==1&&p->waiting_id==MT6797_SPEECH_ON);assert(!receive(p,0xaf20,512));join(t);
 assert(io.ret==16&&b.calls==2&&p->waiting_id==MT6797_SPEECH_OFF);assert(!receive(p,0xaf21,512));finish(p,&f,&b);
}
static void test_ring_wake(bool missed_window) {
 struct backend_context b;struct file f;struct mt6797_speech_port *p=open_ready(&b,&f);f.f_flags=0;
 struct gate g;gate_init(&g);send_gate=&g;b.result=-EBUSY;b.room=missed_window;b.busy_once=missed_window;b.immediate=missed_window;
 struct io io={.file=&f};pthread_t t;assert(!pthread_create(&t,NULL,write_thread,&io));gate_wait(&g);
 /* A real completion before send returns busy is recovered by room(). */
 if(missed_window)mt6797_speech_port_tx_wake(p);
 gate_release(&g);
 if(!missed_window){wait_waiter(p);send_gate=NULL;b.result=0;b.room=true;b.immediate=true;mt6797_speech_port_tx_wake(p);}
 join(t);send_gate=NULL;assert(io.ret==16&&p->speech_active);gate_destroy(&g);
 assert(power(&f,false)==16&&!p->speech_active);finish(p,&f,&b);
}
static void test_destroy_in_send(void) {
 struct backend_context b;struct file f;struct mt6797_speech_port *p=open_ready(&b,&f);
 struct gate g;gate_init(&g);send_gate=&g;struct io writer={.file=&f},remover={.port=p,.ret=99};pthread_t w,d;
 assert(!pthread_create(&w,NULL,write_thread,&writer));gate_wait(&g);
 assert(!pthread_create(&d,NULL,destroy_thread,&remover));
 struct timespec limit=deadline();while(!READ_ONCE(p->stopped)){struct timespec now;clock_gettime(CLOCK_REALTIME,&now);assert(now.tv_sec<limit.tv_sec);usleep(1000);}
 assert(p->context==&b&&b.alive&&p->reference.count==2);
 gate_release(&g);join(w);join(d);send_gate=NULL;gate_destroy(&g);b.alive=false;
 /* A write accepted before STOP is submission only. Destroy drained it. */
 assert(writer.ret==16&&remover.ret==0&&p->context==NULL);unsigned calls=b.calls;
 assert(power(&f,false)==-ESHUTDOWN&&b.calls==calls);assert(!mt6797_speech_release(NULL,&f));
 assert(allocations==deallocations&&skbs==skb_frees);
}
int main(void) {
 test_create_open();test_queue_bounds();
 test_detached_copy(false,false);test_detached_copy(false,true);
 test_detached_copy(true,false);test_detached_copy(true,true);
 test_wait_and_detach();test_ack_order(false);test_ack_order(true);
 test_pending_close(false);test_pending_close(true);
 for(unsigned i=0;i<5;i++)test_bad_rx(i);
 test_tx_errors();test_pending_writer();test_ring_wake(false);test_ring_wake(true);
 test_destroy_in_send();
 assert(allocations==deallocations&&skbs==skb_frees);
 puts("PASS: actual speech port lifecycle, ACK order, copy faults, bounds, wake and detach");
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="speech-port-fixture-") as temporary:
    path = Path(temporary)
    fixture = path / "fixture.c"
    fixture.write_text("\n".join([shim, *(h.read_text() for h in headers), actual, tests]))
    compiler = os.environ.get("CC", "cc")
    subprocess.run([compiler, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-misleading-indentation",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                    str(fixture), "-o", str(path / "fixture")], check=True, timeout=30)
    subprocess.run([str(path / "fixture")], check=True, timeout=30)
print(json.dumps({"result": "PASS", "scope": "actual-source host lifecycle; no hardware claim",
                  "sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in [source, *headers, Path(__file__)]}}, sort_keys=True))
