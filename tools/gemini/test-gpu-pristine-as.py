#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual MMU command, enable, allocation and reset functions on host."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/gpu/drm/panfrost/panfrost_mmu.c").read_text()

# Cross-file safety prerequisites: no software-pristine rearm after failed
# reset/power-on, and neither job nor perfcnt submission consumes a negative AS.
device = (root / "drivers/gpu/drm/panfrost/panfrost_device.c").read_text()
reset = device.split("int panfrost_device_reset(", 1)[1].split("\n}", 1)[0]
expected = ("panfrost_gpu_prepare_power_on", "panfrost_gpu_soft_reset",
            "panfrost_gpu_power_on")
positions = []
for name in expected:
    pattern = rf"ret = {name}\(pfdev\);\s*if \(ret\)\s*return ret;"
    match = re.search(pattern, reset)
    assert match, name
    positions.append(match.start())
assert positions == sorted(positions)
assert positions[-1] < reset.index("panfrost_mmu_reset(pfdev)")
for filename, arg, label, effect in (
        ("panfrost_job.c", "job->mmu", "err_hwsubmit", "job_write(pfdev, JS_HEAD_NEXT_LO"),
        ("panfrost_perfcnt.c", "perfcnt->mapping->mmu", "err_vunmap", "cfg = GPU_PERFCNT_CFG_AS")):
    caller = (root / "drivers/gpu/drm/panfrost" / filename).read_text()
    pattern = (r"ret = panfrost_mmu_as_get\(pfdev, " + re.escape(arg)
               + r"\);\s*if \(ret < 0\)\s*goto " + label + ";")
    match = re.search(pattern, caller)
    assert match and match.end() < caller.index(effect), filename


def function(name):
    match = re.search(r"(?:static )?(?:int|void) " + name + r"\(", source)
    assert match, name
    opening = source.index("{", match.end())
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


names = ("wait_ready", "write_cmd", "lock_region",
         "mmu_hw_do_operation_locked", "panfrost_mmu_enable",
         "panfrost_mmu_disable", "panfrost_mmu_as_get",
         "panfrost_mmu_as_put", "panfrost_mmu_reset")
body = "\n".join(function(name) for name in names)
regs = (root / "drivers/gpu/drm/panfrost/panfrost_regs.h").read_text()
regs = re.sub(r"^#include .*\n", "", regs, flags=re.MULTILINE)
regs = regs[:regs.index("#define gpu_write")] + "\n#endif\n"
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define BIT(n) (1UL<<(n))
#define GENMASK_ULL(h,l) ((~0ULL<<(l)) & (~0ULL>>(63-(h))))
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((u64)(v)>>32))
#define fls64(v) ((v)?64-__builtin_clzll(v):0)
#define const_ilog2(v) (63-__builtin_clzll(v))
#define max(a,b) ((a)>(b)?(a):(b))
#define ffz(v) __builtin_ctzl(~(v))
#define NUM_JOB_SLOTS 3
#define PANFROST_COMP_BIT_MMU 2
#define ARM_MALI_LPAE 7
typedef int atomic_t;
#define atomic_read(p) (*(p))
#define atomic_set(p,v) (*(p)=(v))
#define atomic_inc_return(p) (++*(p))
#define atomic_dec(p) (--*(p))
#define set_bit(n,p) (*(p)|=BIT(n))
#define clear_bit(n,p) (*(p)&=~BIT(n))
static int held, warnings, resets, diagnostics;
#define spin_lock(p) do { (void)(p); assert(!held); held=1; } while(0)
#define spin_unlock(p) do { (void)(p); assert(held); held=0; } while(0)
#define lockdep_assert_held(p) do { (void)(p); assert(held); } while(0)
#define WARN_ON(c) ({ bool b=!!(c); warnings+=b; b; })
#define dev_dbg(...) do {} while(0)
#define dev_err(...) do {} while(0)
struct list_head { struct list_head *next,*prev; };
static void INIT_LIST_HEAD(struct list_head *h) { h->next=h->prev=h; }
static bool list_empty(struct list_head *h) { return h->next==h; }
static void list_add(struct list_head *e,struct list_head *h) {
 e->next=h->next; e->prev=h; h->next->prev=e; h->next=e;
}
static void list_del_init(struct list_head *e) {
 e->prev->next=e->next; e->next->prev=e->prev; INIT_LIST_HEAD(e);
}
static void list_move(struct list_head *e,struct list_head *h) { list_del_init(e); list_add(e,h); }
#define entry(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_for_each_entry_reverse(p,h,m) \
 for(p=entry((h)->prev,__typeof__(*p),m); &(p)->m!=(h); p=entry((p)->m.prev,__typeof__(*p),m))
#define list_for_each_entry_safe(p,n,h,m) \
 for(p=entry((h)->next,__typeof__(*p),m),n=entry((p)->m.next,__typeof__(*p),m); \
 &(p)->m!=(h); p=n,n=entry((n)->m.next,__typeof__(*n),m))
struct panfrost_device;
static int panfrost_gpu_mt6797_init(struct panfrost_device *p) { (void)p; return 0; }
struct compatible { int (*vendor_init)(struct panfrost_device *); };
struct panfrost_device {
 struct compatible *comp;
 int as_lock;
 unsigned long as_alloc_mask,as_faulty_mask,as_configured_mask,as_enable_failed_mask;
 unsigned long is_suspended[1];
 struct list_head as_lru_list;
 struct { unsigned as_present; } features;
 char *iomem;
};
struct pgtable { int fmt; };
#define io_pgtable_ops_to_pgtable(p) (p)
struct panfrost_mmu {
 int as; atomic_t as_count; struct list_head list;
 struct pgtable *pgtbl_ops;
 struct { u64 transtab,memattr,transcfg; } cfg;
};
struct write { unsigned reg; u32 value; } writes[128];
static unsigned nwrites,polls,fail_poll;
static void mmu_write(struct panfrost_device *p,unsigned reg,u32 value) {
 (void)p; assert(nwrites<128); writes[nwrites++]=(struct write){reg,value};
}
#define readl_relaxed_poll_timeout_atomic(addr,val,condition,delay,timeout) \
 ({ (void)(addr); (void)(delay); (void)(timeout); polls++; \
 val=(polls==fail_poll)?1:0; (condition)?0:-ETIMEDOUT; })
static void panfrost_gpu_mt6797_timeout_diag(struct panfrost_device *p,u32 as,u32 cmd,bool before,u32 status) {
 (void)p;(void)as;(void)cmd;(void)before;(void)status; diagnostics++;
}
static void panfrost_device_schedule_reset(struct panfrost_device *p) { (void)p;resets++; }
'''
tests = r'''
static struct compatible comp;
static struct panfrost_device dev;
static struct panfrost_mmu a,b;
static struct pgtable pg;
static char mmio[16384];
static void clear_trace(void) { nwrites=polls=fail_poll=0; resets=diagnostics=0; }
static void init_mmu(struct panfrost_mmu *m) {
 memset(m,0,sizeof(*m)); m->as=-1; INIT_LIST_HEAD(&m->list); m->pgtbl_ops=&pg;
 m->cfg.transtab=0x40001007; m->cfg.memattr=0x484d48;
}
static void fixture(void) {
 assert(!held); memset(&dev,0,sizeof(dev)); comp.vendor_init=panfrost_gpu_mt6797_init;
 dev.comp=&comp; dev.features.as_present=3; dev.iomem=mmio;
 INIT_LIST_HEAD(&dev.as_lru_list); pg.fmt=ARM_MALI_LPAE;
 init_mmu(&a); init_mmu(&b); warnings=0; clear_trace();
}
static unsigned commands(unsigned cmd) {
 unsigned n=0; for(unsigned i=0;i<nwrites;i++)
  if((writes[i].reg==AS_COMMAND(0)||writes[i].reg==AS_COMMAND(1))&&writes[i].value==cmd)n++;
 return n;
}
static bool wrote(unsigned reg) {
 for(unsigned i=0;i<nwrites;i++)if(writes[i].reg==reg)return true;
 return false;
}
static void unallocated(struct panfrost_mmu *m) {
 assert(m->as==-1 && m->as_count==0 && list_empty(&m->list));
}
int main(void) {
 fixture(); assert(panfrost_mmu_as_get(&dev,&a)==0);
 assert(a.as_count==1 && dev.as_alloc_mask==1 && dev.as_configured_mask==1);
 assert(!commands(AS_COMMAND_FLUSH_MEM) && commands(AS_COMMAND_UPDATE)==1 && polls==3);
 assert(nwrites==7 && writes[0].reg==AS_TRANSTAB_LO(0));
 clear_trace(); assert(panfrost_mmu_as_get(&dev,&a)==0 && a.as_count==2 && !nwrites);
 panfrost_mmu_as_put(&dev,&a); panfrost_mmu_as_put(&dev,&a);
 assert(panfrost_mmu_as_get(&dev,&b)==1 && dev.as_configured_mask==3);
 assert(!commands(AS_COMMAND_FLUSH_MEM));
 /* Successful reset rearms all physical ASes, not the first context only. */
 panfrost_mmu_reset(&dev); unallocated(&a); unallocated(&b);
 assert(!dev.as_alloc_mask&&!dev.as_configured_mask&&!dev.as_enable_failed_mask);
 clear_trace(); assert(panfrost_mmu_as_get(&dev,&a)==0 && !commands(AS_COMMAND_FLUSH_MEM));
 /* LRU reuse must flush, with exactly the original full-range encoding. */
 fixture(); dev.features.as_present=1; assert(panfrost_mmu_as_get(&dev,&a)==0);
 panfrost_mmu_as_put(&dev,&a); clear_trace(); assert(panfrost_mmu_as_get(&dev,&b)==0);
 unallocated(&a); assert(b.as_count==1 && dev.as_alloc_mask==1);
 assert(commands(AS_COMMAND_FLUSH_MEM)==1 && writes[0].reg==AS_LOCKADDR_LO(0));
 assert(writes[0].value==0x3f && writes[1].value==0);
 /* Disable remains flushing; a freed AS is not reset-pristine. */
 spin_lock(&dev.as_lock); clear_trace(); panfrost_mmu_disable(&dev,0);
 assert(commands(AS_COMMAND_FLUSH_MEM)==1 && dev.as_configured_mask==1);
 list_del_init(&b.list); b.as=-1;b.as_count=0;dev.as_alloc_mask=0;spin_unlock(&dev.as_lock);
 clear_trace(); assert(panfrost_mmu_as_get(&dev,&a)==0 && commands(AS_COMMAND_FLUSH_MEM)==1);
 /* Non-MT6797 and nonlegacy successful sequences still flush. */
 for(unsigned variant=0;variant<2;variant++) {
  fixture(); if(variant)pg.fmt=0;else comp.vendor_init=NULL;
  assert(panfrost_mmu_as_get(&dev,&a)==0 && commands(AS_COMMAND_FLUSH_MEM)==1);
 }
 /* Every pristine wait failure: no published allocation or retry before reset. */
 for(unsigned failure=1;failure<=3;failure++) {
  fixture(); fail_poll=failure;
  assert(panfrost_mmu_as_get(&dev,&a)==-ETIMEDOUT);
  unallocated(&a); assert(!dev.as_alloc_mask&&!dev.as_configured_mask);
  assert(dev.as_enable_failed_mask==1 && resets==1 && diagnostics==1);
  assert(nwrites==(failure==1?0:failure==2?6:7));
  unsigned n=nwrites; assert(panfrost_mmu_as_get(&dev,&a)==-EIO && nwrites==n);
  panfrost_mmu_reset(&dev); clear_trace();
  assert(panfrost_mmu_as_get(&dev,&a)==0 && !commands(AS_COMMAND_FLUSH_MEM));
 }
 /* Every LRU reuse failure detaches both zero-ref old and failed new owner. */
 for(unsigned failure=1;failure<=5;failure++) {
  fixture();dev.features.as_present=1; assert(panfrost_mmu_as_get(&dev,&a)==0);
  panfrost_mmu_as_put(&dev,&a); clear_trace();fail_poll=failure;
  assert(panfrost_mmu_as_get(&dev,&b)==-ETIMEDOUT);
  unallocated(&a);unallocated(&b);assert(list_empty(&dev.as_lru_list));
  assert(!dev.as_alloc_mask && dev.as_configured_mask==1 && dev.as_enable_failed_mask==1);
  if(failure<=3)assert(!wrote(AS_TRANSTAB_LO(0)));
  assert(resets==1 && diagnostics==1);
 }
 /* Faulty re-enable failure preserves earlier refs/list/mask; no premature unmask. */
 for(unsigned failure=1;failure<=5;failure++) {
  fixture();assert(panfrost_mmu_as_get(&dev,&a)==0);dev.as_faulty_mask=BIT(0)|BIT(16);
  clear_trace();fail_poll=failure;assert(panfrost_mmu_as_get(&dev,&a)==-ETIMEDOUT);
  assert(a.as==0&&a.as_count==1&&dev.as_alloc_mask==1&&!list_empty(&a.list));
  assert(dev.as_faulty_mask==(BIT(0)|BIT(16)) && !wrote(MMU_INT_MASK));
 }
 fixture();assert(panfrost_mmu_as_get(&dev,&a)==0);dev.as_faulty_mask=BIT(0)|BIT(16);
 clear_trace();assert(panfrost_mmu_as_get(&dev,&a)==0&&a.as_count==2);
 assert(!dev.as_faulty_mask&&commands(AS_COMMAND_FLUSH_MEM)==1&&wrote(MMU_INT_MASK));
 /* No idle LRU victim: leave state intact and make no hardware access. */
 fixture();dev.features.as_present=1;assert(panfrost_mmu_as_get(&dev,&a)==0);
 clear_trace();assert(panfrost_mmu_as_get(&dev,&b)==-EBUSY);unallocated(&b);
 assert(a.as_count==1 && dev.as_alloc_mask==1 && !nwrites);
 /* Zero range and UNLOCK keep their existing geometry/command behavior. */
 clear_trace();assert(lock_region(&dev,0,0,0)==0&&!nwrites);
 assert(mmu_hw_do_operation_locked(&dev,-1,0,0,5)==0&&!nwrites);
 assert(mmu_hw_do_operation_locked(&dev,0,0,0,AS_COMMAND_UNLOCK)==0);
 assert(nwrites==1&&commands(AS_COMMAND_UNLOCK)==1);
 puts("PASS: actual MMU commands/enable/as_get/reset; pristine, reset, free, LRU, faulty, other-platform, 13 failure paths and ownership cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-pristine-as-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + regs + body + tests)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-Wno-unused-parameter",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: successful-reset-only rearm and job/perfcnt negative-AS rejection source contracts")
