#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run production FGADC model configuration, sampling, publication and PM callbacks."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/power/supply/mt6351-fgadc.c').read_text()
header = (root / 'drivers/power/supply/mt6351-fgadc-model.h').read_text()
header = re.sub(r'^#include .*\n','',header,flags=re.M)
def extract(signature):
    start=source.index(signature)
    return source[start:source.index('\n}\n',start)+3]
functions='\n'.join(extract(signature) for signature in (
    'static int mt6351_fgadc_model_sample(', 'static void mt6351_fgadc_model_work(',
    'static int mt6351_fgadc_model_property(', 'static void mt6351_fgadc_model_schedule(',
    'static void mt6351_fgadc_model_stop(', 'static void mt6351_fgadc_model_start(',
    'static int mt6351_fgadc_parse_model(', 'static int mt6351_fgadc_model_battery(',
    'static int mt6351_fgadc_suspend(', 'static int mt6351_fgadc_resume(',
))
prelude=r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int64_t s64;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint32_t u32;
#define ARRAY_SIZE(a) ((int)(sizeof(a)/sizeof((a)[0])))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define clamp(v,a,b) min(max(v,a),b)
#define clamp_t(t,v,a,b) min(max((t)(v),(t)(a)),(t)(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define div_s64(a,b) ((a)/(b))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_delayed_work(w) container_of(w,struct delayed_work,work)
#define HZ 100
#define GFP_KERNEL 0
#define POWER_SUPPLY_OCV_TEMP_MAX 20
struct power_supply_vbat_ri_table {int vbat_uv,ri_uohm;};
struct power_supply_battery_ocv_table {int ocv,capacity;};
struct power_supply_battery_info {
 const struct power_supply_battery_ocv_table *ocv_table[20];
 int ocv_temp[20],ocv_table_size[20];
};
enum {POWER_SUPPLY_STATUS_UNKNOWN,POWER_SUPPLY_STATUS_CHARGING,
 POWER_SUPPLY_STATUS_DISCHARGING,POWER_SUPPLY_STATUS_NOT_CHARGING,POWER_SUPPLY_STATUS_FULL};
enum power_supply_property {POWER_SUPPLY_PROP_VOLTAGE_NOW,POWER_SUPPLY_PROP_CURRENT_NOW,
 POWER_SUPPLY_PROP_STATUS,POWER_SUPPLY_PROP_TEMP,POWER_SUPPLY_PROP_CAPACITY,POWER_SUPPLY_PROP_VOLTAGE_OCV};
union power_supply_propval {int intval;};
struct device_node {int unused;};
struct device {struct device_node *of_node;void *data;};
struct power_supply {struct power_supply_battery_info *battery_info;};
struct work_struct {int unused;};
struct delayed_work {struct work_struct work;};
'''
boundary=r'''
struct mt6351_fgadc {
 struct device *dev;
 struct power_supply *psy,*charger;
 struct mt6351_fg_model model;
 struct mt6351_fg_estimate estimate;
 struct delayed_work model_work;
 int model_lock,model_error;
 bool model_active;
};
static struct device_node node;
static struct device dev={.of_node=&node};
static struct power_supply battery,charger;
static struct mt6351_fgadc fg={.dev=&dev,.psy=&battery,.charger=&charger,.model_error=-ENODATA};
static void *system_freezable_wq=(void *)1;
static int queued,canceled,notified,reads,captures,delay;
static u64 seconds=1;
static s64 current_ua=-1000000,counter_uah=100000;
static int voltage_uv=3600000,temp=250,status=POWER_SUPPLY_STATUS_DISCHARGING;
static int supplier_error,gauge_error,error_property;
static int present=1,monitored=1,config_count=2,res_count=4,property_error,allocation_fail;
static u32 config[]={25,1000000};
static u32 resistance[]={4200000,100000,3200000,100000};
static const struct power_supply_battery_ocv_table ocv[]={{4200000,100},{3200000,0}};
static struct power_supply_battery_info info={.ocv_table={ocv},.ocv_temp={25},.ocv_table_size={2}};
static void *allocations[32];
static int allocated;
static void mutex_lock(int *lock) {assert(!*lock);*lock=1;}
static void mutex_unlock(int *lock) {assert(*lock);*lock=0;}
static u64 ktime_get_boottime_seconds(void) {return seconds;}
static int power_supply_get_property(struct power_supply *psy,enum power_supply_property p,
 union power_supply_propval *v) {
 assert(psy==&charger && fg.model_lock);reads++;
 if (supplier_error && (int)p==error_property)return supplier_error;
 switch(p) {case POWER_SUPPLY_PROP_VOLTAGE_NOW:v->intval=voltage_uv;break;
 case POWER_SUPPLY_PROP_TEMP:v->intval=temp;break;
 case POWER_SUPPLY_PROP_STATUS:v->intval=status;break;default:assert(0);}
 return 0;
}
static int mt6351_fgadc_read(struct mt6351_fgadc *f,s64 *ua,s64 *uah) {
 assert(f==&fg && fg.model_lock);captures++;*ua=current_ua;*uah=counter_uah;return gauge_error;
}
static void queue_delayed_work(void *wq,struct delayed_work *work,int d) {
 assert(wq==system_freezable_wq && work==&fg.model_work && fg.model_lock && fg.model_active);
 queued++;delay=d;
}
static void mod_delayed_work(void *wq,struct delayed_work *work,int d) {queue_delayed_work(wq,work,d);}
static void cancel_delayed_work_sync(struct delayed_work *work) {
 assert(work==&fg.model_work && !fg.model_lock && !fg.model_active && !fg.estimate.initialized);canceled++;
}
static void power_supply_changed(struct power_supply *psy) {assert(psy==&battery && !fg.model_lock);notified++;}
static void *dev_get_drvdata(struct device *d) {assert(d==&dev);return &fg;}
static void *of_find_property(struct device_node *np,const char *name,void *len) {
 assert(np==&node && !len);
 if(!strcmp(name,"mediatek,capacity-model"))return present ? np:NULL;
 assert(!strcmp(name,"monitored-battery"));return monitored ? np:NULL;
}
static int of_property_count_u32_elems(struct device_node *np,const char *name) {
 assert(np==&node);
 if(!strcmp(name,"mediatek,capacity-model"))return config_count;
 assert(!strcmp(name,"mediatek,resistance-table-0"));return res_count;
}
static int of_property_read_u32_array(struct device_node *np,const char *name,u32 *data,int n) {
 assert(np==&node);if(property_error)return property_error;
 const u32 *input=!strcmp(name,"mediatek,capacity-model")?config:resistance;
 assert(n==(input==config?2:4));memcpy(data,input,n*sizeof(*data));return 0;
}
static void *devm_kcalloc(struct device *d,int n,size_t size,int flags) {
 assert(d==&dev && flags==GFP_KERNEL && allocated<32);
 if(allocation_fail)return NULL;
 void *p=calloc(n,size);assert(p);allocations[allocated++]=p;return p;
}
'''
main=r'''
int main(void)
{
 union power_supply_propval val={.intval=123};
 assert(mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_CAPACITY,&val)==-EINVAL);
 present=0;assert(!mt6351_fgadc_parse_model(&fg) && !fg.model.count);
 present=1;fg.charger=NULL;assert(mt6351_fgadc_parse_model(&fg)==-EINVAL);fg.charger=&charger;
 monitored=0;assert(mt6351_fgadc_parse_model(&fg)==-EINVAL);monitored=1;
 for(config_count=0;config_count<4;config_count++)if(config_count!=2)
  assert(mt6351_fgadc_parse_model(&fg)==-EINVAL);
 config_count=10;assert(mt6351_fgadc_parse_model(&fg)==-EINVAL);config_count=2;
 property_error=-EIO;assert(mt6351_fgadc_parse_model(&fg)==-EIO);property_error=0;
 config[0]=INT_MAX;assert(mt6351_fgadc_parse_model(&fg)==-EINVAL);config[0]=25;
 res_count=3;assert(mt6351_fgadc_parse_model(&fg)==-EINVAL);res_count=4;
 allocation_fail=1;assert(mt6351_fgadc_parse_model(&fg)==-ENOMEM);allocation_fail=0;
 assert(!mt6351_fgadc_parse_model(&fg));
 assert(mt6351_fgadc_model_battery(&fg)==-EINVAL);
 battery.battery_info=&info;info.ocv_temp[0]=0;
 assert(mt6351_fgadc_model_battery(&fg)==-EINVAL);info.ocv_temp[0]=25;
 assert(!mt6351_fgadc_model_battery(&fg));
 assert(mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_CAPACITY,&val)==-ENODATA && val.intval==123);
 mt6351_fgadc_model_start(&fg);assert(queued==1 && delay==0);
 mt6351_fgadc_model_work(&fg.model_work.work);
 assert(queued==2 && delay==5*HZ && notified==1 && reads==3 && captures==1);
 assert(!mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_CAPACITY,&val) && val.intval==50);
 assert(!mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_VOLTAGE_OCV,&val) && val.intval==3700000);
 /* Partial supplier reads cannot publish partial estimates or advance CAR. */
 for(error_property=0;error_property<=POWER_SUPPLY_PROP_TEMP;error_property++) {
  if(error_property==POWER_SUPPLY_PROP_CURRENT_NOW)continue;
  supplier_error=-EIO;int old=captures;seconds+=5;
  mt6351_fgadc_model_work(&fg.model_work.work);
  assert(captures==old && fg.estimate.counter_uah==100000);
  assert(mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_CAPACITY,&val)==-EIO);
  supplier_error=0;mt6351_fgadc_model_work(&fg.model_work.work);
  assert(!mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_CAPACITY,&val));
 }
 gauge_error=-ETIMEDOUT;seconds+=5;mt6351_fgadc_model_work(&fg.model_work.work);
 assert(mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_CAPACITY,&val)==-ENODATA);
 gauge_error=0;current_ua=(s64)INT_MAX+1;mt6351_fgadc_model_work(&fg.model_work.work);
 assert(mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_CAPACITY,&val)==-ERANGE);
 current_ua=-1000000;mt6351_fgadc_model_work(&fg.model_work.work);
 seconds+=61;
 assert(mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_CAPACITY,&val)==-ENODATA);
 mt6351_fgadc_model_schedule(&fg);assert(delay==HZ);
 /* Stop prevents both self requeue and external-power rescheduling. */
 assert(!mt6351_fgadc_suspend(&dev) && canceled==1);
 int old_queued=queued,old_reads=reads;
 mt6351_fgadc_model_schedule(&fg);mt6351_fgadc_model_work(&fg.model_work.work);
 assert(queued==old_queued && reads==old_reads);
 assert(mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_CAPACITY,&val)==-ENODATA);
 voltage_uv=3700000;
 assert(!mt6351_fgadc_resume(&dev) && delay==0);
 mt6351_fgadc_model_work(&fg.model_work.work);
 assert(!mt6351_fgadc_model_property(&fg,POWER_SUPPLY_PROP_CAPACITY,&val) && val.intval==60);
 mt6351_fgadc_model_stop(&fg);assert(canceled==2);
 for(int i=0;i<allocated;i++)free(allocations[i]);
 puts("PASS model configuration errors, coherent sampling, failures/staleness, notifications and suspend/resume/stop callbacks");
}
'''
with tempfile.TemporaryDirectory(prefix='mt6351-model-lifecycle-') as tmp:
    src,exe=Path(tmp,'test.c'),Path(tmp,'test')
    src.write_text(prelude+header+boundary+functions+main)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=undefined','-fno-sanitize-recover=all',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
