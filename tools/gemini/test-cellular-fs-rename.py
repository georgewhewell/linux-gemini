#!/usr/bin/env python3
"""Run production rename provider with instrumented VFS boundaries, ASan/UBSan.

This checks ownership and confinement, not real-kernel rename correctness.
"""
import ast
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
tree=ast.parse((root/'tools/gemini/test-cellular-fs-mutations.py').read_text())
shim=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='shim' for t in n.targets))
# Reuse instrumentation of existing VFS boundaries, allowing two retained roots
# and independently faulting the second parent/allocation acquisition.
shim=shim.replace('static int got_op;', 'static int got_op, root_calls, fail_root, allocation_calls, fail_allocation, walk_calls, fail_walk;\nstatic bool cross_mount;\nstatic struct file otherroot;')
a=shim.index('static struct file *mt6797_fs_root_get(');b=shim.index('static void fput',a)
shim=shim[:a]+'''static struct file *mt6797_fs_root_get(u8 drive){got_drive=drive;root_calls++;if(!available||root_calls==fail_root)return NULL;rootrefs++;return cross_mount&&root_calls==2?&otherroot:&rootfile;}
'''+shim[b:]
shim=shim.replace('f==&rootfile&&rootrefs==1','(f==&rootfile||f==&otherroot)&&rootrefs>0').replace('assert(f==&rootfile);return writable','assert(f==&rootfile||f==&otherroot);return writable')
shim=shim.replace('if(alloc_fail)return NULL;', 'allocation_calls++;if(alloc_fail||allocation_calls==fail_allocation)return NULL;').replace('allocs==1','allocs>0')
shim=shim.replace('strcpy(got_parent,p);if(walk_error)', 'strcpy(got_parent,p);walk_calls++;if(walk_calls==fail_walk)return ERR_PTR(-EACCES);if(walk_error)')
source=(root/'drivers/soc/mediatek/mt6797-md1-fs-provider.c').read_text()
source=source[source.index('static struct dentry *mt6797_fs_rename_parent('):source.index('EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_rename_drive)')]
extra=r'''
#define LOOKUP_NO_SYMLINKS 8
struct renamedata {void *mnt_idmap;struct dentry *old_parent,*new_parent,*old_dentry,*new_dentry;void *delegated_inode;unsigned flags;};
static int start_error,rename_error,starts,ends,renames;
static int source_type,target_type;
static bool source_mount,target_mount,source_foreign,target_foreign,same_name;
static char old_leaf[256],new_leaf[256];
static int start_renaming(struct renamedata *rd,int flags,struct qstr *old,struct qstr *new){
 assert(flags==LOOKUP_NO_SYMLINKS&&rd->flags==0&&!rd->delegated_inode&&writers==1&&!locked);
 starts++;memcpy(old_leaf,old->name,old->len);old_leaf[old->len]=0;memcpy(new_leaf,new->name,new->len);new_leaf[new->len]=0;
 if(start_error)return start_error;
 locked=1;rd->old_parent->refs++;
 child.type=source_type;alternate.type=target_type;
 child.mounted=source_mount;alternate.mounted=target_mount;
 child.d_sb=source_foreign?NULL:&rootfile;alternate.d_sb=target_foreign?NULL:&rootfile;
 rd->old_dentry=&child;rd->new_dentry=same_name?&child:&alternate;
 rd->old_dentry->refs++;rd->new_dentry->refs++;return 0;
}
static int vfs_rename(struct renamedata *rd){assert(locked==1&&writers==1&&rd->old_dentry==&child);renames++;return rename_error;}
static void end_renaming(struct renamedata *rd){assert(locked==1);ends++;dput(rd->old_dentry);dput(rd->new_dentry);dput(rd->old_parent);locked=0;}
static void setup(void){reset();root_calls=fail_root=allocation_calls=fail_allocation=walk_calls=fail_walk=0;cross_mount=false;rootfile.f_path.mnt=&rootfile;otherroot.f_path.mnt=&otherroot;otherroot.inode.i_sb=&otherroot;start_error=rename_error=starts=ends=renames=0;source_type=2;target_type=0;source_mount=target_mount=source_foreign=target_foreign=same_name=false;}
'''
tests=r'''
static void check(const char *old,const char *new,int expected,int effects){int r=mt6797_md1_fs_provider_rename_drive('Z',old,'X',new);assert(r==expected&&renames==effects);assert(!rootrefs&&!allocs&&!writers&&!locked&&!parent.refs&&!child.refs&&!alternate.refs);assert(ends==(starts&&!start_error?1:0));}
int main(void){
 setup();check("a/old","b/new",0,1);assert(!strcmp(old_leaf,"old")&&!strcmp(new_leaf,"new"));
 setup();target_type=2;check("old","existing",0,1);
 setup();source_type=target_type=1;check("old","existing",0,1);
 setup();same_name=true;check("same","same",0,1);
 int errors[]={-ENOENT,-EACCES,-ENOTEMPTY,-EINVAL,-ENOSPC,-EIO};
 for(unsigned i=0;i<sizeof(errors)/sizeof(*errors);i++){
  setup();rename_error=errors[i];check("old","new",errors[i],1);
  setup();start_error=errors[i];check("old","new",errors[i],0);
 }
 setup();fail_root=1;check("old","new",-ENODEV,0);
 setup();fail_root=2;check("old","new",-ENODEV,0);
 setup();provider_volatile=false;check("old","new",-EROFS,0);
 setup();writable=false;check("old","new",-EROFS,0);
 setup();cross_mount=true;check("old","new",-EXDEV,0);
 for(int i=1;i<=2;i++){
  setup();fail_allocation=i;check("old","new",-ENOMEM,0);
  setup();fail_walk=i;check("a/old","b/new",-EACCES,0);
 }
 setup();parent.type=2;check("old","new",-ENOTDIR,0);
 setup();write_error=-EROFS;check("old","new",-EROFS,0);
 setup();drift=true;check("old","new",-EACCES,0);
 setup();source_mount=true;check("old","new",-EACCES,0);
 setup();target_mount=true;check("old","new",-EACCES,0);
 setup();source_foreign=true;check("old","new",-EACCES,0);
 setup();target_foreign=true;check("old","new",-EACCES,0);
 setup();source_type=0;check("old","new",-ENOENT,0);
 for(int i=3;i<=4;i++){
  setup();source_type=i;check("old","new",-EACCES,0);
  setup();target_type=i;check("old","new",-EACCES,0);
 }
 const char *bad[]={"","/name","name/",".","..","a/.","a/.."};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){
  setup();check(bad[i],"new",-EINVAL,0);
  setup();check("old",bad[i],-EINVAL,0);
 }
 setup();check(NULL,"new",-ENAMETOOLONG,0);
 setup();check("old",NULL,-ENAMETOOLONG,0);
 char longpath[1026];memset(longpath,'a',1025);longpath[1025]=0;
 setup();check(longpath,"new",-ENAMETOOLONG,0);
 setup();check("old",longpath,-ENAMETOOLONG,0);
 puts("actual RENAME provider: both roots, final confinement, replacement/same-name dispatch and every resource unwind PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(shim+extra+source+tests)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-g',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
