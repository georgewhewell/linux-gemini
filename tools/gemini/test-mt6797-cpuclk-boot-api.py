#!/usr/bin/env python3
"""Run the actual boot-check API with device/CCF/snapshot boundary doubles."""
import os
import subprocess
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
s = (root/'drivers/clk/mediatek/clk-mt6797-cpu.c').read_text()
a = s.index('int mt6797_cpuclk_check_mp2_boot(')
api = s[a:s.index('\n}\n', a)+3]
c = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define EPROBE_DEFER 517
#define CLK_MCU_MP2 2
#define MT6797_CPUCLK_MP2 2
#define MT6797_CPUCLK_CLK26M 0
#define IS_ERR_OR_NULL(p) (!(p))
struct device { void *of_node, *driver; };
struct platform_device { struct device dev; };
struct clk_hw { int tag; };
struct clk { struct clk_hw *hw; };
struct mt6797_cpuclk_snapshot { int tag; };
struct mt6797_cpuclk { struct { struct clk_hw hw; } paths[4]; };
struct of_phandle_args { void *np; int args_count; unsigned args[1]; };
static struct { int driver; } mt6797_cpuclk_driver;
static struct mt6797_cpuclk provider;
static struct platform_device pdev;
static struct clk_hw ref, wrong;
static int fail, locked, node_puts, device_puts, samples, validates;
static int of_property_match_string(void *n,const char *p,const char *v)
{ assert(n && !strcmp(p,"clock-names") && !strcmp(v,"mp2")); return fail==1 ? -ENOENT : 3; }
static int of_parse_phandle_with_args(void *n,const char *p,const char *cells,int i,struct of_phandle_args *a)
{ assert(n && !strcmp(p,"clocks") && !strcmp(cells,"#clock-cells") && i==3);
 a->np=n; a->args_count=fail==6?0:1; a->args[0]=fail==7?1:2; return fail==2?-EINVAL:0; }
static struct platform_device *of_find_device_by_node(void *n)
{ assert(n); return fail==3?NULL:&pdev; }
static void of_node_put(void *n) { assert(n); node_puts++; }
static void device_lock(struct device *d) { assert(d==&pdev.dev && !locked); locked=1; }
static void device_unlock(struct device *d) { assert(d==&pdev.dev && locked); locked=0; }
static struct mt6797_cpuclk *platform_get_drvdata(struct platform_device *p)
{ assert(p==&pdev && locked); return fail==5?NULL:&provider; }
static struct clk_hw *__clk_get_hw(struct clk *c) { return c->hw; }
static struct clk_hw *clk_hw_get_parent_by_index(struct clk_hw *h,int i)
{ assert(h==&provider.paths[2].hw && i==0 && locked); return fail==9?NULL:&ref; }
static unsigned long clk_hw_get_rate(struct clk_hw *h)
{ assert(h==&ref && locked); return fail==10?0:26000000; }
static int mt6797_cpuclk_snapshot(struct mt6797_cpuclk *p,struct mt6797_cpuclk_snapshot *s)
{ assert(p==&provider && locked); samples++; s->tag=123; return fail==11?-EIO:0; }
static int mt6797_cpuclk_validate_cold_mp2(const struct mt6797_cpuclk_snapshot *s,unsigned long r)
{ assert(s->tag==123 && locked); validates++; return !r?-ERANGE:fail==12?-EBUSY:0; }
static void put_device(struct device *d) { assert(d==&pdev.dev && !locked); device_puts++; }
''' + api + r'''
int main(void)
{
 struct device consumer={.of_node=&consumer};
 struct clk clk={.hw=&provider.paths[2].hw};
 const int results[]={0,-ENOENT,-EINVAL,-EPROBE_DEFER,-EPROBE_DEFER,
  -EPROBE_DEFER,-EINVAL,-EINVAL,-EINVAL,-ENOENT,-ERANGE,-EIO,-EBUSY};
 assert(mt6797_cpuclk_check_mp2_boot(NULL,&clk)==-EINVAL);
 assert(mt6797_cpuclk_check_mp2_boot(&consumer,NULL)==-EINVAL);
 for (fail=0;fail<13;fail++) {
  locked=node_puts=device_puts=samples=validates=0;
  pdev.dev.driver=fail==4?NULL:&mt6797_cpuclk_driver.driver;
  clk.hw=fail==8?&wrong:&provider.paths[2].hw;
  assert(mt6797_cpuclk_check_mp2_boot(&consumer,&clk)==results[fail]);
  assert(!locked);
  assert(node_puts==(fail!=1 && fail!=2));
  assert(device_puts==(fail!=1 && fail!=2 && fail!=3));
  assert(samples==(fail==0 || fail>=10));
  assert(validates==(fail==0 || fail==10 || fail==12));
 }
 puts("mt6797-cpuclk-boot-api: pass (13 success/failure paths)");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src, exe = Path(tmp,'test.c'), Path(tmp,'test')
    src.write_text(c)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
