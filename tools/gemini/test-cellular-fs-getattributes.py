#!/usr/bin/env python3
"""Actual provider VFS layer with reference-counted metadata-only adapters.

Exercises the confined walk (getattr/open), reads, seeks, directory
enumeration and close against a fake dentry tree: no real filesystem.
"""
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
walk_struct = source[source.index('struct mt6797_fs_dir_walk {'):source.index('static bool mt6797_fs_dir_actor(')]
provider = (root / 'include/linux/soc/mediatek/mt6797-md1-fs-provider.h').read_text()
provider = provider[provider.index('#define MT6797_MD1_FS_PATH_MAX'):provider.index('#if IS_ENABLED')]
shim = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64;
typedef int64_t loff_t; typedef long ssize_t;
#define TMPFS_MAGIC 0x01021994
#define MNT_READONLY 1
#define SB_RDONLY 1
#define MT6797_FS_CONSUMED 3
#define NAME_MAX 255
#define READ_ONCE(v) (v)
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define PTR_ERR(p) ((long)(p))
#define ERR_PTR(e) ((void *)(long)(e))
#define ERR_CAST(p) ((void *)(p))
#define QSTR_INIT(n,l) {n,l}
#define STATX_MODE 2
#define STATX_SIZE 0x200
#define STATX_ATIME 0x20
#define STATX_MTIME 0x40
#define AT_STATX_DONT_SYNC 0x4000
#define O_RDONLY 0
#define O_RDWR 2
#define FMODE_WRITE 2
#define GFP_KERNEL 0
#define O_LARGEFILE 0100000
#define O_DIRECTORY 0200000
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct timespec64 {s64 tv_sec; long tv_nsec;};
struct super_block {int s_flags;int s_magic;};
struct inode {struct super_block *i_sb;};
struct dentry {struct super_block *d_sb; const char *name; int kind; int refs; bool mounted; struct dentry *parent; const char *data; u64 size;};
struct vfsmount {int mnt_flags;};
struct path {struct vfsmount *mnt; struct dentry *dentry;};
struct file {struct path f_path; struct inode inode; loff_t f_pos; int refs; int flags; int f_mode;};
struct kstat {unsigned mode; u64 size; struct timespec64 atime, mtime;};
struct qstr {const char *name; size_t len;};
struct dir_context; typedef bool (*filldir_t)(struct dir_context *, const char *, int, loff_t, u64, unsigned);
struct dir_context {filldir_t actor; loff_t pos;};
enum {DT_REG = 8, DT_DIR = 4, DT_LNK = 10};
static struct {int stage;} provider_state;
static struct super_block sb, other;
static struct vfsmount mount;
static struct file backing, *provider_file, *provider_extra[2];
static bool provider_volatile;
static struct dentry rootd = {&sb, "", 2, 1, false, NULL, NULL, 4096};
static struct dentry nvram = {&sb, "NVRAM", 2, 0, false, &rootd, NULL, 4096};
static struct dentry filea = {&sb, "a.bin", 1, 0, false, &nvram, "alphaomega", 10};
static struct dentry link = {&sb, "link", 3, 0, false, &nvram, NULL, 0};
static struct dentry special = {&sb, "sock", 4, 0, false, &nvram, NULL, 0};
static struct dentry covered = {&sb, "mnt", 2, 0, true, &nvram, NULL, 4096};
static struct dentry foreign = {&other, "foreign", 2, 0, false, &nvram, NULL, 4096};
static struct dentry topfile = {&sb, "top.txt", 1, 0, false, &rootd, "readonly", 8};
static struct dentry *all[] = {&rootd, &nvram, &filea, &link, &special, &covered, &foreign, &topfile};
static int lookups, stats, sleeps, opens, reads, iterations, rescheds, drop_ro_after_lookup, drop_ro_after_open, fail_stat, fail_open;
static bool locked;
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!locked);locked=true;}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);assert(locked);locked=false;}while(0)
static void might_sleep(void){assert(!locked);sleeps++;}
static void cond_resched(void){assert(!locked);rescheds++;}
static struct inode *file_inode(const struct file *f){return (struct inode *)&f->inode;}
static struct file *get_file(struct file *f){assert(locked);f->refs++;return f;}
static void fput(struct file *f){assert(!locked&&f->refs>0);f->refs--;if(f!=&backing&&!f->refs){f->f_path.dentry->refs--;free(f);}}
static struct dentry *dget(struct dentry *d){d->refs++;return d;}
static void dput(struct dentry *d){assert(!locked&&d->refs>0);d->refs--;}
static bool d_mountpoint(struct dentry *d){return d->mounted;}
static bool d_is_symlink(struct dentry *d){return d->kind==3;}
static bool d_is_reg(struct dentry *d){return d->kind==1;}
static bool d_is_dir(struct dentry *d){return d->kind==2;}
static void *mnt_idmap(struct vfsmount *m){assert(m==&mount);return NULL;}
static const void *current_cred(void){return NULL;}
static struct dentry *lookup_one_positive_unlocked(void *id,struct qstr *q,struct dentry *base){
 assert(!locked&&!id&&base->refs>0);lookups++;
 /* qstr components point into the caller's path and are not NUL terminated. */
 assert(q->len&&q->len<=NAME_MAX&&!memchr(q->name,'/',q->len)&&(!q->name[q->len]||q->name[q->len]=='/'));
 for(unsigned i=0;i<sizeof(all)/sizeof(*all);i++)if(all[i]->parent==base&&strlen(all[i]->name)==q->len&&!memcmp(all[i]->name,q->name,q->len)){
  all[i]->refs++;if(drop_ro_after_lookup){mount.mnt_flags=0;}return all[i];}
 return ERR_PTR(-ENOENT);
}
static int vfs_getattr(struct path *p,struct kstat *s,int mask,int flags){
 assert(!locked&&p->mnt==&mount&&mask==(STATX_MODE|STATX_SIZE|STATX_ATIME|STATX_MTIME)&&flags==AT_STATX_DONT_SYNC);stats++;
 if(fail_stat)return -EIO;
 s->mode=p->dentry->kind==2?0040755:p->dentry->kind==1?0100644:0120777;s->size=p->dentry->size;
 s->atime.tv_sec=1000+p->dentry->size;s->mtime.tv_sec=2000+p->dentry->size;return 0;
}
static struct file *dentry_open(const struct path *p,int flags,const void *cred){
 assert(!locked&&!cred&&p->mnt==&mount&&p->dentry->refs>0);opens++;
 if(fail_open)return ERR_PTR(-ENOMEM);
 struct file *f=calloc(1,sizeof(*f));f->f_path=*p;f->inode.i_sb=p->dentry->d_sb;f->refs=1;f->flags=flags;f->f_mode=(flags&O_RDWR)?FMODE_WRITE:0;p->dentry->refs++;
 if(drop_ro_after_open)sb.s_flags=0;
 return f;
}
static ssize_t kernel_read(struct file *f,void *buf,size_t n,loff_t *pos){
 assert(!locked&&f->f_path.dentry->kind==1);reads++;const struct dentry *d=f->f_path.dentry;
 if(*pos>=(loff_t)d->size)return 0;
 if(n>d->size-*pos)n=d->size-*pos;
 memcpy(buf,d->data+*pos,n);*pos+=n;return n;
}
static loff_t vfs_llseek(struct file *f,loff_t off,int whence){
 loff_t base=whence==SEEK_SET?0:whence==SEEK_CUR?f->f_pos:(loff_t)f->f_path.dentry->size;
 if(base+off<0)return -EINVAL;
 f->f_pos=base+off;return f->f_pos;
}
static int iterate_dir(struct file *f,struct dir_context *ctx){
 assert(!locked&&f->f_path.dentry->kind==2);iterations++;struct dentry *base=f->f_path.dentry;
 const char *dots[]={".",".."};loff_t pos=ctx->pos;
 /* Like ext4: a refused emission leaves the position on that entry. */
 for(;pos<2;pos++){if(!ctx->actor(ctx,dots[pos],(int)strlen(dots[pos]),pos,1,DT_DIR)){goto out;}}
 for(;pos-2<(loff_t)(sizeof(all)/sizeof(*all));pos++){struct dentry *d=all[pos-2];if(d->parent!=base)continue;
  if(!ctx->actor(ctx,d->name,(int)strlen(d->name),pos,pos,d->kind==2?DT_DIR:d->kind==1?DT_REG:DT_LNK))goto out;}
out: ctx->pos=pos;f->f_pos=pos;return 0;
}

static bool parent_locked;static int writers,creates,truncates,writes,fail_create,fail_truncate,fail_want,fail_copy;
static char ram_bytes[64];static struct inode parent_inode;
#define kfree(p) free(p)
static char *kstrdup(const char *s,int flags){(void)flags;return fail_copy?NULL:strdup(s);}
static struct inode *d_inode(struct dentry *d){assert(d->kind==2);parent_inode.i_sb=d->d_sb;return &parent_inode;}
static void inode_lock(struct inode *i){assert(i==&parent_inode&&!parent_locked);parent_locked=true;}
static void inode_unlock(struct inode *i){assert(i==&parent_inode&&parent_locked);parent_locked=false;}
static bool d_is_negative(struct dentry *d){return d->kind==0;}
static int mnt_want_write(struct vfsmount *m){assert(m==&mount);if(fail_want)return -EROFS;writers++;return 0;}
static void mnt_drop_write(struct vfsmount *m){assert(m==&mount&&writers==1);writers--;}
static struct dentry *lookup_one(void *id,struct qstr *q,struct dentry *base){assert(parent_locked&&writers==1);return lookup_one_positive_unlocked(id,q,base);}
static int vfs_create(void *id,struct dentry *d,unsigned mode,void *delegated){
 assert(!id&&!delegated&&mode==0600&&parent_locked&&writers==1&&d==&filea&&d->kind==0&&sb.s_magic==TMPFS_MAGIC);
 creates++;if(fail_create)return -ENOSPC;d->kind=1;d->size=0;d->data=ram_bytes;return 0;}
static int vfs_truncate(const struct path *p,loff_t size){
 assert(!parent_locked&&writers==1&&p->mnt==&mount&&p->dentry==&filea&&size==0);truncates++;
 if(fail_truncate)return -EIO;p->dentry->size=0;return 0;}
static ssize_t kernel_write(struct file *f,const void *data,size_t n,loff_t *pos){
 assert(f->f_mode&FMODE_WRITE);assert(f->f_path.dentry==&filea&&filea.data==ram_bytes);writes++;
 if(*pos>=64)return -ENOSPC;if(n>(size_t)(64-*pos))n=64-*pos;
 memcpy(ram_bytes+*pos,data,n);*pos+=n;if((u64)*pos>filea.size)filea.size=*pos;return n;}
static void reset(void){
 parent_locked=false;writers=creates=truncates=writes=fail_create=fail_truncate=fail_want=fail_copy=0;provider_volatile=false;filea.kind=1;filea.data="alphaomega";filea.size=10;
 lookups=stats=sleeps=opens=reads=iterations=rescheds=drop_ro_after_lookup=drop_ro_after_open=fail_stat=fail_open=0;locked=false;
 sb.s_flags=1;other.s_flags=1;mount.mnt_flags=1;provider_state.stage=3;
 for(unsigned i=0;i<sizeof(all)/sizeof(*all);i++)all[i]->refs=0;
 rootd.refs=1;
 backing=(struct file){.f_path={&mount,&rootd},.inode={&sb},.refs=1};provider_file=&backing;
}
static void quiescent(void){assert(backing.refs==1&&!locked);
 for(unsigned i=1;i<sizeof(all)/sizeof(*all);i++)assert(!all[i]->refs);
 assert(rootd.refs==1);}
'''
tests = r'''
int main(void){
 struct mt6797_md1_fs_attr attr;struct mt6797_md1_fs_entry entry;struct file *f;u8 buf[16];
 reset();assert(mt6797_md1_fs_provider_getattr_drive('X',"NVRAM/a.bin",&attr)==-ENODEV&&!lookups);quiescent();
 assert(IS_ERR(mt6797_md1_fs_provider_open_drive('Y',"NVRAM/a.bin",false))&&!lookups);quiescent();
 struct file xroot;
 reset();xroot=backing;xroot.f_path.dentry=&nvram;provider_extra[0]=&xroot;
 assert(!mt6797_md1_fs_provider_getattr_drive('X',"a.bin",&attr)&&attr.size==10);
 assert(mt6797_md1_fs_provider_getattr_drive('Z',"a.bin",&attr)==-ENOENT);
 assert(mt6797_md1_fs_provider_getattr_drive('X',"../top.txt",&attr)==-EINVAL);
 f=mt6797_md1_fs_provider_open_drive('X',"a.bin",false);assert(!IS_ERR(f));mt6797_md1_fs_provider_close(f);
 assert(xroot.refs==1);provider_extra[0]=NULL;quiescent();
 reset();assert(!mt6797_md1_fs_provider_getattr("NVRAM/a.bin",&attr)&&attr.mode==0100644&&attr.size==10&&attr.atime==1010&&attr.mtime==2010);
 assert(lookups==2&&stats==1&&sleeps==1);quiescent();
 reset();assert(!mt6797_md1_fs_provider_getattr("",&attr)&&attr.mode==0040755&&!lookups&&stats==1);quiescent();
 reset();assert(!mt6797_md1_fs_provider_getattr("NVRAM",&attr)&&attr.mode==0040755);quiescent();
 reset();assert(mt6797_md1_fs_provider_getattr("NVRAM/missing",&attr)==-ENOENT&&lookups==2);quiescent();
 reset();assert(mt6797_md1_fs_provider_getattr("missing/a.bin",&attr)==-ENOENT&&lookups==1);quiescent();
 reset();assert(mt6797_md1_fs_provider_getattr("top.txt/a.bin",&attr)==-ENOTDIR&&lookups==1);quiescent();
 reset();assert(mt6797_md1_fs_provider_getattr("NVRAM/link",&attr)==-EACCES&&!stats);quiescent();
 reset();assert(mt6797_md1_fs_provider_getattr("NVRAM/sock",&attr)==-EACCES&&!stats);quiescent();
 reset();assert(mt6797_md1_fs_provider_getattr("NVRAM/mnt",&attr)==-EACCES&&!stats);quiescent();
 reset();assert(mt6797_md1_fs_provider_getattr("NVRAM/foreign",&attr)==-EACCES&&!stats);quiescent();
 reset();assert(mt6797_md1_fs_provider_getattr("NVRAM/link/x",&attr)==-EACCES&&lookups==2);quiescent();
 const char *bad[]={"/NVRAM","NVRAM/","NVRAM//a.bin",".","..","NVRAM/..","../NVRAM","NVRAM/./a.bin"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){reset();assert(mt6797_md1_fs_provider_getattr(bad[i],&attr)==-EINVAL);assert(!stats);quiescent();}
 { char longest[MT6797_MD1_FS_PATH_MAX+2];memset(longest,'n',sizeof(longest));longest[sizeof(longest)-1]=0;
   reset();assert(mt6797_md1_fs_provider_getattr(longest,&attr)==-ENAMETOOLONG&&!lookups);quiescent();
   longest[MT6797_MD1_FS_PATH_MAX]=0;reset();assert(mt6797_md1_fs_provider_getattr(longest,&attr)==-EINVAL&&!lookups);quiescent();
   memset(longest,'n',256);longest[256]=0;reset();assert(mt6797_md1_fs_provider_getattr(longest,&attr)==-EINVAL&&!lookups);quiescent(); }
 reset();assert(mt6797_md1_fs_provider_getattr(NULL,&attr)==-ENAMETOOLONG);quiescent();
 reset();assert(mt6797_md1_fs_provider_getattr("NVRAM",NULL)==-EINVAL);quiescent();
 reset();provider_state.stage=2;assert(mt6797_md1_fs_provider_getattr("NVRAM",&attr)==-ENODEV&&!lookups);quiescent();
 reset();mount.mnt_flags=0;assert(mt6797_md1_fs_provider_getattr("NVRAM",&attr)==-ENODEV);quiescent();
 reset();drop_ro_after_lookup=1;assert(mt6797_md1_fs_provider_getattr("NVRAM",&attr)==-EACCES&&stats==1);quiescent();
 reset();fail_stat=1;assert(mt6797_md1_fs_provider_getattr("NVRAM",&attr)==-EIO);quiescent();
 /* open: type check, flags, references, RO recheck. */
 reset();f=mt6797_md1_fs_provider_open("NVRAM/a.bin",false);assert(!IS_ERR(f)&&f->flags==(O_RDONLY|O_LARGEFILE)&&f->f_path.dentry==&filea&&filea.refs==1&&backing.refs==1&&opens==1);
 assert(!mt6797_md1_fs_provider_fstat(f,&attr)&&attr.size==10&&attr.mode==0100644);
 assert(mt6797_md1_fs_provider_read(f,buf,5,&f->f_pos)==5&&!memcmp(buf,"alpha",5)&&f->f_pos==5);
 assert(mt6797_md1_fs_provider_llseek(f,-2,SEEK_END)==8&&mt6797_md1_fs_provider_llseek(f,1,SEEK_CUR)==9&&mt6797_md1_fs_provider_llseek(f,0,SEEK_SET)==0);
 assert(mt6797_md1_fs_provider_llseek(f,0,3)==-EINVAL&&mt6797_md1_fs_provider_llseek(f,-1,SEEK_SET)==-EINVAL);
 assert(mt6797_md1_fs_provider_read(f,buf,16,&f->f_pos)==10&&mt6797_md1_fs_provider_read(f,buf,16,&f->f_pos)==0);
 sb.s_flags=0;assert(mt6797_md1_fs_provider_read(f,buf,1,&f->f_pos)==-EACCES&&mt6797_md1_fs_provider_llseek(f,0,SEEK_SET)==-EACCES&&mt6797_md1_fs_provider_fstat(f,&attr)==-EACCES);sb.s_flags=1;
 mt6797_md1_fs_provider_close(f);quiescent();
 reset();f=mt6797_md1_fs_provider_open("NVRAM",true);assert(!IS_ERR(f)&&f->flags==(O_RDONLY|O_LARGEFILE|O_DIRECTORY)&&nvram.refs==1);
 assert(!mt6797_md1_fs_provider_next_entry(f,&entry)&&!strcmp(entry.name,"a.bin")&&entry.attr.size==10&&entry.attr.mode==0100644&&iterations==1);
 /* One lookup opens NVRAM; enumeration looks up all five children once.
  * link, sock, covered mountpoint and foreign superblock are skipped. */
 assert(mt6797_md1_fs_provider_next_entry(f,&entry)==-ENOENT&&rescheds==4&&lookups==6);
 assert(mt6797_md1_fs_provider_next_entry(f,&entry)==-ENOENT);
 mt6797_md1_fs_provider_close(f);quiescent();
 reset();f=mt6797_md1_fs_provider_open("",true);assert(!IS_ERR(f)&&f->f_path.dentry==&rootd&&rootd.refs==2);
 assert(!mt6797_md1_fs_provider_next_entry(f,&entry)&&!strcmp(entry.name,"NVRAM")&&entry.attr.mode==0040755);
 assert(!mt6797_md1_fs_provider_next_entry(f,&entry)&&!strcmp(entry.name,"top.txt")&&mt6797_md1_fs_provider_next_entry(f,&entry)==-ENOENT);
 mt6797_md1_fs_provider_close(f);quiescent();
 reset();f=mt6797_md1_fs_provider_open("NVRAM/a.bin",true);assert(PTR_ERR(f)==-ENOTDIR&&!opens);quiescent();
 reset();f=mt6797_md1_fs_provider_open("NVRAM",false);assert(PTR_ERR(f)==-EISDIR&&!opens);quiescent();
 reset();f=mt6797_md1_fs_provider_open("NVRAM/none",false);assert(PTR_ERR(f)==-ENOENT);quiescent();
 reset();f=mt6797_md1_fs_provider_open("NVRAM/link",false);assert(PTR_ERR(f)==-EACCES&&!opens);quiescent();
 reset();f=mt6797_md1_fs_provider_open("X:/a",false);assert(PTR_ERR(f)==-ENOENT);quiescent();
 reset();provider_state.stage=2;f=mt6797_md1_fs_provider_open("NVRAM",true);assert(PTR_ERR(f)==-ENODEV);quiescent();
 reset();drop_ro_after_open=1;f=mt6797_md1_fs_provider_open("NVRAM",true);assert(PTR_ERR(f)==-EACCES&&opens==1);quiescent();
 reset();fail_open=1;f=mt6797_md1_fs_provider_open("NVRAM",true);assert(PTR_ERR(f)==-ENOMEM);quiescent();
 reset();mt6797_md1_fs_provider_close(NULL);mt6797_md1_fs_provider_close(ERR_PTR(-ENOENT));quiescent();
 reset();assert(mt6797_md1_fs_provider_next_entry(NULL,&entry)==-EINVAL&&mt6797_md1_fs_provider_read(NULL,buf,1,NULL)==-EACCES&&mt6797_md1_fs_provider_llseek(NULL,0,0)==-EACCES);

 /* Actual RAM provider create/write/readback, truncate and honest full errors. */
 reset();assert(PTR_ERR(mt6797_md1_fs_provider_open_ram("NVRAM/a.bin",true,true,false))==-EROFS&&!creates);quiescent();
 reset();provider_volatile=true;sb.s_magic=TMPFS_MAGIC;sb.s_flags=0;mount.mnt_flags=0;filea.kind=0;
 f=mt6797_md1_fs_provider_open_ram("NVRAM/a.bin",true,true,false);
 assert(!IS_ERR(f)&&creates==1&&!writers&&!parent_locked);
 loff_t pos=0;assert(mt6797_md1_fs_provider_write(f,"hello",5,&pos)==5&&pos==5);
 struct file *reader=mt6797_md1_fs_provider_open("NVRAM/a.bin",false);assert(!IS_ERR(reader));
 pos=0;memset(buf,0,sizeof(buf));assert(mt6797_md1_fs_provider_read(reader,buf,5,&pos)==5&&!memcmp(buf,"hello",5));
 assert(mt6797_md1_fs_provider_write(reader,"x",1,&pos)==-EROFS);
 mt6797_md1_fs_provider_close(reader);
 pos=62;assert(mt6797_md1_fs_provider_write(f,"abcd",4,&pos)==2&&pos==64);
 assert(mt6797_md1_fs_provider_write(f,"x",1,&pos)==-ENOSPC);
 pos=16*1024*1024;assert(mt6797_md1_fs_provider_write(f,"x",1,&pos)==-EFBIG);
 mt6797_md1_fs_provider_close(f);quiescent();
 f=mt6797_md1_fs_provider_open_ram("NVRAM/a.bin",true,false,true);
 assert(!IS_ERR(f)&&truncates==1&&filea.size==0&&!writers&&!parent_locked);
 mt6797_md1_fs_provider_close(f);quiescent();
 const char *ram_bad[]={"/NVRAM/a.bin","NVRAM/../a.bin","NVRAM//a.bin","NVRAM/.","NVRAM/..","NVRAM/","NVRAM/link","NVRAM/mnt","NVRAM/foreign","NVRAM/sock"};
 for(unsigned i=0;i<sizeof(ram_bad)/sizeof(*ram_bad);i++){
   int before=creates;f=mt6797_md1_fs_provider_open_ram(ram_bad[i],true,true,true);
   assert(IS_ERR(f)&&creates==before&&!writers&&!parent_locked);quiescent();
 }
 for(int failure=0;failure<4;failure++){
   filea.kind=0;fail_create=failure==0;fail_open=failure==1;fail_copy=failure==2;fail_want=failure==3;
   f=mt6797_md1_fs_provider_open_ram("NVRAM/a.bin",true,true,false);
   assert(IS_ERR(f)&&!writers&&!parent_locked);quiescent();
 }
 fail_create=fail_open=fail_copy=fail_want=0;filea.kind=1;fail_truncate=1;
 f=mt6797_md1_fs_provider_open_ram("NVRAM/a.bin",true,false,true);
 assert(PTR_ERR(f)==-EIO&&!writers&&!parent_locked);quiescent();
 sb.s_magic=0xef53;assert(IS_ERR(mt6797_md1_fs_provider_open_ram("NVRAM/a.bin",true,true,true)));quiescent();
 puts("PASS actual RAM VFS: create/write/readback/truncate, read-only handle refusal, partial/full writes, file bound, traversal/link/mount/foreign/special refusal, failure refs and write-lock cleanup");
 puts("PASS actual provider VFS layer: confined multi-component walk, dot/dot-dot/absolute/symlink/mountpoint/special/foreign refusals, RO rechecks, open flags, read/seek/fstat, entry enumeration skipping refused children, complete reference cleanup");
}
'''
parts = [shim, provider, function('mt6797_fs_readonly'), function('mt6797_fs_tmpfs_writable'), function('mt6797_fs_backing_matches'), function('mt6797_fs_accessible'), function('mt6797_fs_drive_file'), function('mt6797_fs_root_get'),
         function('mt6797_fs_walk_component'), function('mt6797_fs_walk'), function('mt6797_fs_stat'),
         function('mt6797_md1_fs_provider_getattr_drive'), function('mt6797_md1_fs_provider_getattr'), function('mt6797_md1_fs_provider_open_drive'), function('mt6797_md1_fs_provider_open'),
         function('mt6797_md1_fs_provider_open_ram_drive'), function('mt6797_md1_fs_provider_open_ram'), function('mt6797_md1_fs_provider_write'), function('mt6797_md1_fs_provider_fstat'), function('mt6797_md1_fs_provider_read'),
         function('mt6797_md1_fs_provider_llseek'), walk_struct, function('mt6797_fs_dir_actor'),
         function('mt6797_md1_fs_provider_next_entry'), function('mt6797_md1_fs_provider_close'), tests]
with tempfile.TemporaryDirectory(prefix='gemini-fs-getattr-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text('\n'.join(parts))
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-misleading-indentation', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
