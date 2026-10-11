#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual intermodem plan and secure transaction core without hardware."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
def source(name):
    text = (root / name).read_text()
    return re.sub(r'^#include[^\n]*\n', '', text, flags=re.M)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
#define U8_MAX UINT8_MAX
#define U32_MAX UINT32_MAX
#define EXPORT_SYMBOL_GPL(x)
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h)))&(UINT32_MAX<<(l)))
#define SZ_64K 65536U
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
#define round_down(n,a) ((n)&~((a)-1))
#define check_add_overflow(a,b,c) __builtin_add_overflow(a,b,c)
#define MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES 2
#define MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES 4
#define MT6797_MD1_BRINGUP_STAGE1_REGION2_PERMISSION 0x16db44
struct mutex { int locked; };
static void mutex_init(struct mutex *m) {m->locked=0;}
static void mutex_lock(struct mutex *m) {assert(!m->locked);m->locked=1;}
static void mutex_unlock(struct mutex *m) {assert(m->locked);m->locked=0;}
static u32 get_unaligned_le32(const void *p) {u32 v;memcpy(&v,p,4);return v;}
static void put_unaligned_le32(u32 v,void *p) {memcpy(p,&v,4);}
'''
headers = ''.join(source('include/linux/soc/mediatek/' + name + '.h') for name in [
    'mt6797-md1-firmware','mt6797-md1-mpu-remap-plan','mt6797-md1-emi-mpu'])
# Use the actual CHECK_HEADER validator as well as plan generation.
implementation = ''.join(source('drivers/soc/mediatek/' + name + '.c') for name in [
    'mt6797-md1-firmware','mt6797-md1-mpu-remap-plan','mt6797-md1-emi-mpu-core'])
owner=(root/'drivers/soc/mediatek/mt6797-md1-cold-owner.c').read_text()
start=owner.index('int mt6797_md1_cold_owner_md1_md3_smem_get(')
getter=owner[start:owner.index('\n}',start)+3]
implementation += r'''
struct mt6797_md1_cold_owner_backend {
 bool md1_md3_smem_initialized;u64 md1_md3_smem_base;u32 md1_md3_smem_size;
};
static struct mutex mt6797_md1_post_vector_evidence_lock;
static struct mt6797_md1_cold_owner_backend *mt6797_md1_handoff_backend;
static int mt6797_md1_handoff_status,mt6797_md1_cold_owner_fatal_latch;
static int atomic_read(int *p) {return *p;}
''' + getter
tests = r'''
static u8 header[512];
static struct mt6797_md1_firmware_contract contract={0xa000000,0x200000,0x200000,0x100000};
static struct mt6797_md1_mpu_remap_layout layout={
 .md_base=0xb4000000,.md_size=0xa000000,.ap_md_smem_base=0x88000000,
 .ap_md_smem_size=0x200000,.md_mpu_smem_size=0x400000,
 .md1_md3_smem_base=0x88200000,.md1_md3_smem_size=0x200000,.lk_reported_load_complete=true};
static int calls,fail;
static int secure_set(void *ctx,u64 start,u64 end,u32 permission)
{
 (void)ctx;calls++;
 if(calls==3)assert(start==0x88200000&&end==0x883fffff&&permission==((9U<<27)|0xb45b40));
 return calls==fail?-EIO:0;
}
static void rejected_geometry(struct mt6797_md1_mpu_remap_layout changed)
{
 struct mt6797_md1_mpu_remap_plan plan,old;
 memset(&plan,0xa5,sizeof(plan));old=plan;
 assert(mt6797_md1_mpu_remap_plan_build(header,512,&contract,&changed,&plan)<0);
 assert(!memcmp(&plan,&old,sizeof(plan)));
}
int main(void)
{
 memcpy(header,"CHECK_HEADER",12);put_unaligned_le32(6,header+12);put_unaligned_le32(2,header+16);
 put_unaligned_le32(12,header+20);header[168]=1;put_unaligned_le32(0xa000000,header+172);
 put_unaligned_le32(0x100000,header+176);put_unaligned_le32(0xcdcdcdaa,header+184);
 put_unaligned_le32(5,header+192);
 for(unsigned r=0;r<5;r++){put_unaligned_le32(r*0x100000,header+196+8*r);put_unaligned_le32(0x100000,header+200+8*r);}
 memset(header+260,0xff,12);put_unaligned_le32(0x100000,header+348);
 put_unaligned_le32(0x200000,header+352);put_unaligned_le32(512,header+508);
 struct mt6797_md1_mpu_remap_plan plan;
 assert(!mt6797_md1_mpu_remap_plan_build(header,512,&contract,&layout,&plan));
 assert(plan.md1_md3_smem.start==0x88200000&&plan.md1_md3_smem.end==0x883fffff);
 assert(plan.md1_md3_smem.access_permission==0xb45b40&&plan.md1_md3_smem.emi_mpu_region==9);
 assert(plan.md1_md3_smem.check_header_region==255);
 assert(plan.md1_md3_smem.start-plan.smem_offset_ap_to_md==0x40200000);
 /* Region9 must execute secure SET, not borrow the attested MD1 LK skip. */
 assert(!mt6797_md1_emi_mpu_plan_entry_matches(&plan,0x88200000,0x883fffff,(9U<<27)|0xb45b40));
 for(unsigned bad=0;bad<7;bad++) {
  struct mt6797_md1_mpu_remap_layout changed=layout;
  switch(bad){case 0:changed.md1_md3_smem_base++;break;
   case 1:changed.md1_md3_smem_base-=0x10000;break;
   case 2:changed.md1_md3_smem_base+=0x10000;break;
   case 3:changed.md1_md3_smem_size--;break;
   case 4:changed.md1_md3_smem_size=0;break;
   case 5:changed.md_mpu_smem_size--;break;
   case 6:changed.ap_md_smem_base=UINT64_MAX-0x1fffff;changed.md1_md3_smem_base=0;break;}
  rejected_geometry(changed);
 }
 struct mt6797_md1_mpu_plan_entry entries[]={plan.mpu[0],plan.mpu[5],plan.md1_md3_smem};
 struct mt6797_md1_emi_mpu_core core;
 mt6797_md1_emi_mpu_core_init(&core);
 assert(mt6797_md1_emi_mpu_core_apply(&core,0,secure_set,0,&plan,MT6797_MD1_EMI_MPU_INITIAL,entries,2)==-EINVAL&&!calls);
 entries[2].access_permission=0;
 assert(mt6797_md1_emi_mpu_core_apply(&core,0,secure_set,0,&plan,MT6797_MD1_EMI_MPU_INITIAL,entries,3)==-EINVAL&&!calls);
 entries[2]=plan.md1_md3_smem;
 for(fail=1;fail<=3;fail++) {
  calls=0;mt6797_md1_emi_mpu_core_init(&core);
  assert(mt6797_md1_emi_mpu_core_apply(&core,0,secure_set,0,&plan,MT6797_MD1_EMI_MPU_INITIAL,entries,3)==-EIO);
  assert(calls==fail&&core.state==MT6797_MD1_EMI_MPU_FATAL);
  assert(mt6797_md1_emi_mpu_core_apply(&core,0,secure_set,0,&plan,MT6797_MD1_EMI_MPU_INITIAL,entries,3)==-EIO&&calls==fail);
 }
 fail=calls=0;mt6797_md1_emi_mpu_core_init(&core);
 assert(!mt6797_md1_emi_mpu_core_apply(&core,0,secure_set,0,&plan,MT6797_MD1_EMI_MPU_INITIAL,entries,3));
 assert(calls==3&&core.state==MT6797_MD1_EMI_MPU_INITIAL_COMMITTED);
 struct mt6797_md1_cold_owner_backend backend={.md1_md3_smem_base=0x88200000,.md1_md3_smem_size=0x200000};
 u64 address=999;u32 size=999;
 assert(mt6797_md1_cold_owner_md1_md3_smem_get(&address,&size)==-ENODATA&&address==999&&size==999);
 mt6797_md1_handoff_backend=&backend;
 assert(mt6797_md1_cold_owner_md1_md3_smem_get(&address,&size)==-ENODATA&&address==999&&size==999);
 backend.md1_md3_smem_initialized=true;
 const int errors[]={-EAGAIN,-ESHUTDOWN,-EIO};
 for(unsigned i=0;i<3;i++) {
  mt6797_md1_handoff_status=errors[i];
  assert(mt6797_md1_cold_owner_md1_md3_smem_get(&address,&size)==errors[i]&&address==999&&size==999);
 }
 mt6797_md1_handoff_status=0;mt6797_md1_cold_owner_fatal_latch=1;
 assert(mt6797_md1_cold_owner_md1_md3_smem_get(&address,&size)==-ENODATA&&address==999&&size==999);
 mt6797_md1_cold_owner_fatal_latch=0;
 assert(!mt6797_md1_cold_owner_md1_md3_smem_get(&address,&size)&&address==0x88200000&&size==0x200000);
 puts("PASS actual intermodem MPU: CHECK_HEADER/layout bounds, exact region9 address/permissions/order, no LK-lock skip, secure failure terminal");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-intermodem-mpu-') as d:
    path=Path(d);c=path/'test.c';out=path/'test'
    c.write_text(shim+headers+implementation+tests)
    subprocess.run(shlex.split(os.environ.get('HOSTCC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-sign-compare',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',str(c),'-o',str(out)],check=True)
    subprocess.run([str(out)],check=True,timeout=10)
