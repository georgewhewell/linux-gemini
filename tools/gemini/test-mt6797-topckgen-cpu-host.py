#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run production TOPCKGEN CPU-field helpers against a locked register model.

Models shared MFG bits, field conflicts, refused writes and argument failures.
No real MMIO or claim about the meaning of the vendor CPU field is made.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/clk/mediatek/clk-mt6797.c').read_text()

def extract(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

functions = '\n'.join(extract(s) for s in (
    'int mt6797_topckgen_cpu_read(', 'int mt6797_topckgen_cpu_update(',
    'struct mt6797_topckgen *mt6797_topckgen_get('))
defines = '\n'.join(s for s in source.splitlines() if s.startswith((
    '#define MT6797_CLK_MISC_CFG_0 ', '#define MT6797_CPU_FALLBACK_MASK ')))
c = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define GENMASK(h,l) ((~0U << (l)) & (~0U >> (31-(h))))
#define FIELD_GET(mask,v) (((v) & (mask)) >> __builtin_ctz(mask))
#define FIELD_PREP(mask,v) (((v) << __builtin_ctz(mask)) & (mask))
''' + defines + r'''
struct mt6797_topckgen { unsigned char *base; };
static unsigned char base[0x108];
static unsigned int reg, locked, reads, writes, refuse;
static unsigned int mt6797_clk_lock, pending_gpu;
static void lock(unsigned int *p)
{
 assert(p==&mt6797_clk_lock && !locked);
 /* A GPU mux operation can finish after CPU planning but before this lock. */
 reg=(reg & ~GENMASK(2,1)) | pending_gpu<<1;
 locked=1;
}
#define spin_lock_irqsave(p,f) do { (f)=0; lock(p); } while (0)
#define spin_unlock_irqrestore(p,f) do { assert((p)==&mt6797_clk_lock && locked && !(f)); locked=0; } while (0)
static u32 readl(const volatile void *p)
{ assert(locked && p==base+MT6797_CLK_MISC_CFG_0); reads++; return reg; }
static void writel(u32 v, volatile void *p)
{
 assert(locked && p==base+MT6797_CLK_MISC_CFG_0); writes++;
 if (!refuse) reg=v;
}

#define EPROBE_DEFER 517
#define ERR_PTR(v) ((void *)(intptr_t)(v))
#define DL_FLAG_AUTOREMOVE_CONSUMER 1
struct device_node { int unused; };
struct device { struct device_node *of_node; };
struct platform_device { struct device dev; };
struct device_link { int unused; };
static struct device_node node;
static struct platform_device supplier;
static struct mt6797_topckgen top_data={.base=base};
static struct device_link link;
static int property, parsed, compatible, found, bound, data, link_ok;
static int nodes_put, devices_put, dev_locked, links;
static void *of_find_property(struct device_node *n,const char *name,void *len)
{ assert(n && name && !len); return property ? n : NULL; }
static struct device_node *of_parse_phandle(struct device_node *n,const char *name,int idx)
{ assert(n && name && !idx); return parsed ? &node : NULL; }
static int of_device_is_compatible(struct device_node *n,const char *name)
{ assert(n==&node && name); return compatible; }
static void of_node_put(struct device_node *n) { assert(n==&node); nodes_put++; }
static struct platform_device *of_find_device_by_node(struct device_node *n)
{ assert(n==&node); return found ? &supplier : NULL; }
static void device_lock(struct device *d) { assert(d==&supplier.dev && !dev_locked); dev_locked=1; }
static void device_unlock(struct device *d) { assert(d==&supplier.dev && dev_locked); dev_locked=0; }
static int device_is_bound(struct device *d) { assert(d==&supplier.dev && dev_locked); return bound; }
static void *platform_get_drvdata(struct platform_device *p)
{ assert(p==&supplier && dev_locked); return data ? &top_data : NULL; }
static void *device_link_add(struct device *c,struct device *s,int flags)
{
 assert(c && s==&supplier.dev && dev_locked && flags==DL_FLAG_AUTOREMOVE_CONSUMER);
 links++; return link_ok ? &link : NULL;
}
static void put_device(struct device *d) { assert(d==&supplier.dev && !dev_locked); devices_put++; }
''' + functions + r'''
static void reset_owner(void)
{
 property=parsed=compatible=found=bound=data=link_ok=1;
 nodes_put=devices_put=dev_locked=links=0;
}
static void reset(u32 initial, unsigned int gpu)
{ reg=initial; pending_gpu=gpu; locked=reads=writes=refuse=0; }
int main(void)
{
 struct mt6797_topckgen top={.base=base};
 unsigned int cases=0;
 const u32 backgrounds[]={0,~0U,0xa5a5a5a5U,0x5a5a5a5aU};
 for (unsigned int b=0;b<4;b++) for (unsigned int gpu=0;gpu<4;gpu++)
  for (unsigned int old=0;old<4;old++) for (unsigned int next=0;next<4;next++) {
   u32 initial=(backgrounds[b] & ~MT6797_CPU_FALLBACK_MASK) | old<<4;
   reset(initial,gpu); u8 value=255;
   assert(!mt6797_topckgen_cpu_read(&top,&value));
   assert(value==old && reads==1 && !writes && !locked);
   pending_gpu=(gpu+1)%4;
   assert(!mt6797_topckgen_cpu_update(&top,old,next));
   assert(((reg>>4)&3)==next && ((reg>>1)&3)==pending_gpu && !locked);
   assert((reg & ~(MT6797_CPU_FALLBACK_MASK|GENMASK(2,1))) ==
          (initial & ~(MT6797_CPU_FALLBACK_MASK|GENMASK(2,1))));
   assert(writes==(old!=next));
   reset(initial,gpu);
   assert(mt6797_topckgen_cpu_update(&top,(old+1)%4,next)==-EAGAIN);
   assert(!writes && !locked && ((reg>>4)&3)==old);
   if (old!=next) {
    reset(initial,gpu); refuse=1;
    assert(mt6797_topckgen_cpu_update(&top,old,next)==-EIO);
    assert(writes==1 && reads==2 && !locked && ((reg>>4)&3)==old);
   }
   cases++;
  }
 reset(0,0); u8 value=0;
 assert(mt6797_topckgen_cpu_read(NULL,&value)==-EINVAL);
 assert(mt6797_topckgen_cpu_read(&top,NULL)==-EINVAL);
 assert(mt6797_topckgen_cpu_update(NULL,0,3)==-EINVAL);
 assert(mt6797_topckgen_cpu_update(&top,4,0)==-EINVAL);
 assert(mt6797_topckgen_cpu_update(&top,0,4)==-EINVAL);
 assert(!reads && !writes && !locked);
 struct device consumer={.of_node=&node};
 reset_owner(); assert(mt6797_topckgen_get(NULL)==ERR_PTR(-EINVAL));
 consumer.of_node=NULL; assert(mt6797_topckgen_get(&consumer)==ERR_PTR(-EINVAL));
 consumer.of_node=&node;
 const int expected[]={-ENOENT,-EINVAL,-EINVAL,-EPROBE_DEFER,-EPROBE_DEFER,-EPROBE_DEFER,-ENOMEM,0};
 for (unsigned int i=0;i<8;i++) {
  reset_owner();
  int *fields[]={&property,&parsed,&compatible,&found,&bound,&data,&link_ok};
  if (i<7) *fields[i]=0;
  struct mt6797_topckgen *owner=mt6797_topckgen_get(&consumer);
  assert(owner==(expected[i] ? ERR_PTR(expected[i]) : &top_data));
  assert(!dev_locked && nodes_put==(i>=2) && devices_put==(i>=4));
  assert(links==(i>=6));
 }
 printf("PASS %u shared-register cases, supplier lifecycle and invalid arguments\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-topckgen-cpu-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(c)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
