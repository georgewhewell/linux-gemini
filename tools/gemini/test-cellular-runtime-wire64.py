#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual complete CCCI wire helpers; literal vendor 64-slot fixtures."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--reference-source", type=Path,
                    help="compile an original common.c and require the WM-word runtime rejection")
parser.add_argument("--reference-lk-source", type=Path,
                    help="compile an original LK parser and require unused-option runtime rejection")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
directory = root / "drivers/net/wwan/mediatek"
header = (directory / "mtk_ccci_common.h").read_text()
source = (directory / "mtk_ccci_common.c").read_text()
native = (directory / "mtk_mt6797_ccci.c").read_text()
policy_start = native.index("static int mt6797_ccci_runtime_policy(")
policy = native[policy_start:native.index("\n}", policy_start) + 3]
metadata_header = (root / "include/linux/soc/mediatek/mt6797-md1-lk-handoff.h").read_text()
metadata_structs = "\n".join(re.search(r"struct " + name + r" \{.*?\n};", metadata_header, re.S).group(0)
    for name in ["mt6797_md1_lk_runtime_options", "mt6797_md1_lk_runtime_metadata"])
configured = int(re.search(r"^CONFIG_MTK_MT6797_MD1_SUPPORT=(\d+)$",
    (root / "arch/arm64/configs/gemini_defconfig").read_text(), re.M).group(1))
lk_source = (root / "drivers/soc/mediatek/mt6797-md1-lk-handoff.c").read_text()
lk_tag = re.search(r"struct mt6797_md1_lk_tag \{.*?\n};", metadata_header, re.S).group(0)
lk_constants = metadata_header[metadata_header.index("#define MT6797_MD1_LK_INFO_V2_VERSION"):
                               metadata_header.index("struct mt6797_md1_mpu_remap_plan;")]
lk_constants += "\n" + "\n".join(re.findall(r"^#define MT6797_MD1_LK_TAG_(?:DATA_OFFSET|DATA_SIZE_OFFSET|NEXT_OFFSET)\s+.*$", lk_source, re.M)) + "\n"

def function(text, signature):
    start = text.index(signature)
    return text[start:text.index("\n}", start) + 3]

def lk_functions(text):
    # Match the kernel's pointer-sign compiler convention only for these
    # unchanged byte-buffer/string helpers; all other warnings remain fatal.
    return ('#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wpointer-sign"\n' +
            "\n".join(function(text, signature) for signature in [
        "static bool mt6797_md1_lk_range_valid(",
        "static int mt6797_md1_lk_tag_at(",
        "int mt6797_md1_lk_runtime_options_parse("]) +
            '\n#pragma GCC diagnostic pop\n')
# Exact 72-byte query captured from attributed C4 boot
# 60a3d930-8045-4933-9c96-1519270a7062, dmesg-first-event.txt.
c4_query = bytes.fromhex(
    "434343490203030303030303030303030303030303030303030303030303030300000000000000000000000000000000000000000000000000000000000000000000000043434349")
assert len(c4_query) == 72
def strip_includes(text):
    return re.sub(r'^#include[^\n]*\n', '', text, flags=re.M)
shim = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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
tests = r'''
static u8 query[72], runtime[2048], reply[116], hs1[88];
static struct mtk_ccci_runtime_config config = {
 .smem_md_addr=0x40000000, .smem_size=0x100000,
 .random_seed=0x12345678, .ccmni_mtu=3568,
 .load_type=9, .modem_capability=8, .modem_capability_known=true,
};
static void empty_query(void)
{
 memset(query, 0, sizeof(query));
 put_unaligned_le32(0x49434343, query); put_unaligned_le32(0x49434343, query+68);
}
static size_t check_records(size_t total, unsigned tail_support)
{
 size_t at=0;
 for (unsigned id=0; id<64; id++) {
  assert(at+8<=total && runtime[at]==id && !runtime[at+2] && !runtime[at+3]);
  u32 len=get_unaligned_le32(runtime+at+4);
  assert(at+8+len<=total);
  if (id>=25) assert(!len && runtime[at+1]==(id==63 ? tail_support : 0));
  at+=8+len;
 }
 assert(at==total); return at;
}
static void check_sbp(size_t total, u32 bitmap)
{
 size_t at=0;
 for (unsigned id=0; id<64; id++) {
  assert(runtime[at]==id); u32 bytes=get_unaligned_le32(runtime+at+4);
  if(id==9) {
   assert(bytes==16);
   assert(get_unaligned_le32(runtime+at+8)==0);
   assert(get_unaligned_le32(runtime+at+12)==bitmap);
   assert(get_unaligned_le32(runtime+at+16)==0);
   assert(get_unaligned_le32(runtime+at+20)==0);
  }
  if(id==7 && bytes) assert(get_unaligned_le32(runtime+at+8)==config.random_seed);
  if(id==10 && bytes) assert(get_unaligned_le32(runtime+at+8)==1);
  at+=8+bytes;
 }
 assert(at==total);
}
static void refused_sbp(int error)
{
 size_t length=999, response_length=998;
 assert(mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&length)==error);
 assert(length==999);
 memcpy(hs1+16,query,72);
 assert(mtk_ccci_runtime_v2_reply(hs1,88,&config,0,runtime,2048,&length,reply,116,&response_length)==error);
 assert(length==999&&response_length==998);
}
static void check_configured_policy(void)
{
 struct mt6797_md1_lk_runtime_metadata metadata = {.load_type=9,.image_type=8};
 struct mt6797_md1_lk_runtime_metadata old;
 s32 selected=-99; bool override=true; size_t len=0;
 assert(CONFIG_MTK_MT6797_MD1_SUPPORT==12);
 empty_query(); query[4+9]=2;
 /* Missing gate, disabled/negative gate, or missing support uses board policy;
  * contradictory LK-only availability must not replace actual option rules. */
 for (unsigned kind=0;kind<4;kind++) {
  metadata.options=(struct mt6797_md1_lk_runtime_options){
   .gate=kind==3?1:kind==2?-1:0, .gate_present=kind!=0,
   .support=-1,.support_present=kind!=3};
  metadata.capability_available=true; old=metadata;
  assert(!mt6797_ccci_runtime_policy(&metadata,CONFIG_MTK_MT6797_MD1_SUPPORT,&selected,&override));
  assert(selected==12&&!override&&!memcmp(&metadata,&old,sizeof(metadata)));
  config.load_type=metadata.load_type;config.modem_capability=selected;
  config.modem_capability_known=selected>0;
  assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));check_sbp(len,63);
 }
 metadata.options=(struct mt6797_md1_lk_runtime_options){
  .gate=2,.gate_present=true,.support=8,.support_present=true};
 metadata.capability_available=false;old=metadata;
 assert(!mt6797_ccci_runtime_policy(&metadata,12,&selected,&override));
 assert(selected==8&&override&&!memcmp(&metadata,&old,sizeof(metadata)));
 config.modem_capability=selected;
 assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));check_sbp(len,51);
 /* Image type is neither selected policy nor the independent load-type gate. */
 metadata.image_type=21;metadata.load_type=1;
 assert(!mt6797_ccci_runtime_policy(&metadata,12,&selected,&override));
 config.load_type=metadata.load_type;config.modem_capability=selected;
 assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));check_sbp(len,0);
 const s32 invalid[]={0,-1,22};const int errors[]={-ENODATA,-ENODATA,-EOPNOTSUPP};
 for(unsigned i=0;i<3;i++) {
  metadata.options.support=invalid[i];selected=-99;override=false;
  assert(mt6797_ccci_runtime_policy(&metadata,12,&selected,&override)==errors[i]);
  assert(selected==-99&&!override);
 }
 metadata.options.support=0x5a000025;
 assert(!mt6797_ccci_runtime_policy(&metadata,12,&selected,&override));
 config.load_type=9;config.modem_capability=selected;
 assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));check_sbp(len,61);
 metadata.options=(struct mt6797_md1_lk_runtime_options){0};
 assert(!mt6797_ccci_runtime_policy(&metadata,0,&selected,&override));
 assert(selected==0&&!override);
 config.modem_capability=selected;config.modem_capability_known=false;refused_sbp(-ENODATA);
 for(unsigned i=0;i<3;i++) {
  const int failures[]={-EMSGSIZE,-EEXIST,-ERANGE};
  metadata.option_parse_errno=failures[i];selected=-99;override=true;
  assert(mt6797_ccci_runtime_policy(&metadata,12,&selected,&override)==failures[i]);
  assert(selected==-99&&override);
 }
 metadata.option_parse_errno=0;
 assert(mt6797_ccci_runtime_policy(&metadata,22,&selected,&override)==-EOPNOTSUPP);
 assert(mt6797_ccci_runtime_policy(&metadata,-1,&selected,&override)==-ENODATA);
 config.load_type=9;config.modem_capability=8;config.modem_capability_known=true;
 puts("PASS actual configured-policy/wire path: board12 WM63, option precedence, invalid override/parse refusal, raw metadata immutable, independent load/image types");
}
/* Original tables contain no device values: these are synthetic v2 tag copies. */
static u8 option_table[1024];
static unsigned option_count;
static void option_reset(void)
{
 memset(option_table,0,sizeof(option_table)); option_count=0;
}
static void option_add(const char *name,u32 bytes,s32 value)
{
 unsigned at=76*option_count, data=512+16*option_count;
 assert(option_count<6 && strlen(name)<64 && bytes<=8);
 if(option_count)put_unaligned_le32(at,option_table+at-76+72);
 strcpy((char *)option_table+at,name);
 put_unaligned_le32(data,option_table+at+64);
 put_unaligned_le32(bytes,option_table+at+68);
 put_unaligned_le32((u32)value,option_table+data);
 option_count++;
}
static void option_rejected(int error)
{
 struct mt6797_md1_lk_runtime_options options,old;
 memset(&options,0xa5,sizeof(options));old=options;
 assert(mt6797_md1_lk_runtime_options_parse(option_table,sizeof(option_table),option_count,&options)==error);
 assert(!memcmp(&options,&old,sizeof(options)));
}
static void option_policy(s32 expected,bool expected_override)
{
 struct mt6797_md1_lk_runtime_metadata metadata={.load_type=9,.image_type=21};
 s32 selected=-99;bool override=!expected_override;size_t len=0;
 metadata.option_parse_errno=mt6797_md1_lk_runtime_options_parse(option_table,sizeof(option_table),option_count,&metadata.options);
 assert(metadata.option_parse_errno == 0);
 assert(!mt6797_ccci_runtime_policy(&metadata,12,&selected,&override));
 assert(selected==expected&&override==expected_override);
 config.load_type=metadata.load_type;config.modem_capability=selected;config.modem_capability_known=true;
 empty_query();query[4+9]=2;
 assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));
 check_sbp(len,expected==12?63:51);
}
static void check_unused_option(unsigned kind)
{
 unsigned gate=kind/8, shape=kind/2%4, order=kind%2;
 const u32 sizes[]={0,3,8,4};
 assert(kind<24);
 option_reset();
 if(order&&gate)option_add("opt_using_lk_val",4,gate==1?0:-1);
 option_add("opt_md1_support",sizes[shape],8);
 if(shape==3)option_add("opt_md1_support",4,9);
 if(!order&&gate)option_add("opt_using_lk_val",4,gate==1?0:-1);
 option_policy(12,false);
 struct mt6797_md1_lk_runtime_options options;
 assert(!mt6797_md1_lk_runtime_options_parse(option_table,sizeof(option_table),option_count,&options));
 assert(!options.support_present&&!options.support);
}
static void check_gated_options(void)
{
 for(unsigned kind=0;kind<24;kind++)check_unused_option(kind);
 const u32 sizes[]={0,3,8};
 for(unsigned order=0;order<2;order++) {
  for(unsigned shape=0;shape<3;shape++) {
   option_reset();
   if(order)option_add("opt_using_lk_val",4,1);
   option_add("opt_md1_support",sizes[shape],8);
   if(!order)option_add("opt_using_lk_val",4,1);
   option_rejected(-EMSGSIZE);
  }
  option_reset();
  if(order)option_add("opt_using_lk_val",4,1);
  option_add("opt_md1_support",4,8);option_add("opt_md1_support",4,8);
  if(!order)option_add("opt_using_lk_val",4,1);
  option_rejected(-EEXIST);
 }
 for(unsigned shape=0;shape<3;shape++) {
  option_reset();option_add("opt_using_lk_val",sizes[shape],0);
  option_rejected(-EMSGSIZE);
 }
 option_reset();option_add("opt_using_lk_val",4,0);option_add("opt_using_lk_val",4,0);
 option_rejected(-EEXIST);
 for(unsigned gate=0;gate<3;gate++) {
  option_reset();option_add("opt_using_lk_val",4,(s32)gate-1);option_add("opt_md1_support",4,8);
  option_policy(gate==2?8:12,gate==2);
  struct mt6797_md1_lk_runtime_options options;
  assert(!mt6797_md1_lk_runtime_options_parse(option_table,sizeof(option_table),option_count,&options));
  assert(options.support_present&&options.support==8);
 }
 option_reset();option_add("opt_md1_support",4,8);option_policy(12,false);
 struct mt6797_md1_lk_runtime_options diagnostic;
 assert(!mt6797_md1_lk_runtime_options_parse(option_table,sizeof(option_table),option_count,&diagnostic));
 assert(!diagnostic.gate_present&&diagnostic.support_present&&diagnostic.support==8);
 option_reset();option_add("opt_using_lk_val",4,1);option_policy(12,false);
 const s32 bad_values[]={0,-1,22};const int bad_errors[]={-ENODATA,-ENODATA,-EOPNOTSUPP};
 for(unsigned i=0;i<3;i++) {
  option_reset();option_add("opt_using_lk_val",4,1);option_add("opt_md1_support",4,bad_values[i]);
  struct mt6797_md1_lk_runtime_metadata metadata={};s32 selected=-99;bool override=false;
  assert(!mt6797_md1_lk_runtime_options_parse(option_table,sizeof(option_table),option_count,&metadata.options));
  assert(mt6797_ccci_runtime_policy(&metadata,12,&selected,&override)==bad_errors[i]);
  assert(selected==-99&&!override);
 }
 /* An unused semantic error must never hide structural corruption. */
 option_reset();option_add("opt_md1_support",3,8);option_add("unused",4,0);
 put_unaligned_le32(UINT32_MAX,option_table+76+64);option_rejected(-ERANGE);
 option_reset();option_add("opt_md1_support",3,8);option_add("unused",4,0);
 put_unaligned_le32(UINT32_MAX,option_table+72);option_rejected(-ERANGE);
 option_reset();option_add("opt_md1_support",3,8);memset(option_table,'x',64);
 option_rejected(-EBADMSG);
 option_reset();option_add("unused",4,0);option_policy(12,false);
 config.load_type=9;config.modem_capability=8;config.modem_capability_known=true;
 puts("PASS actual LK parser/policy/wire: 24 unused malformed/duplicate controls, both tag orders, enabled failures, valid scalar diagnostics, structural refusal and immutable outputs");
}
int main(int argc,char **argv)
{
 if(argc==2){check_unused_option((unsigned)strtoul(argv[1],NULL,10));return 0;}
 assert(argc==1);
 check_gated_options();
 size_t len=999, reply_len=999;
 empty_query(); memset(runtime, 0xa5, sizeof(runtime));
 assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));
 assert(len==512); check_records(len,0); assert(runtime[512]==0xa5);
 /* Literal IDs and payload sizes from vendor feature switch, not copied
  * implementation enums: known C3-equivalent 124 payload bytes +64*8. */
 const unsigned known[]={0,1,5,7,8,9,10,19,20};
 for(unsigned i=0;i<sizeof(known)/sizeof(known[0]);i++) query[4+known[i]]=2;
 assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));
 assert(len==636); check_records(len,0); check_sbp(len,51);
 mtk_ccci_header_init((void *)hs1,0,0,88);
 ((struct mtk_ccci_header *)hs1)->data1=0;
 ((struct mtk_ccci_header *)hs1)->reserved=cpu_to_le32(0x5555ffff);
 memcpy(hs1+16,query,72);
 assert(!mtk_ccci_runtime_v2_reply(hs1,88,&config,0,runtime,2048,&len,reply,116,&reply_len));
 assert(len==636 && reply_len==116 && get_unaligned_le32(reply+4)==116);
 memcpy(hs1+16,c4_query,72);
 assert(!mtk_ccci_runtime_v2_reply(hs1,88,&config,0,runtime,2048,&len,reply,116,&reply_len));
 assert(len==636 && reply_len==116);
 size_t at=0;
 for(unsigned id=0;id<64;id++) {
  assert(runtime[at]==id);
  u32 bytes=get_unaligned_le32(runtime+at+4);
  if(id>=25) assert(!bytes && runtime[at+1]==(id<=27 ? 1 : 0));
  at+=8+bytes;
 }
 assert(at==636);
 /* Literal public WM values, independent of LK load type9 and image type. */
 const u32 legacy[]={51,57,59,61,63,19,41,45,39,23,25,29,35,39};
 empty_query(); query[4+9]=2;
 for(s32 cap=1;cap<=21;cap++) {
  config.modem_capability=cap;
  assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));
  assert(len==528); check_records(len,0); check_sbp(len,cap<8?0:legacy[cap-8]);
 }
 const s32 enhanced[]={0x5a000000,0x5a000001,0x5a000025,0x5a000039,0x5a00003f};
 const u32 enhanced_wm[]={0,1,61,61,63};
 for(unsigned i=0;i<5;i++) {
  config.modem_capability=enhanced[i];
  assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));
  check_records(len,0); check_sbp(len,enhanced_wm[i]);
 }
 config.modem_capability_known=false; refused_sbp(-ENODATA);
 for(u32 type=1;type<8;type++) {
  config.load_type=type;
  assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));
  check_sbp(len,0);
 }
 config.load_type=0; refused_sbp(-ENODATA);
 config.load_type=22; refused_sbp(-ENODATA);
 config.load_type=9; config.modem_capability_known=true;
 config.modem_capability=0; refused_sbp(-ENODATA);
 config.modem_capability=-1; refused_sbp(-ENODATA);
 config.modem_capability=22; refused_sbp(-EOPNOTSUPP);
 /* Missing capability/load type matters only when feature9 is selected. */
 config.modem_capability_known=false; config.load_type=0;
 empty_query();
 assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));
 assert(len==512);check_records(len,0);
 config.load_type=9;config.modem_capability=8;config.modem_capability_known=true;
 for(unsigned version=0;version<16;version++) {
  empty_query(); query[4+63]=(version<<4)|2; len=999;
  assert(mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len)==-EOPNOTSUPP);
  assert(len==999);
  for(unsigned kind=3;kind<=4;kind++) {
   query[4+63]=(version<<4)|kind;
   assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));
   assert(len==512); check_records(len,1);
  }
  query[4+63]=version<<4;
  assert(!mtk_ccci_runtime_v2_build(query,72,&config,runtime,2048,&len));
  check_records(len,version<<4);
 }
 empty_query(); memset(runtime,0xa5,sizeof(runtime)); len=999;
 assert(mtk_ccci_runtime_v2_build(query,72,&config,runtime,2047,&len)==-EINVAL);
 assert(len==999 && runtime[0]==0xa5);
 u8 *cursor=runtime+2044;
 assert(mtk_ccci_runtime_append(&cursor,runtime+2048,63,0,NULL,0)==-ENOSPC);
 assert(cursor==runtime+2044 && runtime[2044]==0xa5);
 check_configured_policy();
 config.smem_md_addr=UINT32_MAX; reply_len=999;
 assert(mtk_ccci_runtime_v2_response(&config,reply,116,&reply_len)==-ERANGE);
 assert(reply_len==999);
 puts("PASS actual CCCI wire helpers: literal64 headers,636-byte known runtime/116-byte reply, distinct UBIN load/capability and16-byte SBP, unknown/invalid capability refusal, slot63 required refusal and optional/backward versions, buffer/address overflow");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-ccci-wire64-") as temp:
    path = Path(temp)
    fixture = "static const u8 c4_query[72] = {" + ",".join(map(str, c4_query)) + "};\n"
    sources = [("current", source, lk_source)]
    if args.reference_source:
        # Preserve the original complete wire implementation. The new standalone
        # conversion supplies its header-declared symbol but does not alter it.
        conversion = source[source.index("/*\n * Public MT6797 ccci_util_lib_fo.c"):]
        sources.append(("original", args.reference_source.read_text() + "\n" + conversion, lk_source))
    if args.reference_lk_source:
        sources.append(("original-lk", source, args.reference_lk_source.read_text()))
    for label, selected, selected_lk in sources:
        test = path / (label + ".c")
        output = path / label
        test.write_text("\n".join([
            shim, f"#define CONFIG_MTK_MT6797_MD1_SUPPORT {configured}",
            strip_includes(header), strip_includes(selected), metadata_structs,
            lk_constants, lk_tag, lk_functions(selected_lk), policy, fixture, tests]))
        subprocess.run(shlex.split(os.environ.get("HOSTCC", os.environ.get("CC", "cc"))) +
                       ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-fno-pie", "-no-pie"] + shlex.split(os.environ.get("HOSTCFLAGS", "")) +
                       [str(test), "-o", str(output)], check=True)
        if label == "current":
            subprocess.run([str(output)], check=True, timeout=5)
        elif label == "original-lk":
            for kind in range(24):
                result = subprocess.run([str(output), str(kind)], capture_output=True, text=True, timeout=5)
                assert result.returncode != 0, "original LK parser unexpectedly admitted unused malformed support"
                assert "metadata.option_parse_errno == 0" in result.stderr, result.stderr
                assert "AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr
            print("PASS original LK parser compile0 then all24 unused-option runtime controls rejected")
        else:
            result = subprocess.run([str(output)], capture_output=True, text=True, timeout=5)
            assert result.returncode != 0, "original wire unexpectedly admitted the WM contract"
            assert "get_unaligned_le32(runtime+at+12)==bitmap" in result.stderr, result.stderr
            assert "AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr
            print("PASS original common.c compile0 then WM-word runtime rejection")
