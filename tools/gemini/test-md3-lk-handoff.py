#!/usr/bin/env python3
"""Compile actual optional MD3 parser/acquisition functions; no device access."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def clean(text):
    return re.sub(r'^#include[^\n]*\n', '', text, flags=re.M)


def function(text, name):
    match = re.search(r'^(?:static )?(?:int|void) ' + name + r'\(', text, re.M)
    assert match, name
    start = text.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end] + '\n'


SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
typedef uint8_t u8; typedef int8_t s8; typedef uint32_t u32;
typedef int32_t s32; typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define U32_MAX UINT32_MAX
#define U64_MAX UINT64_MAX
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define EXPORT_SYMBOL_GPL(x)
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define check_add_overflow(a,b,c) __builtin_add_overflow((a),(b),(c))
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v,p,4); return v; }
static u64 get_unaligned_le64(const void *p) { u64 v; memcpy(&v,p,8); return v; }
static void put32(void *p,u32 v) { memcpy(p,&v,4); }
static void put64(void *p,u64 v) { memcpy(p,&v,8); }
'''

PROBE_SHIM = r'''
#define MT6797_MD1_LIVE_MD3_BLOCK_BASE 0xbe000000ULL
#define MT6797_MD1_LIVE_MD3_BLOCK_SIZE 0xc00000U
#define MT6797_MD1_LIVE_SMEM_BASE 0x88000000ULL
#define MT6797_MD1_LIVE_SMEM_SIZE 0x600000U
#define MT6797_MD1_LIVE_CCCI_COMPAT "mediatek,reserve-memory-ccci_md"
struct device { int id; };
struct resource { u64 start, end; };
struct device_node { bool ccci, reg; int cells, stride, error, gets, puts;
                    struct resource ranges[2]; struct device_node *next; };
static struct device_node parent, nodes[3];
static bool have_parent = true;
static struct device_node *of_find_node_by_path(const char *p) {
    (void)p; if (!have_parent) return NULL; parent.gets++; return &parent;
}
static struct device_node *next_node(struct device_node *old) {
    struct device_node *n = old ? old->next : nodes;
    if (old) old->puts++;
    if (n) n->gets++;
    return n;
}
#define for_each_child_of_node(p,n) for ((n)=next_node(NULL); (n); (n)=next_node(n))
static void of_node_put(struct device_node *n) { n->puts++; }
static bool of_device_is_compatible(struct device_node *n,const char *s) { (void)s; return n->ccci; }
static bool of_property_present(struct device_node *n,const char *s) { (void)s; return n->reg; }
static int of_property_count_u32_elems(struct device_node *n,const char *s) { (void)s; return n->cells; }
static int of_n_addr_cells(struct device_node *n) { return n->stride; }
static int of_n_size_cells(struct device_node *n) { (void)n; return 0; }
static int of_address_to_resource(struct device_node *n,int i,struct resource *r) {
    if (n->error) return n->error; assert(i >= 0 && i < 2); *r=n->ranges[i]; return 0;
}
static u64 resource_size(const struct resource *r) { return r->end-r->start+1; }
static bool mt6797_md1_phys_ranges_overlap(u64 a,u64 as,u64 b,u64 bs) { return a < b+bs && b < a+as; }
static pthread_mutex_t mt6797_md1_lk_handoff_plan_lock = PTHREAD_MUTEX_INITIALIZER;
#define mutex_lock(p) assert(pthread_mutex_lock(p)==0)
#define mutex_unlock(p) assert(pthread_mutex_unlock(p)==0)
static int action_error;
static int devm_add_action_or_reset(struct device *d,void (*f)(void*),void *a) {
    (void)d; if(action_error) f(a); return action_error;
}
static struct mt6797_md3_lk_snapshot mt6797_md3_lk_handoff_snapshot;
static struct mt6797_md1_lk_info mt6797_md3_lk_load_info;
static struct device *mt6797_md3_lk_handoff_owner;
static int mt6797_md3_lk_handoff_error = -ENODATA;
'''

TESTS = r'''
static u8 table[2048];
static const u32 payload[] = {800,804,852,892,1080,1084,1088,1092};
static const u32 lengths[] = {4,48,40,188,4,4,4,4};
static struct mt6797_md1_lk_info info;
static struct mt6797_md3_lk_contract contract;
static struct mt6797_md3_lk_snapshot out;
static unsigned int cases;
static void fixture(void) {
    memset(table,0,sizeof(table));
    info=(struct mt6797_md1_lk_info){.table_size=sizeof(table),.version=2,.tag_count=8,.load_flags=5};
    contract=(struct mt6797_md3_lk_contract){0xbe000000,0xc00000,0x88000000,0x600000};
    for(unsigned i=0;i<8;i++) {
        strcpy((char*)table+i*76,mt6797_md3_tag_names[i]);
        put32(table+i*76+64,payload[i]); put32(table+i*76+68,lengths[i]);
        put32(table+i*76+72,(i+1)*76);
    }
    put32(table+800,2); put64(table+804,0xb4000000); put32(table+812,0xa000000);
    table[816]=0; table[818]=12;
    put64(table+828,contract.md_base); put32(table+836,contract.md_size);
    table[840]=2; table[842]=2;
    put64(table+852,contract.smem_base); put32(table+864,0x200000);
    put32(table+868,0x400000); put32(table+872,0x200000);
    put32(table+876,0x200000); put32(table+880,0x200000); put32(table+884,0x600000);
    memcpy(table+892,"CHECK_HEADER",12); put32(table+904,1); put32(table+908,1);
    put32(table+912,2); strcpy((char*)table+916,"MT6797_S00"); table[1060]=3;
    put32(table+1064,0xc00000); put32(table+1068,0x410000); put32(table+1076,188);
    put32(table+1080,0x40f724); put32(table+1084,1); put32(table+1088,2); put32(table+1092,2);
}
static int parse(void) { return mt6797_md3_lk_snapshot_parse(&info,table,sizeof(table),&contract,&out); }
static void good(void) { cases++; assert(parse()==0); }
static void bad(void) {
    struct mt6797_md3_lk_snapshot before;
    memset(&out,0xa5,sizeof(out)); before=out; cases++; assert(parse()<0);
    assert(!memcmp(&out,&before,sizeof(out)));
}
#define BAD(change) do { fixture(); change; bad(); } while(0)
static void parser_tests(void) {
    fixture(); good(); assert(out.raw_image_size==0x40f724 && out.effective_image_size==0x40f724);
    assert(out.header_image_size==0x410000 && out.options.support==2 && out.c2k_lte_mode==2);
    assert(!memcmp(out.check_header,table+892,188));
    fixture(); info.tag_count=5; good(); assert(!out.options.gate_present && !out.c2k_lte_mode_present);
    fixture(); put32(table+1084,0xffffffff); put32(table+1088,0xffffffff); good();
    assert(out.options.gate==-1 && out.options.support==-1);
    fixture(); put32(table+904,2); put32(table+1068,0x400000); good(); assert(out.effective_image_size==0x400000);
    fixture(); put32(table+904,2); put32(table+1068,0); good(); assert(out.effective_image_size==0x40f724);
    /* Linked offsets need not be physically ordered. */
    fixture(); u8 tmp[76]; memcpy(tmp,table+76,76); memcpy(table+76,table+152,76); memcpy(table+152,tmp,76);
    put32(table+72,152); put32(table+152+72,76); put32(table+76+72,228); good();
    BAD(info.load_flags=1); BAD(info.error=-1); BAD(info.md_error[2]=-1);
    BAD(info.version=1); BAD(info.table_size--); BAD(info.tag_count=0); BAD(info.tag_count=1000);
    BAD(contract.md_base=UINT64_MAX-3); BAD(contract.md_size=0); BAD(contract.smem_base=0xbe000000);
    BAD(put32(table+800,0)); BAD(put32(table+800,5)); BAD(table[840]=0); BAD(table[840]=4);
    BAD(table[841]=1); BAD(table[842]=12); BAD(put32(table+836,0xb00000)); BAD(put64(table+828,0xbf000000));
    BAD(put32(table+868,0x300000)); BAD(put32(table+872,0)); BAD(put32(table+880,0));
    BAD(put32(table+880,0xffffffff)); BAD(put32(table+884,0x500000)); BAD(put32(table+876,0x100000));
    BAD(put64(table+852,UINT64_MAX-1024));
    BAD(put32(table+864,0x200001); put32(table+876,0x200001); put32(table+880,0x1fffff));
    BAD(table[892]='X'); BAD(table[1060]=1); BAD(memset(table+916,'A',16)); BAD(table[916]='Q');
    BAD(put32(table+904,3)); BAD(put32(table+912,12)); BAD(put32(table+1076,284));
    BAD(put32(table+1080,0)); BAD(put32(table+1080,0xd00000));
    BAD(put32(table+904,2)); BAD(put32(table+904,2); put32(table+1064,0));
    BAD(put32(table+904,2); put32(table+1064,0xd00000));
    BAD(put32(table+72,0)); BAD(put32(table+72,40)); BAD(put32(table+72,2040));
    BAD(memset(table,'a',64)); BAD(put32(table+64,UINT32_MAX-1));
    BAD(strcpy((char*)table+76,"hdr_count")); BAD(strcpy((char*)table+76,"unknown"));
    BAD(put32(table+3*76+68,187)); BAD(put32(table+7*76+68,8));
    BAD(put32(table+3*76+64,0)); BAD(put32(table+3*76+64,850));
    /* Last next is not a link; original LK count controls termination. */
    fixture(); put32(table+7*76+72,UINT32_MAX); good();
    for(unsigned n=1;n<sizeof(table);n++) {
        fixture(); info.table_size=n; struct mt6797_md3_lk_snapshot prior;
        memset(&out,0xa5,sizeof(out)); prior=out;
        int r=mt6797_md3_lk_snapshot_parse(&info,table,n,&contract,&out);
        if(r) assert(!memcmp(&prior,&out,sizeof(out))); cases++;
    }
    /* Deterministic arbitrary tag/header damage exercises bounds under ASAN. */
    unsigned seed=0x12345678;
    for(unsigned n=0;n<10000;n++) {
        fixture(); seed=seed*1664525+1013904223; unsigned off=(seed>>8)%sizeof(table);
        table[off]^=1U<<(seed&7); struct mt6797_md3_lk_snapshot prior;
        memset(&out,0xa5,sizeof(out)); prior=out; int r=parse();
        if(r) assert(!memcmp(&prior,&out,sizeof(out))); cases++;
    }
}
static void dt_fixture(void) {
    memset(&parent,0,sizeof(parent)); memset(nodes,0,sizeof(nodes)); have_parent=true;
    for(int i=0;i<3;i++) {
        nodes[i].reg=true; nodes[i].stride=4; nodes[i].cells=4;
        nodes[i].ranges[0]=(struct resource){0x40000000+i*0x10000,0x40000fff+i*0x10000};
        if(i<2) nodes[i].next=&nodes[i+1];
    }
}
static void dt_check(int expected) {
    cases++; assert(mt6797_md3_lk_reserved_check()==expected);
    assert(parent.gets==parent.puts); for(int i=0;i<3;i++) assert(nodes[i].gets==nodes[i].puts);
}
static void reserved_tests(void) {
    dt_fixture(); dt_check(0);
    dt_fixture(); have_parent=false; dt_check(-ENODEV);
    dt_fixture(); nodes[1].ranges[0]=(struct resource){0xbe000000,0xbe00ffff}; dt_check(-ERANGE);
    dt_fixture(); nodes[1].cells=8; nodes[1].ranges[1]=(struct resource){0xbe000000,0xbe00ffff}; dt_check(-ERANGE);
    dt_fixture(); nodes[1].cells=8; nodes[1].ranges[1]=(struct resource){0x88400000,0x88400fff}; dt_check(-ERANGE);
    dt_fixture(); nodes[1].ccci=true; nodes[1].cells=8; dt_check(-EINVAL);
    dt_fixture(); nodes[1].ranges[0]=(struct resource){0x41000000,0x40ffffff}; dt_check(-ERANGE);
    dt_fixture(); nodes[1].ranges[0]=(struct resource){0,UINT64_MAX}; dt_check(-ERANGE);
    dt_fixture(); nodes[1].error=-EIO; dt_check(-EIO);
    dt_fixture(); nodes[1].cells=5; dt_check(-EINVAL);
    dt_fixture(); nodes[1].reg=false; nodes[1].cells=-1; dt_check(0);
    dt_fixture(); nodes[1].ccci=true; nodes[1].ranges[0]=(struct resource){0xbe000000,0xbe00ffff}; dt_check(0);
}
static void *reader(void *unused) {
    (void)unused; struct mt6797_md3_lk_snapshot snap;
    for(int i=0;i<20000;i++) {
        memset(&snap,0xa5,sizeof(snap)); int r=mt6797_md3_lk_handoff_get(&snap);
        assert(r==0 || r==-ENODATA);
        if(!r) assert(snap.raw_image_size==0x40f724 && !memcmp(snap.check_header,"CHECK_HEADER",12));
        else assert(snap.check_header[0]==0xa5);
    } return NULL;
}
static void lifetime_tests(void) {
    struct device a={1},b={2}; pthread_t thread;
    assert(mt6797_md3_lk_handoff_get(NULL)==-EINVAL);
    assert(mt6797_md3_lk_handoff_get(&out)==-ENODATA);
    fixture(); dt_fixture(); action_error=-ENOMEM;
    mt6797_md3_lk_preserve(&a,&info,table); assert(mt6797_md3_lk_handoff_get(&out)==-ENODATA);
    action_error=0; mt6797_md3_lk_preserve(&a,&info,table);
    memset(table,0,sizeof(table)); assert(mt6797_md3_lk_handoff_get(&out)==0);
    assert(!memcmp(out.check_header,"CHECK_HEADER",12));
    mt6797_md3_lk_preserve(&b,&info,table); mt6797_md3_lk_handoff_remove(&b);
    assert(mt6797_md3_lk_handoff_get(&out)==0); mt6797_md3_lk_handoff_remove(&a);
    assert(mt6797_md3_lk_handoff_get(&out)==-ENODATA);
    fixture(); info.load_flags=1; mt6797_md3_lk_preserve(&a,&info,table);
    memset(&out,0xa5,sizeof(out)); assert(mt6797_md3_lk_handoff_get(&out)==-ENODATA);
    assert(out.check_header[0]==0xa5); mt6797_md3_lk_handoff_remove(&a);
    fixture(); dt_fixture(); nodes[1].error=-EIO; mt6797_md3_lk_preserve(&a,&info,table);
    assert(mt6797_md3_lk_handoff_get(&out)==-EIO); mt6797_md3_lk_handoff_remove(&a);
    fixture(); dt_fixture(); assert(pthread_create(&thread,NULL,reader,NULL)==0);
    for(int i=0;i<200;i++) { mt6797_md3_lk_preserve(&a,&info,table); mt6797_md3_lk_handoff_remove(&a); }
    assert(pthread_join(thread,NULL)==0); cases+=14;
}
int main(void) { parser_tests(); reserved_tests(); lifetime_tests(); printf("PASS %u parser/bounds/lifetime cases\n",cases); }
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default=os.environ.get('HOSTCC', os.environ.get('CC', 'cc')))
    p.add_argument('--negative', choices=['load-flag', 'bind', 'copy', 'v1-size'])
    p.add_argument('--header', type=Path, help='optional private exact 188-byte retained MD3 check header')
    args = p.parse_args()
    md3 = (ROOT / 'drivers/soc/mediatek/mt6797-md3-lk-handoff.c').read_text()
    probe = (ROOT / 'drivers/soc/mediatek/mt6797-md1-lk-handoff-probe.c').read_text()
    # This source-order check complements executed copy/clear/get lifetime tests.
    body = function(probe, 'mt6797_md1_lk_handoff_probe')
    assert body.index('mt6797_md3_lk_preserve(') < body.index('mt6797_md1_lk_handoff_plan_attested = true')
    assert 'return mt6797_md3' not in body
    mutations = {
        'load-flag': ('if (!(info->load_flags & BIT(MT6797_MD3_LK_INDEX)))', 'if (0)'),
        'bind': ('|| header[168] != 3', '|| false'),
        'copy': ('memcpy(out.check_header, header, sizeof(out.check_header));', ''),
        'v1-size': ('if (out.header_version == 2) {', 'if (out.header_version >= 1) {'),
    }
    if args.negative:
        old, new = mutations[args.negative]
        assert md3.count(old) == 1
        md3 = md3.replace(old, new)
    units = [SHIM]
    for name in ['mt6797-md1-lk-handoff.h', 'mt6797-md3-lk-handoff.h']:
        units.append(clean((ROOT / 'include/linux/soc/mediatek' / name).read_text()))
    units.append(clean((ROOT / 'drivers/soc/mediatek/mt6797-md1-lk-handoff.c').read_text()))
    units += [clean(md3), PROBE_SHIM]
    for name in ['mt6797_md3_lk_handoff_get', 'mt6797_md3_lk_handoff_remove',
                 'mt6797_md3_lk_reserved_check', 'mt6797_md3_lk_preserve']:
        units.append(function(probe, name))
    tests = TESTS
    if args.header:
        data = args.header.read_bytes()
        assert len(data) == 188
        units.append('static const u8 retained_header[] = {' + ','.join(map(str, data)) + '};')
        tests = tests.replace('put32(table+1080,0x40f724);',
                              'memcpy(table+892,retained_header,188); put32(table+1080,0x40f724);')
    units.append(tests)
    with tempfile.TemporaryDirectory(prefix='md3-lk-host-') as temp:
        source = Path(temp) / 'actual.c'
        binary = Path(temp) / 'actual'
        source.write_text('\n'.join(units))
        subprocess.run([args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-pointer-sign', '-Wno-sign-compare', '-Wno-misleading-indentation',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    main()
