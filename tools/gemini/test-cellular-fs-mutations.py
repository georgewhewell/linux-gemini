#!/usr/bin/env python3
"""Execute the actual namespace mutator with instrumented VFS boundaries.

Checks authority/confinement, callback selection and resource ownership;
this is not a real-kernel tmpfs integration test.
"""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'drivers/soc/mediatek/mt6797-md1-fs-provider.c').read_text()
source = source[source.index('int mt6797_md1_fs_provider_mutate_drive('):source.index('EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_mutate_drive)')]
header = (root/'include/linux/soc/mediatek/mt6797-md1-fs-provider.h').read_text()
enum = header[header.index('enum mt6797_md1_fs_mutation {'):header.index('#if IS_ENABLED')]
shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8;
#define MT6797_MD1_FS_PATH_MAX 1024
#define NAME_MAX 255
#define GFP_KERNEL 0
#define READ_ONCE(x) (x)
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define QSTR_INIT(s,n) {s,n}
struct qstr {const char *name;size_t len;};
struct inode {void *i_sb;};
struct dentry {void *d_sb;struct inode inode;int type,refs;bool mounted;};
struct file {struct {void *mnt;} f_path;struct inode inode;};
static struct file rootfile;
static struct dentry parent,child,alternate;
static int rootrefs,allocs,writers,locked,calls,lookup_calls;
static int walk_error,lookup_error,write_error,operation_error;
static bool available,provider_volatile,writable,alloc_fail,drift,splice;
static u8 got_drive;
static char got_parent[1025],got_leaf[256];
static int got_op;
static void might_sleep(void){}
static struct file *mt6797_fs_root_get(u8 drive){got_drive=drive;if(!available)return NULL;rootrefs++;return &rootfile;}
static void fput(struct file *f){assert(f==&rootfile&&rootrefs==1);rootrefs--;}
static bool mt6797_fs_tmpfs_writable(struct file *f){assert(f==&rootfile);return writable&&!(drift&&locked);}
static char *kstrdup(const char *s,int ignored){(void)ignored;if(alloc_fail)return NULL;allocs++;return strdup(s);}
static void kfree(void *p){assert(p&&allocs==1);allocs--;free(p);}
static struct dentry *mt6797_fs_walk(struct file *f,const char *p){assert(f==&rootfile);strcpy(got_parent,p);if(walk_error)return ERR_PTR(walk_error);parent.refs++;return &parent;}
static bool d_is_dir(struct dentry *d){return d->type==1;}
static bool d_is_reg(struct dentry *d){return d->type==2;}
static bool d_is_symlink(struct dentry *d){return d->type==3;}
static bool d_is_negative(struct dentry *d){return d->type==0;}
static bool d_mountpoint(struct dentry *d){return d->mounted;}
static struct inode *d_inode(struct dentry *d){return &d->inode;}
static struct inode *file_inode(struct file *f){return &f->inode;}
static int mnt_want_write(void *mnt){(void)mnt;if(write_error)return write_error;writers++;return 0;}
static void mnt_drop_write(void *mnt){(void)mnt;assert(writers==1);writers--;}
static void *mnt_idmap(void *mnt){return mnt;}
static void inode_lock(struct inode *i){assert(i==&parent.inode&&!locked);locked++;}
static void inode_unlock(struct inode *i){assert(i==&parent.inode&&locked==1);locked--;}
static struct dentry *lookup_one(void *map,struct qstr *q,struct dentry *p){(void)map;assert(p==&parent&&locked==1&&writers==1);lookup_calls++;memcpy(got_leaf,q->name,q->len);got_leaf[q->len]=0;if(lookup_error)return ERR_PTR(lookup_error);child.refs++;return &child;}
static void dput(struct dentry *d){assert(!IS_ERR(d)&&d->refs>0);d->refs--;}
static void callback(int op,struct inode *p,struct dentry *d,void *deleg){assert(p==&parent.inode&&d==&child&&locked==1&&writers==1&&!deleg);calls++;got_op=op;}
static struct dentry *vfs_mkdir(void *map,struct inode *p,struct dentry *d,unsigned mode,void *deleg){(void)map;callback(0,p,d,deleg);assert(mode==0770);if(operation_error){dput(d);return ERR_PTR(operation_error);}if(splice){dput(d);alternate.refs++;return &alternate;}d->type=1;return d;}
static int vfs_rmdir(void *map,struct inode *p,struct dentry *d,void *deleg){(void)map;callback(1,p,d,deleg);return operation_error;}
static int vfs_unlink(void *map,struct inode *p,struct dentry *d,void *deleg){(void)map;callback(2,p,d,deleg);return operation_error;}
static void reset(void){assert(!rootrefs&&!allocs&&!writers&&!locked&&!parent.refs&&!child.refs&&!alternate.refs);parent=(struct dentry){.type=1};child=(struct dentry){.d_sb=&rootfile};rootfile.inode.i_sb=&rootfile;available=provider_volatile=writable=true;alloc_fail=drift=splice=false;walk_error=lookup_error=write_error=operation_error=calls=lookup_calls=0;got_parent[0]=got_leaf[0]=0;}
'''
tests = r'''
static void check(const char *path,int op,int expected,int effects){int r=mt6797_md1_fs_provider_mutate_drive('X',path,op);assert(r==expected&&calls==effects);assert(!rootrefs&&!allocs&&!writers&&!locked&&!parent.refs&&!child.refs&&!alternate.refs);}
int main(void){
 for(int op=0;op<3;op++){
  reset();child.type=op==0?0:op==1?1:2;check("NVRAM/item",op,0,1);assert(got_op==op&&got_drive=='X'&&!strcmp(got_parent,"NVRAM")&&!strcmp(got_leaf,"item"));
  reset();child.type=op==0?0:op==1?1:2;operation_error=-ENOSPC;check("item",op,-ENOSPC,1);assert(!strcmp(got_parent,""));
  reset();available=false;check("item",op,-ENODEV,0);
  reset();provider_volatile=false;check("item",op,-EROFS,0);assert(!lookup_calls);
  reset();writable=false;check("item",op,-EROFS,0);
  reset();alloc_fail=true;check("item",op,-ENOMEM,0);
  reset();walk_error=-EACCES;check("link/item",op,-EACCES,0);
  reset();parent.type=2;check("file/item",op,-ENOTDIR,0);
  reset();write_error=-EROFS;check("item",op,-EROFS,0);assert(!lookup_calls);
  reset();lookup_error=-ENOMEM;check("item",op,-ENOMEM,0);
  reset();drift=true;check("item",op,-EACCES,0);
  reset();child.mounted=true;check("item",op,-EACCES,0);
  reset();child.type=3;check("item",op,-EACCES,0);
  reset();child.d_sb=NULL;check("item",op,-EACCES,0);
  const char *bad[]={"","/item","item/",".","..","p/.","p/.."};
  for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){reset();check(bad[i],op,-EINVAL,0);assert(!lookup_calls);}
 }
 reset();splice=true;check("item",0,0,1);
 reset();child.type=1;check("item",0,-EEXIST,0);
 reset();check("item",1,-ENOENT,0);
 reset();check("item",2,-ENOENT,0);
 reset();child.type=2;check("item",1,-ENOTDIR,0);
 reset();child.type=1;check("item",2,-EISDIR,0);
 reset();child.type=4;check("item",2,-EACCES,0);
 reset();child.type=1;operation_error=-ENOTEMPTY;check("item",1,-ENOTEMPTY,1);
 reset();check("item",99,-EINVAL,0);
 reset();check(NULL,0,-ENAMETOOLONG,0);
 char longpath[1026];memset(longpath,'a',1025);longpath[1025]=0;
 reset();check(longpath,0,-ENAMETOOLONG,0);
 longpath[256]=0;reset();check(longpath,0,-EINVAL,0);
 puts("actual namespace mutator: authority, confinement, VFS errors and reference cleanup PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.c').write_text(shim+enum+source+tests)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
