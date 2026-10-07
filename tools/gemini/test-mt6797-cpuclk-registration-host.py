#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production CCF registration, topology and registration failures."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/clk/mediatek/clk-mt6797-cpu.c').read_text()
header = (root / 'drivers/clk/mediatek/clk-mt6797-cpu.h').read_text()

def extract(text, signature, end='\n}\n'):
    start = text.index(signature)
    return text[start:text.index(end, start) + len(end)]

types = '\n'.join(extract(header, 'enum ' + name + ' {', '\n};') for name in
                  ('mt6797_cpuclk_domain', 'mt6797_cpuclk_source'))
types += '\n' + '\n'.join(extract(source, 'struct ' + name + ' {', '\n};') for name in
                         ('mt6797_cpuclk_pll_hw', 'mt6797_cpuclk_path_hw', 'mt6797_cpuclk'))
types += '\n' + extract(source, 'static const char *const mt6797_cpuclk_path_names[]', '\n};')
functions = '\n'.join(extract(source, 'static int ' + name + '(') for name in
                      ('mt6797_cpuclk_register_branches', 'mt6797_cpuclk_register_paths'))
prelude = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define CLK_GET_RATE_NOCACHE 1
#define CLK_SET_RATE_PARENT 2
struct clk_hw;
struct clk_parent_data { const char *fw_name; struct clk_hw *hw; };
struct clk_init_data { const char *name; const void *ops;
 const struct clk_parent_data *parent_data; unsigned int num_parents, flags; };
struct clk_hw { const struct clk_init_data *init; unsigned int flags; };
struct clk_hw_onecell_data { struct clk_hw *hws[4]; };
struct mt6797_cpuclk_backend { int unused; };
struct mt6797_cpuclk_observer { int unused; };
static int mt6797_cpuclk_branch_ops, mt6797_cpuclk_path_ops;
static unsigned int calls, failure;
static int devm_clk_hw_register(void *dev, struct clk_hw *hw)
{
 assert(dev && hw && hw->init && hw->init->name);
 const struct clk_init_data *init=hw->init;
 assert(init->flags & CLK_GET_RATE_NOCACHE);
 if (init->ops==&mt6797_cpuclk_branch_ops) {
  assert(init->num_parents==1);
  assert(!strcmp(init->parent_data[0].fw_name,"clk26m"));
 } else {
  assert(init->ops==&mt6797_cpuclk_path_ops && init->num_parents==4);
  assert(!strcmp(init->parent_data[0].fw_name,"clk26m"));
  assert(!strcmp(init->parent_data[2].fw_name,"mainpll"));
  assert(!strcmp(init->parent_data[3].fw_name,"univpll"));
 }
 calls++;
 if(calls==failure) return -ENOMEM;
 hw->flags=init->flags;
 return 0;
}
'''
main = r'''
int main(void)
{
 for(failure=0;failure<=7;failure++) {
  struct clk_hw_onecell_data clocks={0};
  struct mt6797_cpuclk p={.dev=(void *)&p,.clk_data=&clocks};
  calls=0;
  int ret=mt6797_cpuclk_register_branches(&p);
  if(failure && failure<=3) {
   assert(ret==-ENOMEM && calls==failure); continue;
  }
  assert(!ret && calls==3);
  assert(p.branches[0].domain==MT6797_CPUCLK_MP0);
  assert(p.branches[1].domain==MT6797_CPUCLK_MP1);
  assert(p.branches[2].domain==MT6797_CPUCLK_CCI);
  ret=mt6797_cpuclk_register_paths(&p);
  assert(ret==(failure ? -ENOMEM : 0));
  assert(calls==(failure ? failure : 7));
  for(unsigned int d=0;d<4;d++) {
   if(failure && d>=failure-4) { assert(!clocks.hws[d]); continue; }
   assert(clocks.hws[d]==&p.paths[d].hw && p.paths[d].domain==d);
   struct clk_hw *parent=p.paths[d].parents[1].hw;
   if(d==2) assert(parent==&p.plls[2].hw);
   else assert(parent==&p.branches[d==3 ? 2 : d].hw);
   assert(!!(p.paths[d].hw.flags&CLK_SET_RATE_PARENT)==(d!=2));
  }
 }
 puts("PASS CCF branch/path topology, A72 isolation and all 7 registration failures");
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-clk-registration-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(prelude + types + functions + main)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-sign-compare', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
