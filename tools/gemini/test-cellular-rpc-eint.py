#!/usr/bin/env python3
"""Compile actual bounded RPC parser/builder and immutable DT provider loader."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text()
header = (root / 'drivers/net/wwan/mediatek/mtk_mt6797_ccci_rpc.h').read_text()
provider = source[source.index('static void mt6797_ccci_read_sim_eint('):source.index('static int mt6797_ccci_probe(')]
shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef int32_t s32;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define BIT(n) (1U<<(n))
'''
adapter = r'''
struct device_node { bool present, enabled; u32 raw[7]; int counts[7]; };
struct device { struct device_node *of_node; };
struct mt6797_ccci { struct device *dev; struct mt6797_rpc_eint sim_eint[2]; };
static struct device_node nodes[2]; static int node_puts, gets;
static struct device_node *of_get_child_by_name(struct device_node *p,const char *name)
{ (void)p; unsigned n=!strcmp(name,"sim-eint-2"); gets++; return nodes[n].present?&nodes[n]:NULL; }
static bool of_device_is_available(struct device_node *n) { return n->enabled; }
static unsigned prop(const char *p) {
 const char *names[]={"mediatek,eint-number","mediatek,debounce-us","mediatek,active-high",
 "mediatek,edge-triggered","mediatek,socket-type","mediatek,dedicated-enable","mediatek,source-selector"};
 for(unsigned i=0;i<7;i++) { if(!strcmp(names[i],p))return i; }
 assert(0);return 0;
}
static int of_property_count_u32_elems(struct device_node *n,const char *p) {return n->counts[prop(p)];}
static int of_property_read_u32(struct device_node *n,const char *p,u32 *v) {*v=n->raw[prop(p)];return 0;}
static void of_node_put(struct device_node *n) {assert(n);node_puts++;}
'''
tests = r'''
static const u8 request[68]={
 0,0,0,0,68,0,0,0,32,0,0,128,0,0,0,0,5,64,0,0,3,0,0,0,23,0,0,0,
 'M','D','1','_','S','I','M','1','_','H','O','T','_','P','L','U','G','_','E','I','N','T',0,0,
 4,0,0,0,23,0,0,0,4,0,0,0,6,0,0,0};
int main(void) {
 struct device dev={0}; struct mt6797_ccci c={.dev=&dev}; u8 b[80],reply[40];s32 status;
 const u32 expected[2][7]={{0,100,1,0,0,0,1},{1,100,1,0,0,0,0}};
 for(unsigned s=0;s<2;s++) { nodes[s].present=nodes[s].enabled=true;
  for(unsigned t=0;t<7;t++){nodes[s].counts[t]=1;nodes[s].raw[t]=expected[s][t];}
  nodes[s].raw[1]=100000;
 }
 mt6797_ccci_read_sim_eint(&c); assert(gets==2 && node_puts==2);
 for(unsigned s=0;s<2;s++)for(unsigned t=0;t<7;t++) {
  memcpy(b,request,68);b[35]='1'+s;mt6797_rpc_write(b+64,t);
  assert(!mt6797_rpc_eint_reply(b,68,c.sim_eint,reply,40,&status));assert(status==0);
  assert(mt6797_rpc_read(reply+36)==expected[s][t]);
  assert(mt6797_rpc_read(reply+16)==0xffff4005 && mt6797_rpc_read(reply+20)==2);
  assert(mt6797_rpc_read(reply+4)==40 && mt6797_rpc_read(reply+8)==33);
 }
 /* Actual DT loader: each missing/multicell/invalid field stays invalid. */
 for(unsigned t=0;t<7;t++) {
  nodes[0].counts[t]=2;memset(c.sim_eint,0,sizeof(c.sim_eint));mt6797_ccci_read_sim_eint(&c);
  memcpy(b,request,68);mt6797_rpc_write(b+64,t);
  assert(!mt6797_rpc_eint_reply(b,68,c.sim_eint,reply,40,&status));assert(status==-12);
  assert((s32)mt6797_rpc_read(reply+36)==-12);nodes[0].counts[t]=1;
 }
 nodes[0].enabled=false;memset(c.sim_eint,0,sizeof(c.sim_eint));mt6797_ccci_read_sim_eint(&c);
 assert(!mt6797_rpc_eint_reply(request,68,c.sim_eint,reply,40,&status));assert(status==-11);
 nodes[0].enabled=true;memset(c.sim_eint,0,sizeof(c.sim_eint));mt6797_ccci_read_sim_eint(&c);
 memcpy(b,request,68);mt6797_rpc_write(b,0x12345678);mt6797_rpc_write(b+12,1);
 assert(!mt6797_rpc_eint_reply(b,68,c.sim_eint,reply,40,&status));
 assert(mt6797_rpc_read(reply)==0x12345678 && mt6797_rpc_read(reply+12)==1);
 for(unsigned n=0;n<68;n++)assert(mt6797_rpc_eint_reply(b,n,c.sim_eint,reply,40,&status)<0);
 const unsigned offsets[]={12,20,24,52,60};
 for(unsigned i=0;i<ARRAY_SIZE(offsets);i++){memcpy(b,request,68);mt6797_rpc_write(b+offsets[i],UINT32_MAX);
  assert(mt6797_rpc_eint_reply(b,68,c.sim_eint,reply,40,&status)<0);}
 memcpy(b,request,68);mt6797_rpc_write(b+16,0x4006);
 assert(mt6797_rpc_eint_reply(b,68,c.sim_eint,reply,40,&status)==-EOPNOTSUPP);
 const unsigned bad_name[]={28,50,56};
 for(unsigned i=0;i<ARRAY_SIZE(bad_name);i++){memcpy(b,request,68);b[bad_name[i]]^=1;
  assert(!mt6797_rpc_eint_reply(b,68,c.sim_eint,reply,40,&status));assert(status<0);}
 memcpy(b,request,68);b[29]=0;
 assert(!mt6797_rpc_eint_reply(b,68,c.sim_eint,reply,40,&status));assert(status==-2);
 memcpy(b,request,68);mt6797_rpc_write(b+64,7);
 assert(!mt6797_rpc_eint_reply(b,68,c.sim_eint,reply,40,&status));assert(status==-12);
 u32 v;assert(!mt6797_rpc_eint_value(6,112,&v));assert(!mt6797_rpc_eint_value(2,2,&v));
 assert(mt6797_rpc_eint_value(1,100999,&v)&&v==100);
 printf("PASS actual RPC EINT: literal C6 request, all14 board values, DT field/disabled ownership, status errors, header identity, malformed/count/padding/overflow/unsupported guards\n");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-rpc-eint-') as d:
    p=Path(d)
    (p/'test.c').write_text(shim+header+adapter+provider+tests)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=5)
