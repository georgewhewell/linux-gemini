#!/usr/bin/env python3
"""Host-execute the actual capture/gate C with non-sleeping lock mocks."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root / "panfrost_mmu.c").read_text()
code = source[source.index("static void panfrost_pte_mutator("):source.index("static u64 mair_to_memattr(")]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
typedef unsigned long long u64;
#define PAGE_SHIFT 12
#define PAGE_SIZE 4096
#include "panfrost_pte_diag.h"
struct arm_lpae_io_pgtable_walk_data { u64 ptes[4]; };
struct io_pgtable_ops { int (*pgtable_walk)(struct io_pgtable_ops *, unsigned long, void *); };
struct panfrost_mmu {
 bool pte_diag_enabled; unsigned int pte_diag_mutators;
 int pte_diag_gate; struct { unsigned int ias; } pgtbl_cfg;
 struct io_pgtable_ops *pgtbl_ops;
};
struct panfrost_device { struct { void *dev; } base; unsigned long mt6797_pte_diag_seen; };
struct panfrost_gem_mapping {
 struct panfrost_mmu *mmu;
 struct { u64 start; } mmnode;
 struct bo { struct { struct { u64 size; } base; } base; } *obj;
};
struct panfrost_job {
 struct panfrost_mmu *mmu; struct panfrost_device *pfdev;
 unsigned int bo_count; u64 jc; struct panfrost_gem_mapping **mappings;
};
#define spin_lock_irqsave(lock, flags) do { assert(!*(lock)); *(lock)=1; (flags)=0; } while(0)
#define spin_unlock_irqrestore(lock, flags) do { assert(*(lock)); *(lock)=0; (void)(flags); } while(0)
#define spin_trylock_irqsave(lock, flags) ((flags)=0, *(lock) ? 0 : (*(lock)=1))
static int test_and_set_bit(int bit, unsigned long *v) { int old=!!(*v & (1UL<<bit)); *v |= 1UL<<bit; return old; }
static int logs, walks;
static struct panfrost_mmu *observed;
static u64 addresses[9];
static char logtext[16384];
static void logged(const char *fmt, ...) {
 va_list ap; va_start(ap,fmt);
 vsnprintf(logtext+strlen(logtext),sizeof(logtext)-strlen(logtext),fmt,ap);
 va_end(ap); logs++;
}
#define dev_info(dev, ...) do { assert(!observed->pte_diag_gate || observed->pte_diag_mutators==123); logged(__VA_ARGS__); } while(0)
static int walk(struct io_pgtable_ops *ops, unsigned long va, void *wd) {
 struct arm_lpae_io_pgtable_walk_data *d=wd;
 assert(observed->pte_diag_gate && !observed->pte_diag_mutators);
 assert(walks<9); addresses[walks++]=va;
 d->ptes[0]=0x100000003ULL; d->ptes[3]=0x10a5da443ULL;
 return va ? 0 : -ENOENT;
}
'''
tests = r'''
int main(void) {
 struct io_pgtable_ops ops={.pgtable_walk=walk};
 struct panfrost_mmu mmu={.pgtbl_cfg={.ias=48},.pgtbl_ops=&ops};
 struct panfrost_device dev={0};
 struct bo bo={.base.base.size=4096};
 struct panfrost_gem_mapping map[12], *maps[12];
 struct panfrost_job job={.mmu=&mmu,.pfdev=&dev,.bo_count=12,.jc=0xa00e480,.mappings=maps};
 observed=&mmu;
 for (unsigned int i=0;i<12;i++) { map[i]=(struct panfrost_gem_mapping){.mmu=&mmu,.mmnode.start=0x8000+i,.obj=&bo}; maps[i]=&map[i]; }
 panfrost_mmu_first_submit_pte_diag(&job,0);
 panfrost_pte_mutator(&mmu,true);
 assert(!logs && !walks && !dev.mt6797_pte_diag_seen && !mmu.pte_diag_mutators);
 mmu.pte_diag_enabled=true;
 for (int i=0;i<2048;i++) panfrost_pte_mutator(&mmu,true);
 assert(mmu.pte_diag_mutators==2048);
 panfrost_mmu_first_submit_pte_diag(&job,0);
 assert(logs==1 && !walks && dev.mt6797_pte_diag_seen);
 for (int i=0;i<2048;i++) panfrost_pte_mutator(&mmu,false);
 panfrost_mmu_first_submit_pte_diag(&job,0);
 assert(logs==1 && !walks); /* consumed on failure; no retries */
 dev.mt6797_pte_diag_seen=0; logs=0;
 panfrost_mmu_first_submit_pte_diag(&job,1);
 assert(walks==9 && logs==10 && addresses[0]==job.jc);
 assert(strstr(logtext,"bos=12 sampled=8 omitted=4"));
 assert(strstr(logtext,"0000000100000003,0000000000000000,0000000000000000,000000010a5da443"));
 for (int i=0;i<8;i++) assert(addresses[i+1]==((0x8000ULL+i)<<12));
 assert(!mmu.pte_diag_gate);
 dev.mt6797_pte_diag_seen=0; walks=logs=0; job.jc=~0ULL;
 logtext[0]=0;
 maps[1]=NULL; map[2].mmu=NULL;
 panfrost_mmu_first_submit_pte_diag(&job,1);
 assert(walks==6 && logs==10);
 assert(strstr(logtext,"bo=1 va=0 size=0 offset=0 ret=-2 pte=0000000000000000,0000000000000000,0000000000000000,0000000000000000"));
 assert(strstr(logtext,"ret=-34 pte=0000000000000000,0000000000000000,0000000000000000,0000000000000000"));
 dev.mt6797_pte_diag_seen=0; walks=logs=0; job.bo_count=0;
 panfrost_mmu_first_submit_pte_diag(&job,0);
 assert(!walks && logs==1);
 job.bo_count=12;
 dev.mt6797_pte_diag_seen=0; walks=logs=0;
 mmu.pte_diag_gate=1; mmu.pte_diag_mutators=123;
 panfrost_mmu_first_submit_pte_diag(&job,0);
 assert(!walks && logs==1 && mmu.pte_diag_gate==1);
 assert(panfrost_pte_bo_count(0)==0 && panfrost_pte_bo_count(~0U)==8);
 assert(!panfrost_pte_iova_valid(0,0) && !panfrost_pte_iova_valid(0,64));
 assert(panfrost_pte_iova_valid((1ULL<<48)-2,48));
 assert(panfrost_pte_iova_valid((1ULL<<48)-1,48));
 assert(!panfrost_pte_iova_valid(1ULL<<48,48));
 puts("actual C capture/gate PASS: default-off, nested writers, busy/refusal latch, 9-walk cap, invalid mappings/IOVA");
}
'''
assert 'bool, 0400)' in source
assert 'static bool mt6797_first_submit_pte;' in source
assert 'mmu->pte_diag_enabled = mt6797_first_submit_pte &&' in source
assert 'pfdev->comp->vendor_init == panfrost_gpu_mt6797_init &&' in source
assert 'fmt == ARM_MALI_LPAE && mmu->pgtbl_ops->pgtable_walk' in source
assert source.count('ops->map_pages(') == 1
assert source.count('ops->unmap_pages(') == 2
map_code = source[source.index('static int mmu_map_sg('):source.index('int panfrost_mmu_map(')]
assert map_code.index('panfrost_pte_mutator(mmu, true)') < map_code.index('ops->map_pages(')
assert map_code.count('panfrost_pte_mutator(mmu, false)') == 2
assert 'mmu_unmap_range(mmu, start_iova, total_mapped);\n\tpanfrost_pte_mutator(mmu, false)' in map_code
unmap = source[source.index('void panfrost_mmu_unmap('):source.index('static void mmu_tlb_inv_context_s1')]
assert unmap.index('panfrost_pte_mutator(mapping->mmu, true)') < unmap.index('ops->unmap_pages(')
assert unmap.index('panfrost_pte_mutator(mapping->mmu, false)') < unmap.index('panfrost_mmu_flush_range(')
job_source = (root / 'panfrost_job.c').read_text()
submit = job_source[job_source.index('static int panfrost_job_hw_submit('):job_source.index('err_hwsubmit:')]
assert submit.index('panfrost_mmu_first_submit_pte_diag(job, js)') < submit.index('spin_lock(&pfdev->js->job_lock)')
assert 'GFP_KERNEL, &mapped' in map_code
with tempfile.TemporaryDirectory(prefix='panfrost-pte-host-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(preamble + code + tests)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-variable', '-I', str(root), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('source mutation coverage and pre-job-lock placement PASS')

# Exercise the exact same functions with real host exclusion and two threads.
# A test-only walker rendezvous holds the gate; the kernel walker never waits.
concurrent = preamble.replace('#include <assert.h>', '#include <assert.h>\n#include <pthread.h>\n#include <stdatomic.h>\n#include <sched.h>')
concurrent = concurrent.replace('int pte_diag_gate;', 'pthread_mutex_t pte_diag_gate;')
start = concurrent.index('#define spin_lock_irqsave')
end = concurrent.index('static int test_and_set_bit', start)
concurrent = concurrent[:start] + r'''
#define spin_lock_irqsave(lock, flags) do { (flags)=0; assert(!pthread_mutex_lock(lock)); } while(0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); assert(!pthread_mutex_unlock(lock)); } while(0)
#define spin_trylock_irqsave(lock, flags) ((flags)=0, pthread_mutex_trylock(lock)==0)
static atomic_int inside, release_walk, writer_attempted, writer_entered;
''' + concurrent[end:]
concurrent = concurrent.replace('assert(!observed->pte_diag_gate || observed->pte_diag_mutators==123); ', '')
concurrent = concurrent.replace('assert(observed->pte_diag_gate && !observed->pte_diag_mutators);', r'''
 assert(pthread_mutex_trylock(&observed->pte_diag_gate)==EBUSY);
 assert(!observed->pte_diag_mutators);
 atomic_store(&inside,1);
 while(!atomic_load(&release_walk)) sched_yield();
''')
thread_tests = r'''
static struct panfrost_job job;
static void *capture_thread(void *unused) { panfrost_mmu_first_submit_pte_diag(&job,0); return NULL; }
static void *writer_thread(void *unused) {
 atomic_store(&writer_attempted,1);
 panfrost_pte_mutator(job.mmu,true);
 atomic_store(&writer_entered,1);
 panfrost_pte_mutator(job.mmu,false);
 return NULL;
}
int main(void) {
 struct io_pgtable_ops ops={.pgtable_walk=walk};
 struct panfrost_mmu mmu={.pte_diag_enabled=true,.pgtbl_cfg={.ias=48},.pgtbl_ops=&ops};
 struct panfrost_device dev={0};
 struct bo bo={.base.base.size=4096};
 struct panfrost_gem_mapping map={.mmu=&mmu,.mmnode.start=0x8000,.obj=&bo}, *maps[]={&map};
 pthread_t capture, writer;
 assert(!pthread_mutex_init(&mmu.pte_diag_gate,NULL));
 observed=&mmu;
 job=(struct panfrost_job){.mmu=&mmu,.pfdev=&dev,.jc=0xa00e480,.bo_count=1,.mappings=maps};
 assert(!pthread_create(&capture,NULL,capture_thread,NULL));
 while(!atomic_load(&inside)) sched_yield();
 assert(!pthread_create(&writer,NULL,writer_thread,NULL));
 while(!atomic_load(&writer_attempted)) sched_yield();
 for(int i=0;i<10000;i++) sched_yield();
 assert(!atomic_load(&writer_entered));
 atomic_store(&release_walk,1);
 assert(!pthread_join(capture,NULL)); assert(!pthread_join(writer,NULL));
 assert(atomic_load(&writer_entered) && !mmu.pte_diag_mutators && walks==2);
 assert(!pthread_mutex_destroy(&mmu.pte_diag_gate));
 puts("actual C concurrency PASS: capture excludes new writer until walk copies finish");
}
'''
with tempfile.TemporaryDirectory(prefix='panfrost-pte-concurrency-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(concurrent + code + thread_tests)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall', '-Werror', '-pthread', '-I', str(root), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
