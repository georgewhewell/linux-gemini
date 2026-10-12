#!/usr/bin/env python3
"""Actual fixed-internal helper, with fault-injecting metadata/syscall adapters."""
import ast
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root/'tools/gemini/test-cellular-fs-ram-roots-helper.py').read_text())
pieces = {n.targets[0].id: ast.literal_eval(n.value) for n in tree.body
          if isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Name)
          and n.targets[0].id in ('shim', 'tests')}
shim = pieces['shim']
tests = pieces['tests']
tests = tests[:tests.index('int main(void)')]
tests = 'static bool companion;\n' + tests
tests = tests.replace('!strcmp(path,"md")', '!strcmp(path,companion?"md3":"md")')
tests = tests.replace('MT6797_MD1_FS_REGISTER_RAM_ROOTS', 'MT6797_MD1_FS_REGISTER_NV_ROOTS')
tests = tests.replace('struct mt6797_md1_fs_ram_roots *r', 'struct mt6797_md1_fs_nv_roots *r')
tests = tests.replace('dir->kind==RAMMD', 'dir->kind==MD')
tests = tests.replace('const char *names[]={"/sourceZ","/ramZ","/sourceX","/ramX","/sourceY","/ramY"};\n  for(int i=0;i<6;i++)if(!strcmp(path,names[i]))return newfd(i%2?RAM:ROOT,i/2);',
                      'const char *names[]={"/rwZ","/rwX","/rwY"};\n  for(int i=0;i<3;i++)if(!strcmp(path,names[i]))return newfd(ROOT,i);')
tests = tests.replace('b[24]=2;b[56]=0x53;b[57]=0xef;b[58]=mode==2&&f->index==bad_source?0:1;',
                      'b[24]=2;b[56]=0x53;b[57]=0xef;b[58]=mode==2?0:1;\n if(mode==2)b[96]=4;\n if(mode==11&&f->index==bad_source)b[58]=2;\n ')
tests = tests.replace('st->st_rdev=makedev(179,parts[f->index]);',
                      'st->st_rdev=makedev(179,mode==3&&f->index==bad_source?96:parts[f->index]);')
tests = tests.replace('st->f_blocks=16384;', 'st->f_blocks=sizes[f->index]/4096;')
tests = tests.replace('&&ram', '')
tests = tests.replace('st->f_blocks=16385;', 'st->f_blocks=sizes[f->index]/4096+1;')
tests = tests.replace('st->f_type=EXT4_SUPER_MAGIC;', 'st->f_type=TMPFS_MAGIC;')
tests = tests.replace('st->f_flag=f->kind==RAM||f->kind==RAMMD?0:ST_RDONLY;', 'st->f_flag=0;')
tests = tests.replace('st->f_flag=0;\n return 0;', 'st->f_flag=ST_RDONLY;\n return 0;')
tests = tests.replace('if(mode==5', 'if(mode==10&&f->index==bad_source)st->f_files=65537;\n if(mode==5')
tests = tests.replace('assert(super_reads==3);provider_opens++;', 'assert(mode>=12||super_reads==3);provider_opens++;')
tests = tests.replace('if(f->kind==BLOCK){assert(cmd==BLKGETSIZE64);', '''if(cmd==MT6797_MD1_FS_CANCEL){assert(f->kind==PROVIDER&&!arg);registered++;
 if(fault())return -1;
 if(mode==13){errno=EBUSY;return -1;}
 if(mode==14){errno=EINTR;return -1;}return 0;}
 if(f->kind==BLOCK){assert(cmd==BLKGETSIZE64);''')
tests = tests.replace('cmd==MT6797_MD1_FS_REGISTER_NV_ROOTS',
                      'cmd==(companion?MT6797_MD3_FS_REGISTER_NV_ROOTS:MT6797_MD1_FS_REGISTER_NV_ROOTS)')
tests += r'''
static void exercise(void){
 char *paths[]={"/rwZ","/rwX","/rwY"};
 reset(0,0,0);assert(!register_internal_roots_for(paths,companion)&&registered==1&&provider_opens==1);drained();int count=calls;
 for(int i=1;i<=count;i++){reset(0,0,i);assert(register_internal_roots_for(paths,companion)<0&&registered<=1);drained();}
 int bad_modes[]={1,3,4,5,6,7,8,10,11};
 for(unsigned m=0;m<sizeof(bad_modes)/sizeof(*bad_modes);m++)for(int bad=0;bad<3;bad++){
  reset(bad_modes[m],bad,0);assert(register_internal_roots_for(paths,companion)<0&&!registered&&!provider_opens);drained();
 }
 reset(2,0,0);assert(!register_internal_roots_for(paths,companion)&&registered==1);drained(); /* mounted ext4 can carry journal-recovery marker */
 reset(9,0,0);assert(register_internal_roots_for(paths,companion)==-EINTR&&registered==1);drained();
 char *tmp=paths[1];paths[1]=paths[2];paths[2]=tmp;
 reset(0,0,0);assert(register_internal_roots_for(paths,companion)<0&&!registered);drained();
 for(int i=0;i<3;i++){
  reset(2,0,0);assert(!source_authority_mode(&sources[i],NULL,0,NULL,NULL,1)&&!provider_opens&&super_reads==1);drained();
 }
 reset(12,0,0);assert(!cancel_authority()&&registered==1&&provider_opens==1&&!super_reads);drained();
 for(int i=1;i<=2;i++){reset(12,0,i);assert(cancel_authority()==-EIO&&registered<=1);drained();}
 reset(13,0,0);assert(cancel_authority()==-EBUSY&&registered==1);drained();
 reset(14,0,0);assert(cancel_authority()==-EINTR&&registered==1);drained();
 puts("PASS actual rescue cancellation: one ioctl, consumed EBUSY/error/EINTR refusal, FD cleanup, no block reads/writes or retry");
 puts("PASS actual internal helper: fixed devices/order/type/size, RW ext4 mount/root/md confinement, dirty journal admission, damaged-filesystem refusal, all fault unwinds, one ioctl including EINTR, no block writes or re-seed");
}
int main(void){
 companion=false;exercise();companion=true;exercise();
 puts("PASS MD3 helper: only existing md3 siblings, exact dedicated ioctl, no MD1 fallback, no creation or seeding");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-internal-nv-helper-') as directory:
    p = Path(directory)
    helper = root/'tools/gemini/mt6797-fs-register.c'
    (p/'test.c').write_text(shim+'\n#include "'+str(helper)+'"\n'+tests)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(p/'test.c'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True, timeout=10)
