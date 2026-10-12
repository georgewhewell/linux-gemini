#!/usr/bin/env python3
"""Actual optional MD3 authority: fixed siblings, atomic publication, isolation."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/soc/mediatek/mt6797-md1-fs-provider.c').read_text()
def function(name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
typedef uint32_t u32; typedef uint8_t u8; typedef uint64_t sector_t;
#define U32_MAX UINT32_MAX
#define EPROBE_DEFER 517
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define READ_ONCE(x) (x)
#define EXPORT_SYMBOL_GPL(x)
#define MAJOR(d) ((d)>>20)
#define MINOR(d) ((d)&0xfffff)
#define MKDEV(a,b) (((a)<<20)|(b))
#define MMC_BLOCK_MAJOR 179
#define MNT_READONLY 1
#define SB_RDONLY 1
#define FMODE_WRITE 2
#define TMPFS_MAGIC 0x01021994
#define EXT4_SUPER_MAGIC 0xef53
#define MT6797_MD1_FS_BACKING_INTERNAL 2U
#define MT6797_FS_MODEM_MD1 0U
#define MT6797_FS_MODEM_MD3 2U
struct block_device { unsigned bd_dev; sector_t sectors; };
struct super_block { unsigned s_flags, s_magic; struct block_device *s_bdev; struct dentry *s_root; };
struct inode { struct super_block *i_sb; };
struct dentry { bool dir, mountpoint; struct dentry *d_parent; struct { unsigned len; const char *name; } d_name; struct inode inode; int refs; };
struct vfsmount { unsigned mnt_flags; };
struct path { struct vfsmount *mnt; struct dentry *dentry; };
struct file { struct path f_path; unsigned f_mode; int refs; };
struct kstatfs { long f_type, f_bsize, f_frsize; uint64_t f_blocks, f_bfree, f_files; };
struct mt6797_md1_fs_stats {u32 block_size,blocks,free_blocks;};
static struct file original[3], candidate[3];
static struct vfsmount mounts[3], alien_mount;
static struct block_device devices[3];
static struct super_block super[3];
static struct dentry parents[3], md[3], md3[3], alien;
static struct file *provider_file, *provider_extra[2], *md3_roots[3];
static unsigned provider_volatile;
static bool spin, inode_held;
static int mode, stat_calls, checks;
static int mt6797_fs_md3_cancel(void);
#define spin_lock_irqsave(p,f) do { (f)=0; assert(!spin&&!inode_held); spin=true; } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(f); assert(spin); spin=false; } while(0)
static struct inode *file_inode(struct file *f) { return &f->f_path.dentry->inode; }
static struct inode *d_inode(struct dentry *d) { return &d->inode; }
static bool d_is_dir(struct dentry *d) { return d->dir; }
static bool d_mountpoint(struct dentry *d) { return d->mountpoint; }
static struct dentry *dget_parent(struct dentry *d) { assert(!spin); d->d_parent->refs++; return d->d_parent; }
static void dput(struct dentry *d) { assert(d&&d->refs>0&&!spin); d->refs--; }
static void inode_lock(struct inode *i) { (void)i; assert(!spin&&!inode_held); inode_held=true; }
static void inode_unlock(struct inode *i) { (void)i; assert(inode_held); inode_held=false; }
static sector_t bdev_nr_sectors(struct block_device *d) { return d->sectors; }
static struct file *get_file(struct file *f) { assert(spin&&f); f->refs++; return f; }
static struct file *fget(int fd) { assert(!spin); if(fd<20||fd>22)return NULL; candidate[fd-20].refs++; return &candidate[fd-20]; }
static void fput(struct file *f) { assert(!spin&&f&&f->refs>0); f->refs--; }
static int vfs_statfs(struct path *p, struct kstatfs *out) {
 assert(!spin&&!inode_held); stat_calls++;
 *out=(struct kstatfs){EXT4_SUPER_MAGIC,4096,4096,p->dentry->inode.i_sb->s_bdev->sectors/8,7,4096};
 if(stat_calls==3 && mode==1)assert(!mt6797_fs_md3_cancel());
 if(stat_calls==3 && mode==2)mounts[0].mnt_flags=MNT_READONLY;
 if(mode==3)out->f_files=65537;
 return mode==4?-EIO:0;
}
static int restore_calls;
static int mt6797_md1_fs_provider_restore_drive(u8 drive, const char *path) { assert(drive=='Z'&&!strcmp(path,"x")); restore_calls++; return 17; }
'''
state = (root / 'drivers/soc/mediatek/mt6797-md1-fs-state.h').read_text()
functions = ('mt6797_fs_readonly', 'mt6797_fs_tmpfs_writable', 'mt6797_fs_internal_writable',
             'mt6797_fs_backing_matches', 'mt6797_fs_accessible', 'mt6797_fs_roots_accessible',
             'mt6797_fs_drive_file', 'mt6797_fs_root_get', 'mt6797_fs_root_get_for',
             'mt6797_fs_validate_root', 'mt6797_fs_validate_md3_root',
             'mt6797_fs_md3_roots_accessible', 'mt6797_md3_fs_provider_claim',
             'mt6797_fs_md3_cancel', 'mt6797_fs_md3_register', 'mt6797_fs_provider_restore_drive')
tests = r'''
static const int fds[3]={20,21,22};
static void reset(void) {
 assert(!spin&&!inode_held);
 memset(md3_roots,0,sizeof(md3_roots)); stat_calls=mode=restore_calls=0;
 provider_volatile=2; provider_state=(struct mt6797_fs_state){.stage=MT6797_FS_CONSUMED}; md3_state=(struct mt6797_fs_state){0};
 for(int i=0;i<3;i++){
  devices[i]=(struct block_device){MKDEV(179,i==0?6:i==1?8:9),i==0?65536:i==1?16384:25536};
  mounts[i]=(struct vfsmount){0}; super[i]=(struct super_block){0,EXT4_SUPER_MAGIC,&devices[i],&parents[i]};
  parents[i]=(struct dentry){.dir=true,.d_parent=&parents[i],.inode={&super[i]}};
  md[i]=(struct dentry){.dir=true,.d_parent=&parents[i],.d_name={2,"md"},.inode={&super[i]}};
  md3[i]=(struct dentry){.dir=true,.d_parent=&parents[i],.d_name={3,"md3"},.inode={&super[i]}};
  original[i]=(struct file){.f_path={&mounts[i],&md[i]},.refs=1};
  candidate[i]=(struct file){.f_path={&mounts[i],&md3[i]}};
 }
 provider_file=&original[0]; provider_extra[0]=&original[1]; provider_extra[1]=&original[2];
 alien=(struct dentry){.dir=true,.d_parent=&alien,.inode={&super[0]}}; alien_mount=(struct vfsmount){0};
}
static void balanced(bool published) {
 assert(!spin&&!inode_held);
 for(int i=0;i<3;i++) { assert(original[i].refs==1); assert(candidate[i].refs==(published?1:0)); assert(parents[i].refs==0); assert(md3_roots[i]==(published?&candidate[i]:NULL)); }
 assert(!alien.refs); assert(provider_state.stage==MT6797_FS_CONSUMED); checks++;
}
static void failure(void) { assert(mt6797_fs_md3_register(fds)<0); balanced(false); assert(md3_state.stage==MT6797_FS_CANCELLED); assert(mt6797_md3_fs_provider_claim()<0); }
int main(void) {
 reset(); assert(!mt6797_fs_md3_register(fds)); balanced(true);
 assert(mt6797_fs_root_get_for(2,'Z')==NULL); /* published is not consumed */
 assert(mt6797_fs_md3_register(fds)==-EALREADY); balanced(true);
 assert(!mt6797_md3_fs_provider_claim()); assert(mt6797_md3_fs_provider_claim()==-EALREADY);
 assert(mt6797_fs_md3_cancel()==-EBUSY); balanced(true);
 for(int i=0;i<3;i++) { u8 drive="ZXY"[i]; struct file *a=mt6797_fs_root_get_for(0,drive),*b=mt6797_fs_root_get_for(2,drive); assert(a==&original[i]&&b==&candidate[i]&&a!=b); fput(a); fput(b); }
 for(int mdid=0;mdid<4;mdid++)for(int drive=0;drive<256;drive++)if((mdid!=0&&mdid!=2)||(drive!='Z'&&drive!='X'&&drive!='Y'))assert(!mt6797_fs_root_get_for(mdid,drive));
 balanced(true);
 assert(mt6797_fs_provider_restore_drive(2,'Z',"x")==-EOPNOTSUPP&&!restore_calls);
 assert(mt6797_fs_provider_restore_drive(0,'Z',"x")==17&&restore_calls==1);
 mounts[1].mnt_flags=MNT_READONLY; assert(!mt6797_fs_root_get_for(2,'X')); assert(mt6797_md3_fs_provider_claim()==-ENODEV);
 reset(); assert(!mt6797_fs_md3_register(fds)); assert(!mt6797_fs_md3_cancel()); balanced(false); assert(mt6797_fs_md3_register(fds)==-EALREADY);
 for(int scenario=0;scenario<17;scenario++) {
  reset();
  if(scenario==0)candidate[1].f_path.mnt=&alien_mount;
  if(scenario==1)md3[1].inode.i_sb=&super[0];
  if(scenario==2)md3[1].d_parent=&alien;
  if(scenario==3)md3[1].d_name.name="mdx";
  if(scenario==4)md3[1].d_name.len=2;
  if(scenario==5)md[1].d_name.name="xx";
  if(scenario==6)md[1].d_parent=&alien;
  if(scenario==7)md3[1].mountpoint=true;
  if(scenario==8)md3[1].dir=false;
  if(scenario==9)candidate[1].f_mode=FMODE_WRITE;
  if(scenario==10)devices[1].bd_dev=MKDEV(179,96); /* SD rejected before any VFS use */
  if(scenario==11)devices[1].bd_dev=MKDEV(179,32);
  if(scenario==12)devices[1].sectors++;
  if(scenario==13)super[1].s_magic=TMPFS_MAGIC;
  if(scenario==14)mounts[1].mnt_flags=MNT_READONLY;
  if(scenario==15)mode=3;
  if(scenario==16)mode=4;
  /* Wrong original backing is rejected without starting registration. */
  if(scenario>=10&&scenario<=14){assert(mt6797_fs_md3_register(fds)==-ENODEV);balanced(false);assert(md3_state.stage==MT6797_FS_EMPTY);}
  else failure();
 }
 for(int i=0;i<3;i++){reset();int bad[3]={20,21,22};bad[i]=-1;assert(mt6797_fs_md3_register(bad)==-EBADF);balanced(false);}
 reset();int swapped[3]={21,20,22};assert(mt6797_fs_md3_register(swapped)==-EXDEV);balanced(false);
 reset();int aliased[3]={20,20,22};assert(mt6797_fs_md3_register(aliased)==-EXDEV);balanced(false);
 for(int i=0;i<2;i++){reset();provider_volatile=i;assert(mt6797_fs_md3_register(fds)==-ENODEV);assert(md3_state.stage==MT6797_FS_EMPTY);balanced(false);}
 reset();provider_extra[1]=NULL;assert(mt6797_fs_md3_register(fds)==-ENODEV);balanced(false);
 reset();provider_state.stage=MT6797_FS_READY;assert(mt6797_fs_md3_register(fds)==-ENODEV);assert(md3_state.stage==MT6797_FS_EMPTY);provider_state.stage=MT6797_FS_CONSUMED;balanced(false);
 reset();mode=1;assert(mt6797_fs_md3_register(fds)==-ECANCELED);balanced(false);assert(md3_state.stage==MT6797_FS_CANCELLED);
 reset();mode=2;assert(mt6797_fs_md3_register(fds)==-EROFS);balanced(false);assert(md3_state.stage==MT6797_FS_CANCELLED);
 printf("PASS MD3 actual authority: %d balanced scenarios, fixed sibling/partition isolation, publication/cancel/claim, remount, no MD1 archive fallback\n",checks);
}
'''
parts = [shim, state, 'static struct mt6797_fs_state provider_state, md3_state;']
parts += [function(n) for n in functions]
parts += [tests]
program = '\n'.join(parts)
mutations = {
    'wrong-sibling-name': ('!memcmp(file->f_path.dentry->d_name.name, "md3", 3)', 'true'),
    'cross-mount-authority': ('file->f_path.mnt != md1->f_path.mnt ||', 'false ||'),
    'ready-without-claim': ('if (md3_state.stage == MT6797_FS_CONSUMED &&', 'if (md3_state.stage >= MT6797_FS_READY &&'),
    'MD1-archive-fallback': ('if (modem != MT6797_FS_MODEM_MD1)\n\t\treturn -EOPNOTSUPP;', 'if (false)\n\t\treturn -EOPNOTSUPP;'),
    'cancel-publication-race': ('if (s->stage != MT6797_FS_INSTALLING)\n\t\treturn -ECANCELED;', 'if (false)\n\t\treturn -ECANCELED;'),
}
with tempfile.TemporaryDirectory(prefix='gemini-md3-fs-') as td:
    p = Path(td)
    for name, mutation in [('actual', None), *mutations.items()]:
        code = program
        if mutation:
            old, new = mutation
            assert code.count(old) == 1, name
            code = code.replace(old, new)
        (p/'test.c').write_text(code)
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',
                        '-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
        result = subprocess.run([str(p/'test')],capture_output=True,text=True,timeout=10)
        if mutation:
            assert result.returncode != 0, f'negative control survived: {name}'
            print('REJECTED', name)
        else:
            if result.returncode: print(result.stderr)
            result.check_returncode()
            print(result.stdout,end='')
