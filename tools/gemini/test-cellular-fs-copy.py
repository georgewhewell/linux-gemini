#!/usr/bin/env python3
"""Exercise production bounded copy with fault-injected file I/O boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'drivers/soc/mediatek/mt6797-md1-fs-provider.c').read_text()
s=s[s.index('int mt6797_md1_fs_provider_copy_drive('):s.index('EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_copy_drive)')]
shim=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;typedef uint64_t u64;typedef int64_t loff_t;typedef long ssize_t;
#define MT6797_MD1_FS_PATH_MAX 1024
#define GFP_KERNEL 0
#define READ_ONCE(v) (v)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
struct mt6797_md1_fs_attr {u64 size;};
struct file {int inode;struct {int unused;} f_path;};
static struct file source,destination;
static bool provider_volatile,writable,allocation_fail,alias;
static int refs,allocs,opens,reads,writes,truncates,fstat_error,source_error,destination_error,truncate_error;
static int read_fault,write_fault;static ssize_t read_result,write_result;
static size_t length,read_chunk,write_chunk,copied;
static void might_sleep(void){}
static void cond_resched(void){}
static void *kvmalloc(size_t n,int flags){(void)flags;if(allocation_fail)return NULL;allocs++;return malloc(n);}
static void kvfree(void *p){if(p){assert(allocs==1);allocs--;free(p);}}
static struct file *mt6797_md1_fs_provider_open_ram_drive(u8 drive,const char *path,bool write,bool create,bool truncate){
 assert(!truncate);opens++;if(!write){assert(drive=='Z'&&!create&&!strcmp(path,"source"));if(source_error)return ERR_PTR(source_error);refs++;return &source;}
 assert(drive=='Y'&&create&&!strcmp(path,"destination"));if(destination_error)return ERR_PTR(destination_error);refs++;return &destination;
}
static int mt6797_md1_fs_provider_fstat(struct file *f,struct mt6797_md1_fs_attr *a){assert(f==&source);a->size=length;return fstat_error;}
static void *file_inode(struct file *f){return alias?&source.inode:&f->inode;}
static void fput(struct file *f){assert((f==&source||f==&destination)&&refs>0);refs--;}
static bool mt6797_fs_tmpfs_writable(struct file *f){assert(f==&destination);return writable;}
static int vfs_truncate(void *p,loff_t size){assert(p==&destination.f_path&&size==0);truncates++;return truncate_error;}
static ssize_t mt6797_md1_fs_provider_read(struct file *f,void *data,size_t n,loff_t *pos){assert(f==&source&&n<=65536);reads++;if(reads==read_fault)return read_result;if(read_chunk&&n>read_chunk)n=read_chunk;for(size_t i=0;i<n;i++)((u8 *)data)[i]=(u8)((*pos+i)%251);*pos+=n;return n;}
static ssize_t mt6797_md1_fs_provider_write(struct file *f,const void *data,size_t n,loff_t *pos){assert(f==&destination&&n<=65536&&truncates==1);writes++;if(writes==write_fault)return write_result;if(write_chunk&&n>write_chunk)n=write_chunk;for(size_t i=0;i<n;i++)assert(((const u8 *)data)[i]==(u8)((*pos+i)%251));*pos+=n;copied+=n;return n;}
static void reset(void){assert(!refs&&!allocs);provider_volatile=writable=true;allocation_fail=alias=false;opens=reads=writes=truncates=fstat_error=source_error=destination_error=truncate_error=read_fault=write_fault=0;read_result=write_result=0;length=100;read_chunk=write_chunk=copied=0;}
'''
tests=r'''
static void check(int expected){assert(mt6797_md1_fs_provider_copy_drive('Z',"source",'Y',"destination")==expected);assert(!refs&&!allocs);}
int main(void){
 reset();check(0);assert(copied==length);
 reset();length=0;check(0);assert(!reads&&!writes&&truncates==1);
 reset();length=16*1024*1024;check(0);assert(copied==length);
 reset();length=16*1024*1024+1;check(-EFBIG);assert(opens==1&&!truncates);
 reset();read_chunk=7;write_chunk=3;check(0);assert(copied==length&&reads>1&&writes>1);
 reset();allocation_fail=true;check(-ENOMEM);assert(opens==1&&!truncates);
 reset();source_error=-EROFS;check(-EROFS);assert(opens==1);
 reset();fstat_error=-EACCES;check(-EACCES);assert(opens==1);
 reset();read_chunk=7;read_fault=2;read_result=-EIO;check(-EIO);assert(opens==1&&!truncates);
 reset();read_fault=1;read_result=0;check(-EIO);assert(opens==1&&!truncates);
 reset();destination_error=-ENOSPC;check(-ENOSPC);assert(!truncates);
 reset();alias=true;check(-EINVAL);assert(!truncates&&!writes);
 reset();writable=false;check(-EROFS);assert(!truncates);
 reset();provider_volatile=false;check(-EROFS);assert(!truncates);
 reset();truncate_error=-EACCES;check(-EACCES);assert(!writes);
 reset();write_chunk=7;write_fault=2;write_result=-ENOSPC;check(-ENOSPC);assert(copied==7);
 reset();write_fault=1;write_result=0;check(-EIO);assert(!copied);
 reset();assert(mt6797_md1_fs_provider_copy_drive('Z',"same",'Z',"same")==-EINVAL&&!opens);
 reset();assert(mt6797_md1_fs_provider_copy_drive('Z',NULL,'Y',"destination")==-ENAMETOOLONG&&!opens);
 char big[1026];memset(big,'a',1025);big[1025]=0;
 reset();assert(mt6797_md1_fs_provider_copy_drive('Z',"source",'Y',big)==-ENAMETOOLONG&&!opens);
 puts("actual bounded copy: full/empty/limit/partial I/O, same-inode refusal, pre-write read failures, write errors and cleanup PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(shim+s+tests)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
