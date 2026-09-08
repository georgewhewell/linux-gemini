#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual kernel archive validator against a producer-generated fixture."""
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
from nvram_restore_archive import archive_bytes
root = Path(__file__).resolve().parents[2]
fixture = runpy.run_path(str(Path(__file__).with_name('test-nvram-restore-archive.py')))
header = (root/'drivers/soc/mediatek/mt6797-md1-fs-archive.h').read_text()
shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint32_t u32;
static void cond_resched(void) {}
'''
test = r'''
static void put(u8 *p,u32 v){for(int i=0;i<4;i++)p[i]=v>>(8*i);}
static void bad(u8 *p,size_t n){
 struct mt6797_fs_archive v={NULL,123,456};
 assert(mt6797_fs_archive_validate(p,n,&v)==-EINVAL);
 assert(v.data==NULL&&v.count==123&&v.size==456);
}
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
 u8 original[448],data[449];assert(fread(original,1,448,f)==448);fclose(f);
 struct mt6797_fs_archive v;
 assert(!mt6797_fs_archive_validate(original,448,&v)&&v.count==3&&v.size==448&&v.data==original);
 for(size_t n=0;n<448;n++)bad(original,n);
 memcpy(data,original,448);data[448]=0;bad(data,449);
 bad(NULL,448);assert(mt6797_fs_archive_validate(original,448,NULL)==-EINVAL);
 for(unsigned k=0;k<8;k++){memcpy(data,original,448);data[k]^=1;bad(data,448);}
 const u32 counts[]={0,4,4097,0xffffffff};
 for(unsigned k=0;k<4;k++){memcpy(data,original,448);put(data+8,counts[k]);bad(data,448);}
 const unsigned fields[]={12,16,20,24};
 for(unsigned k=0;k<4;k++){memcpy(data,original,448);put(data+fields[k],0xffffffff);bad(data,448);}
 memcpy(data,original,448);put(data+20,437);bad(data,448);
 memcpy(data,original,448);put(data+24,3);bad(data,448);
 memcpy(data,original,448);memcpy(data+156,data+16,4);memcpy(data+168,data+28,128);bad(data,448);
 memcpy(data,original,448);memcpy(data+168,data+28,128);
 assert(!mt6797_fs_archive_validate(data,448,&v)); /* same path, different drive */
 const char *paths[]={"", "/a", "a/", "a//b", "a/../b", "./a", "..", "a\\b", "Z:a", "a\nb",
 "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80", "\xf5\x80\x80\x80"};
 for(unsigned k=0;k<sizeof(paths)/sizeof(*paths);k++){
  memcpy(data,original,448);memset(data+28,0,128);memcpy(data+28,paths[k],strlen(paths[k]));bad(data,448);
 }
 memcpy(data,original,448);memset(data+28,'a',128);bad(data,448);
 memcpy(data,original,448);data[30]='x';bad(data,448); /* nonzero after terminator */
 const char *good[]={"caf\xc3\xa9", "\xf0\x9f\x8c\x8d", "a/b", "..."};
 for(unsigned k=0;k<4;k++){
  memcpy(data,original,448);memset(data+28,0,128);memcpy(data+28,good[k],strlen(good[k]));
  assert(!mt6797_fs_archive_validate(data,448,&v));
 }
 /* Empty payloads have equal consecutive offsets; no unused trailing bytes. */
 memcpy(data,original,448);put(data+12,436);
 for(unsigned i=0;i<3;i++){put(data+20+140*i,436);put(data+24+140*i,0);}
 assert(!mt6797_fs_archive_validate(data,436,&v));
 puts("PASS actual kernel archive validator: producer ABI, malformed bounds/paths/UTF8/duplicates, empty data, unchanged failure output");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-archive-validator-') as temp:
    p = Path(temp)
    (p/'fixture').write_bytes(archive_bytes(fixture['fixture'](), **fixture['PROFILE']))
    (p/'test.c').write_text(shim+header+test)
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),str(p/'fixture')],check=True,timeout=30)
