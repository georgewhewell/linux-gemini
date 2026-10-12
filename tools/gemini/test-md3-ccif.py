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
typedef uint32_t u32; typedef uint64_t u64;
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
#define wmb() do { barriers++; } while(0)
typedef int irqreturn_t;
#define IRQ_HANDLED 1
typedef int atomic_t;
static int atomic_read(atomic_t *x){return *x;}
static void atomic_set(atomic_t *x,int n){*x=n;}
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
 if(p==apbuf+0x14){u32 q;memcpy(&q,apbuf+0x10,4);q&=~v;memcpy(apbuf+0x10,&q,4);}
}
static u32 readl(const void*p){
 u32 v;assert(inside(p,shmem,sizeof(shmem))||inside(p,apbuf,sizeof(apbuf)));reads++;memcpy(&v,p,4);return reads==read_corrupt_at?v^1:v;
}
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
 printf("PASS %u actual-source CCIF resource/lifetime/failure cases\n",cases);return 0;
}
"""
def clean(s):return re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
def run(source,label,success):
 with tempfile.TemporaryDirectory(prefix='md3-ccif-') as t:
  c=Path(t)/'test.c';out=Path(t)/'test';c.write_text(SHIM+clean(source)+TESTS)
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
  ('MD1-memory-wrong-span','MD3_AP3_BASE 0x88400000','MD3_AP3_BASE 0x88000000'),
  ('IRQ-autoenable','IRQF_NO_AUTOEN,', '0,'),
  ('wrong-old-ring-offset','MD3_RING_START 0x19000','MD3_RING_START 0x200000'),
  ('fake-flow-tail','ret = md3_ccif_write_verify(c, offset, MD3_FLOW_HEADER);','ret = md3_ccif_write_verify(c, offset, MD3_FLOW_HEADER); writel(0x4354524c, c->smem + offset + 12);'),
  ('retry-after-partial','if (c->attempted) {','if (false) {'),
  ('missing-final-live','return md3_ccif_live(c);','return 0;'),
  ('wrong-shared-clock','CLK_INFRA_AP_C2K_CCIF_1);','CLK_INFRA_AP_C2K_CCIF_0);')):
  assert a in s,name;run(s.replace(a,b),name,False)

if __name__=='__main__':main()
