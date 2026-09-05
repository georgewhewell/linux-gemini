#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual CCCI handoff and carveout admission; no device access."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text()
core = (root / 'drivers/soc/mediatek/mt6797-md1-cldma-core.c').read_text()
header = (root / 'include/linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h').read_text()

def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 3]

probe = function(source, 'static int mt6797_ccci_probe(')
assert 'of_parse_phandle' not in probe and 'of_address_to_resource' not in probe
assert probe.index('mt6797_ccci_validate_handoff') < probe.index('devm_ioremap_wc')
constants = '\n'.join(re.findall(r'^#define MT6797_MD1_CLDMA_LK_.*$', core, re.M))
constants += '\n' + '\n'.join(re.findall(
    r'^#define MTK_CCCI_RUNTIME_(?:AP_SIZE|MD_SIZE|OFFSET)\s+.*$',
    (root / 'drivers/net/wwan/mediatek/mtk_ccci_common.h').read_text(), re.M))
layout = re.search(r'struct mt6797_md1_mpu_remap_layout \{.*?\n};', header, re.S).group(0)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define EPROBE_DEFER 517
#define U32_MAX UINT32_MAX
#define SZ_1M 0x100000
#define IORESOURCE_MEM 0x200
#define MTK_CCCI_NET_RX_SIZE 3584
#define MT6797_MD1_POST_VECTOR_GO 7
struct resource { u64 start, end; unsigned long flags; };
struct mt6797_md1_post_vector_evidence { int token; bool passive_complete; };
struct mt6797_md1_mpu_remap_plan { u64 smem_offset_ap_to_md; };
struct mt6797_ccci {
 void *dev;
 struct { u32 smem_md_addr, smem_size, random_seed, ccmni_mtu; } runtime_config;
};
'''
shim += constants + '\n' + layout + r'''
static int wait_error, layout_error, plan_error, step, token, random_calls;
static bool complete;
static struct mt6797_md1_mpu_remap_layout saved;
static u64 offset;
static int mt6797_md1_cold_owner_wait_handoff(void *dev,
 struct mt6797_md1_post_vector_evidence *e)
{ (void)dev; assert(step++ == 0); e->token=token; e->passive_complete=complete; return wait_error; }
static int mt6797_md1_lk_handoff_layout_get(struct mt6797_md1_mpu_remap_layout *l)
{ assert(step++ == 1); *l=saved; return layout_error; }
static int mt6797_md1_lk_handoff_plan_get(struct mt6797_md1_mpu_remap_plan *p)
{ assert(step++ == 2); p->smem_offset_ap_to_md=offset; return plan_error; }
static u32 get_random_u32(void) { random_calls++; return 123; }
'''
shim += function(core, 'int mt6797_md1_cldma_lk_carveout_validate(')
shim += function(source, 'static int mt6797_ccci_validate_handoff(')
tests = r'''
static void reset(void)
{
 wait_error=layout_error=plan_error=step=random_calls=0;
 token=MT6797_MD1_POST_VECTOR_GO; complete=true; offset=0x48000000;
 saved=(struct mt6797_md1_mpu_remap_layout){
 .md_base=MT6797_MD1_CLDMA_LK_MD_BASE, .md_size=MT6797_MD1_CLDMA_LK_MD_SIZE,
 .ap_md_smem_base=MT6797_MD1_CLDMA_LK_SMEM_BASE,
 /* Independent live C2 RMPU slice, not the validator's constant. */
 .ap_md_smem_size=0x00200000, .lk_reported_load_complete=true };
}
static void rejected(int expected)
{
 struct resource r, old;
 struct mt6797_ccci c, previous;
 memset(&r, 0xa5, sizeof(r)); old=r;
 memset(&c, 0xa5, sizeof(c)); previous=c;
 assert(mt6797_ccci_validate_handoff(&c, &r)==expected);
 assert(!memcmp(&r,&old,sizeof(r)) && !memcmp(&c,&previous,sizeof(c)));
 assert(!random_calls);
}
int main(void)
{
 struct resource r;
 struct mt6797_ccci c={0};
 reset(); assert(!mt6797_ccci_validate_handoff(&c,&r));
 assert(r.start==0x88000000 && r.end==0x881fffff && r.flags==IORESOURCE_MEM);
 assert(c.runtime_config.smem_md_addr==0x40000000 && c.runtime_config.smem_size==SZ_1M);
 assert(MTK_CCCI_RUNTIME_OFFSET + MTK_CCCI_RUNTIME_AP_SIZE +
        MTK_CCCI_RUNTIME_MD_SIZE <= c.runtime_config.smem_size);
 assert(c.runtime_config.smem_size <= r.end-r.start+1);
 assert(step==3 && random_calls==1);
 const int errors[]={-ENODATA,-EPROBE_DEFER,-ETIMEDOUT,-ESHUTDOWN,-EIO};
 for (unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
  reset(); wait_error=errors[i]; rejected(errors[i]==-ENODATA ? -EPROBE_DEFER : errors[i]);
  reset(); layout_error=errors[i]; rejected(errors[i]==-ENODATA ? -EPROBE_DEFER : errors[i]);
  reset(); plan_error=errors[i]; rejected(errors[i]);
 }
 reset(); token=0; rejected(-EPERM);
 reset(); complete=false; rejected(-EPERM);
 reset(); saved.lk_reported_load_complete=false; rejected(-EPERM);
 reset(); saved.md_base++; rejected(-ERANGE);
 reset(); saved.md_size--; rejected(-ERANGE);
 reset(); saved.ap_md_smem_base++; rejected(-ERANGE);
 reset(); saved.ap_md_smem_size=0; rejected(-ERANGE);
 reset(); saved.ap_md_smem_size++; rejected(-ERANGE);
 reset(); saved.ap_md_smem_size=0x00600000; rejected(-ERANGE);
 reset(); saved.ap_md_smem_size=0x00100000; rejected(-ERANGE);
 reset(); offset=UINT64_MAX; rejected(-ERANGE);
 reset(); offset=saved.ap_md_smem_base+1; rejected(-ERANGE);
 puts("PASS actual CCCI live smem: exact 2 MiB AP-MD1 slice, 27 refusal paths including whole 6 MiB mblock, output untouched");
}
'''
with tempfile.TemporaryDirectory(prefix='cellular-live-smem-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(shim + tests)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=5)
