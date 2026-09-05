#!/usr/bin/env python3
"""Run actual handoff C functions with pthread wait/device-link test adapters.

This tests publication/wakeup races, not kernel device-core or MMIO emulation.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/soc/mediatek/mt6797-md1-cold-owner.c").read_text()


def function(name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


shim = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#define EPROBE_DEFER 517
#define DL_FLAG_AUTOREMOVE_CONSUMER 16
#define DL_STATE_CONSUMER_PROBE 3
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define msecs_to_jiffies(x) (x)
struct device { int unused; };
struct device_link { int status; };
struct mt6797_md1_post_vector_evidence {
 bool observed, passive_complete;
 int token;
};
static pthread_mutex_t mt6797_md1_post_vector_evidence_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static int mt6797_md1_handoff_waitq;
static int mt6797_md1_handoff_status;
static struct device supplier, consumer;
static struct device *mt6797_md1_handoff_supplier = &supplier;
static struct mt6797_md1_post_vector_evidence mt6797_md1_post_vector_evidence;
static int links, fail_link, link_status;
static struct device_link *device_link_add(struct device *c, struct device *s, int flags)
{
 static struct device_link link;
 link.status = link_status;
 assert(c == &consumer && s == &supplier);
 assert(flags == DL_FLAG_AUTOREMOVE_CONSUMER);
 __atomic_add_fetch(&links, 1, __ATOMIC_SEQ_CST);
 return fail_link ? NULL : &link;
}
static void wake_up_all(int *queue)
{
 assert(queue == &mt6797_md1_handoff_waitq);
 pthread_mutex_lock(&wait_lock);
 pthread_cond_broadcast(&wait_cond);
 pthread_mutex_unlock(&wait_lock);
}
/* Accelerate only elapsed timeout; retain the actual wait predicate/locking. */
#define wait_event_timeout(queue, condition, timeout) ({ \
 assert(&(queue) == &mt6797_md1_handoff_waitq && (timeout) == 10000); \
 struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); \
 deadline.tv_nsec += 100000000; \
 if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; } \
 pthread_mutex_lock(&wait_lock); \
 while (!(condition)) { \
  if (pthread_cond_timedwait(&wait_cond, &wait_lock, &deadline) == ETIMEDOUT) break; \
 } \
 int ready = !!(condition); pthread_mutex_unlock(&wait_lock); ready; })
'''
tests = r'''
static struct mt6797_md1_post_vector_evidence output;
static int result;
static void reset(int status)
{
 mt6797_md1_handoff_status = status; links = 0; fail_link = 0;
 link_status = DL_STATE_CONSUMER_PROBE;
 output = (struct mt6797_md1_post_vector_evidence){ .token = -1 };
}
static void *waiter(void *unused)
{
 (void)unused;
 result = mt6797_md1_cold_owner_wait_handoff(&consumer, &output);
 return NULL;
}
int main(void)
{
 pthread_t thread;
 struct mt6797_md1_post_vector_evidence ready = {
  .observed = true, .passive_complete = true, .token = 17 };
 reset(-EPROBE_DEFER); waiter(NULL);
 assert(result == -EPROBE_DEFER && links == 0 && output.token == -1);
 reset(-EAGAIN); fail_link = 1; waiter(NULL);
 assert(result == -ENOMEM && output.token == -1);
 reset(-EAGAIN); link_status = 5; waiter(NULL);
 assert(result == -EPROBE_DEFER && output.token == -1);
 reset(-EAGAIN); link_status = 0; waiter(NULL);
 assert(result == -EPROBE_DEFER && output.token == -1);
 reset(-EAGAIN); waiter(NULL);
 assert(result == -ETIMEDOUT && output.token == -1);
 reset(-EAGAIN); mt6797_md1_cold_post_vector_publish(&ready); waiter(NULL);
 assert(result == 0 && output.token == 17 && links == 1);
 for (int i = 0; i < 32; i++) {
  reset(-EAGAIN);
  assert(!pthread_create(&thread, NULL, waiter, NULL));
  while (!READ_ONCE(links)) usleep(100);
  if (i & 1) usleep(1000);
  if (i % 3 == 0) mt6797_md1_handoff_finish(-ESHUTDOWN);
  else if (i % 3 == 1) mt6797_md1_handoff_finish(-EIO);
  else mt6797_md1_cold_post_vector_publish(&ready);
  assert(!pthread_join(thread, NULL));
  assert(result == (i % 3 == 0 ? -ESHUTDOWN : i % 3 == 1 ? -EIO : 0));
  assert(output.token == (result ? -1 : 17));
 }
 reset(-EAGAIN); mt6797_md1_handoff_finish(-ESHUTDOWN);
 mt6797_md1_cold_post_vector_publish(&ready); mt6797_md1_handoff_finish(0);
 waiter(NULL); assert(result == -ESHUTDOWN && links == 0);
 puts("PASS actual handoff C: pending, ready, timeout, errors, 32 publication races, shutdown cannot resurrect");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cellular-handoff-") as directory:
    path = Path(directory)
    cfile = path / "test.c"
    cfile.write_text(shim + "\n".join(function(name) for name in (
        "mt6797_md1_cold_owner_wait_handoff", "mt6797_md1_handoff_finish",
        "mt6797_md1_cold_post_vector_publish")) + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-pthread", str(cfile),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=5)

cleanup = function("mt6797_md1_cold_cancel_passive_observer")
assert cleanup.index("handoff_finish(-ESHUTDOWN)") < cleanup.rindex("cancel_work_sync(")
assert cleanup.index("mt6797_md1_stage2_dev = NULL") < cleanup.rindex("cancel_work_sync(")
assert ".shutdown = mt6797_md1_cold_owner_shutdown" in source
transport = (root / "drivers/net/wwan/mediatek/mtk_mt6797_ccci.c").read_text()
assert ".shutdown = mt6797_ccci_remove" in transport
assert "atomic_xchg(&ccci->teardown, 1)" in transport
assert "!evidence.passive_complete" in transport
assert "if (!mapping.reset_completed)" in transport
assert "mt6797_ccci_clean_reset_check(ccci)" in transport
print("PASS lifecycle ordering and reset-lease/fresh-state transport admission wiring")
