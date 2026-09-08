#!/usr/bin/env python3
"""Execute production iterative XDELETE with traversal/resource faults."""
import os
from pathlib import Path
import subprocess
import tempfile
r=Path(__file__).resolve().parents[2]
s=(r/'drivers/soc/mediatek/mt6797-md1-fs-provider.c').read_text();s=s[s.index('int mt6797_md1_fs_provider_xdelete_drive('):s.index('EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_xdelete_drive)')]
shim=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint32_t u32;
#define MT6797_MD1_FS_PATH_MAX 1024
#define GFP_KERNEL 0
#define READ_ONCE(x) (x)
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define S_ISDIR(m) ((m)==1)
#define S_ISREG(m) ((m)==2)
#define MT6797_MD1_FS_RMDIR 1
#define MT6797_MD1_FS_UNLINK 2
struct file {bool live;int cursor;};
struct mt6797_md1_fs_entry {char name[256];struct {int mode;} attr;};
static struct file authority,pool[514];
static bool provider_volatile,available,writable;
static int refs,allocs,allocation_calls,fail_allocation,opens,fail_open,iterations,fail_iter,mutations,fail_mutation,mode;
static void might_sleep(void){}
static void cond_resched(void){}
static struct file *mt6797_fs_root_get(u8 drive){assert(drive=='X');if(!available)return NULL;assert(!authority.live);authority.live=true;refs++;return &authority;}
static bool mt6797_fs_tmpfs_writable(struct file *f){assert(f==&authority);return writable;}
static void fput(struct file *f){assert(f&&f->live&&refs>0);f->live=false;refs--;}
static void *kmalloc(size_t n,int flag){(void)flag;allocation_calls++;if(allocation_calls==fail_allocation)return NULL;void *p=malloc(n);assert(p);allocs++;return p;}
static void *kcalloc(size_t n,size_t size,int flag){void *p=kmalloc(n*size,flag);if(p)memset(p,0,n*size);return p;}
static void kfree(void *p){if(p){assert(allocs>0);allocs--;free(p);}}
static struct file *mt6797_md1_fs_provider_open_drive(u8 drive,const char *path,bool dir){assert(drive=='X'&&dir&&strlen(path)<=1024);opens++;if(opens==fail_open)return ERR_PTR(-EACCES);assert(opens<514);struct file *f=&pool[opens];assert(!f->live);f->live=true;f->cursor=0;refs++;return f;}
static int mt6797_md1_fs_provider_next_entry(struct file *f,struct mt6797_md1_fs_entry *e){assert(f->live);iterations++;if(iterations==fail_iter)return -EIO;if(mode!=2&&f->cursor++)return -ENOENT;strcpy(e->name,"a");e->attr.mode=mode==1?1:2;return 0;}
static int mt6797_md1_fs_provider_mutate_drive(u8 drive,const char *path,int op){assert(drive=='X'&&strlen(path)<=1024&&(op==1||op==2));mutations++;return mutations==fail_mutation?-EACCES:0;}
static void reset(void){assert(!refs&&!allocs);provider_volatile=available=writable=true;allocation_calls=fail_allocation=opens=fail_open=iterations=fail_iter=mutations=fail_mutation=mode=0;}
'''
tests=r'''
static void check(const char *name,u32 flags,int result){assert(mt6797_md1_fs_provider_xdelete_drive('X',name,flags)==result);assert(!refs&&!allocs);}
int main(void){
 reset();check("tree",4,1);assert(mutations==1);
 reset();check("tree",28,2);assert(mutations==2);
 reset();check("tree",16,0);assert(!mutations);
 reset();available=false;check("tree",28,-ENODEV);
 reset();provider_volatile=false;check("tree",28,-EROFS);
 reset();writable=false;check("tree",28,-EROFS);
 for(int i=1;i<=3;i++){reset();fail_allocation=i;check("tree",28,-ENOMEM);assert(!opens);}
 reset();fail_open=1;check("tree",28,-EACCES);
 reset();fail_iter=1;check("tree",28,-EIO);
 reset();fail_mutation=1;check("tree",28,-EACCES);
 reset();fail_mutation=2;check("tree",28,-EACCES);assert(mutations==2);
 reset();mode=1;fail_open=5;check("tree",28,-EACCES);assert(opens==5);
 reset();mode=1;fail_iter=20;check("tree",28,-EIO);assert(opens==20);
 reset();mode=1;check("tree",28,-ENAMETOOLONG);assert(opens>400&&!mutations);
 reset();mode=2;check("tree",4,-E2BIG);assert(iterations==4097&&mutations==4096);
 reset();check("",28,-EINVAL);assert(!opens&&!mutations);
 reset();check("",4,1);
 reset();check("tree",0,-EINVAL);
 reset();check("tree",32,-EINVAL);
 reset();check(NULL,28,-ENAMETOOLONG);
 puts("actual XDELETE: allocation/traversal/mutation failures, deep-path and 4096-entry bounds, partial-error status and cleanup PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(shim+s+tests)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
