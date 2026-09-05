#!/usr/bin/env python3
"""Actual provider/Panfrost admission functions: no parallel policy copy."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
kernel = root.parents[2]
scp_source = (root / "mtk-scpsys.c").read_text()
gpu_source = (kernel / "drivers/gpu/drm/panfrost/panfrost_gpu.c").read_text()

def extract(src, name):
    at = src.index(name + "(")
    start = src.rfind("\n", 0, at) + 1
    pos = src.index("{", at) + 1
    depth = 1
    while depth:
        depth += (src[pos] == "{") - (src[pos] == "}")
        pos += 1
    return src[start:pos]

pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#define EPROBE_DEFER 517
#define BIT(n) (1U<<(n))
#define GFP_KERNEL 0
#define GPU_PM_VREG_OFF 3
#define GPU_PM_CLK_DIS 4
#define MT6797_POWER_DOMAIN_MFG_CORE0 7
struct device_node { int id; } gpu={1}, provider={2}, wrong={3};
struct device { struct device_node *of_node; } dev={&provider}, gpu_dev={&gpu};
struct scp { struct device *dev; struct device_node *gpuldo_gpu; int gpuldo_lock; bool gpuldo_mode;
 struct { bool admitted, failed; } gpuldo; } state;
static struct scp *gpuldo_provider;
struct of_phandle_args { struct device_node *np; int args_count; unsigned args[1]; };
static int flag, supply, count, names_count, bad_index, bad_provider, bad_args;
static int bad_name, parse_error, refs, platform, alloc_fail, control_calls;
static int mode_calls;
static const char *names[]={"core0","core1","core2","core3"};
#define smp_load_acquire(p) (*(p))
static bool of_property_read_bool(struct device_node *np,const char *p) {
 assert(np==&gpu && !strcmp(p,"mediatek,gemini-gpuldo-owner")); return flag; }
static void *of_find_property(struct device_node *np,const char *p,void *unused) {
 (void)unused; assert(np==&gpu && !strcmp(p,"mali-supply")); return supply?&gpu:NULL; }
static int of_count_phandle_with_args(struct device_node *np,const char *p,const char *c) {
 (void)np;(void)p;(void)c; return count; }
static int of_property_count_strings(struct device_node *np,const char *p) {
 (void)np;(void)p; return names_count; }
static int of_parse_phandle_with_args(struct device_node *np,const char *p,const char *c,
 int i,struct of_phandle_args *a) {
 (void)np;(void)p;(void)c; if(parse_error && i==2) return -EINVAL;
 a->np=bad_provider?&wrong:&provider; a->args_count=bad_args?0:1;
 a->args[0]=7+i+(bad_index?1:0); refs++; return 0; }
static void of_node_put(struct device_node *np) { assert(np); refs--; }
static int of_property_read_string_index(struct device_node *np,const char *p,int i,const char **s) {
 (void)np;(void)p; *s=bad_name?"bad":names[i]; return 0; }
static void mutex_lock(int *p) { assert(!*p); *p=1; }
static void mutex_unlock(int *p) { assert(*p); *p=0; }
struct panfrost_compatible { int num_supplies; const char **supply_names; unsigned pm_features; };
struct panfrost_device { struct { struct device *dev; } base;
 const struct panfrost_compatible *comp; bool mt6797_gpuldo_domain_supply; void *mfgcfg; };
static bool of_machine_is_compatible(const char *s) { (void)s; return platform; }
static void *devm_kmemdup(void *d,const void *p,size_t n,int g) {
 (void)d;(void)g; if(alloc_fail) return NULL; void *q=malloc(n); assert(q); memcpy(q,p,n); return q; }
static void panfrost_mt6797_gpuldo_prepare(struct panfrost_device *p) { (void)p; control_calls++; }
static void *syscon_regmap_lookup_by_phandle(struct device_node *n,const char *p) {
 (void)n;(void)p; control_calls++; return &gpu; }
#define IS_ERR(p) false
#define PTR_ERR(p) (-EINVAL)
#define dev_err_probe(...) (-EINVAL)
static int panfrost_mt6797_set_supported_hw(struct panfrost_device *p) { (void)p; return 0; }
'''
main = r'''
static void fresh(void) {
 flag=platform=1; supply=0; count=names_count=4;
 bad_index=bad_provider=bad_args=bad_name=parse_error=refs=alloc_fail=control_calls=mode_calls=0;
 state=(struct scp){.dev=&dev,.gpuldo_gpu=&gpu,.gpuldo_mode=true,.gpuldo={true,false}};
 gpuldo_provider=&state;
}
static void check(int expected) {
 struct panfrost_compatible orig={.num_supplies=1,.supply_names=names,
  .pm_features=BIT(GPU_PM_VREG_OFF)|BIT(GPU_PM_CLK_DIS)|BIT(1)};
 struct panfrost_device p={.base={&gpu_dev},.comp=&orig};
 int ret=panfrost_gpu_mt6797_init(&p);
 assert((ret==0)==(expected==0)); assert(refs==0 && !state.gpuldo_lock);
 assert(orig.num_supplies==1 && orig.pm_features==(BIT(GPU_PM_VREG_OFF)|BIT(GPU_PM_CLK_DIS)|BIT(1)));
 if(ret) { assert(control_calls==0 && !p.mt6797_gpuldo_domain_supply); }
 else if(flag) {
  assert(p.mt6797_gpuldo_domain_supply && p.comp->num_supplies==0);
  assert(!p.comp->supply_names && p.comp->pm_features==BIT(1)); free((void*)p.comp);
 } else { assert(!p.mt6797_gpuldo_domain_supply && p.comp==&orig); }
}
int main(void) {
 fresh(); check(0);
 fresh(); flag=0; check(0); /* Ordinary path unchanged. */
 fresh(); platform=0; check(-1);
 fresh(); supply=1; check(-1);
 fresh(); gpuldo_provider=NULL; check(-1); /* GPU-only flag/disabled owner. */
 fresh(); state.gpuldo_mode=false; check(-1);
 fresh(); state.gpuldo.admitted=false; check(-1);
 fresh(); state.gpuldo.failed=true; check(-1);
 fresh(); state.gpuldo_gpu=&wrong; check(-1);
 fresh(); count=3; check(-1);
 fresh(); names_count=5; check(-1);
 fresh(); bad_provider=1; check(-1);
 fresh(); bad_index=1; check(-1);
 fresh(); bad_args=1; check(-1);
 fresh(); bad_name=1; check(-1);
 fresh(); parse_error=1; check(-1);
 fresh(); alloc_fail=1; check(-1);
 puts("actual provider + Panfrost init: 17 ownership/mismatch paths PASS");
}
'''
assert "!pfdev->mt6797_gpuldo_domain_supply" in (kernel / "drivers/gpu/drm/panfrost/panfrost_device.c").read_text()
assert "pfdev->mt6797_gpuldo_domain_supply ? 0 :" in (kernel / "drivers/gpu/drm/panfrost/panfrost_devfreq.c").read_text()
cc = os.environ.get("HOSTCC", "/nix/store/061ybr50r1m2jnvih4vvpgmgwb37df3r-gcc-wrapper-12.2.0/bin/gcc")
with tempfile.TemporaryDirectory(prefix="gemini-gpuldo-provider-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(pre + extract(scp_source, "mtk_scpsys_gpuldo_gpu_admit") +
                                extract(gpu_source, "panfrost_gpu_mt6797_init") + main)
    subprocess.run([cc,"-std=gnu11","-Wall","-Wextra","-Werror",str(path / "test.c"),"-o",str(path / "test")],check=True)
    subprocess.run([str(path / "test")],check=True)
