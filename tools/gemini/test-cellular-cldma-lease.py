#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual CLDMA lease/reader C with host lifecycle and MMIO adapters."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/soc/mediatek/mt6797-md1-cold-owner.c").read_text()


def function(name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


defines = "\n".join(re.findall(
    r"^#define MT6797_MD1_(?:AP_CLDMA_\w+|CLDMA_\w+)\s+.*$", source, re.M))
shim = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define __iomem
#define IORESOURCE_MEM 0x200
#define MT6797_MD1_RES_AP_CLDMA_AO 0
#define MT6797_MD1_RES_AP_CLDMA_PDN 1
#define MT6797_MD1_POST_VECTOR_GO 1
#define EPROBE_DEFER 517
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define atomic_read(x) READ_ONCE(*(x))
#define atomic_set(x,v) WRITE_ONCE(*(x), (v))
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
struct device { int id; };
struct resource { uint64_t start, end; unsigned long flags; };
#define resource_type(r) ((r)->flags)
#define resource_size(r) ((r)->end - (r)->start + 1)
struct work_struct { int unused; };
struct mt6797_md1_cldma_mapping { void *ao, *pdn; bool reset_completed; };
struct mt6797_md1_post_vector_evidence {
 int token; bool passive_complete, cldma_register_activity;
};
struct mt6797_md1_cold_owner_cldma {
 unsigned coda_version, ul_status, so_status, l2_tx_status, l2_rx_status, irq_mask, ip_busy;
};
struct mt6797_md1_cold_owner_backend {
 void *base[2]; bool cldma_leased, cldma_reset_completed; struct work_struct passive_observe_work;
};
static pthread_mutex_t mt6797_md1_post_vector_evidence_lock = PTHREAD_MUTEX_INITIALIZER;
static struct device consumers[2] = {{0}, {1}};
static unsigned char ao_memory[0x1e00], pdn_memory[0x1e00];
static struct mt6797_md1_cold_owner_backend backend;
static struct mt6797_md1_cold_owner_backend *mt6797_md1_handoff_backend;
static struct device *mt6797_md1_cldma_consumer;
static bool mt6797_md1_cldma_lease_taken;
static int mt6797_md1_handoff_status, mt6797_md1_cold_owner_fatal_latch;
static int wait_error, linked[2], cancel_count, action_error, shutdown_on_cancel, reads;
static struct mt6797_md1_post_vector_evidence input;
static void (*release_action)(void *);
static void *release_data;
static int mt6797_md1_cold_owner_wait_handoff(struct device *consumer,
 struct mt6797_md1_post_vector_evidence *evidence)
{
 if (wait_error) return wait_error;
 __atomic_add_fetch(&linked[consumer->id], 1, __ATOMIC_SEQ_CST);
 *evidence = input;
 return 0;
}
static void cancel_work_sync(struct work_struct *work)
{
 assert(work == &backend.passive_observe_work);
 assert(mt6797_md1_cldma_lease_taken && mt6797_md1_cldma_consumer);
 assert(linked[mt6797_md1_cldma_consumer->id]);
 assert(!backend.cldma_leased);
 /* The worker takes this same lock: acquiring it proves no lock inversion. */
 assert(!pthread_mutex_trylock(&mt6797_md1_post_vector_evidence_lock));
 if (shutdown_on_cancel) mt6797_md1_handoff_status = -ESHUTDOWN;
 pthread_mutex_unlock(&mt6797_md1_post_vector_evidence_lock);
 cancel_count++;
}
static int devm_add_action_or_reset(struct device *consumer,
 void (*action)(void *), void *data)
{
 assert(consumer == data && cancel_count == 1);
 if (action_error) { action(data); return action_error; }
 release_action = action; release_data = data;
 return 0;
}
static void devm_release_action(struct device *consumer,
 void (*action)(void *), void *data)
{
 assert(consumer == data && action == release_action && data == release_data);
 action(data); release_action = NULL;
}
static unsigned readl(void *address)
{
 uintptr_t a = (uintptr_t)address;
 assert((a >= (uintptr_t)ao_memory && a < (uintptr_t)ao_memory + sizeof(ao_memory)) ||
        (a >= (uintptr_t)pdn_memory && a < (uintptr_t)pdn_memory + sizeof(pdn_memory)));
 reads++; return 0;
}
'''
tests = r'''
static struct resource ao, pdn;
static void reset(void)
{
 backend = (struct mt6797_md1_cold_owner_backend){ .base = {ao_memory, pdn_memory}, .cldma_reset_completed = true };
 mt6797_md1_handoff_backend = &backend;
 mt6797_md1_cldma_consumer = NULL; mt6797_md1_cldma_lease_taken = false;
 mt6797_md1_handoff_status = mt6797_md1_cold_owner_fatal_latch = 0;
 wait_error = linked[0] = linked[1] = cancel_count = action_error = shutdown_on_cancel = reads = 0;
 release_action = NULL; release_data = NULL;
 input = (struct mt6797_md1_post_vector_evidence){MT6797_MD1_POST_VECTOR_GO, true, true};
 ao = (struct resource){MT6797_MD1_AP_CLDMA_AO_BASE,
  MT6797_MD1_AP_CLDMA_AO_BASE + MT6797_MD1_CLDMA_SIZE - 1, IORESOURCE_MEM};
 pdn = (struct resource){MT6797_MD1_AP_CLDMA_PDN_BASE,
  MT6797_MD1_AP_CLDMA_PDN_BASE + MT6797_MD1_CLDMA_SIZE - 1, IORESOURCE_MEM};
}
static int acquire(int who, struct mt6797_md1_cldma_mapping *out)
{
 return mt6797_md1_cold_owner_acquire_cldma(&consumers[who], &ao, &pdn, out);
}
static void *racer(void *context)
{
 struct mt6797_md1_cldma_mapping out = {};
 return (void *)(intptr_t)acquire((intptr_t)context, &out);
}
int main(void)
{
 struct mt6797_md1_cldma_mapping out = {};
 struct mt6797_md1_cold_owner_cldma observation;
 reset(); assert(mt6797_md1_cold_owner_acquire_cldma(NULL, &ao, &pdn, &out) == -EINVAL);
 for (int field = 0; field < 6; field++) {
  reset();
  switch (field) {
  case 0: ao.start++; break; case 1: pdn.start++; break;
  case 2: ao.end++; break; case 3: pdn.end++; break;
  case 4: ao.flags = 0; break; case 5: pdn.flags = 0; break;
  }
  assert(acquire(0, &out) == -EINVAL && !linked[0] && !cancel_count);
 }
 for (int error = 0; error < 9; error++) {
  reset();
  switch (error) {
  case 0: wait_error = -EPROBE_DEFER; break;
  case 1: wait_error = -ETIMEDOUT; break;
  case 2: wait_error = -ENOMEM; break;
  case 3: input.token = 0; break;
  case 4: input.passive_complete = false; break;
  case 5: backend.cldma_reset_completed = false; break;
  case 6: mt6797_md1_handoff_status = -ESHUTDOWN; break;
  case 7: mt6797_md1_handoff_backend = NULL; break;
  case 8: mt6797_md1_cold_owner_fatal_latch = 1; break;
  }
  assert(acquire(0, &out) < 0 && !out.ao && !out.pdn);
  assert(!cancel_count && !mt6797_md1_cldma_lease_taken);
 }
 reset(); action_error = -ENOMEM;
 assert(acquire(0, &out) == -ENOMEM && !out.ao && !out.pdn);
 assert(cancel_count == 1 && !mt6797_md1_cldma_consumer);
 assert(mt6797_md1_cldma_lease_taken && mt6797_md1_cold_owner_fatal_latch);
 assert(acquire(1, &out) == -EBUSY);
 reset(); shutdown_on_cancel = 1;
 assert(acquire(0, &out) == -ESHUTDOWN && !out.ao && !release_action);
 assert(!mt6797_md1_cldma_consumer && mt6797_md1_cldma_lease_taken);
 reset(); assert(!mt6797_md1_cold_cldma_registers_observe(&backend, &observation));
 assert(reads == 7); reads = 0;
 input.cldma_register_activity = false; /* reset proof, not circular queue activity */
 assert(!acquire(0, &out) && out.ao == ao_memory && out.pdn == pdn_memory);
 assert(cancel_count == 1 && backend.cldma_leased && release_action && out.reset_completed);
 assert(mt6797_md1_cold_cldma_registers_observe(&backend, &observation) == -EBUSY);
 assert(!reads && acquire(0, &out) == -EBUSY && acquire(1, &out) == -EBUSY);
 mt6797_md1_cold_release_cldma(&consumers[1]);
 assert(mt6797_md1_cldma_consumer == &consumers[0]);
 release_action(release_data); release_action(release_data);
 assert(!mt6797_md1_cldma_consumer && mt6797_md1_cldma_lease_taken);
 assert(acquire(1, &out) == -EBUSY && backend.cldma_leased);
 for (int trial = 0; trial < 32; trial++) {
  pthread_t threads[2]; void *results[2]; reset();
  for (intptr_t i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, racer, (void *)i));
  for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], &results[i]));
  assert(((intptr_t)results[0] == 0 && (intptr_t)results[1] == -EBUSY) ||
         ((intptr_t)results[1] == 0 && (intptr_t)results[0] == -EBUSY));
  assert(cancel_count == 1 && backend.cldma_leased);
 }
 puts("PASS actual CLDMA lease: exact ranges, linked handoff, errors, drain, single owner/32 races, terminal teardown, no observer MMIO after lease");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-cldma-lease-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(shim + defines + "\n" + "\n".join(
        function(name) for name in (
            "mt6797_md1_cold_release_cldma", "mt6797_md1_cold_owner_acquire_cldma",
            "mt6797_md1_cold_cldma_registers_observe")) + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-pthread", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=10)

consumer = (root / "drivers/net/wwan/mediatek/mtk_mt6797_ccci.c").read_text()
probe = consumer[consumer.index("static int mt6797_ccci_probe("):]
assert "devm_platform_ioremap_resource" not in consumer
assert "devm_ioremap_resource(" in function("mt6797_md1_cold_map_resource")
assert probe.index("mt6797_md1_cold_owner_acquire_cldma(") < probe.index("mt6797_ccci_alloc_rings(")
assert probe.index("mt6797_md1_cold_owner_acquire_cldma(") < probe.index("devm_request_irq(")
worker = function("mt6797_md1_cold_passive_observe_work")
assert worker.index("backend->cldma_leased") < worker.index("readl(")
assert source.count("schedule_work(&backend->passive_observe_work)") == 1
# All supplier AP CLDMA register reads are in the guarded, drained worker helper.
assert source.count("mt6797_md1_cold_cldma_registers_observe(") == 4
assert source.count("backend->base[MT6797_MD1_RES_AP_CLDMA_AO]") == 2
assert source.count("backend->base[MT6797_MD1_RES_AP_CLDMA_PDN]") == 2
dd = (root / "drivers/base/dd.c").read_text()
unbind = dd[dd.index("static void __device_release_driver("):]
assert unbind.index("device_unbind_cleanup(dev)") < unbind.index("device_links_driver_cleanup(dev)")
cleanup = dd[dd.index("static void device_unbind_cleanup("):]
assert cleanup.index("devres_release_all(dev)") < cleanup.index("\n}")
print("PASS actual wiring: no duplicate reservation, consumer devres before supplier link cleanup, one-shot drained observer")
