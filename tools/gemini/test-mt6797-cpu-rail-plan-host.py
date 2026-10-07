#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exhaust CPU rail sequencing over every admitted pair and target selector."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
headers = '\n'.join(re.sub(r'^#include .*\n', '',
    (root / 'include/linux/soc/mediatek' / name).read_text(), flags=re.M)
    for name in ('mt6797-cpu-rails.h', 'mt6797-cpu-rail-plan.h'))
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
typedef uint32_t u32;
#define current get_current()
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define clamp(a,l,h) min(max(a,l),h)
'''
main = r'''
/* Independent electrical check, in millivolts. */
static bool valid(struct mt6797_cpu_rail_pair p)
{
 return p.vproc_uv>=770000 && p.vproc_uv<=1200000 && !(p.vproc_uv%10000) &&
  p.vsram_uv>=1000000 && p.vsram_uv<=1200000 && !(p.vsram_uv%25000) &&
  p.vsram_uv>=p.vproc_uv && p.vsram_uv-p.vproc_uv<=300000;
}
static unsigned int distance(struct mt6797_cpu_rail_pair a,
                             struct mt6797_cpu_rail_pair b)
{
 return abs((int)a.vproc_uv-(int)b.vproc_uv)+abs((int)a.vsram_uv-(int)b.vsram_uv);
}
int main(void)
{
 unsigned int paths=0,writes=0,longest=0;
 for(unsigned int p=770000;p<=1200000;p+=10000)
 for(unsigned int s=1000000;s<=1200000;s+=25000) {
  struct mt6797_cpu_rail_pair initial={p,s};
  if(!valid(initial)) continue;
  for(unsigned int t=770000;t<=1200000;t+=10000) {
   struct mt6797_cpu_rail_pair cur=initial,next,target;
   assert(!mt6797_cpu_rail_target(t,&target));
   unsigned int expected_s=t+100000;
   if(expected_s<1000000) expected_s=1000000;
   if(expected_s>1200000) expected_s=1200000;
   while(expected_s%25000) expected_s++;
   assert(target.vproc_uv==t && target.vsram_uv==expected_s && valid(target));
   unsigned int steps=0;
   for(;;) {
    int ret=mt6797_cpu_rail_next(&cur,t,&next);
    assert(ret>=0);
    if(!ret) { assert(!distance(cur,target)); break; }
    assert(++steps<=8 && valid(next));
    /* Exactly one physical rail changes, and no selector reverses direction. */
    assert((cur.vproc_uv!=next.vproc_uv) != (cur.vsram_uv!=next.vsram_uv));
    assert(next.vproc_uv>=min(cur.vproc_uv,t) && next.vproc_uv<=max(cur.vproc_uv,t));
    assert(next.vsram_uv>=min(cur.vsram_uv,target.vsram_uv) &&
           next.vsram_uv<=max(cur.vsram_uv,target.vsram_uv));
    assert(distance(next,target)<distance(cur,target));
    /* Every intermediate analog value along the changed rail stays valid. */
    for(unsigned int u=0;u<=1000;u++) {
     long long vp=cur.vproc_uv+((long long)next.vproc_uv-cur.vproc_uv)*u/1000;
     long long vs=cur.vsram_uv+((long long)next.vsram_uv-cur.vsram_uv)*u/1000;
     assert(vs>=vp && vs-vp<=300000 && vs>=1000000 && vs<=1200000);
    }
    cur=next; writes++;
   }
   if(steps>longest) longest=steps;
   paths++;
  }
 }
 /* Golden full-range transitions, including quantization margins. */
 struct mt6797_cpu_rail_pair cur={770000,1000000},n;
 assert(mt6797_cpu_rail_next(&cur,1200000,&n)==1 && n.vsram_uv==1025000 && n.vproc_uv==770000);
 cur=(struct mt6797_cpu_rail_pair){1200000,1200000};
 assert(mt6797_cpu_rail_next(&cur,770000,&n)==1 && n.vproc_uv==930000 && n.vsram_uv==1200000);
 /* Invalid inputs must leave the caller's output unchanged. */
 const unsigned int bad[]={0,1,769999,770001,1200001,UINT_MAX};
 for(unsigned int i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
  n=(struct mt6797_cpu_rail_pair){123,456};
  assert(mt6797_cpu_rail_target(bad[i],&n)==-EINVAL && n.vproc_uv==123 && n.vsram_uv==456);
  assert(mt6797_cpu_rail_next(&cur,bad[i],&n)==-EINVAL && n.vproc_uv==123 && n.vsram_uv==456);
 }
 for(unsigned int p=760000;p<=1210000;p+=5000)
 for(unsigned int s=975000;s<=1225000;s+=12500) {
  cur=(struct mt6797_cpu_rail_pair){p,s};
  if(valid(cur)) continue;
  n=(struct mt6797_cpu_rail_pair){123,456};
  assert(mt6797_cpu_rail_next(&cur,1000000,&n)==-EINVAL && n.vproc_uv==123 && n.vsram_uv==456);
 }
 assert(mt6797_cpu_rail_next(NULL,1000000,&n)==-EINVAL);
 assert(mt6797_cpu_rail_target(1000000,NULL)==-EINVAL);
 printf("PASS %u CPU rail paths, %u writes, maximum %u steps; discrete and analog envelopes\n",paths,writes,longest);
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-rail-plan-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(prelude + headers + main)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-O2',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    '-fno-sanitize-recover=all', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
