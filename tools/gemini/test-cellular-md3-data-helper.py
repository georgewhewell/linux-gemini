#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual one-shot four-root helper; all fault paths use typed syscall adapters."""
import ast,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
legacy=ROOT/'tools/gemini/test-cellular-fs-internal-helper.py'
tree=ast.parse(legacy.read_text());nodes=[]
for n in tree.body:
 if isinstance(n,ast.With):break
 nodes.append(n)
env={'__file__':str(legacy)};exec(compile(ast.Module(body=nodes,type_ignores=[]),str(legacy),'exec'),env)
shim=env['shim'];test=env['tests'];test=test[:test.index('static void exercise(void)')]
shim+='''
static int fake_mkdirat(int,const char*,mode_t),fake_fsync(int);
#define mkdirat fake_mkdirat
#define fsync fake_fsync
'''
test=test.replace('RAMMD,PROVIDER}', 'RAMMD,PROVIDER,PERSIST,DATAPARENT,DATAROOT}')
test=test.replace('static int nextfd,', 'static bool exists[2];static unsigned creations,flushes;\nstatic int nextfd,')
test=test.replace('parts[]={6,8,9}', 'parts[]={6,8,9,32}').replace('sizes[]={33554432,8388608,13074432}', 'sizes[]={33554432,8388608,13074432,UINT64_C(22249472)*512}').replace('labels[]={"nvdata","protect1","protect2"}', 'labels[]={"nvdata","protect1","protect2","userdata"}')
test=test.replace(' for(int i=0;i<3;i++){\n  char name[80];', ''' if(!strcmp(path,"/sys/class/block/mmcblk0p32/uevent"))return newfd(UEVENT,3);
 if(!strcmp(path,"/sys/class/block/mmcblk0p32/size"))return newfd(SIZE,3);
 for(int i=0;i<3;i++){
  char name[80];''',1)
test=test.replace('st->st_ino=f->kind==ROOT?2:', 'st->st_ino=(f->kind==ROOT||f->kind==PERSIST)?2:')
test=test.replace(' if(mode==4&&f->index==bad_source&&f->kind==ROOT)', ''' if(f->index==3){
  if(mode==20)st->st_dev=makedev(179,96);
  if(mode==21)st->st_uid=1000;
  if(mode==22&&f->kind!=PERSIST)st->st_mode=S_IFDIR|0770;
  if(mode==23&&f->kind==PERSIST)st->st_ino=12;
 }
 if(mode==4&&f->index==bad_source&&f->kind==ROOT)''')
test=test.replace('st->f_flag=0;\n if(mode==6', '''st->f_flag=0;
 if(f->index==3){st->f_frsize=4096;st->f_blocks=sizes[3]/4096;st->f_bavail=32768;st->f_files=1000000;st->f_ffree=900000;
  if(mode==24)st->f_flag=ST_RDONLY;
  if(mode==25)st->f_bavail=16384;
  if(mode==26)st->f_ffree=128;
 }
 if(mode==6''')
test=test.replace('if(base==AT_FDCWD){', 'if(base==AT_FDCWD){\n  if(!strcmp(path,"/persist"))return newfd(PERSIST,3);',1)
test=test.replace('struct fd *f=owned(base);assert(!strcmp', '''struct fd *f=owned(base);
 if(f->kind==PERSIST||f->kind==DATAPARENT){
  unsigned n=f->kind==PERSIST?0:1;assert(!strcmp(path,n?"md3-data":"gemini-cellular"));
  assert((how->resolve&(RESOLVE_BENEATH|RESOLVE_NO_XDEV))==(RESOLVE_BENEATH|RESOLVE_NO_XDEV));
  if(mode==27){errno=ELOOP;return -1;}if(!exists[n]){errno=ENOENT;return -1;}
  return newfd(n?DATAROOT:DATAPARENT,3);
 }
 assert(!strcmp''',1)
test=test.replace(' if(f->kind==BLOCK){assert(cmd==BLKGETSIZE64);', ''' if(cmd==MT6797_MD3_FS_REGISTER_ROOTS){
  assert(f->kind==PROVIDER);struct mt6797_md3_fs_roots*r=arg;
  assert(r->version==1&&!r->flags&&!r->reserved);
  for(unsigned i=0;i<4;i++){struct fd*d=owned(r->directory_fd[i]);assert(d->kind==(i==3?DATAROOT:MD)&&d->index==(int)i);}
  registered++;if(fault())return -1;if(mode==9){errno=EINTR;return -1;}return 0;
 }
 if(f->kind==BLOCK){assert(cmd==BLKGETSIZE64);''',1)
test+='''
static int fake_mkdirat(int fd,const char*name,mode_t permissions){
 struct fd*f=owned(fd);assert(permissions==0700);unsigned n=f->kind==PERSIST?0:1;
 assert((f->kind==PERSIST||f->kind==DATAPARENT)&&!strcmp(name,n?"md3-data":"gemini-cellular"));
 if(fault())return -1;if(exists[n]){errno=EEXIST;return -1;}exists[n]=true;creations++;return 0;
}
static int fake_fsync(int fd){struct fd*f=owned(fd);assert(f->kind==PERSIST||f->kind==DATAPARENT||f->kind==DATAROOT);flushes++;return fault()?-1:0;}
static void setup(int mode,int bad,int fail,bool present){reset(mode,bad,fail);exists[0]=exists[1]=present;creations=flushes=0;companion=true;}
int main(void){
 char*paths[]={"/rwZ","/rwX","/rwY"};unsigned cases=0;
 for(unsigned present=0;present<2;present++){
  setup(0,0,0,present);assert(!register_internal_roots_for(paths,2));assert(registered==1&&creations==(present?0U:2U)&&flushes==(present?0U:4U));drained();int total=calls;cases++;
  for(int n=1;n<=total;n++){setup(0,0,n,present);assert(register_internal_roots_for(paths,2)<0&&registered<=1);drained();cases++;}
 }
 for(int mode=20;mode<=27;mode++){setup(mode,3,0,true);assert(register_internal_roots_for(paths,2)<0&&!registered);drained();cases++;}
 setup(1,3,0,true);assert(register_internal_roots_for(paths,2)<0&&!registered);drained();cases++;
 setup(9,0,0,true);assert(register_internal_roots_for(paths,2)==-EINTR&&registered==1);drained();cases++;
 printf("PASS %u actual four-root helper cases: fixed p32 directory, create-only-absent+fsync, preserve-existing, typed permission/space/symlink errors, every syscall unwind, no ioctl retry\\n",cases);
}
'''
source=(ROOT/'tools/gemini/mt6797-fs-register.c').read_text()
source=source.replace('#include "../../include/uapi/linux/mt6797_md1_fs.h"','#include "'+str(ROOT/'include/uapi/linux/mt6797_md1_fs.h')+'"')
header=(ROOT/'tools/gemini/mt6797-fs-md3-data.h').read_text()
source=source.replace('#include "mt6797-fs-md3-data.h"',header)
variants={'current':source}
if '--negative-controls' in os.sys.argv:
 variants.update({
  'missing-private-permissions':source.replace('(st.st_mode & 07777) != 0700','0'),
  'missing-directory-sync':source.replace('if (!ret && made && (fsync(fd) || fsync(parent)))','if (!ret && made && 0 && (fsync(fd) || fsync(parent)))'),
  'leaked-fourth-fd':source.replace('if (md3.directory_fd[3] >= 0)','if (md3.directory_fd[3] == -999)'),
 })
with tempfile.TemporaryDirectory(prefix='md3-data-helper-') as tmp:
 p=Path(tmp)
 for name,code in variants.items():
  if name!='current':assert code!=source,name
  c=p/(name+'.c');exe=p/name;c.write_text(shim+code+test)
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(exe)],check=True)
  r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
  if name=='current':assert not r.returncode,r.stderr;print(r.stdout.strip())
  else:assert r.returncode,name;print('REJECTED',name)
