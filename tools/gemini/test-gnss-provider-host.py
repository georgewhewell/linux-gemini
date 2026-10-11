#!/usr/bin/env python3
"""Exercise actual GNSS provider power/credit/lifetime code with host callbacks.

Hardware, regulator core and STP wire parsing are outside this fixture. The
separate GNSS RX test covers actual FIFO admission and STP replay ownership.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile


def function(path, signature):
    source = path.read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    level = 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
headers = Path(sys.argv[2]) if len(sys.argv) > 2 else root
owner = root / 'drivers/soc/mediatek/mt6797-consys.c'
endpoint = root / 'drivers/gnss/mtk-consys.c'


def macro(path, name):
    lines = iter(path.read_text().splitlines(keepends=True))
    for line in lines:
        if re.match(r'^#define\s+' + re.escape(name) + r'\s', line):
            definition = line
            while line.rstrip().endswith('\\'):
                line = next(lines)
                definition += line
            return definition
    raise ValueError(f'Missing production macro {name}')


defines = ''.join(macro(owner, name) for name in
                  ['MT6797_CONSYS_WMT_TIMEOUT_MS', 'MT6797_CONSYS_RADIO_TX_TIMEOUT_MS',
                   'MT6797_CONSYS_RADIO_TX_RETRY_MIN_US', 'MT6797_CONSYS_RADIO_TX_RETRY_MAX_US'])
defines += ''.join(macro(headers / 'include/linux/soc/mediatek/mt6797-consys-stp-session.h', name)
                   for name in ['MT6797_CONSYS_STP_TX_TIMEOUT_MS', 'MT6797_CONSYS_STP_RETRY_LIMIT'])
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
#define EPROBE_DEFER 517
#define ERESTARTSYS 512
#define GFP_KERNEL 0
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define READ_ONCE(p) (p)
#define max_t(t,a,b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define time_after_eq(a,b) ((long)((a)-(b)) >= 0)
#define msecs_to_jiffies(n) (n)
DEFINES
static unsigned long jiffies;
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static unsigned live_allocations;
static void *consumer_lease; static bool consumer_detached;
static void *allocate(size_t n) { void *p = calloc(1,n); assert(p); live_allocations++; return p; }
#define kzalloc_obj(obj, flags) allocate(sizeof(obj))
static void kfree(void *p) {
 assert(p && live_allocations);
 if (p == consumer_lease) { assert(consumer_detached); consumer_lease = NULL; }
 live_allocations--; free(p);
}
struct device { struct device *parent; void *data; };
static void *dev_get_drvdata(struct device *d) { return d->data; }
#define dev_err(d, ...) ((void)(d))
enum mtk_stp_channel { MTK_STP_BT = 0, MTK_STP_GNSS = 2 };
enum { MT6797_CONSYS_OWNER_ACTIVE = 1, MT6797_CONSYS_OWNER_ERROR = 2 };
struct mt6797_consys_stp_session_lease { bool valid; };
struct mtk_wmt_match { u8 opcode; const u8 *data; size_t data_len; };
static const u8 mt6797_consys_wmt_ok[] = {0};
static const struct mtk_wmt_match mt6797_consys_func_reply = {6, mt6797_consys_wmt_ok, 1};
struct mt6797_consys;
struct mt6797_consys_bt_lease { struct mt6797_consys *consys; bool active; };
struct mt6797_consys_gnss_lease { struct mt6797_consys *consys; bool powered; };
struct mt6797_consys_btif_ops {
 int (*wmt_transaction)(void *, const struct mt6797_consys_stp_session_lease *, u8,
                        const u8 *, size_t, const struct mtk_wmt_match *, unsigned long);
 int (*arm_session_timeout)(void *, const struct mt6797_consys_stp_session_lease *);
 void (*stop)(void *);
};
struct mt6797_consys {
 struct mutex lifecycle_lock;
 unsigned state;
 const struct mt6797_consys_stp_session_lease *stp_lease;
 struct mt6797_consys_gnss_lease *gnss_lease;
 struct mt6797_consys_bt_lease *bt_lease;
 const struct mt6797_consys_btif_ops *btif_ops;
 void *btif_context;
};
static struct mt6797_consys consys;
static struct mt6797_consys_stp_session_lease session;
static char events[64]; static unsigned event_count;
static void event(char e) { assert(event_count < sizeof(events)-1); events[event_count++] = e; }
static int wmt_on_error, wmt_off_error, tx_error, credit_error, arm_error;
static unsigned wmt_calls, tx_calls, arm_calls, stop_calls, credit_calls, tx_again;
static enum mtk_stp_channel transmitted_channel;
static int wmt(void *ctx, const struct mt6797_consys_stp_session_lease *s, u8 opcode,
               const u8 *data, size_t n, const struct mtk_wmt_match *reply, unsigned long timeout)
{
 assert(ctx == &consys && consys.lifecycle_lock.held && s == &session && s->valid);
 assert(opcode == 6 && n == 2 && (data[0] == 0 || data[0] == 2) && data[1] <= 1);
 assert(reply->opcode == 6 && reply->data_len == 1 && reply->data[0] == 0);
 assert(timeout == MT6797_CONSYS_WMT_TIMEOUT_MS); wmt_calls++; event(data[1] ? 'N' : 'F');
 return data[1] ? wmt_on_error : wmt_off_error;
}
static int arm(void *ctx, const struct mt6797_consys_stp_session_lease *s)
{ assert(ctx == &consys && consys.lifecycle_lock.held && s == &session); arm_calls++; return arm_error; }
static void stop(void *ctx)
{ assert(ctx == &consys && !consys.lifecycle_lock.held); stop_calls++; event('S'); }
static void mt6797_consys_radio_fence(struct mt6797_consys *c)
{ assert(c->lifecycle_lock.held); c->state = MT6797_CONSYS_OWNER_ERROR; c->stp_lease = NULL; }
static int mt6797_consys_stp_session_transmit(const struct mt6797_consys_stp_session_lease *s,
                                            enum mtk_stp_channel channel, const u8 *data, size_t n)
{
 assert(consys.lifecycle_lock.held && s == &session && s->valid && data && n);
 transmitted_channel = channel; tx_calls++; if (tx_again) { tx_again--; return -EAGAIN; } return tx_error;
}
static int mt6797_consys_stp_session_wait_tx_credit(const struct mt6797_consys_stp_session_lease *s,
                                                 unsigned long timeout)
{ assert(!consys.lifecycle_lock.held && s == &session && timeout); credit_calls++; return credit_error; }
static void usleep_range(unsigned min, unsigned max)
{ assert(min == MT6797_CONSYS_RADIO_TX_RETRY_MIN_US && max == MT6797_CONSYS_RADIO_TX_RETRY_MAX_US); jiffies++; }
struct regulator { unsigned votes; int enable_error, disable_error; };
static int regulator_enable(struct regulator *r)
{ event('E'); if (r->enable_error) return r->enable_error; r->votes++; return 0; }
static int regulator_disable(struct regulator *r)
{ event('D'); assert(r->votes); if (r->disable_error) return r->disable_error; r->votes--; return 0; }
struct mtk_gnss {
 struct mutex lock; bool initialized, registered, unregistering, transitioning, active, faulted;
 struct gnss_device *gdev; const struct mt6797_consys_stp_session_lease *session_lease;
 int transition_queue;
 struct { int (*set_power)(void *, bool); int (*transmit)(void *, const u8 *, size_t); void *context; } transport;
};
static int mtk_gnss_wait_for_transition(struct mtk_gnss *g)
{ assert(!g->transitioning); mutex_lock(&g->lock); return 0; }
static void mtk_gnss_wait_for_transition_uninterruptible(struct mtk_gnss *g)
{ assert(mtk_gnss_wait_for_transition(g) == 0); }
static void mtk_gnss_complete_transition(struct mtk_gnss *g)
{ assert(g->lock.held && g->transitioning); g->transitioning = false; }
struct mt6797_gnss_device {
 struct device *dev; struct mtk_gnss endpoint;
 const struct mt6797_consys_gnss_lease *lease; struct regulator *lna; bool lna_enabled;
};
struct platform_device { struct mt6797_gnss_device *data; };
static void *platform_get_drvdata(struct platform_device *p) { return p->data; }
struct gnss_device { struct mtk_gnss *data; };
static void mtk_gnss_stop(struct mtk_gnss *g);
static void gnss_deregister_device(struct gnss_device *g)
{ event('U'); mtk_gnss_stop(g->data); }
static void gnss_put_device(struct gnss_device *g)
{ assert(!g->data->registered && !g->data->gdev); event('P'); consumer_detached = true; }
static int mtk_gnss_receive(void *ctx, const u8 *payload, size_t n)
{ (void)ctx; (void)payload; (void)n; return 0; }
static int mt6797_consys_stp_session_unbind_gnss(
 const struct mt6797_consys_stp_session_lease *s,
 int (*receive)(void *, const u8 *, size_t), void *ctx)
{
 struct mtk_gnss *g = ctx;
 assert(s == &session && receive == mtk_gnss_receive && !g->transitioning && !g->active);
 event('R'); return 0;
}
#define wake_up_all(q) ((void)(q))
#define wait_event(q,condition) do { (void)(q); assert(condition); } while (0)
'''.replace('DEFINES', defines)
for name in ['static int mt6797_consys_func_wmt(', 'static bool mt6797_consys_radio_owned(',
             'static int mt6797_consys_radio_transmit(', 'int mt6797_consys_bt_transmit(',
             'mt6797_consys_gnss_acquire(', 'int mt6797_consys_gnss_set_power(',
             'int mt6797_consys_gnss_transmit(', 'int mt6797_consys_gnss_release(']:
    extracted = function(owner, name)
    if name == 'mt6797_consys_gnss_acquire(':
        extracted = 'const struct mt6797_consys_gnss_lease *\n' + extracted
    code += extracted + '\n'
for name in ['static int mt6797_gnss_lna_off(', 'static void mt6797_gnss_cleanup_lna(',
             'static int mt6797_gnss_set_power(', 'static int mt6797_gnss_transmit(',
             'int mtk_gnss_write(', 'void mtk_gnss_stop(', 'void mtk_gnss_unregister(',
             'static void mt6797_gnss_remove(']:
    code += function(endpoint, name) + '\n'
code += r'''
static const struct mt6797_consys_btif_ops ops = {wmt, arm, stop};
static struct device parent, child;
static struct regulator lna;
static struct mt6797_gnss_device device;
static struct gnss_device gdev;
static struct mt6797_consys_bt_lease bt;
static void reset(void)
{
 assert(live_allocations == 0);
 memset(&consys, 0, sizeof(consys)); memset(&device, 0, sizeof(device));
 memset(&lna, 0, sizeof(lna)); memset(events, 0, sizeof(events)); event_count = 0;
 wmt_on_error = wmt_off_error = tx_error = credit_error = arm_error = 0;
 wmt_calls = tx_calls = arm_calls = stop_calls = credit_calls = tx_again = 0; jiffies = 0;
 session.valid = true; consys.state = MT6797_CONSYS_OWNER_ACTIVE;
 consys.stp_lease = &session; consys.btif_ops = &ops; consys.btif_context = &consys;
 parent = (struct device){.data = &consys}; child = (struct device){.parent = &parent};
 device.dev = &child; device.lna = &lna;
 bt = (struct mt6797_consys_bt_lease){&consys, true}; consys.bt_lease = &bt;
}
static void acquire(void)
{
 const struct mt6797_consys_stp_session_lease *borrowed;
 device.lease = mt6797_consys_gnss_acquire(&child, &borrowed);
 assert(!IS_ERR(device.lease) && borrowed == &session && wmt_calls == 0 && lna.votes == 0);
 device.endpoint = (struct mtk_gnss){.initialized = true, .registered = true,
  .transport = {mt6797_gnss_set_power, mt6797_gnss_transmit, &device},
  .gdev = &gdev, .session_lease = borrowed};
 gdev.data = &device.endpoint; consumer_lease = (void *)device.lease; consumer_detached = false;
}
static void release(void)
{
 mtk_gnss_unregister(&device.endpoint); mt6797_gnss_cleanup_lna(&device);
 assert(mt6797_consys_gnss_release(device.lease) == 0);
}
int main(void)
{
 const u8 payload[] = {1,2,3}; const struct mt6797_consys_stp_session_lease *borrowed;
 reset(); assert(PTR_ERR(mt6797_consys_gnss_acquire(NULL, &borrowed)) == -EINVAL);
 parent.data = NULL; assert(PTR_ERR(mt6797_consys_gnss_acquire(&child, &borrowed)) == -EPROBE_DEFER);
 parent.data = &consys; consys.state = MT6797_CONSYS_OWNER_ERROR;
 assert(PTR_ERR(mt6797_consys_gnss_acquire(&child, &borrowed)) == -ESHUTDOWN && !borrowed);
 consys.state = MT6797_CONSYS_OWNER_ACTIVE; acquire();
 assert(PTR_ERR(mt6797_consys_gnss_acquire(&child, &borrowed)) == -EBUSY && !borrowed);
 assert(live_allocations == 1 && mt6797_consys_gnss_transmit(device.lease, payload, 3) == -ESHUTDOWN);
 assert(mt6797_gnss_set_power(&device, true) == 0 && !strcmp(events, "EN"));
 assert(mt6797_gnss_set_power(&device, true) == 0 && lna.votes == 1 && wmt_calls == 1);
 device.endpoint.active = true;
 tx_again = 2; assert(mtk_gnss_write(&device.endpoint, payload, 3) == 3);
 assert(tx_calls == 3 && credit_calls == 2 && arm_calls == 1 && transmitted_channel == MTK_STP_GNSS);
 credit_error = -ETIMEDOUT; tx_again = 1;
 assert(mtk_gnss_write(&device.endpoint, payload, 3) == -EAGAIN && device.endpoint.active);
 credit_error = -ERESTARTSYS; tx_again = 1;
 assert(mtk_gnss_write(&device.endpoint, payload, 3) == -ERESTARTSYS && device.endpoint.active);
 credit_error = 0; tx_again = ~0U; unsigned previous_calls = tx_calls;
 assert(mtk_gnss_write(&device.endpoint, payload, 3) == -EAGAIN && device.endpoint.active);
 assert(jiffies == MT6797_CONSYS_RADIO_TX_TIMEOUT_MS + 2);
 assert(tx_calls - previous_calls == MT6797_CONSYS_RADIO_TX_TIMEOUT_MS + 1);
 credit_error = 0; tx_again = 1;
 assert(mt6797_consys_bt_transmit(&bt, payload, 3) == 0 && transmitted_channel == MTK_STP_BT);
 struct platform_device pdev = {&device}; mt6797_gnss_remove(&pdev);
 assert(!strcmp(events, "ENUDFRP") && !consys.gnss_lease && live_allocations == 0 && lna.votes == 0);
 reset(); acquire(); lna.enable_error = -EIO;
 assert(mt6797_gnss_set_power(&device, true) == -EIO && wmt_calls == 0 && lna.votes == 0); release();
 reset(); acquire(); wmt_on_error = -EPROTO;
 assert(mt6797_gnss_set_power(&device, true) == -EPROTO && !strcmp(events, "ENSD"));
 assert(consys.state == MT6797_CONSYS_OWNER_ERROR && stop_calls == 1 && lna.votes == 0);
 assert(mt6797_consys_gnss_set_power(device.lease, true) == -ESHUTDOWN && wmt_calls == 1); release();
 reset(); acquire(); assert(mt6797_gnss_set_power(&device, true) == 0); wmt_off_error = -EIO;
 assert(mt6797_gnss_set_power(&device, false) == -EIO && !strcmp(events, "ENDFS"));
 assert(!lna.votes && stop_calls == 1 && consys.state == MT6797_CONSYS_OWNER_ERROR); release();
 reset(); acquire(); assert(mt6797_gnss_set_power(&device, true) == 0); lna.disable_error = -EIO;
 assert(mt6797_gnss_set_power(&device, false) == -EIO && device.lna_enabled && lna.votes == 1);
 assert(!consys.gnss_lease->powered && wmt_calls == 2);
 lna.disable_error = 0; release(); assert(lna.votes == 0 && !device.lna_enabled);
 reset(); acquire(); device.lna = NULL;
 assert(mt6797_gnss_set_power(&device, true) == 0 && !strcmp(events, "N"));
 assert(mt6797_gnss_set_power(&device, false) == 0 && !strcmp(events, "NF")); release();
 reset(); acquire(); assert(mt6797_gnss_set_power(&device, true) == 0); device.endpoint.active = true;
 tx_error = -ESHUTDOWN; assert(mtk_gnss_write(&device.endpoint, payload, 3) == -ESHUTDOWN);
 assert(!device.endpoint.active && device.endpoint.faulted && stop_calls == 1);
 mtk_gnss_stop(&device.endpoint); assert(!lna.votes && device.endpoint.faulted);
 assert(mtk_gnss_write(&device.endpoint, payload, 3) == -EIO); release();
 reset(); acquire(); assert(mt6797_gnss_set_power(&device, true) == 0); device.endpoint.active = true;
 arm_error = -EIO; assert(mtk_gnss_write(&device.endpoint, payload, 3) == -EIO && stop_calls == 1); release();
 reset(); acquire(); session.valid = false; consys.state = MT6797_CONSYS_OWNER_ERROR; consys.stp_lease = NULL;
 assert(mt6797_consys_gnss_transmit(device.lease, payload, 3) == -ESHUTDOWN && tx_calls == 0);
 assert(mt6797_consys_bt_transmit(&bt, payload, 3) == -ESHUTDOWN); release();
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='gnss-provider-') as tmp:
    source, binary = Path(tmp) / 'test.c', Path(tmp) / 'test'
    source.write_text(code)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-sign-compare', '-g', '-fno-pie', '-no-pie',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS GNSS provider power, whole-frame credit, shared BT path and teardown ordering')
