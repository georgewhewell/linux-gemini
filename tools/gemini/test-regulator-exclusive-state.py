#!/usr/bin/env python3
"""Actual core acquisition/accounting and devres bodies; no device access."""
import os
import re
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
core=(root/'drivers/regulator/core.c').read_text()
devres=(root/'drivers/regulator/devres.c').read_text()
def fn(src,name):
    # Definitions only (several accounting functions have earlier prototypes).
    for match in re.finditer(r'(?m)^(?:static )?(?:struct regulator \*|void |int )'+re.escape(name)+r'\(',src):
        at=match.start()
        brace=src.index('{',at)
        semi=src.find(';',at,brace)
        if semi<0: break
    else:
        raise AssertionError('missing definition '+name)
    start=at
    # Function return type occupies a preceding line for multiline signatures?
    pos=brace+1;depth=1
    while depth:
        depth+=(src[pos]=='{')-(src[pos]=='}');pos+=1
    return src[start:pos]+'\n'

pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#define EPROBE_DEFER 517
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define EXPORT_SYMBOL_GPL(x)
#define GFP_KERNEL 0
#define fallthrough __attribute__((fallthrough))
#define DL_FLAG_STATELESS 1
#define PM_SUSPEND_ON 0
#define REGULATOR_CHANGE_STATUS 1
#define REGULATOR_EVENT_ENABLE 1
#define REGULATOR_EVENT_PRE_DISABLE 2
#define REGULATOR_EVENT_ABORT_DISABLE 3
#define REGULATOR_EVENT_DISABLE 4
#define NOTIFY_STOP_MASK 0x8000
#define lockdep_assert_held_once(p) ((void)(p))
#define dev_warn(...) ((void)0)
#define rdev_warn(...) ((void)0)
#define rdev_err(...) ((void)0)
static int warnings;
#define WARN_ON(c) ((c)?(++warnings,1):0)
#define WARN(c,...) WARN_ON(c)
enum regulator_get_type {NORMAL_GET,EXCLUSIVE_GET,OPTIONAL_GET,EXCLUSIVE_GET_STRICT,MAX_GET_TYPE};
struct device {int kobj, refs;};
struct device_link {int x;};
struct regulator;
struct regulator_dev {
 unsigned open_count,use_count; bool exclusive; int owner;
 struct device dev; struct regulator *supply;
 struct {int n_resolved,n_coupled;} coupling_desc;
 struct {struct {int base;} mutex;} unused;
 struct {int base;} mutex;
 struct constraints {bool always_on;} *constraints;
 int enabled, read_error, enable_error, disable_error;
};
struct regulator {struct regulator_dev *rdev; unsigned enable_count,uA_load;
 struct device *dev; bool device_link; void *debugfs; char *supply_name; int list;};
static struct regulator_dev rail,parent,*dummy_regulator_rdev;
static struct regulator upstream;
static struct constraints constraints;
static int regulator_list_mutex,objects,modules,reads,enables,disables;
static int fail_alloc,fail_devres; static void *managed;
static void mutex_lock(void *p){(void)p;} static void mutex_unlock(void *p){(void)p;}
static void regulator_lock(struct regulator_dev *r){(void)r;}
static void regulator_unlock(struct regulator_dev *r){(void)r;}
static void debugfs_remove_recursive(void *p){(void)p;}
static void device_link_remove(struct device *a,struct device *b){(void)a;(void)b;}
static void sysfs_remove_link(void *a,void *b){(void)a;(void)b;}
static void list_del(void *p){(void)p;}
static void kfree_const(void *p){assert(!p);}
static void kfree(void *p){objects--;free(p);}
static void get_device(struct device *d){d->refs++;}
static void put_device(struct device *d){assert(d->refs>0);d->refs--;}
static bool try_module_get(int owner){(void)owner;modules++;return true;}
static void module_put(int owner){(void)owner;assert(modules>0);modules--;}
static bool have_full_constraints(void){return true;}
static int regulator_resolve_supply(struct regulator_dev *r){(void)r;return 0;}
static struct regulator *create_regulator(struct regulator_dev *r,struct device *d,const char *id){
 (void)d;(void)id;if(fail_alloc)return NULL;struct regulator *p=calloc(1,sizeof(*p));assert(p);p->rdev=r;objects++;return p;}
static void link_and_create_debugfs(struct regulator *r,struct regulator_dev *d,struct device *v){(void)r;(void)d;(void)v;}
static struct device_link *device_link_add(struct device *a,struct device *b,int f){(void)a;(void)b;(void)f;return NULL;}
static int _regulator_is_enabled(struct regulator_dev *r){reads++;return r->read_error?r->read_error:r->enabled;}
static int regulator_balance_voltage(struct regulator_dev *r,int s){(void)r;(void)s;return 0;}
static bool regulator_ops_is_valid(struct regulator_dev *r,int op){(void)r;(void)op;return true;}
static int drms_uA_update(struct regulator_dev *r){(void)r;return 0;}
static int _notifier_call_chain(struct regulator_dev *r,int e,void *p){(void)r;(void)e;(void)p;return 0;}
static int _regulator_do_enable(struct regulator_dev *r){enables++;if(r->enable_error)return r->enable_error;r->enabled=1;return 0;}
static int _regulator_do_disable(struct regulator_dev *r){disables++;if(r->disable_error)return r->disable_error;r->enabled=0;return 0;}
static int _regulator_disable(struct regulator *r);
static int _regulator_enable(struct regulator *r);
static void destroy_regulator(struct regulator *r);
static int regulator_enable(struct regulator *r){return _regulator_enable(r);}
static int _regulator_get_common_check(struct device *d,const char *id,enum regulator_get_type t){(void)d;return !id||t>=MAX_GET_TYPE?-EINVAL:0;}
static struct regulator_dev *regulator_dev_lookup(struct device *d,const char *id){(void)d;(void)id;get_device(&rail.dev);return &rail;}
static void *devres_alloc(void (*release)(struct device *,void *),size_t n,int f){(void)release;(void)f;return fail_devres?NULL:calloc(1,n);}
static void devres_free(void *p){free(p);}
static void devres_add(struct device *d,void *p){(void)d;assert(!managed);managed=p;}
'''
names=['_regulator_handle_consumer_enable','_regulator_handle_consumer_disable',
       '_regulator_enable','_regulator_disable','destroy_regulator',
       '_regulator_get_common','_regulator_get','regulator_get_exclusive_state',
       '_regulator_put','regulator_put']
code=''.join(fn(core,n) for n in names)
code+=fn(devres,'devm_regulator_release')+fn(devres,'devm_regulator_get_exclusive_state')
main=r'''
static void fresh(int on){
 assert(!objects&&!modules&&!managed);
 rail=(struct regulator_dev){.enabled=on,.constraints=&constraints};
 parent=(struct regulator_dev){.constraints=&constraints};
 upstream=(struct regulator){.rdev=&parent};
 warnings=reads=enables=disables=fail_alloc=fail_devres=0;
}
static void done(struct regulator *r){
 regulator_put(r);assert(!objects&&!modules&&!warnings);
 assert(!rail.open_count&&!rail.exclusive&&!rail.dev.refs);
}
int main(void){
 struct regulator *r;bool on;
 for(int state=0;state<2;state++){
  fresh(state);on=!state;r=regulator_get_exclusive_state(NULL,"vgpu",&on);
  assert(!IS_ERR(r)&&on==state&&reads==1);
  assert(r->enable_count==(unsigned)state&&rail.use_count==(unsigned)state);
  if(!on)assert(!_regulator_enable(r));
  assert(r->enable_count==1&&rail.use_count==1&&enables==!state);
  assert(!_regulator_disable(r));assert(!r->enable_count&&!rail.use_count&&disables==1);
  done(r);
 }
 fresh(1);rail.read_error=-EIO;rail.use_count=7;on=true;
 r=regulator_get_exclusive_state(NULL,"vgpu",&on);
 assert(IS_ERR(r)&&PTR_ERR(r)==-EIO&&on&&reads==1);
 assert(!objects&&!modules&&!rail.dev.refs&&!rail.open_count&&!rail.exclusive&&rail.use_count==7);
 /* Ordinary exclusive API deliberately preserves its old error-as-OFF behavior. */
 fresh(1);rail.read_error=-EIO;r=_regulator_get(NULL,"vgpu",EXCLUSIVE_GET);
 assert(!IS_ERR(r)&&!r->enable_count&&!rail.use_count);done(r);
 fresh(1);fail_alloc=1;on=true;r=regulator_get_exclusive_state(NULL,"vgpu",&on);
 assert(IS_ERR(r)&&on&&!reads&&!objects&&!modules&&!rail.dev.refs);
 fresh(1);fail_devres=1;on=true;r=devm_regulator_get_exclusive_state(NULL,"vgpu",&on);
 assert(IS_ERR(r)&&on&&!reads&&!objects&&!modules);
 fresh(1);on=false;r=devm_regulator_get_exclusive_state(NULL,"vgpu",&on);
 assert(!IS_ERR(r)&&on&&managed&&r->enable_count==1);
 /* Failed GPU admission retains this exact live handle; no automatic put. */
 assert(objects==1&&rail.exclusive&&rail.use_count==1);
 assert(!_regulator_disable(r));devm_regulator_release(NULL,managed);free(managed);managed=NULL;
 assert(!objects&&!modules&&!warnings);
 for(int kind=0;kind<2;kind++){
  fresh(1);rail.supply=&upstream;rail.use_count=5;on=false;
  if(kind==0)parent.enable_error=-EIO;else parent.read_error=-EIO;
  r=regulator_get_exclusive_state(NULL,"vgpu",&on);
  assert(IS_ERR(r)&&PTR_ERR(r)==-EIO&&!on);
  assert(!objects&&!modules&&!rail.dev.refs&&!rail.open_count&&!rail.exclusive&&rail.use_count==5);
  assert(!upstream.enable_count&&!parent.use_count);
 }
 fresh(1);rail.supply=&upstream;on=false;r=regulator_get_exclusive_state(NULL,"vgpu",&on);
 assert(!IS_ERR(r)&&on&&upstream.enable_count==1&&parent.use_count==1);
 assert(!_regulator_disable(r));assert(!upstream.enable_count&&!parent.use_count);done(r);
 fresh(1);on=false;r=regulator_get_exclusive_state(NULL,"vgpu",&on);rail.disable_error=-EIO;
 assert(_regulator_disable(r)==-EIO&&r->enable_count==1&&rail.use_count==1);
 /* Test-only cleanup after proving retained state, not a proposed hardware retry. */
 rail.disable_error=0;assert(!_regulator_disable(r));done(r);
 puts("PASS actual regulator core: strict0/1/readerror, legacy semantics, allocation/devres, retained vote, upstream success/failures, enable/disable/put accounting");
}
'''
with tempfile.TemporaryDirectory(prefix='regulator-exclusive-state-') as d:
    p=Path(d);(p/'test.c').write_text(pre+code+main)
    cc=os.environ.get('HOSTCC','/nix/store/061ybr50r1m2jnvih4vvpgmgwb37df3r-gcc-wrapper-12.2.0/bin/gcc')
    subprocess.run([cc,'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=undefined',
                    '-fno-sanitize-recover=all',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
