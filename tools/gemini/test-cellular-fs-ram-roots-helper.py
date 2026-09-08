#!/usr/bin/env python3
"""Execute the actual three-root helper with syscall adapters; no device I/O."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
helper = root / 'tools/gemini/mt6797-fs-register.c'
shim = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
static int fake_open(const char *,int,...),fake_close(int),fake_fstat(int,struct stat *);
static int fake_fstatfs(int,struct statfs *),fake_fstatvfs(int,struct statvfs *);
static ssize_t fake_read(int,void *,size_t),fake_pread(int,void *,size_t,off_t);
static int fake_ioctl(int,unsigned long,...);
static long fake_syscall(long,...);
#define open fake_open
#define close fake_close
#define read fake_read
#define pread fake_pread
#define fstat fake_fstat
#define fstatfs fake_fstatfs
#define fstatvfs fake_fstatvfs
#define ioctl fake_ioctl
#define syscall fake_syscall
#define main helper_main
'''
tests = r'''
#undef main
enum {BLOCK,UEVENT,SIZE,ROOT,MD,RAM,RAMMD,PROVIDER};
struct fd {int kind,index;bool live;};
static struct fd fds[128];
static int nextfd,calls,fail_at,mode,bad_source,registered,provider_opens,super_reads;
static const int parts[]={6,8,9};
static const uint64_t sizes[]={33554432,8388608,13074432};
static const char *labels[]={"nvdata","protect1","protect2"};
static bool fault(void){calls++;if(calls==fail_at){errno=EIO;return true;}return false;}
static int newfd(int kind,int i){assert(nextfd<128);fds[nextfd]=(struct fd){kind,i,true};return nextfd++;}
static struct fd *owned(int fd){assert(fd>0&&fd<nextfd&&fds[fd].live);return &fds[fd];}
static int fake_open(const char *path,int flags,...){
 assert(flags&O_CLOEXEC);assert(flags&O_NOFOLLOW);if(fault())return -1;
 for(int i=0;i<3;i++){
  char name[80];snprintf(name,sizeof(name),"/dev/mmcblk0p%d",parts[i]);
  if(!strcmp(name,path)){assert((flags&O_ACCMODE)==O_RDONLY);return newfd(BLOCK,i);}
  snprintf(name,sizeof(name),"/sys/dev/block/179:%d/uevent",parts[i]);if(!strcmp(name,path))return newfd(UEVENT,i);
  snprintf(name,sizeof(name),"/sys/dev/block/179:%d/size",parts[i]);if(!strcmp(name,path))return newfd(SIZE,i);
 }
 assert(!strcmp(path,"/dev/mt6797-md1-fs-provider")&&(flags&O_ACCMODE)==O_RDWR);
 assert(super_reads==3);provider_opens++;return newfd(PROVIDER,0);
}
static int fake_close(int fd){owned(fd)->live=false;return 0;}
static ssize_t fake_read(int fd,void *buf,size_t capacity){
 struct fd *f=owned(fd);if(fault())return -1;char value[128];
 if(f->kind==UEVENT)snprintf(value,sizeof(value),"PARTN=%d\nPARTNAME=%s\n",parts[f->index],mode==1&&f->index==bad_source?"wrong":labels[f->index]);
 else {assert(f->kind==SIZE);snprintf(value,sizeof(value),"%llu\n",(unsigned long long)(sizes[f->index]/512));}
 assert(strlen(value)<capacity);memcpy(buf,value,strlen(value));return strlen(value);
}
static ssize_t fake_pread(int fd,void *buf,size_t n,off_t off){
 struct fd *f=owned(fd);assert(f->kind==BLOCK&&n==1024&&off==1024);if(fault())return -1;
 unsigned char *b=buf;memset(b,0,n);uint32_t blocks=sizes[f->index]/4096;
 for(int j=0;j<4;j++)b[4+j]=blocks>>(8*j);
 b[24]=2;b[56]=0x53;b[57]=0xef;b[58]=mode==2&&f->index==bad_source?0:1;super_reads++;return n;
}
static int fake_fstat(int fd,struct stat *st){
 struct fd *f=owned(fd);if(fault())return -1;memset(st,0,sizeof(*st));
 bool ram=f->kind==RAM||f->kind==RAMMD;
 st->st_mode=f->kind==BLOCK?(S_IFBLK|0600):(S_IFDIR|0700);
 st->st_rdev=makedev(179,parts[f->index]);
 st->st_dev=ram?makedev(0,100+(mode==3?0:f->index)):st->st_rdev;
 st->st_ino=f->kind==ROOT?2:f->kind==RAM?1:12;
 if(mode==4&&f->index==bad_source&&f->kind==ROOT)st->st_dev=makedev(179,99);
 return 0;
}
static int fake_fstatfs(int fd,struct statfs *st){
 struct fd *f=owned(fd);if(fault())return -1;memset(st,0,sizeof(*st));
 bool ram=f->kind==RAM||f->kind==RAMMD;
 st->f_type=ram?TMPFS_MAGIC:EXT4_SUPER_MAGIC;st->f_bsize=4096;st->f_blocks=16384;st->f_files=4096;
 if(mode==5&&f->index==bad_source&&ram)st->f_blocks=16385;
 if(mode==8&&f->index==bad_source&&ram)st->f_type=EXT4_SUPER_MAGIC;
 return 0;
}
static int fake_fstatvfs(int fd,struct statvfs *st){
 struct fd *f=owned(fd);if(fault())return -1;memset(st,0,sizeof(*st));
 st->f_flag=f->kind==RAM||f->kind==RAMMD?0:ST_RDONLY;
 if(mode==6&&f->index==bad_source&&f->kind==ROOT)st->f_flag=0;
 return 0;
}
static long fake_syscall(long nr,...){
 assert(nr==SYS_openat2);va_list ap;va_start(ap,nr);int base=va_arg(ap,int);const char *path=va_arg(ap,const char *);
 struct open_how *how=va_arg(ap,struct open_how *);size_t size=va_arg(ap,size_t);va_end(ap);
 assert(size==sizeof(*how)&&(how->flags&O_DIRECTORY));
 assert((how->resolve&(RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS))==(RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS));
 if(fault())return -1;
 if(base==AT_FDCWD){
  const char *names[]={"/sourceZ","/ramZ","/sourceX","/ramX","/sourceY","/ramY"};
  for(int i=0;i<6;i++)if(!strcmp(path,names[i]))return newfd(i%2?RAM:ROOT,i/2);
  assert(false);
 }
 struct fd *f=owned(base);assert(!strcmp(path,"md")&&(f->kind==ROOT||f->kind==RAM));
 assert((how->resolve&(RESOLVE_BENEATH|RESOLVE_NO_XDEV))==(RESOLVE_BENEATH|RESOLVE_NO_XDEV));
 if(mode==7&&f->index==bad_source){errno=ELOOP;return -1;}
 return newfd(f->kind==RAM?RAMMD:MD,f->index);
}
static int fake_ioctl(int fd,unsigned long cmd,...){
 struct fd *f=owned(fd);va_list ap;va_start(ap,cmd);void *arg=va_arg(ap,void *);va_end(ap);
 if(f->kind==BLOCK){assert(cmd==BLKGETSIZE64);if(fault())return -1;*(uint64_t *)arg=sizes[f->index];return 0;}
 assert(f->kind==PROVIDER&&cmd==MT6797_MD1_FS_REGISTER_RAM_ROOTS);registered++;
 struct mt6797_md1_fs_ram_roots *r=arg;assert(r->version==1&&!r->flags&&!r->reserved);
 for(int i=0;i<3;i++){struct fd *dir=owned(r->directory_fd[i]);assert(dir->kind==RAMMD&&dir->index==i);}
 if(fault())return -1;
 if(mode==9){errno=EINTR;return -1;}return 0;
}
static void reset(int m,int bad,int fail){
 memset(fds,0,sizeof(fds));nextfd=1;calls=registered=provider_opens=super_reads=0;mode=m;bad_source=bad;fail_at=fail;
}
static void drained(void){for(int i=1;i<nextfd;i++)assert(!fds[i].live);}
int main(void){
 char *paths[]={"/sourceZ","/ramZ","/sourceX","/ramX","/sourceY","/ramY"};
 reset(0,0,0);assert(!register_ram_roots(paths)&&registered==1&&provider_opens==1);drained();int count=calls;
 for(int i=1;i<=count;i++){reset(0,0,i);assert(register_ram_roots(paths)<0&&registered<=1);drained();}
 for(int m=1;m<=8;m++)for(int bad=0;bad<3;bad++){
  reset(m,bad,0);assert(register_ram_roots(paths)<0&&!registered&&!provider_opens);drained();
 }
 reset(9,0,0);assert(register_ram_roots(paths)==-EINTR&&registered==1);drained();
 /* Swapping X/Y source mount arguments cannot pass source block identity. */
 char *tmp=paths[2];paths[2]=paths[4];paths[4]=tmp;
 reset(0,0,0);assert(register_ram_roots(paths)<0&&!registered);drained();
 for(int i=0;i<3;i++){
  reset(0,0,0);assert(!source_authority(&sources[i],NULL,0,NULL,NULL)&&!provider_opens&&super_reads==1);drained();
 }
 puts("PASS actual three-root helper: all syscall failures drain FDs, distinct roots and fixed identities, dirty/disk/quota/alias/symlink/writable-source refusals, swapped-source refusal, metadata-only prechecks, exactly one ioctl and no EINTR retry");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-roots-helper-') as directory:
    p=Path(directory)
    (p/'test.c').write_text(shim+'\n#include "'+str(helper)+'"\n'+tests)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
