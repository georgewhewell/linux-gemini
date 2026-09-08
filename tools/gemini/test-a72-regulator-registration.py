#!/usr/bin/env python3
"""Exercise production regulator registration with DT/core boundary doubles.

Checks exact node/descriptor/init-data wiring and refusal/reference cleanup.
This does not simulate the regulator core's hardware initialization effects.
"""
import os
import subprocess
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
def extract(file, signature):
    s = (root/file).read_text(); start = s.index(signature)
    return s[start:s.index('\n}\n',start)+3]
functions = extract('drivers/regulator/da9213-legacy-regulator.c','static int da9213_legacy_register_regulators(')
functions += extract('drivers/regulator/mt6797-bigidvfs-sram.c','static int mt6797_bigidvfs_sram_probe(')
c = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define GFP_KERNEL 0
#define DA9213_LEGACY_BUCKA 0
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
struct device_node { int id; };
struct device { struct device_node *of_node; };
struct platform_device { struct device dev; };
struct regulator_desc { int id; };
struct regulator_init_data { struct device_node *node; const struct regulator_desc *desc; };
struct regulator_config { struct device *dev; void *driver_data;
 struct device_node *of_node; struct regulator_init_data *init_data; };
struct regulator_dev { int id; };
struct variant { unsigned num_bucks; };
struct da9213_legacy { struct device *dev; struct variant *variant; };
struct mt6797_bigidvfs_sram { struct device *dev; int lock; };
static struct regulator_desc da9213_legacy_regulators[]={{0},{1}};
static struct regulator_desc mt6797_bigidvfs_sram_desc={2};
static struct device_node nodes[]={{0},{1},{2},{3},{4}};
static struct regulator_init_data data[3];
static struct regulator_dev rdev;
static struct mt6797_bigidvfs_sram sram;
static int refs[5], missing, disabled, parse_fail, reg_fail, alloc_fail;
static int parses, registrations;
static struct device_node *of_get_child_by_name(struct device_node *p,const char *name)
{
 int i;
 if (!strcmp(name,"regulators")) {assert(p==&nodes[3]);i=4;}
 else {assert(p==&nodes[4]);i=!strcmp(name,"BUCKA")?0:1;}
 if (missing==i) return NULL;
 refs[i]++; return &nodes[i];
}
static void of_node_put(struct device_node *n) {assert(refs[n->id]>0);refs[n->id]--;}
static int of_device_is_available(struct device_node *n) {return disabled!=n->id;}
static struct regulator_init_data *of_get_regulator_init_data(struct device *d,struct device_node *n,const struct regulator_desc *desc)
{
 assert(d && n->id==desc->id); parses++;
 if (parse_fail==n->id) return NULL;
 data[n->id]=(struct regulator_init_data){n,desc}; return &data[n->id];
}
static struct regulator_dev *devm_regulator_register(struct device *d,const struct regulator_desc *desc,struct regulator_config *config)
{
 assert(config->dev==d && config->driver_data && config->of_node);
 assert(config->init_data==&data[desc->id]);
 assert(config->init_data->node==config->of_node && config->init_data->desc==desc);
 registrations++; return reg_fail==desc->id?(void *)(intptr_t)-EIO:&rdev;
}
static void *devm_kzalloc(struct device *d,size_t size,int flags)
{assert(d && size==sizeof(sram) && flags==0); memset(&sram,0,sizeof(sram));return alloc_fail?NULL:&sram;}
static void mutex_init(int *m) {*m=1;}
static void platform_set_drvdata(struct platform_device *p,void *v) {assert(p && v==&sram);}
static int dev_err_probe(struct device *d,int ret,const char *msg) {assert(d && msg);return ret;}
static void dev_info(struct device *d,const char *msg) {assert(d && msg);}
''' + functions + r'''
static void reset(void)
{ memset(refs,0,sizeof(refs));missing=disabled=parse_fail=reg_fail=-1;alloc_fail=parses=registrations=0; }
static void released(void) {for(int i=0;i<5;i++)assert(refs[i]==0);}
int main(void)
{
 struct device dev={&nodes[3]};struct variant variant={2};
 struct da9213_legacy chip={&dev,&variant};struct platform_device pdev={{&nodes[2]}};
 reset();assert(da9213_legacy_register_regulators(&chip)==0);assert(parses==2 && registrations==2);released();
 for(int i=0;i<2;i++) {
  reset();parse_fail=i;assert(da9213_legacy_register_regulators(&chip)==-ENOMEM);assert(registrations==i);released();
  reset();reg_fail=i;assert(da9213_legacy_register_regulators(&chip)==-EIO);assert(registrations==i+1);released();
  reset();missing=i;assert(da9213_legacy_register_regulators(&chip)==-EINVAL);released();
  reset();disabled=i;assert(da9213_legacy_register_regulators(&chip)==0);assert(parses==1 && registrations==1);released();
 }
 reset();missing=4;assert(da9213_legacy_register_regulators(&chip)==-EINVAL);released();
 reset();assert(mt6797_bigidvfs_sram_probe(&pdev)==0);assert(parses==1 && registrations==1);
 reset();alloc_fail=1;assert(mt6797_bigidvfs_sram_probe(&pdev)==-ENOMEM);assert(parses==0 && registrations==0);
 reset();parse_fail=2;assert(mt6797_bigidvfs_sram_probe(&pdev)==-ENOMEM);assert(parses==1 && registrations==0);
 reset();reg_fail=2;assert(mt6797_bigidvfs_sram_probe(&pdev)==-EIO);assert(parses==1 && registrations==1);
 puts("A72 regulator registration: pass exact DT/init-data wiring and failure cleanup");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src, exe = Path(tmp,'test.c'),Path(tmp,'test');src.write_text(c)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-Wall','-Wextra','-Werror','-Wno-sign-compare',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
