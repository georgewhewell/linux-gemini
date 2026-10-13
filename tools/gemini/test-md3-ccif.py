#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual reset-held CCIF resource/preparation functions with bounded fake MMIO."""
import os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'drivers/soc/mediatek/mt6797-md3-ccif.c'
SHIM=r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define BIT(n) (1U<<(n))
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define min(a,b) ((a)<(b)?(a):(b))
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define U32_MAX UINT32_MAX
#define rmb() do { barriers++; } while(0)
#define mb() rmb()
#define __iomem
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095L)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define GFP_KERNEL 0
#define IORESOURCE_MEM 1
#define GIC_SPI 0
#define IRQ_TYPE_LEVEL_LOW 8
#define IRQ_TYPE_EDGE_FALLING 2
#define IRQF_NO_AUTOEN 0x80000
#define CLK_INFRA_AP_C2K_CCIF_0 1
#define CLK_INFRA_AP_C2K_CCIF_1 2
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p,v) (*(p)=(v))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
typedef void (*mt6797_md3_ccif_observer)(void*,bool,void*);
#define wmb() do { barriers++; } while(0)
typedef int irqreturn_t;
#define IRQ_HANDLED 1
typedef int atomic_t;
static int atomic_read(atomic_t *x){return __atomic_load_n(x,__ATOMIC_SEQ_CST);}
static void atomic_set(atomic_t *x,int n){__atomic_store_n(x,n,__ATOMIC_SEQ_CST);}
static int atomic_xchg(atomic_t *x,int n){return __atomic_exchange_n(x,n,__ATOMIC_SEQ_CST);}
struct mutex { pthread_mutex_t v; };
static void mutex_init(struct mutex *x){assert(!pthread_mutex_init(&x->v,0));}
static void mutex_lock(struct mutex *x){assert(!pthread_mutex_lock(&x->v));}
static void mutex_unlock(struct mutex *x){assert(!pthread_mutex_unlock(&x->v));}
struct device_node { int type; };
struct device { struct device_node *of_node; };
struct platform_device { struct device dev; };
struct reset_control { int held; };
struct resource { u64 start,end; };
struct clk_bulk_data { const char *id; };
struct of_phandle_args { struct device_node *np; unsigned args_count,args[3]; };
struct mt6797_md3_lk_snapshot {
 int mpu_error;
 struct {u64 base_phys;u32 ap_md3_offset,ap_md3_size;} smem;
 struct {u32 start,end;} mpu[4];
};
struct mt6797_md3_ccif;
static struct device_node node={0},irqnode={1},clknode={2};
static struct platform_device pdev={{&node}};
static struct reset_control reset={1};
static struct resource resource;
static struct mt6797_md3_lk_snapshot snapshot;
static unsigned char apbuf[0x1000],shmem[0x200000];
static unsigned calls,failcall,writes,reads,clears,barriers,clock_gets,clock_enables,clock_disables;
static unsigned requests,disabled,node_gets,node_puts,actions,live_calls,live_fail,read_corrupt_at;
static int bad_irq_spec,bad_clock_spec,bad_clock_names,bad_resource,duplicate_irq,held,live_error,clock_error;
static int reg_action_error,irq_count=2,clock_count=2;
static void *allocation;
static void (*action)(void*);static void *action_data;
static int point(void){return ++calls==failcall?-EIO:0;}
static int reset_control_status(struct reset_control *r){assert(r==&reset);return held;}
static int mt6797_md1_cold_owner_verify_md3_domain(struct device*d,struct reset_control*r){
 assert(d==&pdev.dev&&r==&reset);live_calls++;return live_calls==live_fail?-ESHUTDOWN:live_error;
}
static int mt6797_md1_cold_owner_md3_execution_live(struct device*d,struct reset_control*r,bool released){assert(released&&held==0);return mt6797_md1_cold_owner_verify_md3_domain(d,r);}
static unsigned enabled,synced,observations;static bool wdt_on_publication;static bool invoke_enable;
static pthread_mutex_t irq_guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t irq_changed=PTHREAD_COND_INITIALIZER;
static bool irq_running,observer_entered,release_observer,stop_entered,stream_stop;
static void drain_fake_irq(void){pthread_mutex_lock(&irq_guard);while(irq_running)pthread_cond_wait(&irq_changed,&irq_guard);pthread_mutex_unlock(&irq_guard);}
static irqreturn_t md3_ccif_unexpected_irq(int irq,void*data);
static void enable_irq(int irq){assert(irq==181||irq==319);enabled++;if(invoke_enable)md3_ccif_unexpected_irq(irq,allocation);}
static void synchronize_irq(int irq){assert(irq==181||irq==319);synced++;drain_fake_irq();}
static void disable_irq(int irq){assert(irq==181||irq==319);disabled++;drain_fake_irq();}
static void disable_irq_nosync(int irq){assert(irq==181||irq==319);disabled++;}
static int of_irq_parse_one(struct device_node*n,int idx,struct of_phandle_args*a){
 assert(n==&node&&idx<2);if(point())return -EIO;node_gets++;
 *a=(struct of_phandle_args){&irqnode,3,{GIC_SPI,idx?287:149,idx?2:8}};
 if(bad_irq_spec)a->args[bad_irq_spec-1]++;return 0;
}
static int of_parse_phandle_with_args(struct device_node*n,const char*p,const char*c,int idx,struct of_phandle_args*a){
 assert(n==&node&&!strcmp(p,"clocks")&&!strcmp(c,"#clock-cells")&&idx<2);
 if(point())return -EIO;node_gets++;*a=(struct of_phandle_args){&clknode,1,{idx+1}};
 if(bad_clock_spec)a->args[0]+=10;return 0;
}
static int of_device_is_compatible(struct device_node*n,const char*s){
 return (n==&irqnode&&!strcmp(s,"mediatek,mt6797-sysirq"))||(n==&clknode&&!strcmp(s,"mediatek,mt6797-infracfg"));
}
static void of_node_put(struct device_node*n){assert(n==&irqnode||n==&clknode);node_puts++;}
static int of_address_to_resource(struct device_node*n,int i,struct resource*r){
 assert(n==&clknode&&!i);if(point())return -EIO;*r=(struct resource){0x10001000,0x10001fff};return 0;
}
static u64 resource_size(const struct resource*r){return r->end-r->start+1;}
static int of_count_phandle_with_args(struct device_node*n,const char*p,const char*c){assert(n==&node);return clock_count;}
static int of_irq_count(struct device_node*n){assert(n==&node);return irq_count;}
static int of_property_match_string(struct device_node*n,const char*p,const char*s){
 assert(n==&node&&!strcmp(p,"clock-names"));return bad_clock_names?-1:!strcmp(s,"ap-c2k-ccif-1");
}
static struct resource *platform_get_resource_byname(struct platform_device*p,int type,const char*n){
 assert(p==&pdev&&type==IORESOURCE_MEM&&!strcmp(n,"ap-c2k-ccif"));if(point())return 0;
 if(bad_resource)resource.start=0x10209000;return &resource;
}
static void *devm_kzalloc(struct device*d,size_t n,int flags){assert(d==&pdev.dev&&!allocation);if(point())return 0;return allocation=calloc(1,n);}
static void *devm_ioremap_resource(struct device*d,struct resource*r){assert(d==&pdev.dev&&r==&resource);return point()?ERR_PTR(-EIO):apbuf;}
static void *devm_request_mem_region(struct device*d,u64 addr,u32 size,const char*n){
 assert(d==&pdev.dev&&addr==0x88400000&&size==0x200000);return point()?0:shmem;
}
static const char *dev_name(struct device*d){assert(d==&pdev.dev);return "test";}
static void *devm_ioremap_wc(struct device*d,u64 addr,u32 size){assert(d==&pdev.dev&&addr==0x88400000&&size==0x200000);return point()?0:shmem;}
static int devm_clk_bulk_get(struct device*d,int n,struct clk_bulk_data*c){
 assert(d==&pdev.dev&&n==2&&!strcmp(c[0].id,"ap-c2k-ccif-0")&&!strcmp(c[1].id,"ap-c2k-ccif-1"));clock_gets++;return point();
}
static int devm_add_action_or_reset(struct device*d,void(*fn)(void*),void*a){
 assert(d==&pdev.dev&&!requests);if(point()){fn(a);return -EIO;}action=fn;action_data=a;actions++;return 0;
}
static int platform_get_irq(struct platform_device*p,int i){assert(p==&pdev&&i<2);if(point())return -EIO;return i&&!duplicate_irq?319:181;}
static int devm_request_irq(struct device*d,int irq,irqreturn_t(*fn)(int,void*),unsigned flags,const char*n,void*a){
 assert(d==&pdev.dev&&(irq==181||irq==319)&&flags==IRQF_NO_AUTOEN&&actions==1&&fn&&a==allocation);requests++;return point();
}
static int clk_bulk_prepare_enable(int n,struct clk_bulk_data*c){assert(n==2);clock_enables++;return clock_error;}
static void clk_bulk_disable_unprepare(int n,struct clk_bulk_data*c){assert(n==2);clock_disables++;}
static bool inside(const void*p,const void*b,size_t n){return (uintptr_t)p>=(uintptr_t)b&&(uintptr_t)p+4<=(uintptr_t)b+n;}
static void writel(u32 v,void*p){
 assert(inside(p,shmem,sizeof(shmem))||inside(p,apbuf,sizeof(apbuf)));writes++;memcpy(p,&v,4);
 if(wdt_on_publication && p==shmem+0x19000+(20+200+150+200)*1024+4*40+24){wdt_on_publication=false;md3_ccif_unexpected_irq(319,allocation);}
 if(p==apbuf+0x14){u32 q;memcpy(&q,apbuf+0x10,4);q&=~v;memcpy(apbuf+0x10,&q,4);}
}
static u32 readl(const void*p){
 u32 v;assert(inside(p,shmem,sizeof(shmem))||inside(p,apbuf,sizeof(apbuf)));reads++;memcpy(&v,p,4);return reads==read_corrupt_at?v^1:v;
}
static void memcpy_toio(void*d,const void*s,size_t n){assert((u8*)d>=shmem&&(u8*)d+n<=shmem+sizeof(shmem));writes++;memcpy(d,s,n);}
static void memcpy_fromio(void*d,const void*s,size_t n){assert((const u8*)s>=shmem&&(const u8*)s+n<=shmem+sizeof(shmem));reads++;memcpy(d,s,n);}
static u32 get_unaligned_le32(const void*p){u32 v;memcpy(&v,p,4);return v;}
static void memset_io(void*p,int v,size_t n){assert(p==shmem&&v==0&&n==sizeof(shmem));clears++;memset(p,v,n);}
"""
TESTS=r"""
static unsigned cases;
static void cleanup(void){if(action)action(action_data);free(allocation);allocation=0;action=0;action_data=0;}
static void init(void){
 cleanup();memset(apbuf,0,sizeof(apbuf));memset(shmem,0xa5,sizeof(shmem));
 resource=(struct resource){0x1020b000,0x1020bfff};
 snapshot=(struct mt6797_md3_lk_snapshot){.smem={0x88000000,0x400000,0x200000},.mpu={{0x88400000,0x885fffff}}};
 calls=failcall=writes=reads=clears=barriers=clock_gets=clock_enables=clock_disables=0;
 enabled=synced=observations=0;invoke_enable=false;wdt_on_publication=false;observer_entered=release_observer=stop_entered=stream_stop=false;
 requests=disabled=node_gets=node_puts=actions=live_calls=live_fail=read_corrupt_at=0;
 bad_irq_spec=bad_clock_spec=bad_clock_names=bad_resource=duplicate_irq=live_error=clock_error=0;held=1;irq_count=clock_count=2;
}
static struct mt6797_md3_ccif *acquire(void){
 struct mt6797_md3_ccif*c=0;assert(!mt6797_md3_ccif_acquire(&pdev,&snapshot,&reset,&c));
 assert(c&&c==allocation&&!writes&&!clears&&!clock_enables&&requests==2&&node_gets==node_puts);return c;
}
static void invalid(void){
 struct mt6797_md3_ccif*c=(void*)1;cases++;
 assert(mt6797_md3_ccif_acquire(&pdev,&snapshot,&reset,&c)<0&&c==(void*)1);
 assert(!writes&&!clears&&!clock_enables&&node_gets==node_puts);
}
#define BAD(x) do{init();x;invalid();}while(0)
static void compare_vendor_layout(void){
 u32 off=0x19000,rx[]={10,100,100,150,50,10,10,10},tx[]={10,100,50,50,50,10,10,10};
 for(unsigned i=0;i<8;i++){
  u32 size=(rx[i]+tx[i])*1024+40;
  const u32 ctl[]={0xee0000ee,0xee0000ee,0,0,rx[i]*1024,0,0,tx[i]*1024};
  assert(!memcmp(shmem+off,ctl,sizeof(ctl)));
  assert(readl(shmem+off+size-8)==0xff0000ff&&readl(shmem+off+size-4)==0xff0000ff);
  for(unsigned j=32;j<size-8;j++)assert(!shmem[off+j]);off+=size;
 }
 assert(off==0xcf940&&readl(shmem+off)==0x464c4f57);
 assert(!readl(shmem+off+4)&&!readl(shmem+off+8)&&!readl(shmem+off+12));
 for(unsigned j=0;j<0x19000;j++)assert(!shmem[j]);
 for(unsigned j=off+16;j<sizeof(shmem);j++)assert(!shmem[j]);
}
static void observe(void*data,bool wdt,void*ap){assert(data==&snapshot&&ap==apbuf);observations+=wdt?10:1;}
static void blocking_observe(void*data,bool wdt,void*ap){
 observe(data,wdt,ap);pthread_mutex_lock(&irq_guard);observer_entered=true;pthread_cond_broadcast(&irq_changed);
 while(!release_observer)pthread_cond_wait(&irq_changed,&irq_guard);pthread_mutex_unlock(&irq_guard);}
static void*irq_thread(void*p){pthread_mutex_lock(&irq_guard);irq_running=true;pthread_mutex_unlock(&irq_guard);
 md3_ccif_unexpected_irq(181,p);pthread_mutex_lock(&irq_guard);irq_running=false;pthread_cond_broadcast(&irq_changed);pthread_mutex_unlock(&irq_guard);return 0;}
static void*stop_thread(void*p){pthread_mutex_lock(&irq_guard);stop_entered=true;pthread_cond_broadcast(&irq_changed);pthread_mutex_unlock(&irq_guard);if(stream_stop)mt6797_md3_ccif_stream_stop(p);else mt6797_md3_ccif_observe_stop(p);return 0;}
static void *prepare_thread(void*p){intptr_t e=mt6797_md3_ccif_prepare(p);return(void*)e;}
int main(void){
 init();struct mt6797_md3_ccif*c=acquire();unsigned n=calls;
 for(unsigned i=1;i<=n;i++){init();failcall=i;invalid();}
 BAD(snapshot.mpu_error=-EIO);BAD(snapshot.smem.base_phys++);BAD(snapshot.smem.ap_md3_offset=0x200000);BAD(snapshot.smem.ap_md3_size--);
 BAD(snapshot.mpu[0].start--);BAD(snapshot.mpu[0].end++);BAD(bad_resource=1);BAD(bad_clock_spec=1);BAD(bad_clock_names=1);
 BAD(bad_irq_spec=1);BAD(bad_irq_spec=2);BAD(bad_irq_spec=3);BAD(irq_count=1);BAD(clock_count=3);BAD(duplicate_irq=1);
 init();c=acquire();assert(!mt6797_md3_ccif_prepare(c));assert(!mt6797_md3_ccif_status(c));
 assert(clock_enables==1&&clears==1&&barriers==2&&!disabled);unsigned total_reads=reads,total_live=live_calls; assert(total_live==12);
 compare_vendor_layout();unsigned saved=writes;assert(mt6797_md3_ccif_prepare(c)==-EALREADY&&writes==saved);cases++;
 init();c=acquire();held=0;assert(mt6797_md3_ccif_prepare(c)==-EBUSY&&!clock_enables&&!writes&&!clears);cases++;
 init();c=acquire();clock_error=-EIO;assert(mt6797_md3_ccif_prepare(c)==-EIO&&!writes&&!clears&&!c->clocks_on);cases++;
 for(unsigned i=1;i<=total_live;i++){
  init();c=acquire();live_fail=i;assert(mt6797_md3_ccif_prepare(c)==-ESHUTDOWN);saved=writes;
  assert(mt6797_md3_ccif_prepare(c)==-EALREADY&&writes==saved);assert(mt6797_md3_ccif_status(c)==-ESHUTDOWN);cases++;
 }
 for(unsigned i=1;i<=total_reads;i++){
  init();c=acquire();read_corrupt_at=i;assert(mt6797_md3_ccif_prepare(c)<0);saved=writes;
  assert(mt6797_md3_ccif_prepare(c)==-EALREADY&&saved==writes);cases++;
 }
 init();c=acquire();writel(1,apbuf+4);assert(mt6797_md3_ccif_prepare(c)==-EUCLEAN);cases++;
 init();c=acquire();assert(md3_ccif_unexpected_irq(181,c)==IRQ_HANDLED);assert(mt6797_md3_ccif_status(c)==-EIO);
 assert(mt6797_md3_ccif_prepare(c)==-EIO&&!writes&&!clears&&!clock_enables);cases++;
 init();c=acquire();pthread_t a,b;void*x,*y;assert(!pthread_create(&a,0,prepare_thread,c));assert(!pthread_create(&b,0,prepare_thread,c));
 assert(!pthread_join(a,&x)&&!pthread_join(b,&y));assert((!x&&(intptr_t)y==-EALREADY)||(!y&&(intptr_t)x==-EALREADY));
 assert(clears==1&&clock_enables==1);cases++;
 saved=writes;cleanup();assert(writes==saved&&clock_disables==1);cases++;
 init();c=acquire();assert(mt6797_md3_ccif_observe_start(c,observe,&snapshot)==-EPERM&&!enabled);cases++;
 init();c=acquire();assert(!mt6797_md3_ccif_prepare(c));held=0;
 assert(mt6797_md3_ccif_observe_start(c,observe,&snapshot)==-EBUSY&&!enabled);cases++;
 init();c=acquire();assert(!mt6797_md3_ccif_prepare(c));writel(128,apbuf+0x10);
 assert(mt6797_md3_ccif_observe_start(c,observe,&snapshot)==-EUCLEAN&&!enabled);cases++;
 init();c=acquire();assert(!mt6797_md3_ccif_prepare(c));saved=writes;
 assert(!mt6797_md3_ccif_observe_start(c,observe,&snapshot)&&enabled==2&&!observations&&writes==saved);
 assert(mt6797_md3_ccif_observe_start(c,observe,&snapshot)==-EPERM&&enabled==2);
 md3_ccif_unexpected_irq(181,c);assert(observations==1&&disabled==1&&!c->irq_enabled[0]&&c->irq_enabled[1]);
 mt6797_md3_ccif_observe_stop(c);assert(disabled==2&&synced==2&&!c->observer&&!c->observer_data);
 assert(mt6797_md3_ccif_observe_start(c,observe,&snapshot)==-EPERM);cases++;
 init();c=acquire();assert(!mt6797_md3_ccif_prepare(c));invoke_enable=true;
 assert(!mt6797_md3_ccif_observe_start(c,observe,&snapshot));
 assert(observations==11&&disabled==2&&enabled==2);mt6797_md3_ccif_observe_stop(c);
 assert(disabled==2&&synced==2&&!c->observer&&!c->observer_data);cleanup();cases++;
 init();c=acquire();assert(!mt6797_md3_ccif_prepare(c));assert(!mt6797_md3_ccif_observe_start(c,blocking_observe,&snapshot));
 pthread_t irq_worker,stop_worker;assert(!pthread_create(&irq_worker,0,irq_thread,c));
 pthread_mutex_lock(&irq_guard);while(!observer_entered)pthread_cond_wait(&irq_changed,&irq_guard);pthread_mutex_unlock(&irq_guard);
 assert(!pthread_create(&stop_worker,0,stop_thread,c));pthread_mutex_lock(&irq_guard);
 while(!stop_entered)pthread_cond_wait(&irq_changed,&irq_guard);
 assert(c->observer==blocking_observe&&c->observer_data==&snapshot);release_observer=true;pthread_cond_broadcast(&irq_changed);pthread_mutex_unlock(&irq_guard);
 assert(!pthread_join(irq_worker,0)&&!pthread_join(stop_worker,0));assert(!c->observer&&!c->observer_data&&disabled==2&&synced==2);cleanup();cases++;

 /* One first-message observer is drained before the exclusive stream adopts
  * the same live execution lease. No second clear/reset/clock acquisition. */
 init();c=acquire();assert(!mt6797_md3_ccif_prepare(c));
 assert(mt6797_md3_ccif_stream_start(c,observe,&snapshot)==-EPERM);
 assert(!mt6797_md3_ccif_observe_start(c,observe,&snapshot));
 assert(mt6797_md3_ccif_stream_start(c,observe,&snapshot)==-EPERM);
 mt6797_md3_ccif_observe_stop(c);held=0;
 assert(!mt6797_md3_ccif_stream_start(c,observe,&snapshot));assert(enabled==4&&clears==1&&clock_enables==1);
 md3_ccif_unexpected_irq(181,c);md3_ccif_unexpected_irq(181,c);
 assert(observations==2&&c->irq_enabled[0]&&c->irq_enabled[1]&&!atomic_read(&c->unexpected_irq));
 assert(mt6797_md3_ccif_stream_start(c,observe,&snapshot)==-EPERM);cases++;
 u8 frame[24]={0},out[128],runtime[584]={1},query[116]={0};
 live_error=-EIO;saved=reads;assert(mt6797_md3_ccif_receive(c,4,out,sizeof(out))==-EIO&&reads==saved);live_error=0;
 assert(!mt6797_md3_ccif_receive(c,4,out,sizeof(out)));
 struct md3_ring_layout layout;assert(!md3_ring_layout(4,&layout));
 saved=writes;writel(BIT(12),apbuf+4);assert(mt6797_md3_ccif_transmit(c,4,frame,24)==-EAGAIN&&writes==saved+1);
 writel(0,apbuf+4);assert(mt6797_md3_ccif_transmit(c,4,frame,24)==24);
 assert(readl(shmem+layout.offset+24)==40&&readl(apbuf+4)==BIT(12)&&readl(apbuf+12)==12);cases++;
 saved=writes;writel(BIT(7),apbuf+4);assert(mt6797_md3_ccif_publish_runtime(c,runtime,sizeof(runtime),query,sizeof(query))==-EAGAIN&&writes==saved+1);
 writel(0,apbuf+4);for(unsigned i=0;i<116;i++)query[i]=i;
 assert(mt6797_md3_ccif_publish_runtime(c,runtime,sizeof(runtime),query,sizeof(query))==1);
 assert(!memcmp(shmem+0x10000,runtime,sizeof(runtime))&&!memcmp(apbuf+0x158,query,sizeof(query)));
 assert(readl(shmem+0xffc8)==0x44444444&&readl(shmem+0xffd0)==UINT32_MAX&&readl(shmem+0xfffc)==0x44444444);
 assert(readl(apbuf+4)==128&&readl(apbuf+12)==7);cases++;
 /* WDT after producer publication keeps positive send disposition and fences
  * every later IO, while IRQ callbacks still have valid resources. */
 writel(0,apbuf+4);wdt_on_publication=true;
 assert(mt6797_md3_ccif_transmit(c,4,frame,24)==24&&!wdt_on_publication);
 assert(atomic_read(&c->unexpected_irq)&&observations==12&&!c->irq_enabled[1]);
 saved=writes;unsigned oldreads=reads;
 assert(mt6797_md3_ccif_transmit(c,4,frame,24)==-ESHUTDOWN);
 assert(mt6797_md3_ccif_receive(c,4,out,sizeof(out))==-ESHUTDOWN);
 assert(mt6797_md3_ccif_publish_runtime(c,runtime,sizeof(runtime),query,sizeof(query))==-ESHUTDOWN);
 assert(writes==saved&&reads==oldreads);mt6797_md3_ccif_stream_stop(c);assert(!c->observer&&!c->streaming);cases++;
 init();c=acquire();assert(!mt6797_md3_ccif_prepare(c));assert(!mt6797_md3_ccif_observe_start(c,observe,&snapshot));mt6797_md3_ccif_observe_stop(c);held=0;live_error=-EIO;
 assert(mt6797_md3_ccif_stream_start(c,observe,&snapshot)==-EIO&&!c->streaming&&enabled==2);live_error=0;
 assert(mt6797_md3_ccif_stream_start(c,observe,&snapshot)==-EPERM);cases++;
 init();c=acquire();assert(!mt6797_md3_ccif_prepare(c));assert(!mt6797_md3_ccif_observe_start(c,observe,&snapshot));mt6797_md3_ccif_observe_stop(c);held=0;
 assert(!mt6797_md3_ccif_stream_start(c,blocking_observe,&snapshot));stream_stop=true;
 assert(!pthread_create(&irq_worker,0,irq_thread,c));pthread_mutex_lock(&irq_guard);
 while(!observer_entered)pthread_cond_wait(&irq_changed,&irq_guard);pthread_mutex_unlock(&irq_guard);
 /* Stop uses same drain primitive: callback/context cannot be freed while ISR owns them. */
 assert(!pthread_create(&stop_worker,0,stop_thread,c));pthread_mutex_lock(&irq_guard);
 while(!stop_entered)pthread_cond_wait(&irq_changed,&irq_guard);assert(c->observer==blocking_observe);
 release_observer=true;pthread_cond_broadcast(&irq_changed);pthread_mutex_unlock(&irq_guard);
 assert(!pthread_join(irq_worker,0)&&!pthread_join(stop_worker,0));mt6797_md3_ccif_stream_stop(c);
 assert(!c->observer&&!c->streaming);cases++;
 cleanup();
 printf("PASS %u actual-source CCIF resource/lifetime/failure cases\n",cases);return 0;
}
"""
def clean(s):return re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
def run(source,label,success):
 with tempfile.TemporaryDirectory(prefix='md3-ccif-') as t:
  c=Path(t)/'test.c';out=Path(t)/'test';c.write_text(SHIM+clean((ROOT/'drivers/soc/mediatek/mt6797-md3-ring.h').read_text())+clean(source)+TESTS)
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-o',str(out),str(c)],check=True)
  r=subprocess.run([str(out)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  if success:
   if r.returncode:raise SystemExit(r.stderr.decode())
   print(r.stdout.decode().strip())
  elif not r.returncode:raise SystemExit('negative control survived: '+label)
  else:print('REJECTED '+label)
def main():
 s=SOURCE.read_text();run(s,'actual',True)
 for name,a,b in (
  ('false-unpublished-after-WDT','writel(queue + 8, c->ap + 0xc);','writel(queue + 8, c->ap + 0xc); if (atomic_read(&c->unexpected_irq)) ret = -EIO;'),
  ('no-stream-supplier-admission','return mt6797_md1_cold_owner_md3_execution_live(c->dev, c->reset, true);','return 0;'),
  ('MD1-memory-wrong-span','MD3_AP3_BASE 0x88400000','MD3_AP3_BASE 0x88000000'),
  ('IRQ-autoenable','IRQF_NO_AUTOEN,', '0,'),
  ('no-masked-handler-drain','synchronize_irq(c->irq[i]);','/* no drain */'),
  ('observer-repeat','if (c->observed ||','if (false ||'),
  ('wrong-old-ring-offset','MD3_RING_START 0x19000','MD3_RING_START 0x200000'),
  ('fake-flow-tail','ret = md3_ccif_write_verify(c, offset, MD3_FLOW_HEADER);','ret = md3_ccif_write_verify(c, offset, MD3_FLOW_HEADER); writel(0x4354524c, c->smem + offset + 12);'),
  ('retry-after-partial','if (c->attempted) {','if (false) {'),
  ('missing-final-live','return md3_ccif_live(c);','return 0;'),
  ('wrong-shared-clock','CLK_INFRA_AP_C2K_CCIF_1);','CLK_INFRA_AP_C2K_CCIF_0);')):
  assert a in s,name;run(s.replace(a,b),name,False)

if __name__=='__main__':main()
