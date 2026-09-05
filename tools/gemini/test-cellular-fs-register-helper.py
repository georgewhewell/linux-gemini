#!/usr/bin/env python3
"""Compile actual helper with metadata/syscall adapters; never mount or ioctl."""
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
helper=root/'tools/gemini/mt6797-fs-register.c'
shim=r'''
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
static int mode,registered,opened[8],closed[8],super_reads;
static int fake_open(const char *path,int flags,...);
static int fake_close(int fd);
static ssize_t fake_read(int fd,void *out,size_t n);
static ssize_t fake_pread(int fd,void *out,size_t n,off_t off);
static int fake_fstat(int fd,struct stat *st);
static int fake_fstatfs(int fd,struct statfs *st);
static int fake_fstatvfs(int fd,struct statvfs *st);
static int fake_ioctl(int fd,unsigned long cmd,...);
static long fake_syscall(long number,...);
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
tests=r'''
#undef main
static int fake_open(const char *path,int flags,...){int fd;
 assert(flags&O_CLOEXEC);assert(flags&O_NOFOLLOW);
 if(!strcmp(path,"/dev/mmcblk0p6")){fd=1;assert((flags&O_ACCMODE)==O_RDONLY);}
 else if(!strcmp(path,"/sys/dev/block/179:6/uevent"))fd=2;
 else if(!strcmp(path,"/sys/dev/block/179:6/size"))fd=3;
 else {assert(!strcmp(path,"/dev/mt6797-md1-fs-provider"));fd=6;}
 opened[fd]++;return fd;}
static int fake_close(int fd){assert(fd>0&&fd<8&&closed[fd]<opened[fd]);closed[fd]++;return 0;}
static ssize_t fake_read(int fd,void *out,size_t n){
 const char *s=fd==2?"MAJOR=179\nMINOR=6\nPARTN=6\nPARTNAME=nvdata\n":"65536\n";
 if(mode==1&&fd==2)s="PARTN=6\nPARTNAME=userdata\n";
 if(mode==2&&fd==3)s="65535\n";
 assert(strlen(s)<n);memcpy(out,s,strlen(s));return strlen(s);}
static ssize_t fake_pread(int fd,void *out,size_t n,off_t off){
 assert(fd==1&&n==1024&&off==1024);super_reads++;unsigned char *b=out;memset(b,0,n);
 b[5]=0x20; /* actual observed geometry8192 blocks, not free count */
 b[24]=2;b[56]=0x53;b[57]=0xef;b[58]=1;
 if(mode==3)b[58]=0;
 if(mode==4)b[96]=4;
 if(mode==5)b[56]=0;
 if(mode==14)return 1023;
 if(mode==15)b[0xe8]=1;
 if(mode==16)b[0x66]=1;
 return n;}
static int fake_fstat(int fd,struct stat *st){memset(st,0,sizeof(*st));
 if(fd==1){st->st_mode=S_IFBLK;st->st_rdev=makedev(179,6);}
 else {assert(fd==4||fd==5);st->st_mode=S_IFDIR;st->st_dev=makedev(179,6);st->st_ino=fd==4?2:13;
  if(mode==8)st->st_dev=makedev(179,29);
  if(mode==9&&fd==4)st->st_ino=42;}
 return 0;}
static int fake_fstatfs(int fd,struct statfs *st){assert(fd==4);memset(st,0,sizeof(*st));
 st->f_type=mode==10?0:EXT4_SUPER_MAGIC;return 0;}
static int fake_fstatvfs(int fd,struct statvfs *st){assert(fd==4);memset(st,0,sizeof(*st));
 st->f_flag=mode==6?0:ST_RDONLY;return 0;}
static long fake_syscall(long number,...){
 va_list ap;va_start(ap,number);int parent=va_arg(ap,int);const char *name=va_arg(ap,const char *);
 struct open_how *how=va_arg(ap,struct open_how *);size_t size=va_arg(ap,size_t);va_end(ap);
 assert(number==SYS_openat2&&size==sizeof(*how));
 assert((how->flags&O_ACCMODE)==O_RDONLY&&(how->flags&O_DIRECTORY));
 assert((how->resolve&(RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS))==(RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS));
 if(parent==AT_FDCWD){assert(!strcmp(name,"/run/private/nvdata"));opened[4]++;return 4;}
 assert(parent==4&&!strcmp(name,"md"));assert((how->resolve&(RESOLVE_BENEATH|RESOLVE_NO_XDEV))==(RESOLVE_BENEATH|RESOLVE_NO_XDEV));
 if(mode==7||mode==11){errno=mode==7?ENOENT:ELOOP;return -1;}
 opened[5]++;return 5;
}
static int fake_ioctl(int fd,unsigned long cmd,...){
 va_list ap;va_start(ap,cmd);void *arg=va_arg(ap,void *);va_end(ap);
 if(fd==1){assert(cmd==BLKGETSIZE64);*(uint64_t *)arg=mode==12?33553920:33554432;return 0;}
 assert(fd==6&&cmd==MT6797_MD1_FS_REGISTER);registered++;
 struct mt6797_md1_fs_registration *r=arg;
 assert(r->version==1&&r->directory_fd==5&&!r->flags&&!r->reserved);
 assert(opened[5]==1&&!closed[5]);
 if(mode==13){errno=EINTR;return -1;}
 return 0;
}
int main(void){
 assert(!partition_identity("PARTN=6\nPARTNAME=nvdata\n","65536\n"));
 assert(partition_identity("PARTN=6\nPARTNAME=nvdata\nPARTNAME=nvdata\n","65536\n")<0);
 assert(partition_identity("PARTN=7\nPARTNAME=nvdata\n","65536\n")<0);
 assert(partition_identity("PARTN=6\nPARTNAME=nvdata","65536\n")<0);
 for(mode=0;mode<=16;mode++){
  registered=super_reads=0;memset(opened,0,sizeof(opened));memset(closed,0,sizeof(closed));
  int ret=register_nvdata("/run/private/nvdata");
  assert((mode==0&&!ret)||(mode!=0&&ret<0));
  assert(registered==((mode==0||mode==13)?1:0));
  for(unsigned i=0;i<8;i++)assert(opened[i]==closed[i]);
  assert(super_reads<=1);
 }
 for(mode=0;mode<=16;mode++){
  registered=super_reads=0;memset(opened,0,sizeof(opened));memset(closed,0,sizeof(closed));
  int ret=register_nvdata(NULL);
  bool reject=(mode>=1&&mode<=5)||mode==12||mode>=14;
  assert((ret<0)==reject&&!registered&&!opened[4]&&!opened[5]&&!opened[6]);
  for(unsigned i=0;i<8;i++)assert(opened[i]==closed[i]);
 }
 puts("PASS actual one-shot FS helper: metadata-only reads, label/size/ext4/dirty/recovery/root/RO/missing-md/symlink refusals, no mount, one ioctl even EINTR, complete FD cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-fs-register-helper-') as d:
    p=Path(d);(p/'test.c').write_text(shim+'\n#include "'+str(helper)+'"\n'+tests)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
