#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual four-root authority, V confinement, space, file limits and durability."""
import ast,os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'drivers/soc/mediatek/mt6797-md1-fs-provider.c').read_text()
header=(ROOT/'drivers/soc/mediatek/mt6797-md3-data-root.h').read_text()
def fn(name):
 m=re.search(r'^(?:static )?[^\n]+\b'+name+r'\([^;{]*\{.*?^}',source,re.M|re.S);assert m,name;return m.group()+'\n'
tree=ast.parse((ROOT/'tools/gemini/test-cellular-md3-fs.py').read_text())
parts={n.targets[0].id:ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and isinstance(n.targets[0],ast.Name) and n.targets[0].id in ('shim','functions','tests')}
shim=parts['shim'].replace('stat_calls, checks','stat_calls');a=shim.index('/* V is deliberately absent');b=shim.index('static int mt6797_fs_md3_cancel',a);shim=shim[:a]+shim[b:]
shim=shim.replace('struct inode { struct super_block *i_sb; };','struct inode { struct super_block *i_sb; unsigned i_uid,i_gid,i_mode,i_nlink; };')
shim=shim.replace('struct vfsmount { unsigned mnt_flags; };','struct vfsmount { unsigned mnt_flags; struct dentry *mnt_root; bool mapped; };')
shim=shim.replace('uint64_t f_blocks, f_bfree, f_files;', 'uint64_t f_blocks, f_bfree, f_files, f_bavail, f_ffree;')
shim=shim.replace('original[3], candidate[3]', 'original[3], candidate[4]').replace('mounts[3],', 'mounts[4],').replace('devices[3]', 'devices[4]').replace('super[3]', 'super[4]')
shim=shim.replace('static bool spin, inode_held;', 'static bool spin; static unsigned inode_held;')
a=shim.index('static void inode_lock(');b=shim.index('static sector_t',a);shim=shim[:a]+'''static void inode_lock(struct inode*i){(void)i;assert(!spin&&!inode_held);inode_held++;}
static void inode_lock_nested(struct inode*i,int n){(void)i;(void)n;assert(inode_held==1);inode_held++;}
static void inode_unlock(struct inode*i){(void)i;assert(inode_held);inode_held--;}
'''+shim[b:]
shim=shim.replace('fd>22','fd>23');a=shim.index('static int vfs_statfs(');b=shim.index('static int restore_calls;',a)
shim=shim[:a]+'''static int vfs_statfs(struct path*p,struct kstatfs*out){
 assert(!spin&&!inode_held);stat_calls++;
 *out=(struct kstatfs){.f_type=EXT4_SUPER_MAGIC,.f_bsize=4096,.f_frsize=4096,
 .f_blocks=p->dentry->inode.i_sb->s_bdev->sectors/8,.f_bfree=32768,
 .f_bavail=32768,.f_files=1000000,.f_ffree=900000};
 if(p->dentry->inode.i_sb==&super[3]){
  if(mode==1)assert(!mt6797_fs_md3_cancel());
  if(mode==2)mounts[3].mnt_flags=MNT_READONLY;
  if(mode==3)out->f_bavail=16384;
  if(mode==4)return -EIO;
  if(mode==5)out->f_ffree=128;
  if(mode==6)out->f_bavail=out->f_bfree+1;
  if(mode==7)out->f_blocks++;
 }else {out->f_bfree=out->f_bavail=7;out->f_files=4096;out->f_ffree=2048;}
 return 0;
}
'''+shim[b:]
shim+='''
#define GLOBAL_ROOT_UID 0
#define GLOBAL_ROOT_GID 0
#define uid_eq(a,b) ((a)==(b))
#define gid_eq(a,b) ((a)==(b))
#define smp_load_acquire(p) (*(p))
#define DIV_ROUND_UP_ULL(n,d) (((uint64_t)(n)+(d)-1)/(d))
#define I_MUTEX_CHILD 1
static int nop_mnt_idmap,other_idmap;
static void*mnt_idmap(struct vfsmount*m){return m->mapped?&other_idmap:&nop_mnt_idmap;}
static bool d_is_reg(struct dentry*d){return !d->dir;}
static bool is_subdir(struct dentry*d,struct dentry*root){for(unsigned i=0;i<10;i++){if(d==root)return true;if(d==d->d_parent)break;d=d->d_parent;}return false;}
#define might_sleep() ((void)0)
typedef uint64_t u64;
static unsigned writes,flushes;
static int write_error,flush_error;
static ssize_t kernel_write(struct file*f,const void*p,size_t n,loff_t*pos){(void)f;(void)p;writes++;if(write_error)return write_error;*pos+=n;return n;}
static int vfs_fsync(struct file*f,int n){(void)f;assert(!n);flushes++;return flush_error;}
'''
shim += r'''
#define __user
#define CAP_SYS_ADMIN 21
static int init_user_ns;
static bool admin=true;
static bool ns_capable(void*n,int c){assert(n==&init_user_ns&&c==21);return admin;}
static int copy_from_user(void*d,const void*s,size_t n){if(!s)return 1;memcpy(d,s,n);return 0;}
static int mt6797_fs_cancel_registration(void){assert(!"MD1 cancellation in MD3 test");return -EOPNOTSUPP;}
static int mt6797_fs_register_fds(const int*f,unsigned n,unsigned r){(void)f;(void)n;(void)r;assert(!"MD1 registration in MD3 test");return -EOPNOTSUPP;}
static int mt6797_fs_register_archive(int f){(void)f;assert(!"MD1 archive in MD3 test");return -EOPNOTSUPP;}
static int mt6797_fs_register_fd(int f,unsigned r){(void)f;(void)r;assert(!"MD1 root in MD3 test");return -EOPNOTSUPP;}
'''
shim = shim.replace('#define MT6797_MD1_FS_BACKING_INTERNAL 2U', '')
shim += (ROOT/'include/uapi/linux/mt6797_md1_fs.h').read_text()
state=(ROOT/'drivers/soc/mediatek/mt6797-md1-fs-state.h').read_text()
reset=parts['tests'][parts['tests'].index('static void reset(void)'):parts['tests'].index('static void balanced')]
functions='\n'.join(fn(n) for n in parts['functions'])+fn('mt6797_fs_sync_file')+fn('mt6797_md1_fs_provider_write')+fn('mt6797_fs_ioctl')
test=r'''
static struct dentry top,parent,data,child,outside;
static unsigned cases;
static void setup(void){
 reset();md3_data_live=false;writes=flushes=write_error=flush_error=0;
 devices[3]=(struct block_device){MKDEV(179,32),22249472};
 top=(struct dentry){.dir=true,.inode={.i_sb=&super[3],.i_mode=0755},.d_parent=&top};
 parent=(struct dentry){.dir=true,.inode={.i_sb=&super[3],.i_mode=0700},.d_parent=&top,.d_name={15,"gemini-cellular"}};
 data=(struct dentry){.dir=true,.inode={.i_sb=&super[3],.i_mode=0700},.d_parent=&parent,.d_name={8,"md3-data"}};
 child=(struct dentry){.inode={.i_sb=&super[3],.i_mode=0600,.i_nlink=1},.d_parent=&data};
 outside=child;outside.d_parent=&top;
 super[3]=(struct super_block){.s_magic=EXT4_SUPER_MAGIC,.s_bdev=&devices[3],.s_root=&top};
 mounts[3]=(struct vfsmount){.mnt_root=&top};
 candidate[3]=(struct file){.f_path={&mounts[3],&data}};cases++;
}
static void balanced4(bool kept){
 assert(!spin&&!inode_held&&!top.refs&&!parent.refs&&!data.refs);
 for(unsigned i=0;i<4;i++)assert(candidate[i].refs==(kept?1:0));
 for(unsigned i=0;i<3;i++)assert(original[i].refs==1);
}
static const int four[]={20,21,22,23};
static void claim(void){assert(!mt6797_fs_md3_register_roots(four,4));assert(!mt6797_fs_md3_data_writable(&candidate[3]));assert(!mt6797_md3_fs_provider_claim());assert(md3_data_live);balanced4(true);}
int main(void){

 struct mt6797_md3_fs_roots abi={.version=1,.directory_fd={20,21,22,23}};
 _Static_assert(sizeof(abi)==28,"native/compat fixed ABI");
 setup();admin=false;assert(mt6797_fs_ioctl(NULL,MT6797_MD3_FS_REGISTER_ROOTS,(unsigned long)&abi)==-EPERM);admin=true;balanced4(false);
 setup();assert(mt6797_fs_ioctl(NULL,MT6797_MD3_FS_REGISTER_ROOTS,0)==-EFAULT);balanced4(false);
 for(unsigned n=0;n<3;n++){setup();if(n==0)abi.version=2;if(n==1)abi.flags=1;if(n==2)abi.reserved=1;
  assert(mt6797_fs_ioctl(NULL,MT6797_MD3_FS_REGISTER_ROOTS,(unsigned long)&abi)==-EINVAL);balanced4(false);assert(md3_state.stage==MT6797_FS_EMPTY);abi.version=1;abi.flags=abi.reserved=0;}
 setup();assert(!mt6797_fs_ioctl(NULL,MT6797_MD3_FS_REGISTER_ROOTS,(unsigned long)&abi));balanced4(true);
 assert(mt6797_fs_ioctl(NULL,MT6797_MD3_FS_REGISTER_ROOTS,(unsigned long)&abi)==-EALREADY);assert(!mt6797_fs_ioctl(NULL,MT6797_MD3_FS_CANCEL,0));balanced4(false);
 for(unsigned n=0;n<4;n++){setup();abi.directory_fd[n]=-1;assert(mt6797_fs_ioctl(NULL,MT6797_MD3_FS_REGISTER_ROOTS,(unsigned long)&abi)==-EBADF);balanced4(false);abi.directory_fd[n]=20+n;}
 setup();assert(!mt6797_fs_md3_register_roots(four,4));balanced4(true);
 assert(!md3_data_live&&!mt6797_fs_md3_data_writable(&candidate[3])&&!mt6797_fs_root_get_for(2,'V'));
 assert(mt6797_fs_md3_register_roots(four,4)==-EALREADY);
 assert(!mt6797_md3_fs_provider_claim());assert(mt6797_md3_fs_provider_claim()==-EALREADY);
 assert(mt6797_fs_md3_cancel()==-EBUSY);assert(md3_data_live);
 struct file*f=mt6797_fs_root_get_for(2,'V');assert(f==&candidate[3]);fput(f);
 assert(!mt6797_fs_root_get_for(0,'V')&&!mt6797_fs_root_get_for(2,'W'));
 balanced4(true);
 struct file regular={.f_path={&mounts[3],&child},.f_mode=FMODE_WRITE};
 assert(mt6797_fs_md3_data_writable(&regular));regular.f_path.dentry=&outside;assert(!mt6797_fs_md3_data_writable(&regular));regular.f_path.dentry=&child;
 child.inode.i_nlink=2;assert(!mt6797_fs_md3_data_writable(&regular));assert(!mt6797_fs_md3_data_entry(&candidate[3],&child));child.inode.i_nlink=1;
 struct mt6797_md1_fs_stats st;assert(!mt6797_fs_md3_data_stats(&candidate[3],&st));assert(st.block_size==4096&&st.blocks==22249472/8&&st.free_blocks==16384);
 loff_t pos=0;char bytes[8]={0};assert(mt6797_md1_fs_provider_write(&regular,bytes,8,&pos)==8&&pos==8&&writes==1&&flushes==1);
 flush_error=-EIO;assert(mt6797_md1_fs_provider_write(&regular,bytes,8,&pos)==-EIO&&writes==2&&flushes==2);flush_error=0;
 write_error=-ENOSPC;assert(mt6797_md1_fs_provider_write(&regular,bytes,8,&pos)==-ENOSPC&&writes==3&&flushes==2);write_error=0;
 mode=3;assert(!mt6797_fs_md3_data_stats(&candidate[3],&st)&&!st.free_blocks);assert(mt6797_md1_fs_provider_write(&regular,bytes,8,&pos)==-ENOSPC&&writes==3);
 mode=5;assert(mt6797_md1_fs_provider_write(&regular,bytes,8,&pos)==-ENOSPC&&writes==3);
 mode=4;assert(mt6797_md1_fs_provider_write(&regular,bytes,8,&pos)==-EIO&&writes==3);
 mode=0;pos=16*1024*1024;assert(mt6797_md1_fs_provider_write(&regular,bytes,1,&pos)==-EFBIG&&writes==3);
 mounts[3].mnt_flags=MNT_READONLY;assert(!mt6797_fs_root_get_for(2,'V'));assert(mt6797_md1_fs_provider_write(&regular,bytes,1,&pos)==-EROFS);balanced4(true);
 for(unsigned n=0;n<21;n++){
  setup();
  switch(n){
  case 0:devices[3].bd_dev=MKDEV(179,96);break;
  case 1:devices[3].bd_dev=MKDEV(179,6);break;
  case 2:devices[3].sectors++;break;
  case 3:super[3].s_magic=TMPFS_MAGIC;break;
  case 4:mounts[3].mnt_flags=MNT_READONLY;break;
  case 5:super[3].s_flags=SB_RDONLY;break;
  case 6:mounts[3].mapped=true;break;
  case 7:candidate[3].f_mode=FMODE_WRITE;break;
  case 8:data.inode.i_uid=1000;break;
  case 9:data.inode.i_gid=1000;break;
  case 10:data.inode.i_mode=0770;break;
  case 11:parent.inode.i_mode=0777;break;
  case 12:top.inode.i_mode=0777;break;
  case 13:data.d_name.name="mdx-data";break;
  case 14:parent.d_name.name="gemini-cellulaz";break;
  case 15:mounts[3].mnt_root=&parent;break;
  case 16:data.mountpoint=true;break;
  case 17:parent.mountpoint=true;break;
  case 18:mode=6;break;
  case 19:mode=7;break;
  case 20:mode=4;break;
  }
  assert(mt6797_fs_md3_register_roots(four,4)<0);balanced4(false);assert(!md3_data_live&&md3_state.stage==MT6797_FS_CANCELLED);
 }
 setup();mode=1;assert(mt6797_fs_md3_register_roots(four,4)==-ECANCELED);balanced4(false);assert(!md3_data_live);
 setup();assert(!mt6797_fs_md3_register_roots(four,4));data.inode.i_mode=0770;assert(mt6797_md3_fs_provider_claim()==-ENODEV&&!md3_data_live);assert(!mt6797_fs_md3_cancel());balanced4(false);
 setup();mode=2;assert(mt6797_fs_md3_register_roots(four,4)==-EROFS);balanced4(false);
 setup();assert(!mt6797_fs_md3_register_roots(four,4));assert(!mt6797_fs_md3_cancel());balanced4(false);assert(!md3_data_live&&mt6797_fs_md3_register_roots(four,4)==-EALREADY);
 setup();provider_state.stage=MT6797_FS_READY;assert(mt6797_fs_md3_register_roots(four,4)==-ENODEV);balanced4(false);assert(md3_state.stage==MT6797_FS_EMPTY);
 setup();assert(!mt6797_fs_md3_register(four));assert(!mt6797_md3_fs_provider_claim());assert(!md3_data_live&&!mt6797_fs_root_get_for(2,'V'));
 for(unsigned n=0;n<3;n++)assert(candidate[n].refs==1);assert(!candidate[3].refs);
 printf("PASS %u actual V authority scenarios; exact p32 private root, no MD1/W alias, atomic generation/ref lifetime, truthful space/16MiB/writeback failures\n",cases);
}
'''
variants={'current':(header,functions)}
if '--negative-controls' in os.sys.argv:
 variants.update({
 'unconfined-p32':(header.replace('is_subdir(file->f_path.dentry, root->f_path.dentry)','true'),functions),
 'privilege-bypass':(header,functions.replace('if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN))','if (false && !ns_capable(&init_user_ns, CAP_SYS_ADMIN))')),
 'missing-claim':(header.replace('!smp_load_acquire(&md3_data_live) || !file','!file'),functions),
 'wrong-partition':(header.replace('MINOR(bdev->bd_dev) == 32','true'),functions),
 'missing-private-mode':(header.replace('(READ_ONCE(inode->i_mode) & 07777) == 0700','true'),functions),
 'missing-space-guard':(header,functions.replace('ret = count ? mt6797_fs_md3_data_space(file, count) : 0;','ret = 0;')),
 'success-on-writeback-error':(header,functions.replace('if (ret)\n\t\t\treturn ret;\n\t}\n\treturn written;', 'if (ret)\n\t\t\treturn written;\n\t}\n\treturn written;')),
 })
with tempfile.TemporaryDirectory(prefix='md3-data-test-') as tmp:
 for name,(hdr,func) in variants.items():
  if name!='current':assert (hdr,func)!=(header,functions),name
  c=Path(tmp)/(name+'.c');exe=c.with_suffix('')
  c.write_text(shim+state+'\nstatic struct mt6797_fs_state provider_state,md3_state;\n'+hdr+func+reset+test)
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-Wno-missing-field-initializers','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(exe)],check=True)
  r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
  if name=='current':assert not r.returncode,r.stderr;print(r.stdout.strip())
  else:assert r.returncode,name;print('REJECTED',name)
