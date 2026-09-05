#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual diagnostic bodies against powered, counted fake MMIO."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
gpu = (root / "drivers/gpu/drm/panfrost/panfrost_gpu.c").read_text()
body = gpu.split("/* MT6797_DIAG_BEGIN:", 1)[1].split("/* MT6797_DIAG_END */", 1)[0]
body = body.split("*/", 1)[1]
assert not re.search(r"\b(?:gpu_write|writel|writeb|regmap_write|pm_runtime_get)\s*\(", body)
assert "%p" not in body
assert "transtab == mmu->cfg.transtab" in body
mmu = (root / "drivers/gpu/drm/panfrost/panfrost_mmu.c").read_text()
assert mmu.index("panfrost_gpu_mt6797_timeout_diag") < mmu.index("panfrost_device_schedule_reset")
job = (root / "drivers/gpu/drm/panfrost/panfrost_job.c").read_text()
assert job.index("panfrost_gpu_mt6797_submit_diag") < job.index("job_write(pfdev, JS_COMMAND_NEXT(js), JS_COMMAND_START)")
assert gpu.index("panfrost_mt6797_fault_diag(pfdev, fault_status, state)") < gpu.index("gpu_write(pfdev, GPU_INT_MASK, 0)")
assert len(re.findall(r"mt6797_diag_seen", gpu)) == 1

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
typedef uint32_t u32;
typedef unsigned long long u64;
#define BIT(n) (1UL << (n))
#define NUM_JOB_SLOTS 3
#define PANFROST_COMP_BIT_MAX 3
struct panfrost_device;
static int panfrost_gpu_mt6797_init(struct panfrost_device *p) { (void)p; return 0; }
struct comp { int (*vendor_init)(struct panfrost_device *); };
struct panfrost_device {
 struct { void *dev; } base;
 struct comp *comp;
 unsigned long is_suspended[1], mt6797_diag_seen;
 bool coherent;
 struct { unsigned selected_coherency, as_present, js_present; } features;
};
struct pgtable { unsigned fmt; };
#define io_pgtable_ops_to_pgtable(x) (x)
struct panfrost_mmu {
 struct pgtable *pgtbl_ops;
 struct { unsigned long quirks; } pgtbl_cfg;
 int as;
 struct { u64 transtab, memattr, transcfg; } cfg;
};
static unsigned reads, lines;
static bool powered=true;
static char output[32768];
static unsigned offset;
static bool bitmap_empty(unsigned long *b, unsigned n) { return !(b[0] & ((1UL<<n)-1)); }
static bool test_and_set_bit(unsigned n, unsigned long *p) {
 return __atomic_fetch_or(p,BIT(n),__ATOMIC_SEQ_CST)&BIT(n);
}
static u32 registers[4096];
static u32 gpu_read(struct panfrost_device *p, u32 reg) {
 (void)p; assert(powered && !(reg&3) && reg/4<4096);
 __atomic_fetch_add(&reads,1,__ATOMIC_RELAXED); return registers[reg/4];
}
static void dev_info(void *dev, const char *fmt, ...) {
 (void)dev; va_list ap; va_start(ap,fmt);
 offset+=vsnprintf(output+offset,sizeof(output)-offset,fmt,ap);
 va_end(ap); lines++; assert(offset<sizeof(output));
}
'''
# Use production register definitions, not copied offsets.
regs = (root / "drivers/gpu/drm/panfrost/panfrost_regs.h").read_text()
regs = regs[:regs.index("#define gpu_write")] + "\n#endif\n"
regs = re.sub(r"^#include .*\n", "", regs, flags=re.MULTILINE)
tests = r'''
static struct comp comp={panfrost_gpu_mt6797_init};
static struct panfrost_device dev={.comp=&comp,.features={0,255,7}};
static void *race(void *unused) {
 (void)unused;
 return (void *)(uintptr_t)panfrost_mt6797_diag_claim(&dev,MT6797_DIAG_FAULT);
}
int main(void) {
 struct pgtable pg={3};
 struct panfrost_mmu mmu={.pgtbl_ops=&pg,.as=0,.cfg={0x12345007,0x484d48,0}};
 registers[AS_TRANSTAB_LO(0)/4]=0x12345007;
 registers[AS_MEMATTR_LO(0)/4]=0x484d48;
 powered=false;
 for(unsigned bit=0;bit<3;bit++) {
  dev.is_suspended[0]=BIT(bit);
  panfrost_gpu_mt6797_submit_diag(&dev,&mmu,0,0x2000000,0);
  panfrost_mt6797_fault_diag(&dev,0x388,1);
  panfrost_gpu_mt6797_timeout_diag(&dev,0,4,true,1);
 }
 dev.is_suspended[0]=0; comp.vendor_init=NULL;
 panfrost_gpu_mt6797_submit_diag(&dev,&mmu,0,0x2000000,0);
 panfrost_mt6797_fault_diag(&dev,0x388,1);
 panfrost_gpu_mt6797_timeout_diag(&dev,0,4,true,1);
 assert(!reads && !lines && !dev.mt6797_diag_seen);
 comp.vendor_init=panfrost_gpu_mt6797_init; powered=true;
 panfrost_gpu_mt6797_submit_diag(&dev,&mmu,0,0x2000000,0);
 assert(lines==4 && strstr(output,"tt_match=1") && !strstr(output,"12345007"));
 panfrost_mt6797_fault_diag(&dev,0xff0388,1);
 assert(lines==17);
 panfrost_gpu_mt6797_timeout_diag(&dev,0,4,true,1);
 assert(lines==20 && strstr(output,"command=4 before=1 polled_status=00000001"));
 unsigned final_reads=reads;
 for(unsigned i=0;i<100;i++) {
  panfrost_gpu_mt6797_submit_diag(&dev,&mmu,0,0x2000000,0);
  panfrost_mt6797_fault_diag(&dev,0x388,1);
  panfrost_gpu_mt6797_timeout_diag(&dev,0,4,false,1);
 }
 assert(reads==final_reads && lines==20);
 assert(final_reads==109);
 /* Absent AS/slots are never inspected; a root mismatch is not hidden. */
 dev.mt6797_diag_seen=0; dev.features.as_present=0; dev.features.js_present=0;
 unsigned before_reads=reads, before_lines=lines;
 panfrost_mt6797_fault_diag(&dev,0x388,1);
 assert(reads==before_reads+4 && lines==before_lines+2);
 registers[AS_TRANSTAB_LO(0)/4]=0x45678007;
 panfrost_gpu_mt6797_submit_diag(&dev,&mmu,0,0x2000000,0);
 assert(strstr(output,"tt_match=0") && !strstr(output,"45678007"));
 /* Claim race uses production atomic operation, no shared print buffer. */
 dev.mt6797_diag_seen=0;
 pthread_t threads[32]; unsigned winners=0;
 for(unsigned i=0;i<32;i++) assert(!pthread_create(&threads[i],NULL,race,NULL));
 for(unsigned i=0;i<32;i++) { void *won; pthread_join(threads[i],&won); winners+=(uintptr_t)won; }
 assert(winners==1);
 printf("PASS: actual diagnostics, 32-way claim race, power/platform refusal, absent-unit guards, root mismatch/redaction, %u reads / 20 lines maximum\n",final_reads);
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-gpu-diag-") as tmp:
    p = Path(tmp)
    (p / "test.c").write_text(prefix + regs + body + tests)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-pthread", str(p / "test.c"),
                    "-o", str(p / "test")], check=True)
    subprocess.run([str(p / "test")], check=True)
