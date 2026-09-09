#!/usr/bin/env python3
"""Controlled pthread interleavings of production TX/stop/wake/disconnect/purge.

The radio spinlock is represented by a real pthread mutex. Netdev, skb, HIF
and workqueue primitives remain host fakes; this does not simulate kernel IRQ
or scheduler behavior. Barriers force both lock orders instead of relying on
an opportunistic stress loop. Run under ASan/UBSan with a bounded deadline.
"""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile

base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-tx-host.py')))
program = base['program'].split('int main(void) {')[0]
program = program.replace('#include <assert.h>', '''#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
enum role { CONTROLLER, WORKER, PRODUCER };
enum point { STOP_HELD, WAKE_HELD, AFTER_WAKE_UNLOCK, SEND_PAUSED, POINTS };
static pthread_mutex_t radio_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static _Thread_local enum role role;
static _Thread_local int after_wake;
static int enabled[POINTS], reached[POINTS], released[POINTS], attempted[3];
static unsigned stop_calls,wake_calls,first_wake_depth,pause_send_call;
static int pause_once(enum point point) {
 assert(!pthread_mutex_lock(&gate_mutex));
 if(!enabled[point] || reached[point]) { assert(!pthread_mutex_unlock(&gate_mutex)); return 0; }
 reached[point]=1; assert(!pthread_cond_broadcast(&gate_cond));
 while(!released[point]) assert(!pthread_cond_wait(&gate_cond,&gate_mutex));
 assert(!pthread_mutex_unlock(&gate_mutex)); return 1;
}
static void lock_radio(int *lock) {
 assert(!pthread_mutex_lock(&gate_mutex)); attempted[role]++;
 assert(!pthread_cond_broadcast(&gate_cond)); assert(!pthread_mutex_unlock(&gate_mutex));
 assert(!pthread_mutex_lock(&radio_mutex)); assert(!*lock); *lock=1;
}
static void unlock_radio(int *lock) {
 assert(*lock); *lock=0; assert(!pthread_mutex_unlock(&radio_mutex));
 if(role==WORKER && after_wake) { after_wake=0; pause_once(AFTER_WAKE_UNLOCK); }
}''')
program = program.replace('#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=1; (f)=0; } while(0)',
                          '#define spin_lock_irqsave(l,f) do { lock_radio(l); (f)=0; } while(0)')
program = program.replace('#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(*(l)); *(l)=0; } while(0)',
                          '#define spin_unlock_irqrestore(l,f) do { (void)(f); unlock_radio(l); } while(0)')
program = program.replace('static ktime_t test_time;', 'static _Atomic ktime_t test_time;')
program = program.replace('static void netif_stop_queue(struct net_device *d) { d->stopped=true; }', '''
static void netif_stop_queue(struct net_device *d) {
 assert(r.lock && r.tx_queue.length==MT6797_RADIO_TX_QUEUE_LIMIT);
 d->stopped=true; stop_calls++;
 if(role==PRODUCER) pause_once(STOP_HELD);
}''')
program = program.replace('static void netif_wake_queue(struct net_device *d) { d->stopped=false; }', '''
static void netif_wake_queue(struct net_device *d) {
 assert(r.lock && r.tx_queue.length<MT6797_RADIO_TX_WAKE_THRESHOLD);
 assert(c.connected && !c.cancel && !r.stopping);
 if(!wake_calls++) first_wake_depth=r.tx_queue.length;
 d->stopped=false;
 if(role==WORKER && pause_once(WAKE_HELD)) after_wake=1;
}''')
program = program.replace(' calls++;\n check_snapshot();',
                          ' calls++;\n if(role==WORKER && calls==pause_send_call) pause_once(SEND_PAUSED);\n check_snapshot();')
# The serial fixture compares live state before/after a read; concurrent enqueue
# may legitimately change it. Validate the production formatter's coherent copy.
start = program.index('static void check_snapshot(void) {')
end = program.index('static struct sk_buff *packet(', start)
program = program[:start] + '''static void check_snapshot(void) {
 char text[PAGE_SIZE];
 assert(tx_diagnostics_show((struct device *)&dev,NULL,text)>0);
 assert(field(text,"queue_limit")==MT6797_RADIO_TX_QUEUE_LIMIT);
 assert(field(text,"queue_wake_below")==MT6797_RADIO_TX_WAKE_THRESHOLD);
 assert(field(text,"queue_depth")<=field(text,"queue_high_water"));
 assert(field(text,"queue_high_water")<=MT6797_RADIO_TX_QUEUE_LIMIT);
 assert(field(text,"enqueued")==field(text,"dequeued")+field(text,"purged")+field(text,"queue_depth"));
 assert(field(text,"dequeued")==field(text,"hif_submitted")+field(text,"send_failed")+field(text,"send_active"));
}
''' + program[end:]
program += base['base']['function']('mt6797_radio_disconnect')
program += r'''
static void reset(void) {
 setup(); role=CONTROLLER; after_wake=0;
 memset(enabled,0,sizeof(enabled)); memset(reached,0,sizeof(reached));
 memset(released,0,sizeof(released)); memset(attempted,0,sizeof(attempted));
 stop_calls=wake_calls=first_wake_depth=pause_send_call=0;
}
static void wait_point(enum point point) {
 assert(!pthread_mutex_lock(&gate_mutex));
 while(!reached[point]) assert(!pthread_cond_wait(&gate_cond,&gate_mutex));
 assert(!pthread_mutex_unlock(&gate_mutex));
}
static void release_point(enum point point) {
 assert(!pthread_mutex_lock(&gate_mutex)); released[point]=1;
 assert(!pthread_cond_broadcast(&gate_cond)); assert(!pthread_mutex_unlock(&gate_mutex));
}
static void wait_attempt(enum role who) {
 assert(!pthread_mutex_lock(&gate_mutex));
 while(!attempted[who]) assert(!pthread_cond_wait(&gate_cond,&gate_mutex));
 assert(!pthread_mutex_unlock(&gate_mutex));
}
static void fill(unsigned count) { for(unsigned i=0;i<count;i++) submit(ETH_P_IP); }
static void *producer(void *count) { role=PRODUCER; fill(*(unsigned *)count); return NULL; }
static void *worker(void *unused) { (void)unused; role=WORKER; run(); return NULL; }
static void join(pthread_t thread) { assert(!pthread_join(thread,NULL)); }
int main(void) {
 const unsigned limit=MT6797_RADIO_TX_QUEUE_LIMIT, wake=MT6797_RADIO_TX_WAKE_THRESHOLD;
 assert(limit==16 && wake==8); /* This candidate's explicit expected policy. */
 pthread_t tx,drain; unsigned one=1;

 /* Producer reaches the cap and stops while a worker is trying the same
  * lock. Draining after the stop must wake at depth 7, never at depth 8. */
 reset(); fill(limit-1); enabled[STOP_HELD]=1;
 assert(!pthread_create(&tx,NULL,producer,&one)); wait_point(STOP_HELD);
 assert(!pthread_create(&drain,NULL,worker,NULL)); wait_attempt(WORKER);
 release_point(STOP_HELD); join(tx); join(drain);
 assert(stop_calls==1 && first_wake_depth==wake-1 && !dev.stopped);
 assert(sends==limit && freed==limit && !dev.tx_dropped && !r.tx_diag.busy);
 assert(r.tx_diag.queue_high_water==limit); check_snapshot();

 /* Worker wakes under the lock, then a waiting producer refills to the cap
  * before the worker continues. The new stop must survive the earlier wake;
  * subsequent drain must wake again without a lost stop/wake transition. */
 reset(); fill(limit); enabled[WAKE_HELD]=enabled[AFTER_WAKE_UNLOCK]=1;
 assert(!pthread_create(&drain,NULL,worker,NULL)); wait_point(WAKE_HELD);
 unsigned refill=limit-(wake-1);
 assert(!pthread_create(&tx,NULL,producer,&refill)); wait_attempt(PRODUCER);
 release_point(WAKE_HELD); wait_point(AFTER_WAKE_UNLOCK); join(tx);
 assert(r.tx_queue.length==limit && dev.stopped && stop_calls==2 && wake_calls==1);
 assert(r.tx_diag.queue_high_water==limit); check_snapshot();
 release_point(AFTER_WAKE_UNLOCK); join(drain);
 assert(!dev.stopped && wake_calls==wake+1 && first_wake_depth==wake-1);
 assert(sends==limit+refill && freed==limit+refill && !r.tx_diag.busy && !dev.tx_dropped);

 /* Full-queue BUSY retains every skb byte and issues no new work request.
  * The same caller-owned skb can be retried after drain, exactly once. */
 reset(); fill(limit);
 struct sk_buff *retained=packet(ETH_P_IP); memset(retained->cb,0xa5,sizeof(retained->cb));
 struct sk_buff original=*retained; unsigned requests=queued;
 assert(mt6797_radio_xmit(retained,&dev)==NETDEV_TX_BUSY);
 assert(!memcmp(retained,&original,sizeof(original)) && !freed && queued==requests);
 assert(r.tx_diag.enqueued==limit && r.tx_diag.busy==1 && r.tx_queue.length==limit);
 run(); assert(freed==limit && !dev.stopped);
 assert(mt6797_radio_xmit(retained,&dev)==NETDEV_TX_OK); run();
 assert(freed==limit+1 && sends==limit+1 && r.tx_diag.hif_submitted==limit+1);

 /* Cancellation arrives after dequeue crosses the wake threshold but
  * while an already-started HIF call is paused. Production disconnect and
  * purge remove the pending tail; finishing that call must not wake a
  * cancelled connection. A newly offered skb is rejected, not retained. */
 reset(); fill(limit); pause_send_call=limit-wake+1; enabled[SEND_PAUSED]=1;
 assert(!pthread_create(&drain,NULL,worker,NULL)); wait_point(SEND_PAUSED);
 assert(r.tx_queue.length==wake-1 && dev.stopped && !wake_calls);
 assert(!mt6797_radio_disconnect(r.wdev.wiphy,&dev,WLAN_REASON_DEAUTH_LEAVING));
 mt6797_radio_purge_tx(&r); submit(ETH_P_IP);
 assert(c.cancel && r.tx_diag.purged==wake-1 && r.tx_diag.rejected==1);
 release_point(SEND_PAUSED); join(drain);
 assert(dev.stopped && !wake_calls && !r.tx_diag.send_failed);
 assert(sends==limit-wake+1 && freed==limit+1 && dev.tx_dropped==wake);
 assert(r.tx_diag.dequeued==limit-wake+1 && !r.tx_queue.length);
 check_snapshot();
 puts("PASS: production 16/8 queue stop-before-drain, wake-before-refill, strict wake depth, BUSY ownership/retry, disconnect-before-wake, purge and normal drain with controlled pthread lock interleavings");
}
'''
if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='gemini-tx-queue-race-') as temporary:
        path = Path(temporary)
        (path/'test.c').write_text(program)
        subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-O1', '-g', '-pthread',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(path/'test.c'), '-o', str(path/'test')], check=True)
        subprocess.run([str(path/'test')], check=True, timeout=20)
