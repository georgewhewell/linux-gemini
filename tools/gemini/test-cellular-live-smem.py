#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual CCCI handoff and carveout admission; no device access."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference-source', type=Path,
                    help='compile original native admission and require configured-policy runtime rejection')
parser.add_argument('--reference-extent-root', type=Path,
                    help='compile original parser/admission/response and require v1 extent runtime rejection')
args = parser.parse_args()
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
runtime_header=(root / 'drivers/net/wwan/mediatek/mtk_ccci_common.h').read_text()
metadata_header=(root / 'include/linux/soc/mediatek/mt6797-md1-lk-handoff.h').read_text()
constants += '\n' + re.search(r'^#define MT6797_MD1_LK_SMEM_LAYOUT_SIZE\s+.*$', metadata_header, re.M).group(0)
common = (root / 'drivers/net/wwan/mediatek/mtk_ccci_common.c').read_text()
lk_source = (root / 'drivers/soc/mediatek/mt6797-md1-lk-handoff.c').read_text()
handoff = (root / 'drivers/soc/mediatek/mt6797-md1-lk-handoff-probe.c').read_text()
publication = re.search(r'plan_layout = \(struct mt6797_md1_mpu_remap_layout\) \{.*?\n\t};', handoff, re.S).group(0)
constants += '\n' + '\n'.join(re.findall(r'^#define MT6797_MD1_LK_SMEM_.*$', lk_source, re.M))
constants += '\n' + '\n'.join(re.findall(r'^#define MTK_CCCI_(?:FEATURE_COUNT|RUNTIME_V2_RESPONSE_SIZE|AP_FEATURE_PATTERN)\s+.*$', runtime_header, re.M))
constants += '\n#define MTK_CCCI_FEATURE_NOT_EXIST 0\n'
conversion = common[common.index('/*\n * Public MT6797 ccci_util_lib_fo.c'):]
configured = int(re.search(r'^CONFIG_MTK_MT6797_MD1_SUPPORT=(\d+)$',
    (root / 'arch/arm64/configs/gemini_defconfig').read_text(), re.M).group(1))
runtime_config=re.search(r'struct mtk_ccci_runtime_config \{.*?\n};',runtime_header,re.S).group(0)
metadata_structs='\n'.join(re.search(r'struct '+name+r' \{.*?\n};',metadata_header,re.S).group(0) for name in ['mt6797_md1_lk_runtime_options','mt6797_md1_lk_runtime_metadata'])
smem_struct=re.search(r'struct mt6797_md1_lk_smem_layout \{.*?\n};',metadata_header,re.S).group(0)
modem_struct=re.search(r'struct mt6797_md1_lk_modem_info \{.*?\n};',metadata_header,re.S).group(0).replace('s8 load_error;', 'int8_t load_error;')
response_struct=re.search(r'struct mtk_ccci_feature_response \{.*?\n} __packed;',common,re.S).group(0)
layout = re.search(r'struct mt6797_md1_mpu_remap_layout \{.*?\n};', header, re.S).group(0)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
typedef uint64_t u64;
typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8;
typedef uint32_t __le32;
#define __packed __attribute__((packed))
#define cpu_to_le32(x) ((u32)(x))
#define check_add_overflow(a,b,c) __builtin_add_overflow(a,b,c)
static u32 get_unaligned_le32(const void *p) { u32 n; memcpy(&n,p,4); return n; }
static u64 get_unaligned_le64(const void *p) { u64 n; memcpy(&n,p,8); return n; }
static void put_unaligned_le32(u32 n, void *p) { memcpy(p,&n,4); }
static void put_unaligned_le64(u64 n, void *p) { memcpy(p,&n,8); }
#define BIT(x) (1U << (x))
#define EXPORT_SYMBOL_GPL(x)
#define dev_info(dev, ...) ((void)(dev))
#define EPROBE_DEFER 517
#define U32_MAX UINT32_MAX
#define SZ_1M 0x100000
#define IORESOURCE_MEM 0x200
#define MTK_CCCI_NET_RX_SIZE 3584
#define MT6797_MD1_POST_VECTOR_GO 7
struct resource { u64 start, end; unsigned long flags; };
struct mt6797_md1_post_vector_evidence { int token; bool passive_complete; };
struct mt6797_md1_mpu_remap_plan { u64 smem_offset_ap_to_md; };
'''
shim += runtime_config+'\n'+metadata_structs+r'''
struct mt6797_ccci {
 void *dev;struct mtk_ccci_runtime_config runtime_config;
};
'''
shim += constants + '\n' + layout + '\n' + smem_struct + '\n' + modem_struct + '\n' + response_struct + r'''
static int wait_error, layout_error, plan_error, metadata_error, memory_error, step, token, random_calls;
static struct mt6797_md1_lk_runtime_metadata metadata;
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
static int mt6797_md1_cold_owner_md1_md3_smem_get(u64 *base,u32 *size)
{ assert(step++ == 3); if(memory_error)return memory_error;*base=0x88200000;*size=0x200000;return 0; }
static int mt6797_md1_lk_handoff_runtime_get(struct mt6797_md1_lk_runtime_metadata *m)
{ assert(step++ == 4);if(metadata_error)return metadata_error;*m=metadata;return 0; }
static u32 get_random_u32(void) { random_calls++; return 123; }
'''
shim += function(core, 'int mt6797_md1_cldma_lk_carveout_validate(')
shim += '\n#define CONFIG_MTK_MT6797_MD1_SUPPORT '+str(configured)+'\n'
shim += '\n'.join(re.findall(r'^#define MTK_CCCI_(?:UBIN_FIRST_TYPE|MAX_LOAD_TYPE)\s+.*$', runtime_header,re.M))+'\n'
shim += conversion+'\n'
shim += function(common, 'static u8 mtk_ccci_feature(')+'\n'
shim += r'''
static void publish_geometry(const struct mt6797_md1_lk_smem_layout *parsed)
{
 struct mt6797_md1_lk_smem_layout smem=*parsed;
 struct mt6797_md1_lk_modem_info md1={.base_phys=MT6797_MD1_CLDMA_LK_MD_BASE,
  .size=MT6797_MD1_CLDMA_LK_MD_SIZE};
 struct mt6797_md1_mpu_remap_layout plan_layout;
''' + publication + r'''
 saved=plan_layout;
}
'''
tests = r'''
static u8 raw_smem[40];
static void smem_reset(void)
{
 memset(raw_smem,0,sizeof(raw_smem));
 put_unaligned_le64(0x88000000,raw_smem);
 put_unaligned_le32(0x200000,raw_smem+12);
 put_unaligned_le32(0x400000,raw_smem+16);
 put_unaligned_le32(0x200000,raw_smem+20);
 put_unaligned_le32(0x200000,raw_smem+24);
 put_unaligned_le32(0x200000,raw_smem+28);
 put_unaligned_le32(0x600000,raw_smem+32);
}
static void rejected_geometry(void)
{
 struct mt6797_md1_lk_smem_layout parsed,old;
 memset(&parsed,0xa5,sizeof(parsed));old=parsed;
 assert(mt6797_md1_lk_smem_layout_parse(raw_smem,sizeof(raw_smem),&parsed)==-ERANGE);
 assert(!memcmp(&parsed,&old,sizeof(parsed)));
}
static void check_geometry(void)
{
 struct mt6797_md1_lk_smem_layout parsed;
 smem_reset();
 assert(!mt6797_md1_lk_smem_layout_parse(raw_smem,sizeof(raw_smem),&parsed));
 assert(parsed.ap_md1_size==0x200000 && parsed.md1_mpu_smem_size==0x400000);
 publish_geometry(&parsed);
 assert(saved.ap_md_smem_size==0x200000 && saved.md_mpu_smem_size==0x400000);
 /* Empty companion memory adds zero, regardless of its unused offset. */
 put_unaligned_le32(0x500000,raw_smem+24);put_unaligned_le32(0,raw_smem+28);
 assert(!mt6797_md1_lk_smem_layout_parse(raw_smem,sizeof(raw_smem),&parsed));
 assert(parsed.md1_mpu_smem_size==parsed.ap_md1_size);
 smem_reset();put_unaligned_le32(0,raw_smem+20);
 assert(!mt6797_md1_lk_smem_layout_parse(raw_smem,sizeof(raw_smem),&parsed));
 const u32 offsets[]={0,0x100000,0x300000};
 for(unsigned i=0;i<3;i++) {smem_reset();put_unaligned_le32(offsets[i],raw_smem+24);rejected_geometry();}
 const u32 ap3[]={0,0x100000,0x200000,0x300000};
 for(unsigned i=0;i<4;i++) {smem_reset();put_unaligned_le32(ap3[i],raw_smem+16);rejected_geometry();}
 smem_reset();put_unaligned_le32(0,raw_smem+12);rejected_geometry();
 smem_reset();put_unaligned_le32(0x500000,raw_smem+28);rejected_geometry();
 smem_reset();put_unaligned_le32(UINT32_MAX,raw_smem+12);rejected_geometry();
 smem_reset();put_unaligned_le32(UINT32_MAX,raw_smem+24);rejected_geometry();
 smem_reset();put_unaligned_le64(UINT64_MAX-0x100000,raw_smem);rejected_geometry();
 /* A nonzero AP1 offset is valid when all slices remain contiguous/disjoint. */
 smem_reset();put_unaligned_le32(0x100000,raw_smem+8);
 put_unaligned_le32(0x300000,raw_smem+24);put_unaligned_le32(0x500000,raw_smem+16);
 put_unaligned_le32(0x100000,raw_smem+20);
 assert(!mt6797_md1_lk_smem_layout_parse(raw_smem,sizeof(raw_smem),&parsed));
 assert(parsed.md1_mpu_smem_size==0x400000);
}
static void reset(void)
{
 wait_error=layout_error=plan_error=metadata_error=memory_error=step=random_calls=0;
 metadata=(struct mt6797_md1_lk_runtime_metadata){.load_type=9,.image_type=12,
 .load_type_valid=true,.capability_available=true,
 .options={.gate=1,.gate_present=true,.support=8,.support_present=true}};
 token=MT6797_MD1_POST_VECTOR_GO; complete=true; offset=0x48000000;
 saved=(struct mt6797_md1_mpu_remap_layout){
 .md_base=MT6797_MD1_CLDMA_LK_MD_BASE, .md_size=MT6797_MD1_CLDMA_LK_MD_SIZE,
 .ap_md_smem_base=MT6797_MD1_CLDMA_LK_SMEM_BASE,
 /* Independent live C2 RMPU slice, not the validator's constant. */
 .ap_md_smem_size=0x00200000, .md_mpu_smem_size=0x00400000,
 .md1_md3_smem_base=0x88200000,.md1_md3_smem_size=0x200000,
 .lk_reported_load_complete=true };
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
static void rejected_response(struct mtk_ccci_runtime_config config,int error)
{
 u8 response[100],old[100];size_t length=999;
 memset(response,0xa5,sizeof(response));memcpy(old,response,sizeof(old));
 assert(mtk_ccci_runtime_v2_response(&config,response,sizeof(response),&length)==error);
 assert(length==999&&!memcmp(response,old,sizeof(response)));
}
int main(int argc,char **argv)
{
 struct resource r;
 struct mt6797_ccci c={0};
 if(argc==2&&!strcmp(argv[1],"policy")) {
  reset();metadata.options=(struct mt6797_md1_lk_runtime_options){0};
  metadata.capability_available=false;
  assert(!mt6797_ccci_validate_handoff(&c,&r));
  assert(c.runtime_config.modem_capability==12&&c.runtime_config.modem_capability_known);
  return 0;
 }
 check_geometry();
 reset();smem_reset();struct mt6797_md1_lk_smem_layout parsed;
 assert(!mt6797_md1_lk_smem_layout_parse(raw_smem,sizeof(raw_smem),&parsed));
 publish_geometry(&parsed);
 assert(!mt6797_ccci_validate_handoff(&c,&r));
 assert(r.start==0x88000000 && r.end==0x881fffff && r.flags==IORESOURCE_MEM);
 assert(c.runtime_config.smem_md_addr==0x40000000 && c.runtime_config.smem_size==0x200000);
 assert(c.runtime_config.md_mpu_smem_size==0x400000);
 assert(c.runtime_config.md1_md3_smem_md_addr==0x40200000&&c.runtime_config.md1_md3_smem_size==0x200000);
 assert(MTK_CCCI_RUNTIME_OFFSET + MTK_CCCI_RUNTIME_AP_SIZE +
        MTK_CCCI_RUNTIME_MD_SIZE <= c.runtime_config.smem_size);
 assert(c.runtime_config.smem_size <= r.end-r.start+1);
 u8 response[100];size_t response_len=999;
 assert(!mtk_ccci_runtime_v2_response(&c.runtime_config,response,sizeof(response),&response_len));
 assert(response_len==100&&response[5]==0x10);
 assert(get_unaligned_le32(response+88)==0x40000000);
 assert(get_unaligned_le32(response+92)==0x400000);
 assert(get_unaligned_le32(response+72)==0x40010000&&get_unaligned_le32(response+76)==0x800);
 assert(get_unaligned_le32(response+80)==0x40010800&&get_unaligned_le32(response+84)==0x800);
 struct mtk_ccci_runtime_config altered=c.runtime_config;
 altered.md_mpu_smem_size=0;rejected_response(altered,-EINVAL);
 altered.md_mpu_smem_size=0x100000;rejected_response(altered,-EINVAL);
 altered=c.runtime_config;altered.smem_size=MTK_CCCI_RUNTIME_OFFSET+0x1000-1;
 rejected_response(altered,-EINVAL);
 altered=c.runtime_config;altered.smem_md_addr=UINT32_MAX-0x400000+1;
 assert(!mtk_ccci_runtime_v2_response(&altered,response,sizeof(response),&response_len));
 altered.smem_md_addr++;rejected_response(altered,-ERANGE);
 assert(step==5 && random_calls==1);
 assert(c.runtime_config.load_type==9&&c.runtime_config.modem_capability==8&&c.runtime_config.modem_capability_known);
 /* Actual native admission must use configured12 when LK has no override. */
 reset();metadata.options=(struct mt6797_md1_lk_runtime_options){0};
 metadata.capability_available=false;
 assert(!mt6797_ccci_validate_handoff(&c,&r));
 assert(c.runtime_config.modem_capability==12&&c.runtime_config.modem_capability_known);
 assert(c.runtime_config.load_type==9&&metadata.image_type==12);
 for (unsigned kind=0;kind<3;kind++) {
  reset();metadata.options.gate=kind==0?0:kind==1?-1:1;
  if(kind==2)metadata.options.support_present=false;
  assert(!mt6797_ccci_validate_handoff(&c,&r));
  assert(c.runtime_config.modem_capability==12&&c.runtime_config.modem_capability_known);
 }
 reset();metadata.options.gate_present=false;
 assert(!mt6797_ccci_validate_handoff(&c,&r));assert(c.runtime_config.modem_capability==12);
 const s32 invalid[]={0,-1,22};const int policy_errors[]={-ENODATA,-ENODATA,-EOPNOTSUPP};
 for(unsigned i=0;i<3;i++) {reset();metadata.options.support=invalid[i];rejected(policy_errors[i]);}
 const int parse_errors[]={-EMSGSIZE,-EEXIST,-ERANGE};
 for(unsigned i=0;i<3;i++) {reset();metadata.option_parse_errno=parse_errors[i];rejected(parse_errors[i]);}
 reset();metadata.load_type=0;metadata.load_type_valid=false;
 assert(!mt6797_ccci_validate_handoff(&c,&r));assert(!c.runtime_config.load_type);
 const int errors[]={-ENODATA,-EPROBE_DEFER,-ETIMEDOUT,-ESHUTDOWN,-EIO};
 for (unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
  reset(); wait_error=errors[i]; rejected(errors[i]==-ENODATA ? -EPROBE_DEFER : errors[i]);
  reset(); layout_error=errors[i]; rejected(errors[i]==-ENODATA ? -EPROBE_DEFER : errors[i]);
  reset(); plan_error=errors[i]; rejected(errors[i]);
  reset(); memory_error=errors[i]; rejected(errors[i]);
  reset(); metadata_error=errors[i]; rejected(errors[i]==-ENODATA ? -EPROBE_DEFER : errors[i]);
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
 reset(); saved.md_mpu_smem_size=0; rejected(-ERANGE);
 reset(); saved.md_mpu_smem_size=0x00100000; rejected(-ERANGE);
 reset();saved.md1_md3_smem_base++;rejected(-ERANGE);
 reset();saved.md1_md3_smem_size--;rejected(-ERANGE);
 reset(); offset=UINT64_MAX; rejected(-ERANGE);
 reset(); offset=saved.ap_md_smem_base+1; rejected(-ERANGE);
 reset();offset=saved.ap_md_smem_base-(UINT32_MAX-0x200000+1ULL);rejected(-ERANGE);
 puts("PASS actual CCCI live smem: parser/publication/admission/response preserve 2MiB AP1 mapping and distinct 4MiB v1 MPU span; zero companion, gaps/overlaps/wrap/address-end refusal; configured12/LK precedence and untouched failure outputs");
}
'''
with tempfile.TemporaryDirectory(prefix='cellular-live-smem-') as directory:
    path = Path(directory)
    sources = [('current', source, common, lk_source)]
    if args.reference_source:
        sources.append(('original', args.reference_source.read_text(), common, lk_source))
    if args.reference_extent_root:
        reference=args.reference_extent_root
        sources.extend([
            ('original-native', (reference/'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text(), common, lk_source),
            ('original-response', source, (reference/'drivers/net/wwan/mediatek/mtk_ccci_common.c').read_text(), lk_source),
            ('original-parser', source, common, (reference/'drivers/soc/mediatek/mt6797-md1-lk-handoff.c').read_text())])
    for label, native, wire, selected_lk in sources:
        admission = function(native, 'static int mt6797_ccci_validate_handoff(')
        helper = (function(native, 'static int mt6797_ccci_runtime_policy(')
                  if 'static int mt6797_ccci_runtime_policy(' in native else '')
        test = path / (label+'.c')
        output = path / label
        geometry=function(selected_lk,'static bool mt6797_md1_lk_range_valid(')+'\n'+function(selected_lk,'int mt6797_md1_lk_smem_layout_parse(')
        response=function(wire,'int mtk_ccci_runtime_v2_response(')
        test.write_text(shim + geometry + response + helper + admission + tests)
        subprocess.run(shlex.split(os.environ.get('HOSTCC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra',
                        '-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie'] +
                       shlex.split(os.environ.get('HOSTCFLAGS','')) + [str(test), '-o', str(output)], check=True)
        if label == 'current':
            subprocess.run([str(output)], check=True, timeout=5)
        else:
            result = subprocess.run([str(output)]+(['policy'] if label=='original' else []), capture_output=True, text=True, timeout=5)
            assert result.returncode != 0, f'{label} unexpectedly satisfied corrected owner contract'
            expected={'original':'c.runtime_config.modem_capability==12&&c.runtime_config.modem_capability_known',
                      'original-native':'c.runtime_config.smem_size==0x200000',
                      'original-response':'get_unaligned_le32(response+92)==0x400000',
                      'original-parser':'parsed.md1_mpu_smem_size==0x400000'}[label]
            assert expected in result.stderr, result.stderr
            assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
            print(f'PASS {label} compile0 then actual owner runtime rejection: exit={result.returncode}')
