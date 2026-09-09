#!/usr/bin/env python3
"""Exercise real AFE clock/runtime-resume functions with clock API failures."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = root / 'sound/soc/mediatek/mt6797'
clock = (source / 'mt6797-afe-clk.c').read_text()
pcm = (source / 'mt6797-afe-pcm.c').read_text()
registers = (source / 'mt6797-reg.h').read_text()
def function(text, name):
    start = re.search(r'^(?:static )?int ' + name + r'\(', text, re.M).start()
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'
program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
struct clk { int id, owned; };
struct mt6797_afe_private { struct clk **clk; };
struct device { void *data; };
struct mtk_base_afe { struct device *dev; struct mt6797_afe_private *platform_priv; void *regmap; };
#define dev_err(...) do {} while (0)
static int step, fail_at, released[8], nreleased, accesses;
static void *dev_get_drvdata(struct device *d) { return d->data; }
static int clk_prepare_enable(struct clk *c) {
 assert(!c->owned); step++;
 if(step==fail_at)return -EIO;
 c->owned=1; return 0;
}
static int clk_set_parent(struct clk *c, struct clk *parent) {
 assert(c->owned && parent->id==6); step++;
 return step==fail_at?-ERANGE:0;
}
static void clk_disable_unprepare(struct clk *c) {
 assert(c->owned); c->owned=0; released[nreleased++]=c->id;
}
static int regmap_write(void *map, unsigned r, unsigned v) {
 (void)map; (void)r; (void)v; accesses++; return 0;
}
static int regmap_update_bits(void *map, unsigned r, unsigned m, unsigned v) {
 (void)map; (void)r; (void)m; (void)v; accesses++; return 0;
}
'''
program += clock[clock.index('enum {'):clock.index('static const char *aud_clks')]
for name in ['mt6797_afe_enable_clock','mt6797_afe_disable_clock']:
    program += function(clock,name)
resume = function(pcm,'mt6797_afe_runtime_resume')
for name in sorted(set(re.findall(r'\b[A-Z][A-Z0-9_]+\b',resume))):
    definition = re.search(r'^#define\s+'+name+r'\s+([^\n]+)',registers,re.M)
    if definition: program += '#define '+name+' '+definition[1]+'\n'
program += resume
program += r'''
int main(void) {
 struct clk clocks[CLK_NUM]; struct clk *ptrs[CLK_NUM];
 struct mt6797_afe_private priv={ptrs}; struct device dev;
 struct mtk_base_afe afe={.dev=&dev,.platform_priv=&priv}; dev.data=&afe;
 for(int failure=0;failure<=5;failure++) {
  memset(clocks,0,sizeof(clocks));
  for(int i=0;i<CLK_NUM;i++){clocks[i].id=i;ptrs[i]=&clocks[i];}
  step=nreleased=accesses=0; fail_at=failure;
  int ret=mt6797_afe_runtime_resume(&dev);
  if(!failure) {
   assert(!ret && accesses==6 && !nreleased);
   assert(!mt6797_afe_disable_clock(&afe));
   int expected[]={3,2,1,0};
   assert(nreleased==4 && !memcmp(released,expected,sizeof(expected)));
  } else {
   assert(ret==(failure==4?-ERANGE:-EIO) && !accesses);
   int completed=failure<=3?failure-1:3;
   assert(nreleased==completed);
   for(int i=0;i<completed;i++)assert(released[i]==completed-i-1);
  }
  for(int i=0;i<CLK_NUM;i++)assert(!clocks[i].owned);
 }
 puts("PASS: every clock failure propagates, unwinds only owned clocks, and prevents all AFE register accesses");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-audio-clock-') as directory:
    c=Path(directory)/'test.c'; binary=Path(directory)/'test'
    c.write_text(program)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fno-pie','-no-pie','-fsanitize=address,undefined',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
