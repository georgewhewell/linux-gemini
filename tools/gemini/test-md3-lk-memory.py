#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual parser + locked-memory admission, with bounded synthetic LK records."""
import hashlib, importlib.util, json, os
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('handoff',Path(__file__).with_name('test-md3-lk-handoff.py'))
base=importlib.util.module_from_spec(spec); spec.loader.exec_module(base)
EXTRA=r'''
static void mpu_fixture(void) {
 static const u32 records[4][5]={
  {0x88400000,0x885fffff,8,0x00b45b68,0},
  {0x88200000,0x883fffff,9,0x00b45b40,0},
  {0xbe000000,0xbe40ffff,16,0x04b45b6c,0},
  {0xbe410000,0xbebfffff,17,0x04b45b6c,0},
 };
 fixture(); info.tag_count=9;
 strcpy((char*)table+608,"md_mpu_inf"); put32(table+672,1152);
 put32(table+676,80); put32(table+680,0); memcpy(table+1152,records,sizeof(records));
}
static void mpu_check(int error) {
 assert(!mt6797_md3_lk_snapshot_parse(&info,table,sizeof(table),&contract,&out));
 if(out.mpu_error!=error) fprintf(stderr,"mpu case%u got%d expected%d\n",cases,out.mpu_error,error);
 assert(out.mpu_error==error); cases++;
}
static void mpu_tests(void) {
 static const u32 physical[]={0xc5c3c1bf,0xcdcbc9c7,0x8f8d8b89,0x97959391};
 static const u32 offset[]={0x8583817f,0x8d8b8987,0x4f4d4b49,0x57555351};
 u32 changed[4]; struct mt6797_md3_lk_snapshot saved;
 mpu_fixture(); mpu_check(0); saved=out;
 /* Actual161 emitted md3img equals the header span, not container dsize.
  * The selected MPU records below also match actual161. Other fixture tag
  * bytes remain synthetic: no full live LK table was captured or retained.
  */
 mpu_fixture(); put32(table+1080,0x410000); mpu_check(0);
 assert(out.raw_image_size==0x410000 && out.effective_image_size==0x410000);
 assert(!mt6797_md3_lk_memory_attest(&out,physical,1U<<17));
 saved=out;
 assert(!mt6797_md3_lk_memory_attest(&out,physical,1U<<17));
 assert(!mt6797_md3_lk_memory_attest(&out,offset,(1U<<17)|(1U<<16)));
 assert(mt6797_md3_lk_memory_attest(&out,physical,1U<<16)==-EUCLEAN);
 for(unsigned bank=0;bank<4;bank++) for(unsigned bit=0;bit<32;bit++) {
  memcpy(changed,physical,sizeof(changed)); changed[bank]^=1U<<bit;
  assert(mt6797_md3_lk_memory_attest(&out,changed,1U<<17)==-EUCLEAN); cases++;
 }
 memcpy(changed,physical,sizeof(changed)); changed[2]=offset[2];changed[3]=offset[3];
 assert(mt6797_md3_lk_memory_attest(&out,changed,1U<<17)==-EUCLEAN);
 for(unsigned row=0;row<4;row++) for(unsigned field=0;field<5;field++) {
  out=saved; ((u32*)&out.mpu[row])[field]^=1;
  assert(mt6797_md3_lk_memory_attest(&out,physical,1U<<17)==-EUCLEAN); cases++;
  mpu_fixture(); table[1152+row*20+field*4]^=1;
  mpu_check(field==2?-EEXIST:-EUCLEAN);
 }
 mpu_fixture(); put32(table+1152+20+8,8); mpu_check(-EEXIST);
 mpu_fixture(); put32(table+1152+8,32); mpu_check(-ERANGE);
 mpu_fixture(); put32(table+676,79); mpu_check(-EMSGSIZE);
 mpu_fixture(); put32(table+676,340); mpu_check(-EMSGSIZE);
 mpu_fixture(); put32(table+676,60); mpu_check(-ENODATA);
 fixture(); mpu_check(-ENOENT);
 mpu_fixture(); put32(table+1068,0x40f724); mpu_check(-ERANGE);
 mpu_fixture(); put32(table+1064,0xb00000); mpu_check(-ERANGE);
 mpu_fixture(); put32(table+1068,0xc00000); mpu_check(-ERANGE);
 mpu_fixture(); put32(table+1068,0); mpu_check(-ERANGE);
 mpu_fixture(); mpu_check(0); dt_fixture(); struct device d={8};
 mt6797_md3_lk_preserve(&d,&info,table); memset(table,0,sizeof(table));
 assert(!mt6797_md3_lk_handoff_get(&out));
 assert(!mt6797_md3_lk_memory_attest(&out,physical,1U<<17));
 mt6797_md3_lk_handoff_remove(&d); cases++;
}
'''

def main():
    parser=(ROOT/'drivers/soc/mediatek/mt6797-md3-lk-handoff.c').read_text()
    probe=(ROOT/'drivers/soc/mediatek/mt6797-md1-lk-handoff-probe.c').read_text()
    units=[base.SHIM]
    for name in ['mt6797-md1-lk-handoff.h','mt6797-md3-lk-handoff.h']:
        units.append(base.clean((ROOT/'include/linux/soc/mediatek'/name).read_text()))
    units.append(base.clean((ROOT/'drivers/soc/mediatek/mt6797-md1-lk-handoff.c').read_text()))
    units.extend([base.clean(parser),base.PROBE_SHIM])
    for name in ['mt6797_md3_lk_handoff_get','mt6797_md3_lk_handoff_remove',
                 'mt6797_md3_lk_reserved_check','mt6797_md3_lk_preserve']:
        units.append(base.function(probe,name))
    tests=base.TESTS.replace('int main(void)',EXTRA+'\nint main(void)')
    tests=tests.replace('parser_tests(); reserved_tests(); lifetime_tests();','parser_tests(); reserved_tests(); lifetime_tests(); mpu_tests();')
    units.append(tests); actual='\n'.join(units)
    negative={
        'lock-bypass':('if (!(lock & BIT(17)))','if (0)'),
        'permission-bypass':('if (memcmp(records, expected, sizeof(records)))','if (0)'),
        'snapshot-forgery':('if (memcmp(expected, s->mpu, sizeof(expected)))','if (0)'),
        'mixed-ddr-banks':('if (banks[bank * 2 + word] != value)','if (bank == 0 && banks[bank * 2 + word] != value)'),
    }
    with tempfile.TemporaryDirectory(prefix='md3-memory-') as directory:
        p=Path(directory)
        for label,source in [('positive',actual)]+[(n,actual.replace(a,b)) for n,(a,b) in negative.items()]:
            (p/'actual.c').write_text(source)
            subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
             '-Wno-pointer-sign','-Wno-sign-compare','-Wno-misleading-indentation','-Wno-unused-parameter',
             '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread',
             str(p/'actual.c'),'-o',str(p/'actual')],check=True)
            r=subprocess.run([str(p/'actual')],capture_output=True,text=True,timeout=30)
            if label=='positive':
                assert r.returncode==0,(r.stdout,r.stderr)
                print(r.stdout,end='')
            else:
                assert r.returncode!=0 and 'Assertion' in r.stderr,(label,r.stdout,r.stderr)
                print('REJECTED '+label)

if __name__=='__main__': main()
