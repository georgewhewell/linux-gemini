#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute native fatal withdrawal and the actual WWAN core FD lifetime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[2]
CORE = (root / "drivers/net/wwan/wwan_core.c").read_text()

def body(source, name):
    match = re.search(r'^(?:static )?[^\n]+\b'+re.escape(name)+r'\([^;]*?\n\{', source, re.M)
    if not match:
        raise ValueError(name)
    return source[match.start():source.index('\n}\n', match.start())+3]

SHIM = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef unsigned __poll_t;
typedef int poll_table;
#define __user
#define EPOLLIN 1U
#define EPOLLOUT 4U
#define EPOLLERR 8U
#define EPOLLHUP 16U
#define EPOLLRDNORM 64U
#define EPOLLWRNORM 256U
#define O_NONBLOCK 2048
#define MTK_CCCI_HEADER_SIZE 16
#define READ_ONCE(v) (v)
#define ERESTARTSYS 512
#define WWAN_PORT_AT 1
#define WWAN_PORT_NMEA 2
#define WWAN_PORT_TX_OFF 0
#define MTK_CCCI_MAX_NET_LINKS 1
#define MT6797_CCCI_ACTIVE_IRQS 7
#define MT6797_MD1_CLDMA_ACTIVE_QUEUES 127
#define CLDMA_L2_TX_MASK_SET 0
#define CLDMA_L2_RX_MASK_SET 1
#define CLDMA_UL_STOP_CMD 2
#define CLDMA_SO_STOP_CMD 3
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define atomic_read(p) atomic_load(p)
#define atomic_set(p,v) atomic_store(p,v)
#define atomic_xchg(p,v) atomic_exchange(p,v)
struct mt6797_ccci;
struct wwan_port;
struct file;
struct work_struct { void (*fn)(struct work_struct *); pthread_mutex_t lock;
 pthread_cond_t cond; bool pending,running; unsigned queued,cancelled; };
struct sk_buff { unsigned len,truesize; struct sk_buff *next; unsigned char data[8]; };
struct sk_buff_head { pthread_mutex_t lock; struct sk_buff *head,*tail; unsigned qlen; };
struct device { atomic_uint refs; bool released; void *parent; struct wwan_port *port; };
struct wwan_device { unsigned removals; };
struct wwan_port_ops { int (*start)(struct wwan_port *); void (*stop)(struct wwan_port *);
 int (*tx)(struct wwan_port *,struct sk_buff *);
 int (*tx_blocking)(struct wwan_port *,struct sk_buff *);
 __poll_t (*tx_poll)(struct wwan_port *,struct file *,poll_table *); };
struct waitqueue { pthread_mutex_t lock; pthread_cond_t cond; unsigned wakes,waiters; };
struct wwan_port { struct device dev; _Atomic(struct wwan_port_ops *) ops;
 pthread_mutex_t ops_lock; unsigned start_count; atomic_uint flags;
 struct sk_buff_head rxq; struct waitqueue waitqueue; unsigned type,unregistered;
 struct mt6797_ccci *owner; };
struct file { struct wwan_port *private_data; unsigned f_flags; };
struct inode { int unused; };
struct net_device { int unused; };
struct mt6797_ccci_link { struct net_device *netdev; unsigned tx_queue; };
struct mt6797_ccci { atomic_int ready,at_open,failed,teardown;
 struct wwan_port *at_port; struct work_struct at_terminal_work; bool at_work_stopped;
 struct sk_buff_head at_rx_queue; unsigned at_rx_bytes,at_rx_memory; bool at_rx_stopped;
 struct mt6797_ccci_link *link[1]; pthread_mutex_t tx_stop_lock;
 uint32_t registers[8],*pdn,*ao; unsigned fatal,crash,cold_fatal,at_events[8]; void *dev; int hs;
 bool owner_alive; void *speech,*imsc; };
/* Speech ownership is exercised by its actual-source fixture. These hooks
 * preserve independent frontend calls while testing the WWAN FD lifetime. */
static void mt6797_speech_port_ready(void *p,bool ready){(void)p;(void)ready;}
static void mt6797_speech_port_stop(void *p,int reason){(void)p;(void)reason;}
static void mt6797_speech_port_tx_wake(void *p){(void)p;}
static void mt6797_imsc_port_ready(void *p,bool ready){(void)p;(void)ready;}
static void mt6797_imsc_port_stop(void *p,int reason){(void)p;(void)reason;}
static void mt6797_imsc_port_tx_wake(void *p){(void)p;}
#define MT6797_IMSC_QUEUE 6U
static _Thread_local unsigned spin_depth;
static pthread_mutex_t *rx_lock;
static _Thread_local bool rx_locked;
static pthread_mutex_t *watched_ops;
static _Thread_local bool trace_ops, held_ops;
static pthread_mutex_t attempt_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t attempt_cond=PTHREAD_COND_INITIALIZER;
static unsigned ops_attempts;
static void trace_ops_lock(pthread_mutex_t *p){
 if(trace_ops&&p==watched_ops){assert(!pthread_mutex_lock(&attempt_lock));ops_attempts++;
 assert(!pthread_cond_broadcast(&attempt_cond));assert(!pthread_mutex_unlock(&attempt_lock));}
}
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!pthread_mutex_lock(p));spin_depth++;if((p)==rx_lock){assert(!rx_locked);rx_locked=true;}}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);if((p)==rx_lock){assert(rx_locked);rx_locked=false;}assert(spin_depth);spin_depth--;assert(!pthread_mutex_unlock(p));}while(0)
#define mutex_lock(p) do{assert(!spin_depth);trace_ops_lock(p);assert(!pthread_mutex_lock(p));if((p)==watched_ops)held_ops=true;}while(0)
#define mutex_unlock(p) do{if((p)==watched_ops){assert(held_ops);held_ops=false;}assert(!pthread_mutex_unlock(p));}while(0)
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define rcu_dereference(p) (p)
#define dev_err(...) ((void)0)
static bool netif_running(struct net_device *p){(void)p;return false;}
static void netif_carrier_on(struct net_device *p){(void)p;}
static void netif_carrier_off(struct net_device *p){(void)p;}
static void netif_wake_queue(struct net_device *p){(void)p;}
static void netif_stop_queue(struct net_device *p){(void)p;}
static void writel(uint32_t v,uint32_t *p){*p=v;}
static uint32_t readl(uint32_t *p){return *p;}
static void set_bit(unsigned n,atomic_uint *p){atomic_fetch_or(p,1U<<n);}
static void clear_bit(unsigned n,atomic_uint *p){atomic_fetch_and(p,~(1U<<n));}
static bool test_bit(unsigned n,atomic_uint *p){return !!(atomic_load(p)&(1U<<n));}
static void queue_init(struct sk_buff_head *q){memset(q,0,sizeof(*q));assert(!pthread_mutex_init(&q->lock,NULL));}
static struct sk_buff *__skb_dequeue(struct sk_buff_head *q){struct sk_buff *s=q->head;if(!s)return NULL;q->head=s->next;if(!q->head)q->tail=NULL;s->next=NULL;q->qlen--;return s;}
static void __skb_queue_tail(struct sk_buff_head *q,struct sk_buff *s){if(q->tail)q->tail->next=s;else q->head=s;q->tail=s;q->qlen++;}
static void kfree_skb(struct sk_buff *s){free(s);}
static void consume_skb(struct sk_buff *s){free(s);}
static bool skb_queue_empty(struct sk_buff_head *q){assert(!pthread_mutex_lock(&q->lock));bool b=!q->qlen;assert(!pthread_mutex_unlock(&q->lock));return b;}
static void skb_queue_purge(struct sk_buff_head *q){assert(!pthread_mutex_lock(&q->lock));struct sk_buff *s;while((s=__skb_dequeue(q)))free(s);assert(!pthread_mutex_unlock(&q->lock));}
static struct sk_buff *skb_dequeue(struct sk_buff_head *q){assert(!pthread_mutex_lock(&q->lock));struct sk_buff *s=__skb_dequeue(q);assert(!pthread_mutex_unlock(&q->lock));return s;}
static void skb_queue_head(struct sk_buff_head *q,struct sk_buff *s){assert(!pthread_mutex_lock(&q->lock));s->next=q->head;q->head=s;if(!q->tail)q->tail=s;q->qlen++;assert(!pthread_mutex_unlock(&q->lock));}
static void skb_pull(struct sk_buff *s,size_t n){assert(s->len>=n);memmove(s->data,s->data+n,s->len-n);s->len-=n;}
static int copy_to_user(void *to,void *from,size_t n){memcpy(to,from,n);return 0;}
static void wake_up_interruptible(struct waitqueue *q){assert(!pthread_mutex_lock(&q->lock));q->wakes++;assert(!pthread_cond_broadcast(&q->cond));assert(!pthread_mutex_unlock(&q->lock));}
#define wait_event_interruptible(q,condition) ({ int ret=0;assert(!spin_depth);assert(!pthread_mutex_lock(&(q).lock));(q).waiters++;assert(!pthread_cond_broadcast(&(q).cond));while(!(condition)){struct timespec end;assert(!clock_gettime(CLOCK_REALTIME,&end));end.tv_sec+=2;int e=pthread_cond_timedwait(&(q).cond,&(q).lock,&end);if(e==ETIMEDOUT){ret=-ETIMEDOUT;break;}assert(!e);}assert(!(q).waiters==0);(q).waiters--;assert(!pthread_mutex_unlock(&(q).lock));ret;})
static void poll_wait(struct file *f,struct waitqueue *q,poll_table *w){(void)f;(void)q;(void)w;}
static struct wwan_device *to_wwan_dev(void *p){return p;}
static void wwan_remove_dev(struct wwan_device *p){p->removals++;}
static void get_device(struct device *d){assert(!d->released);atomic_fetch_add(&d->refs,1);}
static void put_device(struct device *d){assert(!spin_depth);unsigned n=atomic_fetch_sub(&d->refs,1);assert(n);if(n==1)d->released=true;}
static pthread_barrier_t unregister_enter,unregister_release;
static bool pause_unregister;
static void barrier(pthread_barrier_t *);
static void wwan_port_unregister_wwan(struct wwan_port *p){assert(!spin_depth);assert(!p->unregistered++);if(pause_unregister){pause_unregister=false;barrier(&unregister_enter);barrier(&unregister_release);}}
static void wwan_port_unregister_gnss(struct wwan_port *p){(void)p;assert(false);}
static pthread_barrier_t stop_enter,stop_release;
static bool pause_stop;
static void *wwan_port_get_drvdata(struct wwan_port *p){assert(p->owner->owner_alive);
 if(pause_stop){assert(held_ops);pause_stop=false;barrier(&stop_enter);barrier(&stop_release);}return p->owner;}
static void mt6797_ccci_first_fatal(struct mt6797_ccci *c,const char *site){(void)site;c->fatal++;}
static void mt6797_md1_hs_crash(int *hs,int reason){(void)reason;(*hs)++;}
static unsigned cold_count;
static void mt6797_md1_cold_owner_transport_fatal(int reason){(void)reason;cold_count++;}
enum { MT6797_CCCI_AT_OPEN,MT6797_CCCI_AT_CLOSE,MT6797_CCCI_AT_RX_DELIVERED,MT6797_CCCI_AT_RX_CLOSED_DROP };
#define mt6797_ccci_diag_at(d,e) (((struct mt6797_ccci *)(d))->at_events[e]++)
#define diag owner_diag_not_needed
static pthread_barrier_t schedule_enter,schedule_release,tx_enter,tx_release;
static bool pause_schedule,pause_tx;
static void barrier(pthread_barrier_t *b){int n=pthread_barrier_wait(b);assert(!n||n==PTHREAD_BARRIER_SERIAL_THREAD);}
static bool schedule_work(struct work_struct *w){assert(rx_locked);if(pause_schedule){pause_schedule=false;barrier(&schedule_enter);barrier(&schedule_release);}assert(!pthread_mutex_lock(&w->lock));bool queued=!w->pending;if(queued){w->pending=true;w->queued++;}assert(!pthread_mutex_unlock(&w->lock));return queued;}
static void cancel_work_sync(struct work_struct *w){assert(!spin_depth);assert(!pthread_mutex_lock(&w->lock));w->cancelled++;w->pending=false;assert(!pthread_cond_broadcast(&w->cond));while(w->running)assert(!pthread_cond_wait(&w->cond,&w->lock));assert(!pthread_mutex_unlock(&w->lock));}
static void run_work(struct work_struct *w){assert(!pthread_mutex_lock(&w->lock));assert(w->pending&&!w->running);w->pending=false;w->running=true;assert(!pthread_mutex_unlock(&w->lock));w->fn(w);assert(!pthread_mutex_lock(&w->lock));w->running=false;assert(!pthread_cond_broadcast(&w->cond));assert(!pthread_mutex_unlock(&w->lock));}
static void wwan_port_rx(struct wwan_port *p,struct sk_buff *s){assert(rx_locked&&!p->unregistered);assert(!pthread_mutex_lock(&p->rxq.lock));__skb_queue_tail(&p->rxq,s);assert(!pthread_mutex_unlock(&p->rxq.lock));wake_up_interruptible(&p->waitqueue);}
static void wwan_port_txon(struct wwan_port *p){assert(!p->unregistered);if(pause_tx){assert(rx_locked);pause_tx=false;barrier(&tx_enter);barrier(&tx_release);}clear_bit(WWAN_PORT_TX_OFF,&p->flags);wake_up_interruptible(&p->waitqueue);}
static void wwan_port_txoff(struct wwan_port *p){assert(!p->unregistered);set_bit(WWAN_PORT_TX_OFF,&p->flags);}
static void mt6797_ccci_at_stop(struct wwan_port *);
static void wwan_remove_port(struct wwan_port *);
'''

TESTS = r'''
static int fixture_tx(struct wwan_port *p,struct sk_buff *s){(void)s;assert(p->owner->owner_alive);return 0;}
static struct wwan_port_ops ops={.start=mt6797_ccci_at_start,.stop=mt6797_ccci_at_stop,.tx=fixture_tx};
static void init(struct mt6797_ccci *c,struct wwan_port *p,struct wwan_device *dev){
 memset(c,0,sizeof(*c));memset(p,0,sizeof(*p));memset(dev,0,sizeof(*dev));c->owner_alive=true;c->at_port=p;c->ao=c->pdn=c->registers;rx_lock=&c->at_rx_queue.lock;
 queue_init(&c->at_rx_queue);queue_init(&p->rxq);assert(!pthread_mutex_init(&c->tx_stop_lock,NULL));
 assert(!pthread_mutex_init(&p->ops_lock,NULL));assert(!pthread_mutex_init(&p->waitqueue.lock,NULL));assert(!pthread_cond_init(&p->waitqueue.cond,NULL));
 p->ops=&ops;p->type=WWAN_PORT_AT;p->owner=c;p->dev.parent=dev;p->dev.port=p;atomic_init(&p->dev.refs,1);
 assert(!pthread_mutex_init(&c->at_terminal_work.lock,NULL));assert(!pthread_cond_init(&c->at_terminal_work.cond,NULL));
#if CURRENT
 c->at_terminal_work.fn=mt6797_ccci_at_terminal_work;
#endif
}
static void destroy(struct mt6797_ccci *c,struct wwan_port *p){
 assert(!c->at_port&&!c->at_rx_queue.qlen&&!p->rxq.qlen&&p->dev.released);
 assert(!pthread_mutex_destroy(&c->at_rx_queue.lock));assert(!pthread_mutex_destroy(&c->tx_stop_lock));assert(!pthread_mutex_destroy(&p->rxq.lock));assert(!pthread_mutex_destroy(&p->ops_lock));assert(!pthread_mutex_destroy(&p->waitqueue.lock));assert(!pthread_cond_destroy(&p->waitqueue.cond));assert(!pthread_mutex_destroy(&c->at_terminal_work.lock));assert(!pthread_cond_destroy(&c->at_terminal_work.cond));
}
static void open_port(struct wwan_port *p){get_device(&p->dev);assert(!wwan_port_op_start(p));}
static void close_port(struct wwan_port *p){struct file f={.private_data=p};assert(!wwan_port_fops_release(NULL,&f));}
static void expect_count(struct wwan_port *p,unsigned expected){
 mutex_lock(&p->ops_lock);unsigned actual=p->start_count;mutex_unlock(&p->ops_lock);
 if(actual!=expected){fprintf(stderr,"OLD_CORE_RELEASE_ACCOUNTING: expected=%u actual=%u\n",expected,actual);exit(1);}
}
static struct sk_buff *frame(void){struct sk_buff *s=calloc(1,sizeof(*s));assert(s);s->len=4;s->truesize=64;return s;}
struct waiter { struct wwan_port *p; bool tx; int ret; };
static void *wait_thread(void *arg){struct waiter *w=arg;w->ret=w->tx?wwan_wait_tx(w->p,false):wwan_wait_rx(w->p,false);return NULL;}
static void wait_started(struct wwan_port *p,unsigned count){assert(!pthread_mutex_lock(&p->waitqueue.lock));while(p->waitqueue.waiters<count)assert(!pthread_cond_wait(&p->waitqueue.cond,&p->waitqueue.lock));assert(!pthread_mutex_unlock(&p->waitqueue.lock));}
static void *fatal_thread(void *arg){mt6797_ccci_quiesce_from(arg,-ETIMEDOUT,"md-wdt-irq");return NULL;}
#if CURRENT
static atomic_bool cleanup_done;
static void *cleanup_thread(void *arg){mt6797_ccci_cleanup_port(arg);atomic_store(&cleanup_done,true);return NULL;}
static void *work_thread(void *arg){run_work(arg);return NULL;}
#endif
static void *tx_thread(void *arg){mt6797_ccci_tx_wake(arg,1);return NULL;}
static void fatal_waiters(void){
 struct mt6797_ccci c;struct wwan_port p;struct wwan_device dev;init(&c,&p,&dev);open_port(&p);
 struct waiter a={&p,false,999},b={&p,true,999};pthread_t t1,t2;assert(!pthread_create(&t1,NULL,wait_thread,&a));assert(!pthread_create(&t2,NULL,wait_thread,&b));wait_started(&p,2);
 mt6797_ccci_quiesce_from(&c,-ETIMEDOUT,"md-wdt-irq");
 assert(c.at_rx_stopped&&c.failed&&c.fatal==1&&c.hs==1);
 assert(c.registers[0]==7&&c.registers[1]==7&&c.registers[2]==127&&c.registers[3]==127);
#if CURRENT
 assert(c.at_terminal_work.queued==1&&p.ops&&!p.unregistered);run_work(&c.at_terminal_work);
#endif
 struct file f={.private_data=&p};unsigned mask=wwan_port_fops_poll(&f,NULL);
 if((mask&(EPOLLHUP|EPOLLERR))!=(EPOLLHUP|EPOLLERR)){fprintf(stderr,"OLD_FATAL_RETAINED_OPS: no hangup; RX/TX still blocked\n");exit(1);}
 assert(!pthread_join(t1,NULL)&&!pthread_join(t2,NULL));assert(!a.ret&&!b.ret&&!p.ops&&!c.at_port);
 assert(wwan_port_op_start(&p)==-ENODEV);char bytes[8];assert(wwan_port_fops_read(&f,bytes,sizeof(bytes),NULL)==-EIO);
 assert(p.unregistered==1&&dev.removals==1&&!p.dev.released&&p.dev.refs==1);
 mt6797_ccci_at_rx(&c,frame());assert(!c.at_rx_queue.qlen&&!p.rxq.qlen);
 mt6797_ccci_frontend_ready(&c,true);mt6797_ccci_tx_wake(&c,1);assert(!c.ready);
 mt6797_ccci_quiesce_from(&c,-EIO,"duplicate");assert(c.fatal==1);
#if CURRENT
 mt6797_ccci_cleanup_port(&c);mt6797_ccci_cleanup_port(&c);assert(p.unregistered==1&&!c.at_terminal_work.pending);
#endif
 c.owner_alive=false;struct sk_buff s={0};assert(wwan_port_op_tx(&p,&s,false)==-ENODEV);close_port(&p);expect_count(&p,0);assert(p.dev.released);destroy(&c,&p);
}
#if CURRENT
static void multiple_opens(void){
 for(unsigned early=0;early<=4;early++){
  struct mt6797_ccci c;struct wwan_port p;struct wwan_device dev;init(&c,&p,&dev);
  for(unsigned i=0;i<4;i++)open_port(&p);
  expect_count(&p,4);assert(c.at_events[MT6797_CCCI_AT_OPEN]==1);
  for(unsigned i=0;i<early;i++){close_port(&p);expect_count(&p,3-i);}
  assert(c.at_events[MT6797_CCCI_AT_CLOSE]==(early==4));
  mt6797_ccci_cleanup_port(&c);expect_count(&p,0);
  assert(c.at_events[MT6797_CCCI_AT_CLOSE]==1&&!p.ops&&p.unregistered==1);
  c.owner_alive=false;
  for(unsigned i=early;i<4;i++){close_port(&p);expect_count(&p,0);}
  assert(c.at_events[MT6797_CCCI_AT_CLOSE]==1&&p.dev.released&&dev.removals==1);destroy(&c,&p);
 }
 /* A live last close may start a fresh epoch; removal still stops once. */
 struct mt6797_ccci c;struct wwan_port p;struct wwan_device dev;init(&c,&p,&dev);
 open_port(&p);close_port(&p);expect_count(&p,0);open_port(&p);open_port(&p);
 expect_count(&p,2);assert(c.at_events[MT6797_CCCI_AT_OPEN]==2);
 close_port(&p);expect_count(&p,1);mt6797_ccci_cleanup_port(&c);close_port(&p);
 expect_count(&p,0);assert(c.at_events[MT6797_CCCI_AT_CLOSE]==2);destroy(&c,&p);
}
static void *core_close_thread(void *arg){trace_ops=true;close_port(arg);trace_ops=false;return NULL;}
static void *core_remove_thread(void *arg){trace_ops=true;mt6797_ccci_cleanup_port(arg);trace_ops=false;return NULL;}
static void wait_ops_attempts(unsigned count){
 assert(!pthread_mutex_lock(&attempt_lock));while(ops_attempts<count)assert(!pthread_cond_wait(&attempt_cond,&attempt_lock));
 assert(!pthread_mutex_unlock(&attempt_lock));
}
static void close_remove_order(bool close_first){
 struct mt6797_ccci c;struct wwan_port p;struct wwan_device dev;init(&c,&p,&dev);
 unsigned opens=close_first?1:4;for(unsigned i=0;i<opens;i++)open_port(&p);
 watched_ops=&p.ops_lock;ops_attempts=0;pause_stop=true;
 assert(!pthread_barrier_init(&stop_enter,NULL,2));assert(!pthread_barrier_init(&stop_release,NULL,2));
 pthread_t first,others[4];
 assert(!pthread_create(&first,NULL,close_first?core_close_thread:core_remove_thread,close_first?(void *)&p:(void *)&c));
 barrier(&stop_enter); /* Actual stop callback holds ops_lock here. */
 if(close_first)assert(!pthread_create(&others[0],NULL,core_remove_thread,&c));
 else for(unsigned i=0;i<opens;i++)assert(!pthread_create(&others[i],NULL,core_close_thread,&p));
 wait_ops_attempts(1+(close_first?1:opens));
 assert(p.start_count==(close_first?0:opens)&&p.ops&&c.at_events[MT6797_CCCI_AT_CLOSE]==0);
 barrier(&stop_release);assert(!pthread_join(first,NULL));
 for(unsigned i=0;i<(close_first?1:opens);i++)assert(!pthread_join(others[i],NULL));
 watched_ops=NULL;expect_count(&p,0);
 assert(c.at_events[MT6797_CCCI_AT_OPEN]==1&&c.at_events[MT6797_CCCI_AT_CLOSE]==1);
 assert(!p.ops&&p.unregistered==1&&dev.removals==1&&p.dev.released);
 assert(!pthread_barrier_destroy(&stop_enter));assert(!pthread_barrier_destroy(&stop_release));destroy(&c,&p);
}
static void concurrent_closes(void){
 struct mt6797_ccci c;struct wwan_port p;struct wwan_device dev;init(&c,&p,&dev);
 for(unsigned i=0;i<4;i++)open_port(&p);
 watched_ops=&p.ops_lock;ops_attempts=0;mutex_lock(&p.ops_lock);
 pthread_t threads[5];for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,core_close_thread,&p));
 assert(!pthread_create(&threads[4],NULL,core_remove_thread,&c));wait_ops_attempts(5);mutex_unlock(&p.ops_lock);
 for(unsigned i=0;i<5;i++)assert(!pthread_join(threads[i],NULL));
 watched_ops=NULL;expect_count(&p,0);
 assert(c.at_events[MT6797_CCCI_AT_CLOSE]==1&&p.unregistered==1&&p.dev.released);destroy(&c,&p);
}
static void cancelled_queue_race(void){
 struct mt6797_ccci c;struct wwan_port p;struct wwan_device dev;init(&c,&p,&dev);
 assert(!pthread_barrier_init(&schedule_enter,NULL,2));assert(!pthread_barrier_init(&schedule_release,NULL,2));pause_schedule=true;
 pthread_t t1,t2;assert(!pthread_create(&t1,NULL,fatal_thread,&c));barrier(&schedule_enter);
 assert(!pthread_create(&t2,NULL,cleanup_thread,&c));barrier(&schedule_release);assert(!pthread_join(t1,NULL)&&!pthread_join(t2,NULL));
 assert(c.at_work_stopped&&!c.at_terminal_work.pending&&c.at_terminal_work.queued==1&&p.unregistered==1);
 mt6797_ccci_at_terminal_queue(&c);assert(c.at_terminal_work.queued==1&&!c.at_terminal_work.pending);
 assert(!pthread_barrier_destroy(&schedule_enter));assert(!pthread_barrier_destroy(&schedule_release));destroy(&c,&p);
}
static void late_tx_and_purge(void){
 struct mt6797_ccci c;struct wwan_port p;struct wwan_device dev;init(&c,&p,&dev);c.ready=1;open_port(&p);
 mt6797_ccci_at_rx(&c,frame());assert(p.rxq.qlen==1);
 assert(!pthread_barrier_init(&tx_enter,NULL,2));assert(!pthread_barrier_init(&tx_release,NULL,2));pause_tx=true;
 pthread_t t1,t2;assert(!pthread_create(&t1,NULL,tx_thread,&c));barrier(&tx_enter);assert(!pthread_create(&t2,NULL,cleanup_thread,&c));barrier(&tx_release);
 assert(!pthread_join(t1,NULL)&&!pthread_join(t2,NULL));assert(!c.at_port&&!p.rxq.qlen&&p.unregistered==1);
 mt6797_ccci_tx_wake(&c,1);mt6797_ccci_at_rx(&c,frame());assert(!p.rxq.qlen);close_port(&p);
 assert(!pthread_barrier_destroy(&tx_enter));assert(!pthread_barrier_destroy(&tx_release));destroy(&c,&p);
}
static void normal_close(void){
 struct mt6797_ccci c;struct wwan_port p;struct wwan_device dev;init(&c,&p,&dev);open_port(&p);close_port(&p);
 assert(p.ops&&!p.unregistered&&!c.at_rx_stopped&&!c.at_terminal_work.queued);
 c.ready=1;mt6797_ccci_at_rx(&c,frame());assert(c.at_rx_queue.qlen==1);mt6797_ccci_cleanup_port(&c);assert(!c.at_rx_queue.qlen);destroy(&c,&p);
}
static void running_work_cleanup(void){
 struct mt6797_ccci c;struct wwan_port p;struct wwan_device dev;init(&c,&p,&dev);open_port(&p);
 mt6797_ccci_quiesce_from(&c,-EIO,"transport");
 assert(!pthread_barrier_init(&unregister_enter,NULL,2));assert(!pthread_barrier_init(&unregister_release,NULL,2));pause_unregister=true;atomic_store(&cleanup_done,false);
 pthread_t t1,t2;assert(!pthread_create(&t1,NULL,work_thread,&c.at_terminal_work));barrier(&unregister_enter);
 assert(!pthread_create(&t2,NULL,cleanup_thread,&c));
 assert(!pthread_mutex_lock(&c.at_terminal_work.lock));while(!c.at_terminal_work.cancelled)assert(!pthread_cond_wait(&c.at_terminal_work.cond,&c.at_terminal_work.lock));assert(c.at_terminal_work.running&&!atomic_load(&cleanup_done));assert(!pthread_mutex_unlock(&c.at_terminal_work.lock));
 barrier(&unregister_release);assert(!pthread_join(t1,NULL)&&!pthread_join(t2,NULL));assert(atomic_load(&cleanup_done)&&p.unregistered==1&&!c.at_terminal_work.running);
 c.owner_alive=false;close_port(&p);assert(!pthread_barrier_destroy(&unregister_enter));assert(!pthread_barrier_destroy(&unregister_release));destroy(&c,&p);
}
#endif
int main(void){fatal_waiters();
#if CURRENT
 multiple_opens();for(unsigned i=0;i<32;i++){close_remove_order(false);close_remove_order(true);concurrent_closes();cancelled_queue_race();late_tx_and_purge();running_work_cleanup();}normal_close();probe_publication();
 puts("PASS actual WWAN release: multiple opens, mixed closes, removal, locked stop order and concurrent FD release without count wrap or double stop");
#endif
 puts("PASS actual native fatal/core withdrawal: RX/TX wake, HUP, retained FD, purge, idempotence, queue-cancel admission and late-tx lifetime");}
'''

PROBE_SHIM = r'''
struct platform_device { struct device dev; };
struct wwan_port_caps { int unused; };
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define PTR_ERR(p) ((intptr_t)(p))
static struct wwan_port_ops ops;
#define mt6797_ccci_at_ops ops
static struct wwan_port *probe_port;
static unsigned probe_case;
static bool opened_during_create;
static struct wwan_port *wwan_create_port(struct device *dev,unsigned type,
 struct wwan_port_ops *callbacks,struct wwan_port_caps *caps,struct mt6797_ccci *c){
 (void)dev;(void)type;(void)caps;assert(callbacks==&ops&&!c->at_port&&c->at_work_stopped);
 if(probe_case==1)return (void *)(intptr_t)-ENOMEM;
 if(probe_case==3){get_device(&probe_port->dev);assert(!wwan_port_op_start(probe_port));opened_during_create=true;}
 if(probe_case==2||probe_case==3)mt6797_ccci_quiesce_from(c,-EIO,"before-publication");
 return probe_port;
}
static int devm_add_action_or_reset(struct device *dev,void (*action)(void *),void *data){
 (void)dev;struct mt6797_ccci *c=data;
 assert(c->at_port==probe_port&&!c->at_work_stopped);
 if(probe_case==4){action(data);return -ENOMEM;}
 if(probe_case==5){mt6797_ccci_quiesce_from(c,-EIO,"after-publication");run_work(&c->at_terminal_work);}
 return 0;
}
'''

PROBE_TESTS = r'''
static void probe_publication(void){
 for(probe_case=0;probe_case<6;probe_case++){
  struct mt6797_ccci c;struct wwan_port p;struct wwan_device dev;struct platform_device platform={0};init(&c,&p,&dev);
  c.at_port=NULL;c.at_work_stopped=true;probe_port=&p;opened_during_create=false;
  int ret=actual_probe_port(&platform,&c);
  if(probe_case==0){assert(!ret&&c.at_port==&p&&!c.at_work_stopped);mt6797_ccci_cleanup_port(&c);}
  else if(probe_case==1){assert(ret==-ENOMEM&&!c.at_port&&c.at_work_stopped&&!p.unregistered&&!c.at_terminal_work.queued);put_device(&p.dev);}
  else if(probe_case==2||probe_case==3){assert(ret==-ESHUTDOWN&&!c.at_port&&c.at_work_stopped&&p.unregistered==1&&!c.at_terminal_work.queued);}
  else if(probe_case==4){assert(ret==-ENOMEM&&!c.at_port&&c.at_work_stopped&&p.unregistered==1);}
  else {assert(!ret&&!c.at_port&&p.unregistered==1);mt6797_ccci_cleanup_port(&c);}
  if(opened_during_create){c.owner_alive=false;close_port(&p);}destroy(&c,&p);
 }
}
'''

def build_and_run(path, name, driver, current, core_source=CORE):
    owners = ['mt6797_ccci_frontend_ready', 'mt6797_ccci_at_rx_close',
              'mt6797_ccci_remove_port']
    if current:
        owners += ['mt6797_ccci_publish_port', 'mt6797_ccci_at_terminal_work',
                   'mt6797_ccci_at_terminal_queue', 'mt6797_ccci_at_terminal_close',
                   'mt6797_ccci_cleanup_port']
    owners += ['mt6797_ccci_quiesce_from', 'mt6797_ccci_tx_wake',
               'mt6797_ccci_at_start', 'mt6797_ccci_at_stop', 'mt6797_ccci_at_rx']
    core = ['wwan_remove_port', 'wwan_port_op_start', 'wwan_port_op_stop', 'wwan_port_op_tx',
            'is_read_blocked', 'is_write_blocked', 'wwan_wait_rx', 'wwan_wait_tx',
            'wwan_port_fops_poll', 'wwan_port_fops_read', 'wwan_port_fops_release']
    limits = '\n'.join(re.findall(r'^#define MT6797_CCCI_(?:RX_SIZE|AT_RX_\w+)[^\n]*', driver, re.M))
    # The diagnostic counters are not the lifetime mechanism under test.
    native = '\n'.join(body(driver, n).replace('&ccci->diag', 'ccci') for n in owners)
    c = f'#define CURRENT {int(current)}\n' + SHIM + limits + '\n' + native + '\n'
    c += '\n'.join(body(core_source, n) for n in core)
    if current:
        probe_body = body(driver, 'mt6797_ccci_probe')
        assert probe_body.index('ccci->at_work_stopped = true') < probe_body.index('devm_request_irq')
        assert probe_body.index('INIT_WORK(&ccci->at_terminal_work') < probe_body.index('devm_request_irq')
        start = driver.index('\tport = wwan_create_port(')
        end = driver.index('\tret = wwan_register_ops(', start)
        fragment = driver[start:end]
        probe = ('static int actual_probe_port(struct platform_device *pdev,struct mt6797_ccci *ccci){\n'
                 'struct wwan_port_caps at_caps={0};struct wwan_port *port;int ret;\n' +
                 fragment + 'return 0;\n}\n')
        c += PROBE_SHIM + probe + 'static void probe_publication(void);\n'
    c += TESTS
    if current:
        c += PROBE_TESTS
    test = path / (name + '.c')
    binary = path / name
    test.write_text(c)
    cmd = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-Wno-unused-function', '-pthread',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
    ] + shlex.split(os.environ.get('HOSTCFLAGS', '')) + [str(test), '-o', str(binary)]
    start = time.time()
    comp = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    run = None
    if comp.returncode == 0:
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    return dict(command=cmd, compile_exit=comp.returncode,
                run_exit=None if run is None else run.returncode, terminal=True,
                compile_output=comp.stdout + comp.stderr,
                run_output='' if run is None else run.stdout + run.stderr,
                started=start, ended=time.time(),
                driver_sha256=hashlib.sha256(driver.encode()).hexdigest(),
                core_sha256=hashlib.sha256(core_source.encode()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--old-source', type=Path,
                        help='also compile and reject a saved pre-withdrawal native owner')
    parser.add_argument('--old-core', type=Path,
                        help='also compile and reject a saved core with release underflow')
    args = parser.parse_args()
    driver = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text()
    with tempfile.TemporaryDirectory(prefix='gemini-cellular-at-terminal-') as tmp:
        current = build_and_run(Path(tmp), 'current', driver, True)
        result = dict(current=current,
                      fixture_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                      core_sha256=hashlib.sha256(CORE.encode()).hexdigest())
        passed = current['compile_exit'] == 0 and current['run_exit'] == 0
        if args.old_source:
            old = build_and_run(Path(tmp), 'old', args.old_source.read_text(), False)
            result['old'] = old
            passed = (passed and old['compile_exit'] == 0 and old['run_exit'] == 1 and
                      'OLD_FATAL_RETAINED_OPS: no hangup; RX/TX still blocked' in old['run_output'])
        if args.old_core:
            old_core = build_and_run(Path(tmp), 'old-core', driver, True, args.old_core.read_text())
            result['old_core'] = old_core
            passed = (passed and old_core['compile_exit'] == 0 and old_core['run_exit'] == 1 and
                      'OLD_CORE_RELEASE_ACCOUNTING:' in old_core['run_output'])
        print(json.dumps(result, indent=2))
        return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
