#!/usr/bin/env python3
"""Host-run the actual GPULDO C against strict resource/read/PM mocks."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root / 'panfrost_gpu.c').read_text()
code = source[source.index('static void panfrost_mt6797_gpuldo_prepare('):
              source.index('/* MT6797_DIAG_BEGIN:')]
assert 'module_param_named(mt6797_gpuldo_diag, mt6797_gpuldo_diag, bool, 0400)' in source
assert 'static bool mt6797_gpuldo_diag;' in source
assert 'regmap_write' not in code and 'regmap_update' not in code
assert '0xfbc' not in code and '0xfd8' not in code and '0xfe4' not in code
assert 'while (' not in code and 'for (' not in code
job = (root / 'panfrost_job.c').read_text()
job = job[job.index('static int panfrost_job_hw_submit('):job.index('static int panfrost_job_hw_submit(') + 4000]
assert job.index('pm_runtime_get_sync') < job.index('if (ret < 0)') < job.index('panfrost_gpu_mt6797_gpuldo_diag') < job.index('panfrost_mmu_as_get') < job.index('spin_lock')
assert 'lockdep_assert_not_held(&pfdev->js->job_lock)' in job
assert job.count('panfrost_gpu_mt6797_gpuldo_diag') == 1
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
struct device_node { int id; } node={1}, second={2};
struct resource { unsigned long start, size; } resource;
struct regmap { int dummy; } map;
struct compatible { int (*vendor_init)(void *); } comp;
struct panfrost_device { struct { void *dev; } base; struct compatible *comp;
 struct regmap *mt6797_gpuldo_map; int mt6797_gpuldo_status;
 unsigned long mt6797_gpuldo_seen; int as_lock; };
static int panfrost_gpu_mt6797_init(void *p) { (void)p; return 0; }
static bool mt6797_gpuldo_diag;
static int platform, missing, duplicate, available, syscon, resource_error;
static int clocks, resets, lookup_error, lookups, searches, refs, reads, failures;
static int powered, job_locked, logged, log_rc[3];
static unsigned log_value[3];
static bool of_machine_is_compatible(const char *s) { (void)s; return platform; }
static struct device_node *of_node_get(struct device_node *p) { if(p) refs++; return p; }
static void of_node_put(struct device_node *p) { if(p) refs--; }
static struct device_node *of_find_compatible_node(struct device_node *p, void *t, const char *s) {
 (void)t; assert(!strcmp(s,"mediatek,mt6797-infracfg")); searches++;
 if(p) { of_node_put(p); return duplicate ? of_node_get(&second) : NULL; }
 return missing ? NULL : of_node_get(&node);
}
static bool of_device_is_available(struct device_node *p) { assert(p==&node); return available; }
static bool of_device_is_compatible(struct device_node *p, const char *s) { assert(p==&node); assert(!strcmp(s,"syscon")); return syscon; }
static int of_address_to_resource(struct device_node *p, int i, struct resource *r) { assert(p==&node && i==0); *r=resource; return resource_error; }
static unsigned long resource_size(struct resource *r) { return r->size; }
static void *of_find_property(struct device_node *p, const char *s, void *l) { assert(p==&node && !l); return (!strcmp(s,"clocks") ? clocks : resets) ? &node : NULL; }
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
static struct regmap *device_node_to_regmap(struct device_node *p) { assert(p==&node); lookups++; return lookup_error ? (void *)(intptr_t)-EIO : &map; }
static bool test_and_set_bit(int bit, unsigned long *p) { assert(bit==0); return __atomic_fetch_or(p,1,__ATOMIC_SEQ_CST)&1; }
#define lockdep_assert_not_held(p) assert(!*(p))
#define might_sleep() assert(!job_locked)
static int regmap_read(struct regmap *m, unsigned int offset, unsigned int *value) {
 assert(m==&map && powered && !job_locked); assert(reads<3);
 assert(offset==0xfc0U+4U*reads); *value=0x11220000U+reads;
 return failures & (1U<<reads++) ? -EIO : 0;
}
static void dev_info(void *dev, const char *fmt, ...) {
 va_list ap; (void)dev; logged++; va_start(ap,fmt);
 if(strstr(fmt,"fc0_rc")) for(int i=0;i<3;i++) { log_rc[i]=va_arg(ap,int); log_value[i]=va_arg(ap,unsigned); }
 va_end(ap);
}
'''
main = r'''
static void *capture_thread(void *p) {
 panfrost_gpu_mt6797_gpuldo_diag(p); return NULL;
}
static struct panfrost_device fresh(void) {
 platform=available=syscon=1;
 missing=duplicate=resource_error=clocks=resets=lookup_error=0;
 lookups=searches=refs=reads=failures=logged=0; powered=job_locked=0;
 resource=(struct resource){0x10001000,0x1000};
 comp.vendor_init=panfrost_gpu_mt6797_init;
 mt6797_gpuldo_diag=true;
 return (struct panfrost_device){.comp=&comp};
}
int main(void) {
 struct panfrost_device d;
 for(int mode=0;mode<12;mode++) {
  d=fresh();
  switch(mode) {
  case 0: mt6797_gpuldo_diag=false; break;
  case 1: comp.vendor_init=NULL; break;
  case 2: platform=0; break;
  case 3: missing=1; break;
  case 4: duplicate=1; break;
  case 5: available=0; break;
  case 6: syscon=0; break;
  case 7: resource_error=1; break;
  case 8: resource.start=0x10000000; break;
  case 9: resource.size=0xfc8; break;
  case 10: clocks=1; break;
  case 11: resets=1; break;
  }
  panfrost_mt6797_gpuldo_prepare(&d);
  powered=1; panfrost_gpu_mt6797_gpuldo_diag(&d);
  assert(!reads && !lookups && !refs);
  if(mode<3) assert(!searches);
 }
 d=fresh(); lookup_error=1; panfrost_mt6797_gpuldo_prepare(&d);
 powered=1; panfrost_gpu_mt6797_gpuldo_diag(&d);
 assert(lookups==1 && reads==0 && d.mt6797_gpuldo_status==-EIO && refs==0);
 for(int mask=0;mask<8;mask++) {
  d=fresh(); failures=mask;
  panfrost_mt6797_gpuldo_prepare(&d);
  assert(lookups==1 && reads==0 && !refs);
  powered=1; panfrost_gpu_mt6797_gpuldo_diag(&d);
  assert(reads==3 && logged==1);
  for(int i=0;i<3;i++) {
   assert(log_rc[i]==((mask&(1<<i)) ? -EIO : 0));
   assert(log_value[i]==((mask&(1<<i)) ? 0U : 0x11220000U+i));
  }
  for(int i=0;i<100;i++) panfrost_gpu_mt6797_gpuldo_diag(&d);
  assert(reads==3 && logged==1 && lookups==1);
 }
 d=fresh(); panfrost_mt6797_gpuldo_prepare(&d); powered=1;
 pthread_t threads[32];
 for(int i=0;i<32;i++) assert(!pthread_create(&threads[i],NULL,capture_thread,&d));
 for(int i=0;i<32;i++) assert(!pthread_join(threads[i],NULL));
 assert(reads==3 && logged==1 && lookups==1);
 puts("PASS: actual C default/platform/resource gates, exact3 powered reads, all8 failure subsets, no retry");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-gpuldo-test-') as temporary:
    p = Path(temporary)
    (p / 'test.c').write_text(pre + code + main)
    subprocess.run([os.environ.get('HOSTCC', '/nix/store/061ybr50r1m2jnvih4vvpgmgwb37df3r-gcc-wrapper-12.2.0/bin/gcc'),
                    '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('PASS: immutable default-off parameter and pre-AS powered/no-job-lock source wiring')
