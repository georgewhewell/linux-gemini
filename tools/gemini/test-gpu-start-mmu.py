#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual job-config and pre-submit path with host MMIO/PM adapters."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/gpu/drm/panfrost/panfrost_job.c").read_text()
start = source.index("static int panfrost_job_hw_submit(")
end = source.index("\n\t/* GO ! */", start)
prefix = source[start:end]
# Keep the actual pre-submit control path; end before scheduler/IRQ ownership.
prefix += "\n return 0;\nerr_hwsubmit:\n return ret;\n}\n"
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define lower_32_bits(x) ((u32)(x))
#define upper_32_bits(x) ((u32)((u64)(x) >> 32))
#define WARN_ON(x) (x)
#include "drivers/gpu/drm/panfrost/panfrost_regs.h"
enum { HW_FEATURE_FLUSH_REDUCTION, HW_FEATURE_JOBCHAIN_DISAMBIGUATION };
enum { HW_ISSUE_10649 };
enum { ARM_MALI_LPAE, ARM_64_LPAE_S1 };
struct io_pgtable { int fmt; };
#define io_pgtable_ops_to_pgtable(x) (x)
struct panfrost_mmu { struct io_pgtable *pgtbl_ops; };
struct panfrost_device;
struct panfrost_compatible { int (*vendor_init)(struct panfrost_device *); };
struct panfrost_device {
 struct { void *dev; } base;
 struct panfrost_compatible *comp;
 unsigned features, issues;
 int pfdevfreq;
};
struct panfrost_job {
 struct panfrost_device *pfdev;
 struct panfrost_mmu *mmu;
 u64 jc; unsigned requirements, flush_id, seqno;
};
static int panfrost_gpu_mt6797_init(struct panfrost_device *dev) { (void)dev; return 0; }
static int other_init(struct panfrost_device *dev) { (void)dev; return 0; }
static bool panfrost_has_hw_issue(struct panfrost_device *d, unsigned issue)
{ return d->issues & BIT(issue); }
static bool panfrost_has_hw_feature(struct panfrost_device *d, unsigned feature)
{ return d->features & BIT(feature); }
static unsigned panfrost_get_job_chain_flag(struct panfrost_job *job)
{
 return (job->pfdev->features & BIT(HW_FEATURE_JOBCHAIN_DISAMBIGUATION)) &&
        (job->seqno & 1) ? JS_CONFIG_JOB_CHAIN_FLAG : 0;
}
static int pm_error, as_result, next_command, writes, config_writes, config;
static int phase;
static int pm_runtime_get_sync(void *dev)
{ assert(dev); phase = 1; return pm_error; }
static u32 job_read(struct panfrost_device *dev, unsigned reg)
{ (void)dev; assert(phase == 1 && reg == JS_COMMAND_NEXT(1)); return next_command; }
static int panfrost_mmu_as_get(struct panfrost_device *dev, struct panfrost_mmu *mmu)
{ assert(dev && mmu && phase == 1); phase = 2; return as_result; }
static void panfrost_devfreq_record_busy(int *freq)
{ (void)freq; assert(phase == 2 && as_result >= 0); }
/* Optional default-off capture has no MMIO or allocation-policy effects. */
static void panfrost_mmu_first_submit_pte_diag(struct panfrost_job *job, int js)
{ assert(job && js == 1 && phase == 2); }
static void job_write(struct panfrost_device *dev, unsigned reg, u32 value)
{
 assert(dev && phase == 2 && !pm_error && as_result >= 0);
 writes++;
 if (reg == JS_CONFIG_NEXT(1)) { config = value; config_writes++; }
}
static void panfrost_job_write_affinity(struct panfrost_device *d, unsigned requirements, int js)
{ assert(d && js == 1); (void)requirements; }
'''
tests = r'''
static void clear(void)
{ pm_error = next_command = writes = config_writes = config = phase = 0; }
int main(void)
{
 struct panfrost_compatible comp;
 struct panfrost_device dev = { .base.dev = &comp, .comp = &comp };
 struct io_pgtable table;
 struct panfrost_mmu mmu = { &table };
 struct panfrost_job job = { .pfdev = &dev, .mmu = &mmu, .jc = 0xa00e480 };
 unsigned cases = 0;
 for (unsigned platform = 0; platform < 2; platform++)
 for (unsigned legacy = 0; legacy < 2; legacy++)
 for (unsigned issue = 0; issue < 2; issue++)
 for (unsigned flush = 0; flush < 2; flush++)
 for (unsigned chain = 0; chain < 2; chain++)
 for (unsigned seq = 0; seq < 2; seq++)
 for (unsigned as = 0; as < 8; as++) {
  comp.vendor_init = platform ? panfrost_gpu_mt6797_init : other_init;
  table.fmt = legacy ? ARM_MALI_LPAE : ARM_64_LPAE_S1;
  dev.issues = issue ? BIT(HW_ISSUE_10649) : 0;
  dev.features = (flush ? BIT(HW_FEATURE_FLUSH_REDUCTION) : 0) |
                 (chain ? BIT(HW_FEATURE_JOBCHAIN_DISAMBIGUATION) : 0);
  job.seqno = seq; as_result = as;
  unsigned expected = as | JS_CONFIG_THREAD_PRI(8) |
    JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE | JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE |
    (flush ? JS_CONFIG_ENABLE_FLUSH_REDUCTION : 0) |
    ((chain && seq) ? JS_CONFIG_JOB_CHAIN_FLAG : 0) |
    ((issue || (platform && legacy)) ? JS_CONFIG_START_MMU : 0);
  /* First submission, same-AS reuse, and submission after reset/reallocation.
   * There is no sticky software enable flag; every call rebuilds the config. */
  for (unsigned epoch = 0; epoch < 3; epoch++) {
   clear(); assert(!panfrost_job_hw_submit(&job, 1));
   assert(config_writes == 1 && (unsigned)config == expected);
   assert(writes == (flush ? 4 : 3));
  }
  cases++;
 }
 clear(); as_result = 0; pm_error = -EIO;
 assert(panfrost_job_hw_submit(&job, 1) == -EIO && !writes);
 clear(); next_command = 1;
 assert(panfrost_job_hw_submit(&job, 1) == -EINVAL && !writes);
 for (int error = 1; error <= 3; error++) {
  clear(); as_result = error == 1 ? -ETIMEDOUT : error == 2 ? -EBUSY : -EIO;
  assert(panfrost_job_hw_submit(&job, 1) == as_result && !writes);
 }
 assert(cases == 512);
 printf("PASS actual job pre-submit/config: %u combinations x3 first/reuse/reset calls; power/busy/AS errors write no job registers\n", cases);
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-start-mmu-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(shim + prefix + tests)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-Wno-unused-variable", "-I", str(root),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=5)
submit = source[start:source.index("\nstatic ", start + 1)]
assert submit.index("JS_CONFIG_NEXT(js), cfg") < submit.index("JS_COMMAND_NEXT(js), JS_COMMAND_START")
assert submit.index("panfrost_mmu_as_get(") < submit.index("JS_CONFIG_NEXT(js), cfg")
assert "io_pgtable_ops_to_pgtable(job->mmu->pgtbl_ops)->fmt == ARM_MALI_LPAE" in submit
print("PASS per-submit legacy-format/platform gate and config-before-START ordering")
