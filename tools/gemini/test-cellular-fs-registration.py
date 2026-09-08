#!/usr/bin/env python3
"""Execute actual provider registration/claim/cancel with FD/VFS adapters."""
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'drivers/soc/mediatek/mt6797-md1-fs-provider.c').read_text()
def function(name):
    start=source.index(name+'(');start=source.rfind('\n',0,start)+1
    brace=source.index('{',start);end=brace+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+'\n'
shim=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define __user
static int init_user_ns;
#define CAP_SYS_ADMIN 21
static bool admin=true;
static bool ns_capable(void *ns,int cap){assert(ns==&init_user_ns&&cap==21);return admin;}
static int copy_from_user(void *dst,const void *src,size_t n){if(!src)return 1;memcpy(dst,src,n);return 0;}
#define U32_MAX UINT32_MAX
#define EPROBE_DEFER 517
#define MNT_READONLY 1
#define SB_RDONLY 1
#define FMODE_WRITE 2
#define TMPFS_MAGIC 0x01021994
#define EXT4_SUPER_MAGIC 0xef53
#define READ_ONCE(v) (v)
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((intptr_t)(p))
#define dev_warn(...) ((void)0)
struct kstatfs {long f_bsize;uint64_t f_blocks,f_bfree;long f_frsize,f_type;uint64_t f_files;};
struct mt6797_md1_fs_stats {u32 block_size,blocks,free_blocks;};
struct vfsmount {int mnt_flags;};struct super_block {int s_flags,s_magic;};
struct inode {struct super_block *i_sb;};struct dentry {bool dir;};
struct path {struct vfsmount *mnt;struct dentry *dentry;};
struct file {struct path f_path;int f_mode,refs;struct inode inode;};
struct device {int refs;};
static bool enable=true,locked,enable_volatile,provider_volatile;
static int mode,attaches,stats_calls,file_puts,unlock_count;
static struct device target;
static struct vfsmount mount;
static struct super_block sb;
static struct dentry dent;
static struct file backing;
static struct kstatfs observed;
static struct file *provider_file;
static struct device *provider_cold;
static struct mt6797_md1_fs_stats provider_stats;
static void after_unlock(void);
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!locked);locked=true;}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);assert(locked);locked=false;after_unlock();}while(0)
static struct inode *file_inode(struct file *f){return &f->inode;}
static bool d_is_dir(struct dentry *d){return d->dir;}
static struct file *fget(int fd){assert(!locked);if(fd!=7)return NULL;backing.refs++;return &backing;}
static void fput(struct file *f){assert(!locked&&f&&f->refs>0);f->refs--;file_puts++;}
static struct device *get_device(struct device *d){assert(d);d->refs++;return d;}
static void put_device(struct device *d){assert(!locked);if(d){assert(d->refs>0);d->refs--;}}
static int vfs_statfs(struct path *p,struct kstatfs *out)
{(void)p;assert(!locked);stats_calls++;*out=observed;return mode==1?-EIO:0;}
static struct device *mt6797_fs_find_cold(void){assert(!locked);return get_device(&target);}
static int device_attach(struct device *d);
'''
adapter=r'''
static struct mt6797_fs_state provider_state;
'''
tests=r'''
static void after_unlock(void){
 unlock_count++;
 /* Publication is visible, but register has not evaluated target re-attach. */
 if(provider_state.stage==MT6797_FS_READY&&provider_file){
  int old=mode;
  if(old==10||old==11){mode=0;
   if(old==10)assert(!mt6797_md1_fs_provider_claim(&target));
   else assert(!mt6797_fs_cancel_registration());
  }
 }
}
static int device_attach(struct device *d){
 assert(!locked&&d==&target);attaches++;
 if(mode==12){assert(!mt6797_fs_cancel_registration());assert(mt6797_md1_fs_provider_require(d)<0);return 0;}
 if(mode==13)return -EIO;
 assert(!mt6797_md1_fs_provider_require(d));assert(!mt6797_md1_fs_provider_claim(d));return 1;
}
static void reset(int m){
 assert(!locked);enable_volatile=false;provider_volatile=false;mode=m;attaches=stats_calls=file_puts=unlock_count=0;
 mount=(struct vfsmount){MNT_READONLY};sb=(struct super_block){SB_RDONLY,EXT4_SUPER_MAGIC};dent.dir=true;
 backing=(struct file){.f_path={&mount,&dent},.inode={&sb}};target.refs=0;
 observed=(struct kstatfs){4096,12345,6789,4096,EXT4_SUPER_MAGIC,4096};
 provider_state=(struct mt6797_fs_state){0};provider_file=NULL;provider_cold=NULL;
}
static void fail_check(int expected){
 assert(mt6797_fs_register_fd(7,false)==expected);assert(!backing.refs&&!target.refs&&!attaches);
 assert(provider_state.stage==MT6797_FS_CANCELLED&&mt6797_fs_register_fd(7,false)==-EALREADY);
}
int main(void){
 assert(mt6797_md1_fs_provider_enabled());
 struct mt6797_md1_fs_stats stats;
 reset(0);assert(!mt6797_fs_register_fd(7,false));
 assert(attaches==1&&stats_calls==1&&backing.refs==1&&target.refs==1);
 assert(!mt6797_md1_fs_provider_stats(&stats)&&stats.free_blocks==6789);
 assert(mt6797_fs_cancel_registration()==-EBUSY&&mt6797_fs_register_fd(7,false)==-EALREADY);
 assert(backing.refs==1&&target.refs==1&&mt6797_md1_fs_provider_claim(&target)==-EALREADY);
 mount.mnt_flags=0;assert(mt6797_md1_fs_provider_stats(&stats)==-ENODEV);
 reset(0);assert(mt6797_fs_register_fd(-1,false)==-EBADF&&!backing.refs&&!target.refs);
 assert(mt6797_fs_register_fd(7,false)==-EALREADY);
 reset(0);dent.dir=false;fail_check(-ENOTDIR);
 reset(0);mount.mnt_flags=0;fail_check(-EROFS);
 reset(0);sb.s_flags=0;fail_check(-EROFS);
 reset(0);backing.f_mode=FMODE_WRITE;fail_check(-EROFS);
 reset(0);sb.s_magic=0;fail_check(-EINVAL);
 reset(0);observed.f_type=0;fail_check(-EINVAL);
 reset(1);fail_check(-EIO);
 reset(0);observed.f_blocks=UINT64_MAX;fail_check(-EOVERFLOW);
 reset(10);assert(!mt6797_fs_register_fd(7,false));
 assert(!attaches&&provider_state.stage==MT6797_FS_CONSUMED&&backing.refs==1&&target.refs==1);
 reset(11);assert(!mt6797_fs_register_fd(7,false));
 assert(!attaches&&provider_state.stage==MT6797_FS_CANCELLED&&!backing.refs&&!target.refs);
 reset(12);assert(!mt6797_fs_register_fd(7,false));
 assert(attaches==1&&provider_state.stage==MT6797_FS_CANCELLED&&!backing.refs&&!target.refs);
 reset(13);assert(!mt6797_fs_register_fd(7,false));
 assert(attaches==1&&provider_state.stage==MT6797_FS_READY&&backing.refs==1&&target.refs==1);
 assert(!mt6797_fs_cancel_registration()&&!backing.refs&&!target.refs);

 /* The new authority cannot turn writable ext4 into an accepted backing. */
 reset(0);assert(mt6797_fs_register_fd(7,true)==-EPERM&&!backing.refs&&!target.refs);
 reset(0);enable_volatile=true;mount.mnt_flags=0;sb.s_flags=0;
 assert(mt6797_fs_register_fd(7,true)==-EROFS&&!backing.refs&&!target.refs);
 reset(0);enable_volatile=true;sb.s_magic=TMPFS_MAGIC;observed.f_type=TMPFS_MAGIC;
 assert(mt6797_fs_register_fd(7,true)==-EROFS&&!backing.refs&&!target.refs);
 for(int failure=0;failure<7;failure++){
   reset(0);enable_volatile=true;mount.mnt_flags=0;sb.s_flags=0;sb.s_magic=TMPFS_MAGIC;
   observed.f_type=TMPFS_MAGIC;observed.f_blocks=16384;observed.f_files=4096;
   if(failure==0)observed.f_blocks=16385;
   if(failure==1)observed.f_blocks=0;
   if(failure==2)observed.f_files=4097;
   if(failure==3)observed.f_files=0;
   if(failure==4)observed.f_bsize=0;
   if(failure==5)observed.f_bsize=-1;
   if(failure==6)observed.f_blocks=UINT64_MAX;
   assert(mt6797_fs_register_fd(7,true)==-EFBIG&&!backing.refs&&!target.refs);
 }
 reset(0);enable_volatile=true;mount.mnt_flags=0;sb.s_flags=0;sb.s_magic=TMPFS_MAGIC;
 observed.f_type=TMPFS_MAGIC;observed.f_blocks=16384;observed.f_files=4096;
 assert(!mt6797_fs_register_fd(7,true)&&provider_volatile);
 assert(attaches==1&&backing.refs==1&&target.refs==1);
 assert(!mt6797_md1_fs_provider_stats(&stats)&&stats.blocks==16384);
 assert(mt6797_fs_cancel_registration()==-EBUSY);
 sb.s_magic=EXT4_SUPER_MAGIC;assert(mt6797_md1_fs_provider_stats(&stats)==-ENODEV);
 puts("PASS RAM authority: opt-in required, writable ext4 refused, read-only tmpfs refused, zero/oversized capacities refused, bounded tmpfs consumed, backend change invalidates access");

 /* Execute the real ioctl dispatcher: distinct commands cannot weaken disk policy. */
 struct mt6797_md1_fs_registration reg={.version=1,.directory_fd=7};
 reset(0);admin=false;assert(mt6797_fs_ioctl(NULL,MT6797_MD1_FS_REGISTER,(unsigned long)&reg)==-EPERM);admin=true;
 assert(mt6797_fs_ioctl(NULL,1234,(unsigned long)&reg)==-ENOTTY);
 assert(mt6797_fs_ioctl(NULL,MT6797_MD1_FS_REGISTER,0)==-EFAULT);
 reg.flags=1;assert(mt6797_fs_ioctl(NULL,MT6797_MD1_FS_REGISTER_RAM,(unsigned long)&reg)==-EINVAL);reg.flags=0;
 reg.reserved=1;assert(mt6797_fs_ioctl(NULL,MT6797_MD1_FS_REGISTER_RAM,(unsigned long)&reg)==-EINVAL);reg.reserved=0;
 assert(mt6797_fs_ioctl(NULL,MT6797_MD1_FS_REGISTER_RAM,(unsigned long)&reg)==-EPERM);
 assert(!mt6797_fs_ioctl(NULL,MT6797_MD1_FS_REGISTER,(unsigned long)&reg)&&!provider_volatile);
 reset(0);enable_volatile=true;mount.mnt_flags=0;sb.s_flags=0;
 assert(mt6797_fs_ioctl(NULL,MT6797_MD1_FS_REGISTER,(unsigned long)&reg)==-EROFS);
 reset(0);enable_volatile=true;mount.mnt_flags=0;sb.s_flags=0;sb.s_magic=TMPFS_MAGIC;
 observed.f_type=TMPFS_MAGIC;observed.f_blocks=16384;observed.f_files=4096;
 assert(!mt6797_fs_ioctl(NULL,MT6797_MD1_FS_REGISTER_RAM,(unsigned long)&reg)&&provider_volatile);
 puts("PASS actual ioctl: privilege/unknown/copy/shape/opt-in refusals, distinct RO and RAM commands");
 puts("PASS actual FS registration: FD/VFS failures, mount+superblock RO, returned type, refs, single publication, concurrent claim/cancel before attach, cancellation at attach, consumed lifetime");
}
'''
parts=[shim,(root/'include/uapi/linux/mt6797_md1_fs.h').read_text(),(root/'drivers/soc/mediatek/mt6797-md1-fs-state.h').read_text(),adapter]
parts += [function(n) for n in ('mt6797_md1_fs_provider_enabled','mt6797_fs_readonly','mt6797_fs_tmpfs_writable','mt6797_fs_backing_matches','mt6797_fs_accessible',
          'mt6797_md1_fs_provider_require','mt6797_md1_fs_provider_claim',
          'mt6797_md1_fs_provider_stats','mt6797_fs_cancel_registration','mt6797_fs_register_fd','mt6797_fs_ioctl')]
parts += [tests]
with tempfile.TemporaryDirectory(prefix='gemini-fs-registration-') as d:
    p=Path(d);(p/'test.c').write_text('\n'.join(parts))
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
