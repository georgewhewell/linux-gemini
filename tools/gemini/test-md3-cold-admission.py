#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual MD3 admission, device-link wait and cold readback functions."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    m = re.search(r'^(?:static )?(?:int|void) ' + name + r'\([^;]*?\)\n\{', source, re.M)
    assert m, name
    a = source.index('{', m.start())
    depth, end = 1, a + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[m.start():end]


SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef int8_t s8; typedef uint32_t u32;
typedef int32_t s32; typedef uint64_t u64;
#define EPROBE_DEFER 517
#define BIT(n) (1U << (n))
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x),(v),__ATOMIC_SEQ_CST)
#define atomic_read(x) READ_ONCE(*(x))
#define atomic_set(x,v) WRITE_ONCE(*(x),(v))
static _Thread_local int lock_depth;
#define mutex_lock(p) do { assert(!lock_depth); assert(!pthread_mutex_lock(p)); lock_depth++; } while(0)
#define mutex_unlock(p) do { assert(lock_depth==1); lock_depth--; assert(!pthread_mutex_unlock(p)); } while(0)
#define dev_err(d,...) do { (void)(d); if(0) printf(__VA_ARGS__); } while(0)
#define MT6797_MD1_RES_INFRA 0
#define MT6797_MD1_POST_VECTOR_GO 1
#define DL_FLAG_AUTOREMOVE_CONSUMER 1
#define DL_STATE_CONSUMER_PROBE 2
struct device { int id; };
struct device_link { int status; };
struct regmap { u32 r[1024]; };
struct mt6797_md1_post_vector_evidence { int token; bool passive_complete; };
struct mt6797_md1_cold_owner_backend {
 struct device *dev; void *pmic; struct regmap *spm; void *base[1];
 u64 md1_md3_smem_base; u32 md1_md3_smem_size; bool md1_md3_smem_initialized;
};
static struct device supplier={3}, consumers[2]={{0},{1}};
static struct device_link links[2]; static int linked[2], link_error;
static struct regmap spm; static u32 infra[1024];
static struct mt6797_md1_cold_owner_backend backend;
static struct mt6797_md1_cold_owner_backend *mt6797_md1_handoff_backend;
static struct device *mt6797_md1_handoff_supplier;
static struct mt6797_md1_post_vector_evidence mt6797_md1_post_vector_evidence;
static int mt6797_md1_handoff_status, mt6797_md1_cold_owner_fatal_latch;
static pthread_mutex_t mt6797_md1_post_vector_evidence_lock=PTHREAD_MUTEX_INITIALIZER;
static struct device *mt6797_md1_md3_consumer;
static bool mt6797_md1_md3_lease_taken;
static int mt6797_md1_md3_cold_error;
static struct mt6797_md3_lk_snapshot mt6797_md1_md3_snapshot, input;
static int metadata_error, reads, fail_read, fatal_after_read;
static int action_error, action_event, wait_finish;
static void (*release_action)(void *); static void *release_data;
int mt6797_md3_lk_handoff_get(struct mt6797_md3_lk_snapshot *out) {
 if(metadata_error) return metadata_error; *out=input; return 0;
}
static struct device_link *device_link_add(struct device *c,struct device *s,int flags) {
 assert(s==&supplier && flags==DL_FLAG_AUTOREMOVE_CONSUMER);
 if(link_error) return NULL; linked[c->id]++; return &links[c->id];
}
static int wait_ready(void) {
 assert(!lock_depth); assert(!pthread_mutex_lock(&mt6797_md1_post_vector_evidence_lock));
 if(wait_finish) mt6797_md1_handoff_status=0;
 pthread_mutex_unlock(&mt6797_md1_post_vector_evidence_lock); return wait_finish;
}
#define wait_event_timeout(q,cond,timeout) ((cond)?1:wait_ready())
#define msecs_to_jiffies(x) (x)
static int regmap_read(struct regmap *r,unsigned offset,u32 *v) {
 assert(r==&spm && offset/4<1024); reads++;
 if(fatal_after_read==reads) atomic_set(&mt6797_md1_cold_owner_fatal_latch,1);
 if(fail_read==reads) return -EIO; *v=r->r[offset/4]; return 0;
}
static u32 readl(void *p) {
 assert((uintptr_t)p >= (uintptr_t)infra && (uintptr_t)p < (uintptr_t)infra+sizeof(infra));
 reads++; return *(u32*)p;
}
static int devm_add_action_or_reset(struct device *c,void (*fn)(void*),void *arg) {
 assert(c==arg && linked[c->id]);
 assert(!lock_depth); assert(!pthread_mutex_lock(&mt6797_md1_post_vector_evidence_lock));
 if(action_event==1) mt6797_md1_handoff_status=-ESHUTDOWN;
 if(action_event==2) atomic_set(&mt6797_md1_cold_owner_fatal_latch,1);
 if(action_event==3) spm.r[0x328/4] |= BIT(2);
 pthread_mutex_unlock(&mt6797_md1_post_vector_evidence_lock);
 if(action_error) { fn(arg); return action_error; }
 release_action=fn; release_data=arg;
 if(action_event==4) fn(arg);
 return 0;
}
static void devm_release_action(struct device *c,void (*fn)(void*),void *arg) {
 assert(c==arg && fn==release_action && arg==release_data); fn(arg); release_action=NULL;
}
'''

TESTS = r'''
static unsigned cases;
static void fixture(void) {
 memset(&input,0,sizeof(input)); input.smem.base_phys=0x88000000;
 input.smem.md1_md3_offset=0x200000; input.smem.md1_md3_size=0x200000;
 memcpy(input.check_header,"CHECK_HEADER",12);
 memset(&spm,0,sizeof(spm)); memset(infra,0,sizeof(infra));
 spm.r[MT6797_MD1_SPM_C2K_PWR_CON/4]=MT6797_MD1_SPM_PWR_ISO|MT6797_MD1_SPM_PWR_CLK_DIS;
 backend=(struct mt6797_md1_cold_owner_backend){.dev=&supplier,.pmic=&spm,.spm=&spm,.base={infra},
     .md1_md3_smem_base=0x88200000,.md1_md3_smem_size=0x200000,.md1_md3_smem_initialized=true};
 mt6797_md1_handoff_backend=&backend; mt6797_md1_handoff_supplier=&supplier;
 mt6797_md1_post_vector_evidence=(struct mt6797_md1_post_vector_evidence){1,true};
 mt6797_md1_handoff_status=mt6797_md1_cold_owner_fatal_latch=0;
 mt6797_md1_md3_consumer=NULL; mt6797_md1_md3_lease_taken=false;
 mt6797_md1_md3_cold_error=0;
 memset(&mt6797_md1_md3_snapshot,0,sizeof(mt6797_md1_md3_snapshot));
 metadata_error=reads=fail_read=fatal_after_read=action_error=action_event=wait_finish=link_error=0;
 linked[0]=linked[1]=0; links[0].status=links[1].status=DL_STATE_CONSUMER_PROBE;
 release_action=NULL; release_data=NULL;
}
static int acquire(int who,struct mt6797_md3_lk_snapshot *out) {
 return mt6797_md1_cold_owner_acquire_md3(&consumers[who],out);
}
static void reject(int expected,bool taken) {
 struct mt6797_md3_lk_snapshot out,prior; memset(&out,0xa5,sizeof(out)); prior=out; cases++;
 assert(acquire(0,&out)==expected); assert(!memcmp(&out,&prior,sizeof(out)));
 assert(mt6797_md1_md3_lease_taken==taken && !mt6797_md1_md3_consumer && !release_action);
}
static void *racer(void *arg) {
 struct mt6797_md3_lk_snapshot out;
 return (void *)(intptr_t)acquire((intptr_t)arg,&out);
}
static void *observer(void *arg) {
 (void)arg;
 for(int i=0;i<10000;i++) {
   int r=mt6797_md1_cold_owner_verify_md3_cold(&consumers[0]); assert(r==0 || r==-EPERM);
 } return NULL;
}
int main(void) {
 struct mt6797_md3_lk_snapshot out;
 fixture(); assert(mt6797_md1_cold_owner_acquire_md3(NULL,&out)==-EINVAL);
 assert(mt6797_md1_cold_owner_acquire_md3(&consumers[0],NULL)==-EINVAL);
 assert(mt6797_md1_cold_owner_verify_md3_cold(NULL)==-EINVAL);
 fixture(); metadata_error=-ENODATA; reject(-ENODATA,false); assert(!linked[0] && !reads);
 fixture(); metadata_error=-EBADMSG; reject(-EBADMSG,false);
 fixture(); mt6797_md1_handoff_status=-EPROBE_DEFER; reject(-EPROBE_DEFER,false); assert(!linked[0] && !reads);
 fixture(); mt6797_md1_handoff_status=-EAGAIN; reject(-ETIMEDOUT,false); assert(linked[0] && !reads);
 fixture(); link_error=1; reject(-ENOMEM,false);
 fixture(); links[0].status=9; reject(-EPROBE_DEFER,false); assert(!reads);
 fixture(); mt6797_md1_handoff_status=-EAGAIN; wait_finish=1; assert(!acquire(0,&out));
 release_action(release_data); assert(!mt6797_md1_cold_owner_fatal_latch); cases++;
 for(int i=0;i<10;i++) {
   fixture(); int error=-ESHUTDOWN;
   switch(i) {
    case 0: mt6797_md1_handoff_backend=NULL; break;
    case 1: backend.dev=&consumers[1]; break;
    case 2: mt6797_md1_cold_owner_fatal_latch=1; break;
    case 3: mt6797_md1_post_vector_evidence.token=0; error=-EPERM; break;
    case 4: mt6797_md1_post_vector_evidence.passive_complete=false; error=-EPERM; break;
    case 5: backend.md1_md3_smem_initialized=false; error=-EPERM; break;
    case 6: backend.pmic=NULL; error=-ENODEV; break;
    case 7: backend.spm=NULL; error=-ENODEV; break;
    case 8: backend.base[0]=NULL; error=-ENODEV; break;
    case 9: backend.md1_md3_smem_size--; error=-ERANGE; break;
   } reject(error,false); assert(!reads);
 }
 fixture(); backend.md1_md3_smem_base++; reject(-ERANGE,false);
 for(unsigned bit=0;bit<5;bit++) {
   fixture(); spm.r[MT6797_MD1_SPM_C2K_PWR_CON/4]^=BIT(bit); reject(-EBUSY,false);
 }
 fixture(); spm.r[MT6797_MD1_SPM_PWR_STATUS/4]=BIT(28); reject(-EBUSY,false);
 fixture(); spm.r[MT6797_MD1_SPM_PWR_STATUS_2ND/4]=BIT(28); reject(-EBUSY,false);
 for(int i=1;i<=3;i++) { fixture(); fail_read=i; reject(-EIO,false); }
 fixture(); fatal_after_read=1; reject(-ESHUTDOWN,false);
 fixture(); action_error=-ENOMEM; reject(-ENOMEM,true); assert(!mt6797_md1_cold_owner_fatal_latch);
 assert(acquire(1,&out)==-EBUSY);
 for(int i=1;i<=4;i++) {
   fixture(); action_event=i; reject(i==3?-EBUSY:-ESHUTDOWN,true);
 }
 fixture(); assert(!acquire(0,&out)); cases++;
 assert(!memcmp(&input,&out,sizeof(out)) && linked[0] && reads==10);
 assert(mt6797_md1_cold_owner_verify_md3_cold(&consumers[1])==-EPERM);
 assert(!mt6797_md1_cold_owner_verify_md3_cold(&consumers[0]));
 int before=reads; assert(acquire(0,&out)==-EBUSY && acquire(1,&out)==-EBUSY && reads==before);
 mt6797_md1_cold_release_md3(&consumers[1]); assert(!mt6797_md1_cold_owner_verify_md3_cold(&consumers[0]));
 pthread_t t; assert(!pthread_create(&t,NULL,observer,NULL));
 release_action(release_data); release_action(release_data); assert(!pthread_join(t,NULL));
 assert(mt6797_md1_cold_owner_verify_md3_cold(&consumers[0])==-EPERM);
 assert(!mt6797_md1_cold_owner_fatal_latch && !mt6797_md1_handoff_status && backend.md1_md3_smem_initialized);
 assert(mt6797_md1_md3_snapshot.check_header[0]==0 && acquire(1,&out)==-EBUSY);
 fixture(); assert(!acquire(0,&out)); before=reads;
 mt6797_md1_handoff_status=-ESHUTDOWN; mt6797_md1_handoff_backend=NULL;
 assert(mt6797_md1_cold_owner_verify_md3_cold(&consumers[0])==-ESHUTDOWN && reads==before);
 release_action(release_data); cases++;
 fixture(); assert(!acquire(0,&out)); spm.r[MT6797_MD1_SPM_C2K_PWR_CON/4]|=BIT(2);
 assert(mt6797_md1_cold_owner_verify_md3_cold(&consumers[0])==-EBUSY); before=reads;
 spm.r[MT6797_MD1_SPM_C2K_PWR_CON/4]&=~BIT(2);
 assert(mt6797_md1_cold_owner_verify_md3_cold(&consumers[0])==-EBUSY && reads==before);
 assert(!mt6797_md1_cold_owner_fatal_latch); release_action(release_data); cases++;
 for(int trial=0;trial<32;trial++) {
   pthread_t threads[2]; void *res[2]; fixture();
   for(intptr_t i=0;i<2;i++) assert(!pthread_create(&threads[i],NULL,racer,(void*)i));
   for(int i=0;i<2;i++) assert(!pthread_join(threads[i],&res[i]));
   assert(((intptr_t)res[0]==0 && (intptr_t)res[1]==-EBUSY)||((intptr_t)res[1]==0 && (intptr_t)res[0]==-EBUSY));
   assert(reads==10 && !mt6797_md1_cold_owner_fatal_latch); release_action(release_data); cases++;
 }
 printf("PASS %u admission cases: real cold reads/device-link wait, failure boundaries, single-owner races, terminal release, preserved MD1\n",cases);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default=os.environ.get('HOSTCC', os.environ.get('CC', 'cc')))
    p.add_argument('--negative', choices=['clear', 'unbind', 'exclusive', 'reuse', 'late-release', 'retry'])
    args = p.parse_args()
    source = (ROOT / 'drivers/soc/mediatek/mt6797-md1-cold-owner.c').read_text()
    mutations = {
        'clear': ('||\n\t    !backend->md1_md3_smem_initialized', ''),
        'unbind': ('if (READ_ONCE(link->status) != DL_STATE_CONSUMER_PROBE)', 'if (false)'),
        'exclusive': ('if (mt6797_md1_md3_lease_taken) {', 'if (false) {'),
        'reuse': ('/* Leave lease_taken set: no second cold owner in this boot. */',
                  'mt6797_md1_md3_lease_taken = false;'),
        'late-release': ('if (mt6797_md1_md3_consumer != consumer)', 'if (false)'),
        'retry': ('mt6797_md1_md3_cold_error = ret;', 'mt6797_md1_md3_cold_error = 0;'),
    }
    if args.negative:
        old, new = mutations[args.negative]
        assert source.count(old) == 1
        source = source.replace(old, new)
    headers = ''
    for name in ['mt6797-md1-lk-handoff.h', 'mt6797-md3-lk-handoff.h']:
        h = (ROOT / 'include/linux/soc/mediatek' / name).read_text()
        headers += re.sub(r'^#include[^\n]*\n', '', h, flags=re.M)
    # Declarations need actual snapshot types between standard C types and mocks.
    split = SHIM.index('struct device {')
    shim = SHIM[:split] + headers + SHIM[split:]
    defines = '\n'.join(re.findall(r'^#define MT6797_MD1_(?:SPM_(?:C2K_PWR_CON|PWR_STATUS(?:_2ND|_C2K)?|PWR_RST_B|PWR_ISO|PWR_ON|PWR_ON_2ND|PWR_CLK_DIS)|INFRA_(?:C2K_PROT_MASK|PROTECT_EN|PROTECT_STA1))\s+.*$', source, re.M))
    functions = '\n'.join(function(source, n) for n in [
        'mt6797_md1_cold_owner_wait_handoff', 'mt6797_md1_cold_verify_c2k_off',
        'mt6797_md1_md3_preflight_locked', 'mt6797_md1_cold_release_md3',
        'mt6797_md1_cold_owner_acquire_md3', 'mt6797_md1_cold_owner_verify_md3_cold'])
    assert not re.search(r'\b(?:writel|regmap_write|regmap_update_bits|memset_io|clk_prepare_enable)\(', functions)
    with tempfile.TemporaryDirectory(prefix='md3-admission-') as temp:
        c = Path(temp) / 'actual.c'
        exe = Path(temp) / 'actual'
        c.write_text(shim + defines + '\n' + functions + TESTS)
        subprocess.run([args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-misleading-indentation', '-fno-omit-frame-pointer',
                        '-fsanitize=address,undefined', '-pthread', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()
