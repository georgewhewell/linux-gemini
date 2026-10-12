#!/usr/bin/env python3
"""Execute actual optional MPU capture and scalar endpoint under ASAN/UBSAN."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('handoff', Path(__file__).with_name('test-md3-lk-handoff.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)

SEQ = r'''
#include <stdarg.h>
struct seq_file { char data[4096]; size_t length; };
static void seq_printf(struct seq_file *s, const char *fmt, ...) __attribute__((format(printf,2,3)));
static void seq_printf(struct seq_file *s, const char *fmt, ...) {
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(s->data+s->length,sizeof(s->data)-s->length,fmt,ap); va_end(ap);
    assert(n>=0 && (size_t)n<sizeof(s->data)-s->length); s->length+=n;
}
'''

TESTS = r'''
static void mpu_fixture(void) {
    fixture(); info.tag_count=9;
    strcpy((char*)table+608,"md_mpu_inf");
    put32(table+672,1152); put32(table+676,80); put32(table+680,0);
    static const u32 records[4][5]={
        {0x88400000,0x885fffff,8,0x00b45b68,0},
        {0x88200000,0x883fffff,9,0x00b45b40,0},
        {0xbe000000,0xbe40ffff,16,0x04b45b6c,0},
        {0xbe410000,0xbebfffff,17,0x04b45b6c,0}};
    for(unsigned i=0;i<4;i++) for(unsigned j=0;j<5;j++) put32(table+1152+i*20+j*4,records[i][j]);
}
static void status(int error,u32 count,u32 mask) {
    good(); assert(out.mpu_error==error && out.mpu_record_count==count && out.mpu_present_mask==mask);
    for(unsigned i=0;i<4;i++) if(mask & BIT(i)) assert(!memcmp(&out.mpu[i],table+1152+20*i,20));
    if(!mask) { struct mt6797_md3_lk_mpu_entry empty[4]={0}; assert(!memcmp(out.mpu,empty,sizeof(empty))); }
}
static void mpu_tests(void) {
    mpu_fixture(); status(0,4,15);
    fixture(); status(-ENOENT,0,0);
    mpu_fixture(); put32(table+1152+12,0x12345678); status(-EUCLEAN,4,15);
    mpu_fixture(); put32(table+1152+16,17); status(-EUCLEAN,4,15);
    mpu_fixture(); put32(table+1152,0x88410000); status(-EUCLEAN,4,15);
    mpu_fixture(); put32(table+1152+4,0x883fffff); status(-EUCLEAN,4,15);
    mpu_fixture(); put32(table+1068,0x40f724); status(-ERANGE,4,15);
    mpu_fixture(); put32(table+676,60); status(-ENODATA,3,7);
    /* Arbitrary order is retained in fixed region order, not wire order. */
    mpu_fixture(); u8 tmp[20]; memcpy(tmp,table+1152,20); memcpy(table+1152,table+1212,20);
    memcpy(table+1212,tmp,20); good(); assert(!out.mpu_error && out.mpu[0].region==8 && out.mpu[3].region==17);
    /* Other valid regions are skipped; full actual tag length is bounded. */
    mpu_fixture(); put32(table+676,320);
    for(unsigned i=4;i<16;i++) { unsigned id=i-4; if(id>=8) id+=2; put32(table+1152+i*20+8,id); }
    status(0,16,15);
    /* Late duplicate/invalid IDs cannot expose an ambiguous partial table. */
    mpu_fixture(); put32(table+676,100); put32(table+1232+8,8); status(-EEXIST,0,0);
    mpu_fixture(); put32(table+1212+8,32); status(-ERANGE,0,0);
    mpu_fixture(); put32(table+676,321); status(-EMSGSIZE,0,0);
    for(unsigned n=1;n<320;n++) if(n%20) { mpu_fixture(); put32(table+676,n); status(-EMSGSIZE,0,0); }
    mpu_fixture(); put32(table+676,0); bad();
    mpu_fixture(); info.tag_count=10; put32(table+680,684);
    memcpy(table+684,table+608,76); bad();
    mpu_fixture(); put32(table+672,608); bad();
    mpu_fixture(); put32(table+672,UINT32_MAX-10); bad();
    mpu_fixture(); put32(table+672,852); bad();
    /* Full copied values survive loss of LK storage, including unknown policy. */
    mpu_fixture(); put32(table+1152+12,0x12345678); good();
    memset(table,0,sizeof(table)); assert(out.mpu[0].permission==0x12345678 && out.mpu_error==-EUCLEAN);
}
static void show(struct seq_file *s) { memset(s,0,sizeof(*s)); assert(!md3_metadata_show(s,NULL)); assert(s->length<2048); cases++; }
static void scalar_tests(void) {
    struct device a={1},b={2}; struct seq_file s;
    show(&s); assert(strstr(s.data,"captured=0 metadata_errno=-61") && !strstr(s.data,"load_flags="));
    mpu_fixture(); dt_fixture();
    put32(table+1084,UINT32_MAX); put32(table+1092,UINT32_MAX);
    put32(table+1152+12,0x12345678);
    mt6797_md3_lk_preserve(&a,&info,table);
    memset(table,0,sizeof(table)); memset(&info,0,sizeof(info));
    show(&s);
    assert(strstr(s.data,"schema=2 captured=1 metadata_errno=0 execution_owned=0"));
    assert(strstr(s.data,"lk_version=2 table_size=2048 tag_count=9 load_flags=5 lk_errno=0 md3_load_errno=0"));
    assert(strstr(s.data,"md_id=2 load_type=2 record_version=0 record_load_errno=0 md_base=0xbe000000 md_size=12582912"));
    assert(strstr(s.data,"header_version=1 product_version=1 image_type=2 bind_sys_id=3 ext_attr=0"));
    assert(strstr(s.data,"header_ap_md_smem_size=0 header_size=188 header_mem_size=12582912 header_image_size=4259840 raw_size=4257572 effective_size=4257572"));
    assert(strstr(s.data,"smem_base=0x88000000 smem_total_size=6291456 ap_md1_offset=0 ap_md1_size=2097152 md1_md3_offset=2097152 md1_md3_size=2097152 ap_md3_offset=4194304 ap_md3_size=2097152"));
    assert(strstr(s.data,"gate_present=1 gate=-1 support_present=1 support=2 c2k_mode_present=1 c2k_mode=-1"));
    assert(strstr(s.data,"mpu_errno=-117 mpu_record_count=4 mpu_present_mask=0xf"));
    assert(strstr(s.data,"mpu_region=8 start=0x88400000 end=0x885fffff permission=0x12345678 relate_region=0"));
    assert(strstr(s.data,"mpu_region=9 start=0x88200000 end=0x883fffff permission=0xb45b40 relate_region=0"));
    assert(strstr(s.data,"mpu_region=16 start=0xbe000000 end=0xbe40ffff permission=0x4b45b6c relate_region=0"));
    assert(strstr(s.data,"mpu_region=17 start=0xbe410000 end=0xbebfffff permission=0x4b45b6c relate_region=0"));
    unsigned rows=0; for(char *p=s.data; (p=strstr(p,"mpu_region=")); p++) rows++; assert(rows==4);
    assert(mt6797_md3_lk_handoff_get(&out)==0); out.mpu[0].permission=0;
    assert(mt6797_md3_lk_handoff_get(&out)==0 && out.mpu[0].permission==0x12345678);
    mt6797_md3_lk_preserve(&b,&info,table); mt6797_md3_lk_handoff_remove(&b);
    show(&s); assert(strstr(s.data,"captured=1"));
    mt6797_md3_lk_handoff_remove(&a); show(&s); assert(strstr(s.data,"captured=0") && !strstr(s.data,"load_flags="));
    /* Record raw error evidence, without a valid parsed snapshot or execution. */
    mpu_fixture(); info.md_error[2]=-9; mt6797_md3_lk_preserve(&a,&info,table);
    memset(table,0,sizeof(table)); memset(&info,0,sizeof(info)); show(&s);
    assert(strstr(s.data,"metadata_errno=-5") && strstr(s.data,"md3_load_errno=-9"));
    assert(!strstr(s.data,"md_base=") && !strstr(s.data,"mpu_region="));
    mt6797_md3_lk_handoff_remove(&a);
    mpu_fixture(); put32(table+676,60); mt6797_md3_lk_preserve(&a,&info,table); show(&s);
    assert(strstr(s.data,"mpu_present_mask=0x7") && !strstr(s.data,"mpu_region=17") && !strstr(s.data,"mpu_region=0"));
    mt6797_md3_lk_handoff_remove(&a);
    /* Reservation failure stays optional and never grants the snapshot getter. */
    mpu_fixture(); dt_fixture(); nodes[1].cells=8; nodes[1].ranges[1]=(struct resource){0x88400000,0x88400fff};
    mt6797_md3_lk_preserve(&a,&info,table); show(&s);
    assert(strstr(s.data,"metadata_errno=-34") && strstr(s.data,"load_flags=5"));
    assert(mt6797_md3_lk_handoff_get(&out)==-ERANGE); mt6797_md3_lk_handoff_remove(&a);
}
static void *scalar_reader(void *unused) {
    (void)unused;
    for(unsigned i=0;i<20000;i++) {
        struct seq_file s={0}; assert(!md3_metadata_show(&s,NULL));
        assert(s.length<2048);
        if(strstr(s.data,"captured=1")) {
            assert(strstr(s.data,"mpu_record_count=4") && strstr(s.data,"tag_count=9"));
            assert(strstr(s.data,"mpu_region=17") && strstr(s.data,"md_base=0xbe000000"));
        } else assert(!strstr(s.data,"load_flags=") && !strstr(s.data,"mpu_region="));
    } return NULL;
}
int main(void) {
    assert(!base_main()); mpu_tests(); scalar_tests();
    pthread_t reader; struct device a={1}; mpu_fixture(); dt_fixture();
    assert(!pthread_create(&reader,NULL,scalar_reader,NULL));
    for(unsigned i=0;i<200;i++) { mt6797_md3_lk_preserve(&a,&info,table); mt6797_md3_lk_handoff_remove(&a); }
    assert(!pthread_join(reader,NULL)); cases++;
    printf("PASS %u total parser/MPU/scalar/lifetime cases; coherent concurrent read/remove\n",cases);
    return 0;
}
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc',default=os.environ.get('HOSTCC',os.environ.get('CC','cc')))
    p.add_argument('--negative',choices=['mpu-copy','mpu-duplicate','raw-info','tuple','mask'])
    args=p.parse_args()
    md3=(ROOT/'drivers/soc/mediatek/mt6797-md3-lk-handoff.c').read_text()
    probe=(ROOT/'drivers/soc/mediatek/mt6797-md1-lk-handoff-probe.c').read_text()
    assert 'debugfs_create_file("md3_metadata", 0400,' in probe
    assert 'mt6797_md3_lk_memory_attest' not in h.function(probe,'md3_metadata_show')
    mutations={
        'mpu-copy':('memcpy(out->mpu, records, sizeof(records));',''),
        'mpu-duplicate':('if (seen & BIT(entry.region))','if (false && (seen & BIT(entry.region)))'),
        'raw-info':('mt6797_md3_lk_load_info = *info;','memset(&mt6797_md3_lk_load_info, 0, sizeof(mt6797_md3_lk_load_info));'),
        'tuple':('if (cells != stride) {','if (false) {'),
        'mask':('if (!(snapshot.mpu_present_mask & BIT(i)))','if (false)'),
    }
    if args.negative:
        old,new=mutations[args.negative]
        assert md3.count(old)+probe.count(old)==1
        md3=md3.replace(old,new); probe=probe.replace(old,new)
    units=[h.SHIM.replace('typedef uint64_t u64;','typedef unsigned long long u64;'),SEQ]
    for name in ['mt6797-md1-lk-handoff.h','mt6797-md3-lk-handoff.h']:
        units.append(h.clean((ROOT/'include/linux/soc/mediatek'/name).read_text()))
    units += [h.clean((ROOT/'drivers/soc/mediatek/mt6797-md1-lk-handoff.c').read_text()),h.clean(md3),h.PROBE_SHIM]
    for name in ['mt6797_md3_lk_handoff_get','mt6797_md3_lk_handoff_remove',
                 'mt6797_md3_lk_reserved_check','mt6797_md3_lk_preserve','md3_metadata_show']:
        units.append(h.function(probe,name))
    units += [h.TESTS.replace('int main(void)','int base_main(void)').replace('cases); }','cases); return 0; }'),TESTS]
    with tempfile.TemporaryDirectory(prefix='md3-metadata-host-') as tmp:
        source=Path(tmp)/'actual.c'; binary=Path(tmp)/'actual'
        source.write_text('\n'.join(units))
        subprocess.run([args.cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-Wno-pointer-sign','-Wno-sign-compare','-Wno-misleading-indentation','-Wno-unused-parameter',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=30)

if __name__=='__main__': main()
