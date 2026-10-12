#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run actual MD1 shared power backend and optional platform probe with fake I/O."""
import importlib.util, os, re, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('base',Path(__file__).with_name('test-md3-lk-handoff.py'))
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
SHIM=r'''
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;typedef int8_t s8;typedef uint32_t u32;typedef int32_t s32;typedef uint64_t u64;
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) ((~0U>>(31-(h)))&(~0U<<(l)))
#define U32_MAX UINT32_MAX
#define EPROBE_DEFER 517
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define check_add_overflow(a,b,c) __builtin_add_overflow((a),(b),(c))
#define EXPORT_SYMBOL_GPL(x)
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095L)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define GFP_KERNEL 0
#define MT6797_TOPRGU_C2KSYS_SW_RST 16
#define dev_info(...) ((void)0)
struct mutex { pthread_mutex_t value; };
static void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->value,NULL)); }
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->value)); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->value)); }
#define DEFINE_MUTEX(n) struct mutex n={PTHREAD_MUTEX_INITIALIZER}
static u32 get_unaligned_le32(const void *p){u32 v;memcpy(&v,p,4);return v;}
static u64 get_unaligned_le64(const void *p){u64 v;memcpy(&v,p,8);return v;}
static void usleep_range(unsigned a,unsigned b){assert(a==10&&b==20);}
'''
BACKEND_SHIM=r'''
struct device_node { int id; };
struct device { struct device_node *of_node; void *data; int id; };
struct platform_device { struct device dev; };
struct reset_control { int held; };
struct mt6797_md3_ccif { int unused; };
static bool allow_ccif_prepare;
static bool request_ccif;
static int ccif_acquire_error,ccif_prepare_error;
static unsigned ccif_acquires,ccif_prepares;
static struct mt6797_md3_ccif ccif;
struct resource { u64 start,end; };
struct regmap { u32 v[1024]; };
struct of_phandle_args { struct device_node *np; unsigned args_count,args[1]; };
struct attribute_group { int unused; };
static const struct attribute_group mt6797_md3_group={0};
struct mt6797_md1_cold_owner_backend { struct device *dev; struct regmap *spm; };
struct mt6797_md1_mpu_plan_entry { u64 start,end;u32 access_permission;u8 emi_mpu_region; };
struct plan { struct mt6797_md1_mpu_plan_entry md1_md3_smem; };
static DEFINE_MUTEX(mt6797_md1_post_vector_evidence_lock);
static DEFINE_MUTEX(mt6797_md1_stage2_lock);
static struct device supplier,consumer,rival;
static struct regmap spm,infra;
static struct mt6797_md1_cold_owner_backend shared;
static struct mt6797_md1_cold_owner_backend *mt6797_md1_handoff_backend;
static struct device *mt6797_md1_md3_consumer,*mt6797_md1_stage2_dev;
static int mt6797_md1_handoff_status,mt6797_md1_cold_owner_fatal_latch,mt6797_md1_md3_cold_error;
static bool mt6797_md1_stage2_complete,mt6797_md1_stage2_armed;
static struct plan mt6797_md1_stage2_plan;
static struct mt6797_md3_lk_snapshot mt6797_md1_md3_snapshot;
static struct reset_control reset;
static struct device_node node,reset_node;
static unsigned writes,reads,fail_write,fail_read,reset_reads,reset_drop_at,node_puts;
static int preflight_error,resource_error,map_error,reset_spec_error,reset_get_error,reset_assert_error;
static int wdt_error,group_error,acquire_error,verify_error,resource_wrong,reset_count,reset_id,reset_compatible;
static unsigned reset_asserts,reset_gets,groups,acquires;
static bool have_node=true,property=true,power_domain=false,alloc_error=false;
static void *allocated;
#define atomic_read(p) (*(p))
#define MT6797_MD1_INFRA_BASE 0x10001000
#define MT6797_MD1_INFRA_SIZE 0x1000
#define MT6797_MD1_SPM_POWERON_CONFIG_EN 0
#define MT6797_MD1_SPM_POWERON_CONFIG_EN_KEY 0x0b160001
static int reset_control_status(struct reset_control *r) {
 assert(r==&reset);reset_reads++;if(reset_drop_at==reset_reads)r->held=0;return r->held;
}
static int mt6797_md1_md3_preflight_locked(const struct mt6797_md3_lk_snapshot *s) {
 assert(s==&mt6797_md1_md3_snapshot);return preflight_error;
}
static int regmap_read(struct regmap *m,u32 offset,u32 *v) {
 assert(offset<0x1000 && !(offset&3));reads++;if(reads==fail_read)return -EIO;
 *v=m->v[offset/4];return 0;
}
static int regmap_write(struct regmap *m,u32 offset,u32 v) {
 assert(m==&spm && offset==0 && v==0x0b160001);writes++;
 m->v[0]=v;return writes==fail_write?-EIO:0;
}
static int regmap_update_bits(struct regmap *m,u32 offset,u32 mask,u32 v) {
 assert(!(v&~mask));writes++;assert((m==&spm&&offset==0x328)||(m==&infra&&offset==0x220));
 m->v[offset/4]=(m->v[offset/4]&~mask)|v;
 if(m==&spm&&(m->v[0x328/4]&12)==12)spm.v[0x180/4]=spm.v[0x184/4]=BIT(28);
 if(m==&infra)infra.v[0x228/4]&=~mask;
 return writes==fail_write?-EIO:0;
}
static struct device_node *of_find_compatible_node(void *a,void *b,const char *c) {
 assert(!a&&!b&&!strcmp(c,"mediatek,mt6797-infracfg"));return have_node?&node:NULL;
}
static int of_address_to_resource(struct device_node *n,int index,struct resource *r) {
 assert((n==&node||n==&reset_node)&&!index);if(resource_error)return resource_error;
 r->start=n==&reset_node&&reset_spec_error==99?0x10007000:0x10001000;
 r->end=r->start+(n==&reset_node&&reset_spec_error==99?0xff:0xfff)+resource_wrong;return 0;
}
static u64 resource_size(struct resource *r){return r->end-r->start+1;}
static struct regmap *syscon_node_to_regmap(struct device_node *n){assert(n==&node);return map_error?ERR_PTR(map_error):&infra;}
static void of_node_put(struct device_node *n){assert(n==&node||n==&reset_node);node_puts++;}
static int of_count_phandle_with_args(struct device_node *n,const char*a,const char*b){(void)n;(void)a;(void)b;return reset_count;}
static int of_parse_phandle_with_args(struct device_node *n,const char*a,const char*b,int idx,struct of_phandle_args *out){
 (void)n;(void)a;(void)b;assert(!idx);if(reset_spec_error&&reset_spec_error!=99)return reset_spec_error;
 out->np=&reset_node;out->args_count=1;out->args[0]=reset_id;return 0;
}
static int of_device_is_compatible(struct device_node*n,const char*c){assert(n==&reset_node&&!strcmp(c,"mediatek,mt6797-wdt"));return reset_compatible;}
static bool device_property_read_bool(struct device*d,const char*p){(void)d;if(!strcmp(p,"mediatek,prepare-ap-md3-ccif"))return request_ccif;assert(!strcmp(p,"mediatek,experimental-md3-domain-power"));return property;}
static bool device_property_present(struct device*d,const char*p){(void)d;assert(!strcmp(p,"power-domains"));return power_domain;}
static int mtk_wdt_require_boot_recovery(struct device*d){(void)d;return wdt_error;}
static void *devm_kzalloc(struct device*d,size_t n,int flags){(void)d;assert(flags==0);if(alloc_error)return NULL;assert(!allocated);return allocated=calloc(1,n);}
static void platform_set_drvdata(struct platform_device*p,void*d){p->dev.data=d;}
static int devm_device_add_group(struct device*d,const struct attribute_group*g){assert(d->data&&g==&mt6797_md3_group);groups++;return group_error;}
static struct reset_control *devm_reset_control_get_exclusive(struct device*d,const char*n){(void)d;assert(!strcmp(n,"c2k"));reset_gets++;return reset_get_error?ERR_PTR(reset_get_error):&reset;}
int mt6797_md3_lk_handoff_get(struct mt6797_md3_lk_snapshot*s){*s=mt6797_md1_md3_snapshot;return 0;}
static int mt6797_md1_cold_owner_acquire_md3(struct device*d,struct mt6797_md3_lk_snapshot*s){acquires++;if(acquire_error)return acquire_error;mt6797_md1_md3_consumer=d;*s=mt6797_md1_md3_snapshot;return 0;}
static int mt6797_md1_cold_owner_verify_md3_cold(struct device*d){assert(d==mt6797_md1_md3_consumer);return verify_error;}
static int mt6797_md3_ccif_acquire(struct platform_device*p,const struct mt6797_md3_lk_snapshot*s,struct reset_control*r,struct mt6797_md3_ccif**out){
 assert(p&&s&&r==&reset&&!writes&&!reset_asserts&&!acquires);ccif_acquires++;if(ccif_acquire_error)return ccif_acquire_error;*out=&ccif;return 0;
}
static int mt6797_md3_ccif_prepare(struct mt6797_md3_ccif*c){assert(c==&ccif&&writes==7&&reset.held==1);ccif_prepares++;return ccif_prepare_error;}
static int reset_control_assert(struct reset_control*r){assert(r==&reset);reset_asserts++;r->held=1;return reset_assert_error;}
'''
TESTS=r'''
static unsigned cases;
static void fixture(void) {
 static const struct mt6797_md3_lk_mpu_entry entries[]={
 {0x88400000,0x885fffff,8,0xb45b68,0},{0x88200000,0x883fffff,9,0xb45b40,0},
 {0xbe000000,0xbe40ffff,16,0x4b45b6c,0},{0xbe410000,0xbebfffff,17,0x4b45b6c,0}};
 memset(&spm,0,sizeof(spm));memset(&infra,0,sizeof(infra));
 spm.v[0x328/4]=18;infra.v[0x220/4]=infra.v[0x228/4]=0xdeadffff;
 infra.v[0x370/4]=0xc5c3c1bf;infra.v[0x374/4]=0xcdcbc9c7;
 infra.v[0x380/4]=0x8f8d8b89;infra.v[0x384/4]=0x97959391;infra.v[0xf80/4]=BIT(17);
 memset(&mt6797_md1_md3_snapshot,0,sizeof(mt6797_md1_md3_snapshot));
 mt6797_md1_md3_snapshot.modem.base_phys=0xbe000000;mt6797_md1_md3_snapshot.modem.size=0xc00000;
 mt6797_md1_md3_snapshot.smem.base_phys=0x88000000;mt6797_md1_md3_snapshot.smem.ap_md3_offset=0x400000;
 mt6797_md1_md3_snapshot.smem.ap_md3_size=0x200000;mt6797_md1_md3_snapshot.smem.md1_md3_offset=0x200000;
 mt6797_md1_md3_snapshot.smem.md1_md3_size=0x200000;
 mt6797_md1_md3_snapshot.header_image_size=0x410000;mt6797_md1_md3_snapshot.header_mem_size=0xc00000;
 memcpy(mt6797_md1_md3_snapshot.mpu,entries,sizeof(entries));
 shared=(struct mt6797_md1_cold_owner_backend){&supplier,&spm};mt6797_md1_handoff_backend=&shared;
 mt6797_md1_md3_consumer=&consumer;mt6797_md1_stage2_dev=&supplier;
 mt6797_md1_stage2_plan.md1_md3_smem=(struct mt6797_md1_mpu_plan_entry){0x88200000,0x883fffff,0xb45b40,9};
 mt6797_md1_stage2_complete=mt6797_md1_stage2_armed=true;mt6797_md3_power_attempted=false;
 mt6797_md1_handoff_status=mt6797_md1_cold_owner_fatal_latch=mt6797_md1_md3_cold_error=0;
 preflight_error=resource_error=map_error=resource_wrong=0;have_node=true;reset.held=1;
 reads=writes=fail_read=fail_write=reset_reads=reset_drop_at=node_puts=0;
 reset_spec_error=reset_get_error=reset_assert_error=wdt_error=group_error=acquire_error=verify_error=0;
 reset_count=1;reset_id=16;reset_compatible=1;allow_domain_power=property=true;power_domain=alloc_error=false;
 reset_asserts=reset_gets=groups=acquires=0;request_ccif=false;allow_ccif_prepare=true;ccif_acquire_error=ccif_prepare_error=0;ccif_acquires=ccif_prepares=0;assert(!allocated);
}
static void backend(int expected, bool touched) {
 struct mt6797_md3_power_result r;
 assert(mt6797_md1_cold_owner_power_md3(&consumer,&reset,&r)==expected);
 assert(r.state==(expected?MT6797_MD3_POWER_FAILED:MT6797_MD3_POWER_DOMAIN_ON));
 assert(r.error==expected && r.may_have_written==touched);
 assert(!mt6797_md1_cold_owner_fatal_latch);
 assert(!pthread_mutex_trylock(&mt6797_md1_post_vector_evidence_lock.value));
 pthread_mutex_unlock(&mt6797_md1_post_vector_evidence_lock.value);
 cases++;
}
static void backend_tests(void) {
 fixture();backend(0,true);assert(writes==7&&reset.held==1);unsigned total_reads=reads,total_reset=reset_reads;
 assert(infra.v[0x220/4]==(0xdeadffff&~0xe000U));
 fixture();mt6797_md1_md3_consumer=&rival;backend(-EPERM,false);assert(!mt6797_md3_power_attempted&&!mt6797_md1_md3_cold_error);
 fixture();mt6797_md3_power_attempted=true;backend(-EPERM,false);
 fixture();mt6797_md1_md3_snapshot.mpu_error=-ENOENT;backend(-ENOENT,false);
 fixture();preflight_error=-ESHUTDOWN;backend(-ESHUTDOWN,false);
 fixture();reset.held=0;backend(-EUCLEAN,false);
 fixture();reset.held=-EIO;backend(-EIO,false);
 fixture();mt6797_md1_cold_owner_fatal_latch=1;
 struct mt6797_md3_power_result r;assert(mt6797_md1_cold_owner_power_md3(&consumer,&reset,&r)==-ESHUTDOWN);assert(!writes);cases++;
 fixture();mt6797_md1_stage2_complete=false;backend(-EUCLEAN,false);
 fixture();mt6797_md1_stage2_armed=false;backend(-EUCLEAN,false);
 fixture();mt6797_md1_stage2_dev=&rival;backend(-EUCLEAN,false);
 for(unsigned field=0;field<4;field++) {
  fixture();struct mt6797_md1_mpu_plan_entry *p=&mt6797_md1_stage2_plan.md1_md3_smem;
  if(field==0)p->start++;if(field==1)p->end++;if(field==2)p->access_permission^=1;if(field==3)p->emi_mpu_region=8;
  backend(-EUCLEAN,false);assert(!writes);
 }
 for(unsigned i=0;i<5;i++) {
  fixture();unsigned offsets[]={0x370,0x374,0x380,0x384,0xf80};infra.v[offsets[i]/4]^=i==4?BIT(17):1;
  backend(-EUCLEAN,false);assert(!writes);
 }
 for(unsigned n=1;n<=7;n++) {fixture();fail_write=n;backend(-EIO,true);assert(writes==n);}
 for(unsigned n=1;n<=total_reads;n++) {
  fixture();fail_read=n;assert(mt6797_md1_cold_owner_power_md3(&consumer,&reset,&r)==-EIO);
  assert(r.state==MT6797_MD3_POWER_FAILED && r.may_have_written==(writes!=0));cases++;
 }
 for(unsigned n=1;n<=total_reset;n++) {
  fixture();reset_drop_at=n;assert(mt6797_md1_cold_owner_power_md3(&consumer,&reset,&r)==-EUCLEAN);
  assert(r.state==MT6797_MD3_POWER_FAILED && r.may_have_written==(writes!=0));cases++;
 }
}
static void probe(int expected, bool retained, bool mutation) {
 struct platform_device p={.dev={.of_node=&node,.id=2}};
 int ret=mt6797_md3_power_probe(&p);assert(ret==expected);
 if(retained)assert(!ret&&p.dev.data&&((struct mt6797_md3_power_owner*)p.dev.data)->result.state==MT6797_MD3_POWER_FAILED);
 if(!mutation)assert(!writes&&!reset_asserts);
 if(allocated){pthread_mutex_destroy(&((struct mt6797_md3_power_owner*)allocated)->lock.value);free(allocated);allocated=NULL;}
 cases++;
}
static void domain_tests(void) {
 struct mt6797_md3_power_result r;
 fixture();assert(!mt6797_md1_cold_owner_md3_power_ready());
 mt6797_md1_stage2_complete=false;assert(mt6797_md1_cold_owner_md3_power_ready()==-EPROBE_DEFER&&!writes&&!mt6797_md3_power_attempted);
 mt6797_md1_stage2_complete=true;assert(!mt6797_md1_cold_owner_md3_power_ready());cases++;
 fixture();assert(mt6797_md1_cold_owner_verify_md3_domain(&consumer,&reset)==-ESHUTDOWN);cases++;
 for(unsigned i=0;i<11;i++){
  fixture();assert(!mt6797_md1_cold_owner_power_md3(&consumer,&reset,&r));
  assert(!mt6797_md1_cold_owner_verify_md3_domain(&consumer,&reset));
  switch(i){case 0:mt6797_md1_handoff_backend=NULL;break;case 1:mt6797_md1_md3_consumer=&rival;break;
   case 2:mt6797_md1_md3_cold_error=-EIO;break;case 3:mt6797_md1_handoff_status=-EIO;break;
   case 4:mt6797_md1_cold_owner_fatal_latch=1;break;case 5:reset.held=0;break;
   case 6:spm.v[0x328/4]|=BIT(1);break;case 7:spm.v[0x180/4]=0;break;
   case 8:spm.v[0x184/4]=0;break;case 9:fail_read=reads+2;break;case 10:reset_drop_at=reset_reads+2;break;}
  unsigned prev=writes;assert(mt6797_md1_cold_owner_verify_md3_domain(&consumer,&reset)<0&&writes==prev);
  assert(!pthread_mutex_trylock(&mt6797_md1_post_vector_evidence_lock.value));pthread_mutex_unlock(&mt6797_md1_post_vector_evidence_lock.value);cases++;
 }
}
static void probe_tests(void) {
 fixture();allow_domain_power=false;probe(-EPERM,false,false);
 fixture();property=false;probe(-EPERM,false,false);
 fixture();power_domain=true;probe(-EPERM,false,false);
 fixture();reset_count=2;probe(-EINVAL,false,false);
 fixture();reset_spec_error=-EINVAL;probe(-EINVAL,false,false);
 fixture();reset_id=17;probe(-EINVAL,false,false);
 fixture();reset_compatible=0;probe(-EINVAL,false,false);
 fixture();probe(-ERANGE,false,false); /* wrong resource, not TOPRGU */
 fixture();reset_spec_error=99;wdt_error=-EIO;probe(-EIO,false,false);
 fixture();reset_spec_error=99;alloc_error=true;probe(-ENOMEM,false,false);
 fixture();reset_spec_error=99;group_error=-ENOMEM;probe(-ENOMEM,false,false);
 fixture();reset_spec_error=99;reset_get_error=-EBUSY;probe(-EBUSY,false,false);
 fixture();reset_spec_error=99;mt6797_md1_stage2_complete=false;probe(-EPROBE_DEFER,false,false);assert(!acquires&&!mt6797_md3_power_attempted);
 fixture();reset_spec_error=99;mt6797_md1_stage2_armed=false;probe(-EUCLEAN,false,false);assert(!acquires);
 fixture();reset_spec_error=99;acquire_error=-ENODATA;probe(-ENODATA,false,false);
 fixture();reset_spec_error=99;mt6797_md1_md3_snapshot.mpu_error=-ENOENT;probe(-ENOENT,false,false);
 fixture();reset_spec_error=99;verify_error=-EIO;probe(-EIO,false,false);
 fixture();reset_spec_error=99;reset_assert_error=-EIO;probe(0,true,true);assert(!writes);
 fixture();reset_spec_error=99;probe(0,false,true);assert(reset_asserts==1&&writes==7);
 fixture();reset_spec_error=99;map_error=-EIO;probe(0,true,true);assert(reset_asserts==1&&!writes);
 fixture();reset_spec_error=99;reset_drop_at=1;probe(0,true,true);assert(reset_asserts==1&&!writes);
 fixture();reset_spec_error=99;request_ccif=true;allow_ccif_prepare=false;probe(-EPERM,false,false);assert(!ccif_acquires);
 fixture();reset_spec_error=99;request_ccif=true;ccif_acquire_error=-EBUSY;probe(-EBUSY,false,false);assert(ccif_acquires==1&&!ccif_prepares);
 fixture();reset_spec_error=99;request_ccif=true;probe(0,false,true);assert(ccif_acquires==1&&ccif_prepares==1);
 fixture();reset_spec_error=99;request_ccif=true;ccif_prepare_error=-EIO;probe(0,true,true);assert(ccif_prepares==1);
 for(unsigned i=1;i<=7;i++){
  fixture();reset_spec_error=99;fail_write=i;probe(0,true,true);assert(writes==i);
 }
}
int main(void){backend_tests();domain_tests();probe_tests();printf("PASS actual MD3 backend/probe cases=%u\n",cases);}
'''

def main():
    cold=(ROOT/'drivers/soc/mediatek/mt6797-md1-cold-owner.c').read_text()
    owner=(ROOT/'drivers/soc/mediatek/mt6797-md3-power-owner.c').read_text()
    units=[SHIM]
    for n in ['mt6797-md1-lk-handoff.h','mt6797-md3-lk-handoff.h','mt6797-md3-power.h']:
        units.append(base.clean((ROOT/'include/linux/soc/mediatek'/n).read_text()))
    for n in ['mt6797-md1-lk-handoff.c','mt6797-md3-lk-handoff.c','mt6797-md3-power.c']:
        units.append(base.clean((ROOT/'drivers/soc/mediatek'/n).read_text()))
    units.append(BACKEND_SHIM)
    units.append(cold.split('#if IS_ENABLED(CONFIG_MTK_MT6797_MD3_POWER_OWNER)\n',1)[1].split('\n#endif',1)[0])
    units.append('static bool allow_domain_power;\n'+re.search(r'struct mt6797_md3_power_owner \{.*?\n\};',owner,re.S).group())
    units.extend(base.function(owner,n) for n in ['mt6797_md3_reset_spec','mt6797_md3_power_probe'])
    units.append(TESTS)
    actual='\n'.join(units)
    negative={
      'early-lease-consumption':('ret = mt6797_md3_lk_handoff_get(&snapshot);','ret = mt6797_md1_cold_owner_acquire_md3(dev, &snapshot);'),
      'missing-HS1-defer':('if (!mt6797_md1_stage2_complete)\n\t\tret = -EPROBE_DEFER;', 'if (false)\n\t\tret = -EPROBE_DEFER;'),
      'region9-bypass':('inter->access_permission != s->mpu[1].permission','false'),
      'foreign-poisons-owner':('if (ret && attempted)','if (ret)'),
      'reset-bypass':('return ret < 0 ? ret : ret == 1 ? 0 : -EUCLEAN;','return 0;'),
      'release-on-partial':('owner->result.state = MT6797_MD3_POWER_FAILED;\n\t}\n\t/* Retain','owner->result.state = MT6797_MD3_POWER_FAILED;\n\t\treturn ret;\n\t}\n\t/* Retain'),
    }
    with tempfile.TemporaryDirectory(prefix='md3-owner-') as d:
      p=Path(d)
      for label,src in [('positive',actual)]+[(n,actual.replace(a,b)) for n,(a,b) in negative.items()]:
        (p/'actual.c').write_text(src)
        c=subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
          '-Wno-pointer-sign','-Wno-sign-compare','-Wno-misleading-indentation','-Wno-unused-parameter','-Wno-unused-but-set-variable',
          '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread',str(p/'actual.c'),'-o',str(p/'actual')],capture_output=True,text=True)
        assert c.returncode==0,c.stderr
        r=subprocess.run([str(p/'actual')],capture_output=True,text=True,timeout=30)
        if label=='positive':assert r.returncode==0,(r.stdout,r.stderr);print(r.stdout,end='')
        else:assert r.returncode!=0 and 'Assertion' in r.stderr,(label,r.stdout,r.stderr);print('REJECTED '+label)

if __name__=='__main__':main()
