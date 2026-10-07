#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production FGADC property routing and optional charger acquisition."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/power/supply/mt6351-fgadc.c').read_text()

def extract(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

functions = '\n'.join(extract(s) for s in (
    'static int mt6351_fgadc_get_property(',
    'static void mt6351_fgadc_external_power_changed(',
    'static int mt6351_fgadc_get_charger(',
))
model = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int64_t s64;
#define EPROBE_DEFER 517
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((intptr_t)(p))
enum power_supply_property {
 POWER_SUPPLY_PROP_MANUFACTURER, POWER_SUPPLY_PROP_MODEL_NAME,
 POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_CURRENT_NOW,
 POWER_SUPPLY_PROP_CHARGE_COUNTER, POWER_SUPPLY_PROP_VOLTAGE_NOW,
 POWER_SUPPLY_PROP_TEMP, POWER_SUPPLY_PROP_CAPACITY,
 POWER_SUPPLY_PROP_VOLTAGE_OCV,
};
enum {POWER_SUPPLY_STATUS_UNKNOWN, POWER_SUPPLY_STATUS_CHARGING,
 POWER_SUPPLY_STATUS_DISCHARGING, POWER_SUPPLY_STATUS_NOT_CHARGING,
 POWER_SUPPLY_STATUS_FULL};
union power_supply_propval { int intval; const char *strval; };
struct device { void *of_node; };
struct power_supply { void *data; };
struct mt6351_fgadc { struct device *dev; struct power_supply *charger; };
static struct device device;
static struct power_supply supplier, battery;
static struct mt6351_fgadc fg={.dev=&device};
static int captures, reads, notifications, capture_error, supplier_error;
static int native_current, native_counter, supplied_value;
static int present, count, acquisitions;
static struct power_supply *acquired;
static enum power_supply_property requested;
static int mt6351_fgadc_model_property(struct mt6351_fgadc *f,
 enum power_supply_property p, union power_supply_propval *v)
{ (void)f; (void)p; (void)v; return -EINVAL; }
static void mt6351_fgadc_model_schedule(struct mt6351_fgadc *f)
{ assert(f==&fg); }
static void *power_supply_get_drvdata(struct power_supply *psy)
{ assert(psy==&battery); return psy->data; }
static int mt6351_fgadc_read(struct mt6351_fgadc *f, s64 *current, s64 *counter)
{
 assert(f==&fg); captures++;
 *current=native_current; *counter=native_counter; return capture_error;
}
static int power_supply_get_property(struct power_supply *psy,
 enum power_supply_property p, union power_supply_propval *val)
{
 assert(psy==&supplier); reads++; requested=p;
 val->intval=supplied_value; return supplier_error;
}
static void power_supply_changed(struct power_supply *psy)
{ assert(psy==&battery); notifications++; }
static void *of_find_property(void *node, const char *name, void *len)
{
 assert(node==&device && !strcmp(name,"power-supplies") && !len);
 return present ? node : NULL;
}
static int of_count_phandle_with_args(void *node,const char *name,void *cells)
{ assert(node==&device && !strcmp(name,"power-supplies") && !cells); return count; }
static struct power_supply *devm_power_supply_get_by_reference(struct device *dev,
 const char *name)
{ assert(dev==&device && !strcmp(name,"power-supplies")); acquisitions++; return acquired; }
'''
main = r'''
int main(void)
{
 union power_supply_propval value;
 battery.data=&fg; device.of_node=&device;
 assert(!mt6351_fgadc_get_charger(&fg) && !acquisitions && !fg.charger);
 assert(!mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_MANUFACTURER,&value));
 assert(!strcmp(value.strval,"MediaTek") && !captures && !reads);
 assert(!mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_MODEL_NAME,&value));
 assert(!strcmp(value.strval,"MT6351 FGADC") && !captures && !reads);
 assert(mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_VOLTAGE_NOW,&value)==-ENODATA);
 assert(mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_TEMP,&value)==-ENODATA);
 assert(mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_CAPACITY,&value)==-EINVAL);
 assert(!captures && !reads);
 present=1;
 for (count=0;count<=2;count+=2)
  assert(mt6351_fgadc_get_charger(&fg)==-EINVAL && !acquisitions);
 count=-EINVAL;
 assert(mt6351_fgadc_get_charger(&fg)==-EINVAL && !acquisitions);
 count=1; acquired=NULL;
 assert(mt6351_fgadc_get_charger(&fg)==-EPROBE_DEFER && acquisitions==1);
 acquired=ERR_PTR(-ENOMEM);
 assert(mt6351_fgadc_get_charger(&fg)==-ENOMEM && acquisitions==2);
 acquired=&supplier;
 assert(!mt6351_fgadc_get_charger(&fg) && fg.charger==&supplier && acquisitions==3);
 /* Supplier values do not depend on a working FGADC latch or alter units. */
 capture_error=-ETIMEDOUT;
 for (int p=POWER_SUPPLY_PROP_VOLTAGE_NOW;p<=POWER_SUPPLY_PROP_TEMP;p++) {
  supplied_value=p==POWER_SUPPLY_PROP_VOLTAGE_NOW ? 4284000 : -120;
  assert(!mt6351_fgadc_get_property(&battery,p,&value));
  assert(value.intval==supplied_value && requested==p && !captures);
  int errors[]={-EIO,-ENODATA,-ENODEV,-EINVAL};
  for (unsigned int i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
   supplier_error=errors[i];
   assert(mt6351_fgadc_get_property(&battery,p,&value)==errors[i]);
   assert(!captures);
  }
  supplier_error=0;
 }
 /* All charger states survive gauge sign changes and failed gauge latches. */
 for (native_current=-2360;native_current<=2360;native_current+=2360)
 for (supplied_value=POWER_SUPPLY_STATUS_UNKNOWN;
      supplied_value<=POWER_SUPPLY_STATUS_FULL;supplied_value++) {
  assert(!mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_STATUS,&value));
  assert(value.intval==supplied_value && requested==POWER_SUPPLY_PROP_STATUS && !captures);
 }
 supplier_error=-EIO;
 assert(mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_STATUS,&value)==-EIO);
 supplier_error=-ENODEV;
 assert(mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_STATUS,&value)==-ENODEV);
 assert(!captures);
 supplier_error=0;
 int old_reads=reads;
 fg.charger=NULL;
 for (int p=POWER_SUPPLY_PROP_STATUS;p<=POWER_SUPPLY_PROP_CHARGE_COUNTER;p++) {
  assert(mt6351_fgadc_get_property(&battery,p,&value)==-ENODATA);
  capture_error=-EIO;
  assert(mt6351_fgadc_get_property(&battery,p,&value)==-EIO);
  capture_error=-ETIMEDOUT;
 }
 capture_error=0;
 for (native_current=-2360;native_current<=2360;native_current+=2360) {
  native_counter=native_current*3;
  fg.charger=&supplier;
  assert(!mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_CURRENT_NOW,&value));
  assert(value.intval==native_current);
  assert(!mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_CHARGE_COUNTER,&value));
  assert(value.intval==native_counter);
  fg.charger=NULL;
  assert(!mt6351_fgadc_get_property(&battery,POWER_SUPPLY_PROP_STATUS,&value));
  assert(value.intval==(native_current>0 ? POWER_SUPPLY_STATUS_CHARGING :
   native_current<0 ? POWER_SUPPLY_STATUS_DISCHARGING : POWER_SUPPLY_STATUS_NOT_CHARGING));
 }
 assert(reads==old_reads);
 int old_captures=captures;
 mt6351_fgadc_external_power_changed(&battery);
 assert(notifications==1 && captures==old_captures && reads==old_reads);
 puts("PASS FGADC native/supplier routing, errors, units, acquisition and notifications");
}
'''
with tempfile.TemporaryDirectory(prefix='mt6351-fgadc-supplier-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(model + functions + main)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', '-Wno-sign-compare', str(src),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# Supplier lookup and core dependency population both need a firmware node on
# the registered power-supply device, not only its I2C parent.
bq_source = (root / 'drivers/power/supply/bq25890_charger.c').read_text()
start = bq_source.index('static int bq25890_power_supply_init(')
bq_init = bq_source[start:bq_source.index('\n}\n', start) + 3]
bq_model = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define GFP_KERNEL 0
#define PTR_ERR_OR_ZERO(p) ((uintptr_t)(p)>=(uintptr_t)-4095 ? (intptr_t)(p) : 0)
struct device { void *fwnode; };
struct power_supply_desc { const char *name; const int *properties; unsigned num_properties; };
struct power_supply { int unused; };
struct power_supply_config { void *drv_data, *fwnode; char **supplied_to; unsigned num_supplicants; };
struct bq25890_device {
 struct device *dev; int id; char name[40]; bool read_only, read_only_adc;
 struct power_supply_desc desc; struct power_supply *charger;
};
static const int normal_props[]={1,2,3}, ro_props[]={1}, adc_props[]={1,2};
static const struct power_supply_desc bq25890_power_supply_desc={.properties=normal_props,.num_properties=3};
static const struct power_supply_desc bq25890_read_only_desc={.properties=ro_props,.num_properties=1};
#define bq25890_read_only_adc_props adc_props
static char *bq25890_charger_supplied_to[]={"main-battery"};
static int bq25890_id_mutex, bq25890_id, registrations;
static struct power_supply supply;
static void *dev_fwnode(struct device *d) { return d->fwnode; }
static void mutex_lock(int *m) { assert(!*m); *m=1; }
static void mutex_unlock(int *m) { assert(*m); *m=0; }
static int idr_alloc(int *idr,void *bq,int lo,int hi,int flags)
{ assert(idr==&bq25890_id && bq && !lo && !hi && !flags && bq25890_id_mutex); return 0; }
static struct power_supply *devm_power_supply_register(struct device *dev,
 struct power_supply_desc *desc,struct power_supply_config *cfg)
{
 struct bq25890_device *bq=cfg->drv_data;
 assert(bq && bq->dev==dev && !bq25890_id_mutex);
 assert(cfg->fwnode==dev->fwnode && cfg->fwnode);
 assert(!strcmp(desc->name,"bq25890-charger-0"));
 assert(desc->properties==(bq->read_only ? bq->read_only_adc ? adc_props : ro_props : normal_props));
 assert(desc->num_properties==(bq->read_only ? bq->read_only_adc ? 2 : 1 : 3));
 assert(cfg->num_supplicants==(bq->read_only ? 0 : 1));
 registrations++; return &supply;
}
'''
bq_main = r'''
int main(void)
{
 struct device dev={.fwnode=&dev};
 for (int mode=0;mode<3;mode++) {
  struct bq25890_device bq={.dev=&dev,.read_only=mode>0,.read_only_adc=mode==2};
  assert(!bq25890_power_supply_init(&bq) && bq.charger==&supply);
 }
 assert(registrations==3);
 puts("PASS charger firmware-node registration in normal and both read-only modes");
}
'''
with tempfile.TemporaryDirectory(prefix='bq25890-supplier-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(bq_model + bq_init + bq_main)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
