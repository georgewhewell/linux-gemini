#!/usr/bin/env python3
"""Execute production durability/write functions with injected writeback errors."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'drivers/soc/mediatek/mt6797-md1-fs-provider.c').read_text()


def function(name):
    start = source.index(name+'(')
    start = source.rfind('\n', 0, start)+1
    brace = source.index('{', start)
    end = brace+1
    depth = 1
    while depth:
        depth += (source[end] == '{')-(source[end] == '}')
        end += 1
    return source[start:end]+'\n'


shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
typedef int64_t loff_t;
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define FMODE_WRITE 2
#define MT6797_MD1_FS_BACKING_INTERNAL 2
struct super_block {int s_umount;unsigned int s_wb_err;};
struct inode {void *i_sb;};
struct file {struct inode inode;unsigned int f_mode,f_sb_err;};
static struct file target;
/* MD1 has no optional V low-space policy; real V tested separately. */
static int mt6797_fs_md3_data_space(struct file*f,size_t n){(void)f;(void)n;return 0;}
static struct super_block target_sb;
static unsigned int provider_volatile;
static bool backing;
static int writes,flushes,filesystem_flushes,write_error,flush_error,filesystem_error,sb_readers,errseq_error,errseq_calls;
static bool readonly_during_sync;
static ssize_t write_length;
static void might_sleep(void){}
static struct inode *file_inode(struct file *f){assert(f==&target);return &f->inode;}
static bool mt6797_fs_backing_matches(struct file *f,unsigned int mode){assert(f==&target&&mode==provider_volatile);return backing;}
static ssize_t kernel_write(struct file *f,const void *p,size_t count,loff_t *pos){assert(f==&target&&p&&pos);writes++;if(write_error)return write_error;assert((size_t)write_length<=count);*pos+=write_length;return write_length;}
static int vfs_fsync(struct file *f,int datasync){assert(f==&target&&!datasync);flushes++;return flush_error;}
static void down_read(void *lock){assert(lock==&target_sb.s_umount&&!sb_readers);sb_readers++;if(readonly_during_sync)backing=false;}
static void up_read(void *lock){assert(lock==&target_sb.s_umount&&sb_readers==1);sb_readers--;}
static int sync_filesystem(void *sb){assert(sb==&target_sb&&sb_readers==1);filesystem_flushes++;return filesystem_error;}
static int errseq_check_and_advance(unsigned int *error,unsigned int *cursor){assert(error==&target_sb.s_wb_err&&cursor==&target.f_sb_err&&!sb_readers);errseq_calls++;(*cursor)++;return errseq_error;}
static void reset(unsigned int mode){assert(!sb_readers);readonly_during_sync=false;provider_volatile=mode;backing=true;target=(struct file){.inode={&target_sb},.f_mode=FMODE_WRITE};writes=flushes=filesystem_flushes=write_error=flush_error=filesystem_error=errseq_error=errseq_calls=0;write_length=4;}
'''
tests = r'''
int main(void){
 char data[4]={0};loff_t pos;
 reset(0);pos=0;assert(mt6797_md1_fs_provider_write(&target,data,4,&pos)==-EROFS&&!writes&&!flushes);
 reset(1);pos=0;assert(mt6797_md1_fs_provider_write(&target,data,4,&pos)==4&&pos==4&&writes==1&&!flushes);
 reset(2);pos=0;assert(mt6797_md1_fs_provider_write(&target,data,4,&pos)==4&&pos==4&&writes==1&&flushes==1);
 reset(2);pos=0;write_length=2;assert(mt6797_md1_fs_provider_write(&target,data,4,&pos)==2&&pos==2&&flushes==1);
 reset(2);pos=0;flush_error=-EIO;assert(mt6797_md1_fs_provider_write(&target,data,4,&pos)==-EIO&&pos==4&&writes==1&&flushes==1);
 reset(2);pos=0;write_error=-ENOSPC;assert(mt6797_md1_fs_provider_write(&target,data,4,&pos)==-ENOSPC&&!pos&&writes==1&&!flushes);
 reset(2);pos=0;write_length=0;assert(!mt6797_md1_fs_provider_write(&target,data,4,&pos)&&!flushes);
 reset(2);pos=0;backing=false;assert(mt6797_md1_fs_provider_write(&target,data,4,&pos)==-EROFS&&!writes&&!flushes);
 reset(2);pos=0;target.f_mode=0;assert(mt6797_md1_fs_provider_write(&target,data,4,&pos)==-EROFS&&!writes&&!flushes);
 reset(2);pos=16*1024*1024;assert(mt6797_md1_fs_provider_write(&target,data,4,&pos)==-EFBIG&&!writes&&!flushes);
 reset(2);assert(!mt6797_fs_sync_mutation(&target,0)&&filesystem_flushes==1);
 reset(2);filesystem_error=-EIO;assert(mt6797_fs_sync_mutation(&target,0)==-EIO&&filesystem_flushes==1);
 reset(2);errseq_error=-EIO;assert(mt6797_fs_sync_mutation(&target,0)==-EIO&&filesystem_flushes==1&&errseq_calls==1&&target.f_sb_err==1);
 reset(2);filesystem_error=-ENOSPC;errseq_error=-EIO;assert(mt6797_fs_sync_mutation(&target,0)==-ENOSPC&&filesystem_flushes==1&&errseq_calls==1);
 reset(2);filesystem_error=-EIO;assert(mt6797_fs_sync_mutation(&target,-ENOSPC)==-ENOSPC&&!filesystem_flushes);
 reset(2);readonly_during_sync=true;assert(mt6797_fs_sync_mutation(&target,0)==-EROFS&&!filesystem_flushes&&!sb_readers);
 reset(1);assert(!mt6797_fs_sync_mutation(&target,0)&&!filesystem_flushes);
 puts("PASS actual internal NV durability: successful/partial writes sync before acknowledgement; writeback errors returned; underlying errors retained; RO/RAM behavior and bounds preserved");
}
'''
parts = [shim, function('mt6797_fs_sync_mutation'), function('mt6797_fs_sync_file'),
         function('mt6797_md1_fs_provider_write'), tests]
with tempfile.TemporaryDirectory(prefix='gemini-nv-durability-') as directory:
    path = Path(directory)
    (path/'test.c').write_text('\n'.join(parts))
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=10)
