#!/usr/bin/env python3
"""Actual GETATTR codec and provider lookup with metadata-only VFS adapters."""
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
    return source[start:end]
rpc = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_rpc.h').read_text()
rpc = rpc[rpc.index('static u32 mt6797_rpc_read('):rpc.index('static int mt6797_rpc_eint_reply(')]
codec = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_fs.h').read_text()
codec = codec[:codec.index('/* Complete, unfragmented GETDISKINFO')] + '\n#endif\n'
shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef int32_t s32;
#define MNT_READONLY 1
#define SB_RDONLY 1
#define MT6797_FS_CONSUMED 3
#define READ_ONCE(v) (v)
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((intptr_t)(p))
#define QSTR_INIT(n,l) {n,l}
#define STATX_MODE 2
#define AT_STATX_DONT_SYNC 0x4000
struct super_block {int s_flags;};
struct inode {struct super_block *i_sb;};
struct dentry {struct super_block *d_sb;int kind,refs;bool mounted;};
struct vfsmount {int mnt_flags;};
struct path {struct vfsmount *mnt;struct dentry *dentry;};
struct file {struct path f_path;struct inode inode;int refs;};
struct kstat {unsigned mode;};
struct qstr {const char *name;size_t len;};
static struct {int stage;} provider_state;
static struct super_block sb,other;
static struct dentry parent,child;
static struct vfsmount mount;
static struct file backing,*provider_file;
static int scenario,lookups,stats,sleeps;
static bool locked;
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!locked);locked=true;}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);assert(locked);locked=false;}while(0)
static void might_sleep(void){assert(!locked);sleeps++;}
static struct inode *file_inode(struct file *f){return &f->inode;}
static struct file *get_file(struct file *f){assert(locked);f->refs++;return f;}
static void fput(struct file *f){assert(!locked);assert(f->refs==2);f->refs--;}
static void dput(struct dentry *d){assert(!locked&&d->refs==1);d->refs--;}
static bool d_mountpoint(struct dentry *d){return d->mounted;}
static bool d_is_symlink(struct dentry *d){return d->kind==3;}
static bool d_is_reg(struct dentry *d){return d->kind==1;}
static bool d_is_dir(struct dentry *d){return d->kind==2;}
static void *mnt_idmap(struct vfsmount *m){assert(m==&mount);return NULL;}
static struct dentry *lookup_one_positive_unlocked(void *id,struct qstr *q,struct dentry *d){
 assert(!locked&&!id&&d==&parent&&backing.refs==2);lookups++;
 assert(q->len==strlen(q->name)&&!strchr(q->name,'/'));
 if(scenario==1)return (void *)(intptr_t)-ENOENT;
 if(scenario==2)return (void *)(intptr_t)-EACCES;
 if(scenario==3)return (void *)(intptr_t)-EIO;
 child.refs++;if(scenario==4)mount.mnt_flags=0;
 return &child;
}
static int vfs_getattr(struct path *p,struct kstat *s,int mask,int flags){
 assert(!locked&&p->mnt==&mount&&p->dentry==&child&&mask==2&&flags==0x4000);stats++;
 s->mode=0100644;if(scenario==5)return -EIO;
 if(scenario==6)sb.s_flags=0;
 return 0;
}
static void reset(int n){
 scenario=n;lookups=stats=sleeps=0;locked=false;
 sb.s_flags=1;mount.mnt_flags=1;provider_state.stage=3;
 child=(struct dentry){.d_sb=&sb,.kind=1};
 backing=(struct file){.f_path={&mount,&parent},.inode={&sb},.refs=1};provider_file=&backing;
}
'''
tests = r'''
int main(void){
 u8 in[68]={0},reply[32],expected[32]={0};struct mt6797_fs_attr_request request;
 const char *name="FATC0000000.log";unsigned mode;
 in[4]=68;in[8]=14;in[11]=128;in[16]=16;in[17]=16;in[20]=1;in[24]=38;
 in[28]='Z';in[30]=':';in[32]='\\';
 for(unsigned i=0;i<strlen(name);i++)in[34+i*2]=name[i];
 assert(!mt6797_fs_attr_parse(in,68,&request)&&!strcmp(request.name,name)&&request.index==0);
 for(unsigned n=0;n<68;n++)assert(mt6797_fs_attr_parse(in,n,&request)<0);
 for(unsigned off=0;off<28;off+=4){u8 saved[4];memcpy(saved,in+off,4);memset(in+off,255,4);
   assert(mt6797_fs_attr_parse(in,68,&request)<0);memcpy(in+off,saved,4);}
 for(unsigned n=34;n<64;n+=2){u8 old=in[n];for(unsigned c=0;c<4;c++){
   in[n]=(u8[]){0,'/','\\',':'}[c];assert(mt6797_fs_attr_parse(in,68,&request)<0);}
   in[n]=old;in[n+1]=1;assert(mt6797_fs_attr_parse(in,68,&request)<0);in[n+1]=0;}
 mt6797_fs_attr_reply(4,-9,reply);
 expected[4]=32;expected[8]=15;expected[12]=4;expected[16]=16;expected[17]=16;
 expected[18]=expected[19]=255;expected[20]=1;expected[24]=4;
 expected[28]=247;expected[29]=expected[30]=expected[31]=255;
 assert(!memcmp(reply,expected,32));
 assert(mt6797_fs_attr_result(-ENOENT,0)==-9&&mt6797_fs_attr_result(-EACCES,0)==-16);
 assert(mt6797_fs_attr_result(-EMFILE,0)==-5&&mt6797_fs_attr_result(-EIO,0)==-18);
 for(unsigned m=0;m<0100000;m++){
   int want=((m&219)==0&&(m&292)!=0)|((m&0170000)==0040000?16:0);
   assert(mt6797_fs_attr_result(0,m)==want);
 }
 for(int n=0;n<=6;n++){
   reset(n);int ret=mt6797_md1_fs_provider_getattr(name,&mode);
   assert((ret==0)==(n==0));assert(backing.refs==1&&!child.refs&&!locked);
   assert(lookups==1&&sleeps==1);if(!n)assert(mode==0100644&&stats==1);
 }
 for(int n=0;n<5;n++){
   reset(0);if(n==0)child.mounted=true;if(n==1)child.kind=3;
   if(n==2)child.kind=4;
   if(n==3)child.d_sb=&other;
   if(n==4)provider_state.stage=2;
   assert(mt6797_md1_fs_provider_getattr(name,&mode)<0&&!stats&&backing.refs==1&&!child.refs);
 }
 const char *bad[]={"", ".", "..", "../a", "a/b", "a\\b", "Z:escape"};
 for(unsigned n=0;n<sizeof(bad)/sizeof(*bad);n++){
   reset(0);assert(mt6797_md1_fs_provider_getattr(bad[n],&mode)<0&&!lookups&&backing.refs==1);
 }
 puts("PASS actual GETATTR: literal68/32 codec, malformed/path bounds, all32768 mode values, errno mapping, retained-root lookup/refcounts/RO/mount/symlink/special refusals");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-fs-getattr-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(shim + rpc + codec + function('mt6797_fs_readonly') + '\n' + function('mt6797_md1_fs_provider_getattr') + tests)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
