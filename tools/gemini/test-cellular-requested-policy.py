#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual cold policy selection and all CCCI serialization helpers."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
base = root / 'drivers/net/wwan/mediatek'
native = (base / 'mtk_mt6797_ccci.c').read_text()
header = (base / 'mtk_ccci_common.h').read_text()
common = (base / 'mtk_ccci_common.c').read_text()
metadata = (root / 'include/linux/soc/mediatek/mt6797-md1-lk-handoff.h').read_text()


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 3]


def no_includes(text):
    return re.sub(r'^#include[^\n]*\n', '', text, flags=re.M)


# These are admission/source-wiring guards, not substitutes for execution.
assert re.search(r'module_param_named\(requested_radio_policy,\s*'
                 r'mt6797_ccci_requested_radio_policy,\s*int, 0444\)', native)
admit = function(native, 'static int mt6797_ccci_validate_handoff(')
assert admit.index('mt6797_ccci_runtime_policy(') < admit.index('mt6797_ccci_apply_policy_request(') < admit.index('*smem_res =')
assert 'mt6797_ccci_requested_radio_policy ? "linux_request"' in admit
assert 'metadata.load_type' in admit and 'metadata.image_type' in admit
policy = function(native, 'static int mt6797_ccci_runtime_policy(')
request = function(native, 'static int mt6797_ccci_apply_policy_request(')
structs = '\n'.join(re.search(r'struct ' + name + r' \{.*?\n};', metadata, re.S).group(0)
                    for name in ['mt6797_md1_lk_runtime_options', 'mt6797_md1_lk_runtime_metadata'])
shim = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64;
typedef uint16_t __le16; typedef uint32_t __le32;
#define __packed __attribute__((packed))
#define EXPORT_SYMBOL_GPL(x)
#define static_assert _Static_assert
#define BIT(x) (1U << (x))
#define GENMASK(h,l) ((UINT32_MAX >> (31-(h))) & (UINT32_MAX << (l)))
#define U32_MAX UINT32_MAX
#define cpu_to_le32(x) ((u32)(x))
#define le32_to_cpu(x) ((u32)(x))
#define fallthrough __attribute__((fallthrough))
#define check_add_overflow(a,b,c) __builtin_add_overflow(a,b,c)
static void put_unaligned_le32(u32 x, void *p) { memcpy(p, &x, 4); }
static u32 get_unaligned_le32(const void *p) { u32 x; memcpy(&x, p, 4); return x; }
'''
test = r'''
static struct mt6797_md1_lk_runtime_metadata m = {
 .load_type=12,.load_type_valid=true,.image_type=12,
 .options={.gate=1,.gate_present=true,.support=12,.support_present=true}
};
static void refused(s32 raw,s32 req,int error) {
 s32 chosen=-99;
 struct mt6797_md1_lk_runtime_metadata old=m;
 assert(mt6797_ccci_apply_policy_request(&m,raw,req,&chosen)==error);
 assert(chosen==-99&&!memcmp(&m,&old,sizeof(m)));
}
static u8 query[72],original[2048],narrow[2048];
static void serialize(s32 chosen,u8 *out,size_t *len) {
 struct mtk_ccci_runtime_config c={.smem_md_addr=0x40000000,.smem_size=0x200000,
  .md_mpu_smem_size=0x400000,.md1_md3_smem_md_addr=0x40200000,.md1_md3_smem_size=0x200000,
  .random_seed=0x12345678,.ccmni_mtu=3568,.load_type=m.load_type,
  .modem_capability=chosen,.modem_capability_known=true};
 assert(!mtk_ccci_runtime_v2_build(query,72,&c,out,2048,len));
}
int main(void) {
 s32 raw=-99,chosen=-99;bool overridden=false;
 struct mt6797_md1_lk_runtime_metadata old=m;
 assert(!mt6797_ccci_runtime_policy(&m,10,&raw,&overridden)&&raw==12&&overridden);
 assert(!mt6797_ccci_apply_policy_request(&m,raw,0,&chosen)&&chosen==12);
 assert(!mt6797_ccci_apply_policy_request(&m,raw,10,&chosen)&&chosen==10);
 assert(!memcmp(&m,&old,sizeof(m)));
 /* Every public non-C2K subset is admitted, rather than a magic WM59 patch. */
 const s32 allowed[]={8,9,10,13,14,18,20};
 for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++) {
  assert(!mt6797_ccci_apply_policy_request(&m,12,allowed[i],&chosen));
  assert(chosen==allowed[i]);
 }
 const s32 c2k[]={11,12,15,16,17,19,21};
 for(unsigned i=0;i<sizeof(c2k)/sizeof(c2k[0]);i++)refused(12,c2k[i],-EOPNOTSUPP);
 const s32 invalid[]={-1,1,7,22,0x5a00001f,INT32_MAX,INT32_MIN};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)refused(12,invalid[i],-EINVAL);
 refused(0,10,-ENODATA);refused(-1,10,-ENODATA);refused(22,10,-EOPNOTSUPP);
 refused(1,10,-ENODATA);refused(0x5a000000,10,-ENODATA);
 refused(8,10,-ERANGE);refused(9,10,-ERANGE);
 assert(!mt6797_ccci_apply_policy_request(&m,10,9,&chosen)&&chosen==9);
 assert(!mt6797_ccci_apply_policy_request(&m,0x5a00003f,10,&chosen)&&chosen==10);
 for(unsigned i=0;i<6;i++) {
  m=old;
  if(i==0)m.load_type_valid=false;
  if(i==1)m.load_type=0;
  if(i==2)m.load_type=7;
  if(i==3)m.load_type=22;
  if(i==4)m.image_type=7;
  if(i==5)m.image_type=22;
  refused(12,10,-ENODATA);
  assert(!mt6797_ccci_apply_policy_request(&m,12,0,&chosen)&&chosen==12);
 }
 m=old;m.options.gate=0;
 assert(!mt6797_ccci_runtime_policy(&m,10,&raw,&overridden)&&raw==10&&!overridden);
 assert(!mt6797_ccci_apply_policy_request(&m,raw,0,&chosen)&&chosen==10);
 m=old;m.option_parse_errno=-ERANGE;raw=-99;overridden=false;
 assert(mt6797_ccci_runtime_policy(&m,12,&raw,&overridden)==-ERANGE&&raw==-99&&!overridden);
 m=old;
 put_unaligned_le32(0x49434343,query);put_unaligned_le32(0x49434343,query+68);
 /* Matched firmware query: record0 mandatory2, records1..26 optional3. */
 query[4]=2;for(unsigned i=1;i<27;i++)query[4+i]=3;
 size_t a=0,b=0;serialize(12,original,&a);serialize(10,narrow,&b);assert(a==b);
 unsigned diffs=0;size_t offset=0;
 for(unsigned i=0;i<64;i++) {
  assert(narrow[offset]==i);u32 n=get_unaligned_le32(narrow+offset+4);
  if(i==9) {
   assert(n==16&&get_unaligned_le32(original+offset+12)==63&&get_unaligned_le32(narrow+offset+12)==59);
   for(unsigned j=0;j<n+8;j++)if(original[offset+j]!=narrow[offset+j]){assert(j==12);diffs++;}
  } else assert(!memcmp(original+offset,narrow+offset,n+8));
  if(i==12)assert(n==0&&narrow[offset+1]==1); /* No invented C2K owner. */
  offset+=8+n;
 }
 assert(offset==a&&diffs==1);
 query[4+12]=2;
 struct mtk_ccci_runtime_config c={.load_type=12,.modem_capability=10,.modem_capability_known=true};
 size_t unpublished=999;
 assert(mtk_ccci_runtime_v2_build(query,72,&c,narrow,2048,&unpublished)==-EOPNOTSUPP&&unpublished==999);
 puts("PASS actual requested policy: raw LK precedence, immutable metadata, UBIN subset, absent MD3 rejection, exact one-byte WM change, unsupported mandatory C2K refusal");
}
'''
mutations = {'current': request}
if args.negative_controls:
    mutations.update({
        'lost-subset': request.replace('(requested_bitmap & ~raw_bitmap)', '0'),
        'allows-c2k': request.replace('requested_bitmap & BIT(2)', 'false'),
        'ignores-request': request.replace('*selected = requested;', '*selected = raw_policy;'),
        'ignores-load-validation': request.replace('!metadata->load_type_valid ||', ''),
    })
with tempfile.TemporaryDirectory(prefix='gemini-requested-policy-') as tmp:
    for name, body in mutations.items():
        c = Path(tmp) / (name + '.c')
        out = Path(tmp) / name
        c.write_text(shim + no_includes(header) + no_includes(common) + structs + policy + body + test)
        subprocess.run(shlex.split(os.environ.get('HOSTCC', 'cc')) +
                       ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', str(c), '-o', str(out)], check=True)
        r = subprocess.run([str(out)], capture_output=True, text=True, timeout=5)
        if name == 'current':
            assert r.returncode == 0, r.stderr
            print(r.stdout.strip())
        else:
            assert r.returncode != 0 and 'Assertion' in r.stderr, (name, r.returncode, r.stderr)
            print('PASS rejected compiled negative control:', name)
