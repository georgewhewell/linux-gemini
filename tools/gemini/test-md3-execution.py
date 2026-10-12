#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual optional MD3 sequence/IRQ/parser against bounded fake hardware."""
import os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'drivers/soc/mediatek/mt6797-md3-execution.c'
SHIM=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
typedef uint32_t u32; typedef uint64_t u64; typedef int64_t s64;
#define __iomem
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) ((~0U<<(l))&(~0U>>(31-(h))))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define READ_ONCE(x) (x)
#define GFP_KERNEL 0
#define IORESOURCE_MEM 1
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095L)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define rmb() ((void)0)
#define wmb() ((void)0)
struct mutex{pthread_mutex_t v;};
static void mutex_init(struct mutex*m){assert(!pthread_mutex_init(&m->v,0));}
static void mutex_lock(struct mutex*m){assert(!pthread_mutex_lock(&m->v));}
static void mutex_unlock(struct mutex*m){assert(!pthread_mutex_unlock(&m->v));}
typedef struct mutex spinlock_t;
#define spin_lock_init mutex_init
#define spin_lock_irqsave(l,f) do{(f)=0;mutex_lock(l);}while(0)
#define spin_unlock_irqrestore(l,f) do{(void)(f);mutex_unlock(l);}while(0)
typedef int wait_queue_head_t;
#define init_waitqueue_head(p) (*(p)=0)
#define wake_up(p) (*(p)+=1)
#define msecs_to_jiffies(n) (n)
struct device{int unused;};struct platform_device{struct device dev;};struct reset_control{int held;};
struct mt6797_md3_ccif{int unused;};
struct resource{u64 start,end;};
static struct platform_device pdev;static struct reset_control reset;static struct mt6797_md3_ccif ccif;
static unsigned char misc[4096],pll[128],clk[768],ap[4096];
static unsigned points,failpoint,livecalls,livefail,starts,stops,releases,asserts,ends,updates,writes16,writes32,cases;
static int release_error,assert_error,begin_error,arm_error,hs_error,early,timeout_wake,timeout_event,bad_readback,pre_wdt;
static int reset_status_error,resource_bad,resource_fail,spurious,wdt_event,late_event,late_wake;
static bool armed,lease;
static s64 now;
static void(*observer)(void*,bool,void*);static void*observer_data;static void*allocated;
static struct{unsigned off,val;} trace16[16];
static struct{u32 off,mask,value;} trace_shared[8];
static int point(void){return ++points==failpoint?-EIO:0;}
static s64 ktime_get(void){return now;}
static s64 ktime_to_ms(s64 n){return n;}
static void udelay(unsigned n){assert(n==30);}
static void usleep_range(unsigned a,unsigned b){assert(a==100&&b==200);now++;}
static void inject(void);
static void wait_tick(unsigned ms){now+=late_event?10001:ms;if(!timeout_event)inject();}
#define wait_event_timeout(w,c,t) do{(void)(w);if(!(c))wait_tick(t);}while(0)
static int mt6797_md1_cold_owner_md3_execution_begin(struct device*d,struct reset_control*r){
 assert(d==&pdev.dev&&r==&reset&&r->held&&!lease);if(begin_error)return begin_error;if(point())return -EIO;lease=true;return 0;}
static int mt6797_md1_cold_owner_md3_execution_live(struct device*d,struct reset_control*r,bool released){
 assert(d==&pdev.dev&&r==&reset&&lease);livecalls++;if(livecalls==livefail)return -ESHUTDOWN;
 if(point())return -EIO;return r->held==!released?0:-EUCLEAN;}
static int mt6797_md1_cold_owner_md3_execution_update(struct device*d,u32 o,u32 m,u32 v){
 assert(d==&pdev.dev&&lease);assert((o==0xf18&&m==0x44&&v==0x44)||(o==0x368&&m==2&&(v==0||v==2))||(o==0x220&&m==0xe000&&!v));
 assert(updates<8);trace_shared[updates++]=(typeof(trace_shared[0])){o,m,v};return point();}
static void mt6797_md1_cold_owner_md3_execution_end(struct device*d){assert(d==&pdev.dev&&lease&&!armed);lease=false;ends++;}
static int reset_control_assert(struct reset_control*r){assert(r==&reset&&!armed);asserts++;if(assert_error)return assert_error;r->held=1;return 0;}
static int reset_control_deassert(struct reset_control*r){assert(r==&reset&&armed&&lease);releases++;r->held=0;return release_error?release_error:point();}
static int reset_control_status(struct reset_control*r){assert(r==&reset);return reset_status_error?reset_status_error:r->held;}
static int mt6797_md3_ccif_observe_start(struct mt6797_md3_ccif*c,void(*fn)(void*,bool,void*),void*d){
 assert(c==&ccif&&!armed&&reset.held);starts++;if(arm_error)return arm_error;if(point())return -EIO;
 observer=fn;observer_data=d;armed=true;if(early)inject();if(pre_wdt)observer(d,true,ap);return 0;}
static void mt6797_md3_ccif_observe_stop(struct mt6797_md3_ccif*c){assert(c==&ccif&&armed);stops++;armed=false;}
static unsigned off(void*p,void*b,size_t n){uintptr_t a=(uintptr_t)p,z=(uintptr_t)b;assert(a>=z&&a<z+n);return a-z;}
static u32 readl(void*p){u32 v;uintptr_t a=(uintptr_t)p;
 if(a>=(uintptr_t)misc&&a<(uintptr_t)misc+sizeof(misc)){
  unsigned o=off(p,misc,sizeof(misc));assert(o==0x360||o==0x364);if(o==0x364&&!timeout_wake){if(late_wake)now+=1001;return 2;}}
 else {unsigned o=off(p,ap,sizeof(ap));assert(o==0x10||(o>=0x100&&o<0x158));}
 memcpy(&v,p,4);return v;}
static void writel(u32 v,void*p){uintptr_t a=(uintptr_t)p;writes32++;
 if(a>=(uintptr_t)misc&&a<(uintptr_t)misc+sizeof(misc)){assert(off(p,misc,sizeof(misc))==0x360&&lease);}
 else{assert(off(p,ap,sizeof(ap))==0x14&&armed);}memcpy(p,&v,4);}
static unsigned readw(void*p){uint16_t v;uintptr_t a=(uintptr_t)p;
 if(a>=(uintptr_t)pll&&a<(uintptr_t)pll+sizeof(pll))assert(off(p,pll,sizeof(pll))==0x40);
 else{unsigned o=off(p,clk,sizeof(clk));assert(o==0x234||o==0x10||o==0x29c);}
 assert(!reset.held&&lease);memcpy(&v,p,2);return bad_readback&&writes16==10?v^1:v;}
static void writew(unsigned v,void*p){unsigned o;uintptr_t a=(uintptr_t)p;assert(!reset.held&&lease);
 if(a>=(uintptr_t)pll&&a<(uintptr_t)pll+sizeof(pll))o=off(p,pll,sizeof(pll))+0x13000;else o=off(p,clk,sizeof(clk));
 assert(writes16<16);trace16[writes16++]=(typeof(trace16[0])){o,v};uint16_t w=v;memcpy(p,&w,2);}
static void*devm_kzalloc(struct device*d,size_t n,int f){(void)f;assert(d==&pdev.dev);if(resource_fail==1)return 0;return allocated=calloc(1,n);}
static struct resource*platform_get_resource_byname(struct platform_device*p,int t,const char*n){
 static struct resource r;assert(p==&pdev&&t==IORESOURCE_MEM);if(resource_bad)return 0;
 if(!strcmp(n,"c2k-misc"))r=(struct resource){0x1001b000,0x1001bfff};
 else if(!strcmp(n,"c2k-pll"))r=(struct resource){0x3a013000,0x3a01307f};
 else{assert(!strcmp(n,"c2k-clock"));r=(struct resource){0x3a000000,0x3a0002ff};}return &r;}
static u64 resource_size(struct resource*r){return r->end-r->start+1;}
static void*devm_ioremap_resource(struct device*d,struct resource*r){assert(d==&pdev.dev);if(resource_fail==2)return ERR_PTR(-EBUSY);return r->start==0x1001b000?misc:r->start==0x3a013000?pll:clk;}
static int sysfs_emit(char*b,const char*f,...){va_list a;va_start(a,f);int n=vsnprintf(b,4096,f,a);va_end(a);return n;}
static int sysfs_emit_at(char*b,int o,const char*f,...){va_list a;va_start(a,f);int n=vsnprintf(b+o,4096-o,f,a);va_end(a);return n;}
'''
TEST=r'''
static void inject(void){
 assert(armed);u32 mask=spurious?spurious:BIT(7);memcpy(ap+0x10,&mask,4);
 u32 header[]={0,0,0x80000000,0x5555ffff};u32 features[18]={0x49434343};features[17]=0x49434343;features[1]=0x04030201;
 if(hs_error==1)header[1]=88;if(hs_error==2)header[2]|=4;if(hs_error==3)header[3]=0;
 if(hs_error==4)features[0]=0;if(hs_error==5)features[17]=0;if(hs_error==6)features[1]=5;if(hs_error==7)header[2]|=0x10000;
 memcpy(ap+0x100,header,16);memcpy(ap+0x110,features,72);observer(observer_data,wdt_event,ap);
}
static struct mt6797_md3_execution*init(void){
 free(allocated);allocated=0;memset(misc,0,sizeof(misc));memset(pll,0,sizeof(pll));memset(clk,0,sizeof(clk));memset(ap,0,sizeof(ap));
 memset(trace16,0,sizeof(trace16));memset(trace_shared,0,sizeof(trace_shared));
 points=failpoint=livecalls=livefail=starts=stops=releases=asserts=ends=updates=writes16=writes32=0;
 release_error=assert_error=begin_error=arm_error=hs_error=early=timeout_wake=timeout_event=bad_readback=pre_wdt=0;
 reset_status_error=resource_bad=resource_fail=spurious=wdt_event=late_event=late_wake=0;armed=lease=false;reset.held=1;now=0;
 struct mt6797_md3_execution*e=0;assert(!mt6797_md3_execution_acquire(&pdev,&reset,&ccif,&e)&&e==allocated);
 assert(!points&&!writes16&&!writes32&&!starts&&!releases);return e;
}
static void terminal(struct mt6797_md3_execution*e){
 assert(!armed&&!lease&&e->attempted);assert(!e->lease&&!e->irqs);
 if(starts&& !arm_error && stops)assert(stops==1);
 if(ends)assert(asserts==1);
 if(!assert_error&&!reset_status_error&&ends)assert(reset.held&&e->reset_held);
 unsigned n=writes32+writes16+releases+asserts+starts;assert(mt6797_md3_execution_run(e)==-EALREADY);
 assert(n==writes32+writes16+releases+asserts+starts);mt6797_md3_execution_shutdown(e);assert(n==writes32+writes16+releases+asserts+starts);cases++;
}
static void*run_thread(void*p){return(void*)(intptr_t)mt6797_md3_execution_run(p);}
int main(void){
 struct mt6797_md3_execution*e=init();assert(!mt6797_md3_execution_run(e));unsigned n=points,l=livecalls;
 assert(e->hs1&&e->phase==5&&e->reset_held&&!e->released&&e->ccif_irqs==1&&!e->wdt_irqs);
 assert(writes16==10&&updates==4);const unsigned offsets[]={0x13008,0x13008,0x13008,0x13004,0x13074,0x13040,0x234,0x234,0x10,0x29c};
 const unsigned values[]={0x8805,5,1,0,0x10,0x8000,0x8000,0xc124,0x8e43,0xa207};
 for(unsigned i=0;i<10;i++)assert(trace16[i].off==offsets[i]&&trace16[i].val==values[i]);
 char text[4096];assert(mt6797_md3_execution_show(e,text)>0&&strstr(text,"hs1=1")&&strstr(text,"runtime_sent=0 css_ready=0")&&strstr(text,"execution_may_be_running=0"));terminal(e);
 for(unsigned i=1;i<=n;i++){e=init();failpoint=i;assert(mt6797_md3_execution_run(e)<0);terminal(e);}
 for(unsigned i=1;i<=l;i++){e=init();livefail=i;assert(mt6797_md3_execution_run(e)==-ESHUTDOWN);terminal(e);}
 for(int i=1;i<=7;i++){e=init();hs_error=i;assert(mt6797_md3_execution_run(e)==-EPROTO&&!e->hs1);terminal(e);}
 e=init();timeout_wake=1;assert(mt6797_md3_execution_run(e)==-ETIMEDOUT&&now==1000&&!writes16);terminal(e);
 e=init();timeout_event=1;assert(mt6797_md3_execution_run(e)==-ETIMEDOUT&&now==10000&&writes16==10);terminal(e);
 e=init();late_wake=1;assert(mt6797_md3_execution_run(e)==-ETIMEDOUT&&!writes16);terminal(e);
 e=init();late_event=1;assert(mt6797_md3_execution_run(e)==-ETIMEDOUT&&!e->hs1);terminal(e);
 e=init();begin_error=-EPERM;assert(mt6797_md3_execution_run(e)==-EPERM&&!starts&&!releases&&!asserts);terminal(e);
 e=init();arm_error=-EIO;assert(mt6797_md3_execution_run(e)==-EIO&&!releases&&!writes16);terminal(e);
 e=init();release_error=-EIO;assert(mt6797_md3_execution_run(e)==-EIO&&releases==1&&asserts==1&&reset.held&&!writes16);terminal(e);
 e=init();assert_error=-EIO;assert(mt6797_md3_execution_run(e)==-EIO&&!e->reset_held&&e->released&&e->cleanup_error==-EIO);terminal(e);
 e=init();reset_status_error=-EIO;assert(mt6797_md3_execution_run(e)==-EIO&&!e->reset_held&&e->cleanup_error==-EIO);terminal(e);
 e=init();bad_readback=1;assert(mt6797_md3_execution_run(e)==-EIO);terminal(e);
 e=init();early=1;assert(mt6797_md3_execution_run(e)==-EUCLEAN&&!releases&&!writes16);terminal(e);
 e=init();pre_wdt=1;assert(mt6797_md3_execution_run(e)==-EIO&&!releases);terminal(e);
 e=init();wdt_event=1;assert(mt6797_md3_execution_run(e)==-EIO&&e->wdt_irqs==1);terminal(e);
 e=init();spurious=BIT(12);assert(mt6797_md3_execution_run(e)==-EOPNOTSUPP&&e->event_mask==BIT(12)&&!e->sram);terminal(e);
 e=init();spurious=BIT(7)|BIT(6);assert(mt6797_md3_execution_run(e)==-EPROTO);terminal(e);
 e=init();spurious=BIT(7)|BIT(12);assert(!mt6797_md3_execution_run(e)&&e->hs1);terminal(e);
 e=init();pthread_t a,b;void*x,*y;assert(!pthread_create(&a,0,run_thread,e));assert(!pthread_create(&b,0,run_thread,e));
 assert(!pthread_join(a,&x)&&!pthread_join(b,&y));assert((!x&&(intptr_t)y==-EALREADY)||(!y&&(intptr_t)x==-EALREADY));assert(releases==1&&asserts==1);terminal(e);
 for(unsigned i=1;i<=2;i++){e=init();resource_fail=i;struct mt6797_md3_execution*out=(void*)1;assert(mt6797_md3_execution_acquire(&pdev,&reset,&ccif,&out)<0&&out==(void*)1);cases++;}
 e=init();resource_bad=1;struct mt6797_md3_execution*out=(void*)1;assert(mt6797_md3_execution_acquire(&pdev,&reset,&ccif,&out)==-EINVAL&&out==(void*)1);cases++;
 free(allocated);printf("PASS %u actual-source execution/IRQ/HS1/failure/lifetime cases\n",cases);return 0;
}
'''
def run(s,label,success):
 with tempfile.TemporaryDirectory(prefix='md3-execution-') as t:
  c=Path(t)/'test.c';b=Path(t)/'test';c.write_text(SHIM+re.sub(r'^#include[^\n]*\n','',s,flags=re.M)+TEST)
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-o',str(b),str(c)],check=True)
  # fake devres lifetime is intentionally retained across failed acquisition
  env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
  p=subprocess.run([str(b)],capture_output=True,env=env)
  if success:
   if p.returncode:raise SystemExit(p.stderr.decode())
   print(p.stdout.decode().strip())
  elif p.returncode:print('REJECTED '+label)
  else:raise SystemExit('negative control survived: '+label)
def main():
 s=SRC.read_text();run(s,'actual',True)
 for name,a,b in [
 ('no-terminal-reset','ret = reset_control_assert(e->reset);','ret = 0;'),
 ('wrong-PLL-width-order','writew(0x0005, e->pll + 0x08);','writew(0x0001, e->pll + 0x08);'),
 ('missing-wake-poll','if (e->wake_status & BIT(1))','if (true)'),
 ('missing-IRQ-drain','mt6797_md3_ccif_observe_stop(e->ccif);','/* missing drain */'),
 ('HS1-opcode-as-length','if (e->header[1] ||','if (false ||'),
 ('stale-first-sequence','((e->header[2] & BIT(31)) && (e->header[2] & 0x7fff0000))','false'),
 ('malformed-feature-success','if (mask > 4)','if (false)'),
 ('retry-execution','if (e->attempted) {','if (false) {'),
 ('failed-reset-claimed-held','e->reset_held = !ret;','e->reset_held = true;'),
 ]:
  assert a in s,name;run(s.replace(a,b),name,False)

if __name__=='__main__':main()
