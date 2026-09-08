#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual archive registration with instrumented kernel boundaries."""
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
from nvram_restore_archive import archive_bytes
root=Path(__file__).resolve().parents[2]
source=(root/'drivers/soc/mediatek/mt6797-md1-fs-provider.c').read_text()
start=source.index('static int mt6797_fs_register_archive(')
end=source.index('\nstatic int mt6797_fs_register_fds(',start)
function=source[start:end]
header=(root/'drivers/soc/mediatek/mt6797-md1-fs-archive.h').read_text()
shim=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint32_t u32;
#define F_SEAL_WRITE 8
#define F_SEAL_GROW 4
#define F_SEAL_SHRINK 2
#define F_SEAL_SEAL 1
#define FMODE_READ 1
#define TMPFS_MAGIC 0x01021994
#define GFP_KERNEL 0
#define S_ISREG(m) (((m)&0170000)==0100000)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define spin_lock_irqsave(l,f) do{assert(!locked);locked=true;(f)=0;}while(0)
#define spin_unlock_irqrestore(l,f) do{assert(locked);locked=false;(void)(f);}while(0)
static bool locked;
static void cond_resched(void){assert(!locked);}
struct super_block {unsigned long s_magic;};
struct inode {unsigned i_mode;struct super_block *i_sb;loff_t size;};
struct file {struct inode inode;int f_mode;};
#define file_inode(f) (&(f)->inode)
static loff_t i_size_read(struct inode *i){return i->size;}
static struct super_block sb;
static struct file file;
static bool enable_volatile,archive_installing;
enum {MT6797_FS_EMPTY,MT6797_FS_INSTALLING,MT6797_FS_READY,MT6797_FS_CONSUMED,MT6797_FS_CANCELLED};
static struct {int stage;} provider_state;
static int seals, refs, allocations, mode;
static u8 input[448];
static struct file *fget(int fd){assert(!locked);if(fd!=7)return NULL;refs++;return &file;}
static void fput(struct file *f){assert(f==&file&&!locked);refs--;}
static int memfd_get_seals(struct file *f){assert(f==&file);return seals;}
static void *kvmalloc(size_t size,int flags){(void)flags;assert(!locked);if(mode==1)return NULL;allocations++;return malloc(size);}
static void kvfree_sensitive(const void *p,size_t size){if(p){assert(!locked);memset((void *)p,0,size);free((void *)p);allocations--;}}
static long kernel_read(struct file *f,void *p,size_t n,loff_t *pos){
 assert(f==&file&&!locked&&n<=65536);
 if(mode==2)return -ENOSPC;if(mode==3)return 0;
 if(mode==4)provider_state.stage=MT6797_FS_CANCELLED;
 if(n>3)n=3;if(n>(size_t)(448-*pos))n=448-*pos;memcpy(p,input+*pos,n);*pos+=n;return n;
}
'''
state='\nstatic struct mt6797_fs_archive provider_archive;\n'
tests=r'''
static void reset(void){
 assert(!refs&&!archive_installing);kvfree_sensitive(provider_archive.data,provider_archive.size);
 provider_archive=(struct mt6797_fs_archive){0};assert(!allocations);
 provider_state.stage=MT6797_FS_EMPTY;enable_volatile=true;seals=15;mode=0;
 sb.s_magic=TMPFS_MAGIC;file=(struct file){{0100600,&sb,448},FMODE_READ};
}
static void failed(int fd,int expected){assert(mt6797_fs_register_archive(fd)==expected);assert(!provider_archive.data&&!refs&&!allocations&&!archive_installing);}
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f&&fread(input,1,448,f)==448);fclose(f);
 reset();assert(!mt6797_fs_register_archive(7));assert(provider_archive.count==3&&provider_archive.size==448&&!refs&&!archive_installing&&allocations==1);
 assert(mt6797_fs_register_archive(7)==-EALREADY);reset();
 enable_volatile=false;failed(7,-EPERM);reset();failed(8,-EBADF);
 for(int bit=1;bit<=8;bit*=2){reset();seals=15^bit;failed(7,-EPERM);}
 reset();seals=-EINVAL;failed(7,-EPERM);
 reset();sb.s_magic=0;failed(7,-EINVAL);reset();file.inode.i_mode=0040700;failed(7,-EINVAL);
 reset();file.f_mode=0;failed(7,-EINVAL);
 reset();file.inode.size=15;failed(7,-EFBIG);reset();file.inode.size=16777217;failed(7,-EFBIG);
 for(int m=1;m<=4;m++){reset();mode=m;failed(7,m==1?-ENOMEM:m==2?-ENOSPC:m==3?-EIO:-ECANCELED);}
 reset();input[0]^=1;failed(7,-EINVAL);input[0]^=1;
 for(int s=1;s<=4;s++){reset();provider_state.stage=s;failed(7,-EALREADY);}
 reset();archive_installing=true;assert(mt6797_fs_register_archive(7)==-EBUSY&&archive_installing);archive_installing=false;
 reset();puts("PASS actual sealed archive registration: policy, size/seals, short reads, errors, cancellation, duplicate/state gates and resource cleanup");
}
'''
fixture=runpy.run_path(str(Path(__file__).with_name('test-nvram-restore-archive.py')))
with tempfile.TemporaryDirectory(prefix='gemini-archive-registration-') as temp:
 p=Path(temp);(p/'fixture').write_bytes(archive_bytes(fixture['fixture'](),**fixture['PROFILE']))
 (p/'test.c').write_text(shim+header+state+function+tests)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'fixture')],check=True,timeout=30)
