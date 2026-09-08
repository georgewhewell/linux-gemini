#!/usr/bin/env python3
"""Compile the actual immutable RF-clock DT loader with failing OF adapters."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text()
loader=s[s.index('static void mt6797_ccci_read_rf_clock('):s.index('static void mt6797_ccci_read_sim_eint(')]
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
struct device_node {bool present,enabled;int counts[2],errors[2];u32 values[2][4];};
struct device {struct device_node *of_node;};
struct mt6797_ccci {struct device *dev;bool rf_clock_present;struct {uint8_t configuration[4];uint16_t driving[4];} rf_clock;};
static struct device_node node;static int puts_count,reads;
static struct device_node *of_get_child_by_name(struct device_node *p,const char *name)
{(void)p;assert(!strcmp(name,"rf-clock"));return node.present?&node:NULL;}
static bool of_device_is_available(struct device_node *n){return n->enabled;}
static unsigned prop(const char *p){if(!strcmp(p,"mediatek,configuration"))return 0;assert(!strcmp(p,"mediatek,driving"));return 1;}
static int of_property_count_u32_elems(struct device_node *n,const char *p){return n->counts[prop(p)];}
static int of_property_read_u32_array(struct device_node *n,const char *p,u32 *v,unsigned count)
{assert(count==4);reads++;unsigned i=prop(p);if(n->errors[i])return n->errors[i];memcpy(v,n->values[i],16);return 0;}
static void of_node_put(struct device_node *n){assert(n==&node);puts_count++;}
'''
tests=r'''
int main(void){struct device dev={&node};struct mt6797_ccci c={.dev=&dev};
 const struct device_node good={true,true,{4,4},{0,0},{{2,0,1,1},{1,1,1,1}}};
 node=good;mt6797_ccci_read_rf_clock(&c);assert(c.rf_clock_present&&puts_count==1&&reads==2);
 for(unsigned i=0;i<4;i++){assert(c.rf_clock.configuration[i]==good.values[0][i]);assert(c.rf_clock.driving[i]==1);}
 for(unsigned i=0;i<2;i++)for(int n=-2;n<=6;n++)if(n!=4){node=good;node.counts[i]=n;memset(&c,0,sizeof(c));c.dev=&dev;
  mt6797_ccci_read_rf_clock(&c);assert(!c.rf_clock_present);}
 for(unsigned i=0;i<2;i++){node=good;node.errors[i]=-5;memset(&c,0,sizeof(c));c.dev=&dev;
  mt6797_ccci_read_rf_clock(&c);assert(!c.rf_clock_present);}
 for(unsigned i=0;i<2;i++)for(unsigned j=0;j<4;j++){node=good;node.values[i][j]=i?4:3;memset(&c,0,sizeof(c));c.dev=&dev;
  mt6797_ccci_read_rf_clock(&c);assert(!c.rf_clock_present);}
 for(unsigned i=0;i<2;i++){node=good;node.enabled=i;node.present=!i;memset(&c,0,sizeof(c));c.dev=&dev;
  int before=puts_count;mt6797_ccci_read_rf_clock(&c);assert(!c.rf_clock_present&&puts_count-before==!i);}
 puts("PASS actual RF DT loader: exact board policy, missing/disabled child, wrong counts, failed reads, every invalid element; no partial publication");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-rf-metadata-') as d:
 p=Path(d);(p/'test.c').write_text(shim+loader+tests)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
