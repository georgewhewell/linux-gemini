#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production CCI policy, boot adoption and ordered voltage voting."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
src=(root/'include/linux/soc/mediatek/mt6797-cci-policy.h').read_text()
src=re.sub(r'^#include .*\n','',src,flags=re.M)
dts=(root/'arch/arm64/boot/dts/mediatek/mt6797-gemini-pda.dts').read_text()
arrays=''
for n in (0,1):
    a=dts.index(f'cluster{n}_opp:'); b=dts.index('\n\t};',a)
    entries=re.findall(r'opp-hz = /bits/ 64 <(\d+)>;\s+opp-microvolt = <(\d+) ',dts[a:b])
    assert len(entries)==16
    arrays+=f'static const struct mt6797_cci_cpu cpu{n}[]={{'+','.join('{'+f'{hz},{uv},true'+'}' for hz,uv in entries)+'};\n'
prelude=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
typedef uint32_t u32;
#define current get_current()
'''
main=r'''
static const u32 golden_hz[]={169,234,325,416,481,533,611,676,754,819,845,884,910,936,975,988};
static const u32 golden_uv[]={770,800,850,900,940,970,1000,1040,1070,1100,1130,1140,1150,1170,1190,1200};
static u32 hz,uv,vote,peer;
static unsigned int calls,fail_at,ignore_at,fences;
static bool partial;
static int fail_errno;
static u32 boot_hz,boot_uv;
static char trace[64];
static int event(char c)
{ assert(!fences);trace[calls++]=c;trace[calls]=0;return calls==fail_at ? fail_errno : 0; }
static int get_hz(void *ctx,u32 *out)
{ assert(!ctx);int ret=event('h');*out=hz;return ret; }
static int get_uv(void *ctx,u32 *out)
{ assert(!ctx);int ret=event('v');*out=uv;return ret; }
static int set_hz(void *ctx,u32 rate)
{
 assert(!ctx);int ret=event('H');
 for(unsigned int i=0;i<16;i++) if(rate==golden_hz[i]*1000000) assert(uv>=golden_uv[i]*1000);
 if(calls!=ignore_at && (!ret || partial)) hz=rate;
 return ret;
}
static int set_vote(void *ctx,u32 target)
{
 assert(!ctx);int ret=event('V');
 if(calls!=ignore_at && (!ret || partial)) { vote=target;uv=vote>peer?vote:peer; }
 for(unsigned int i=0;i<16;i++) if(hz==golden_hz[i]*1000000)
  assert(uv>=golden_uv[i]*1000 || (hz==boot_hz && uv>=boot_uv));
 return ret;
}
static void fence(void *ctx) { assert(!ctx && !fences);fences++; }
static const struct mt6797_cci_io io={get_hz,get_uv,set_hz,set_vote,fence};
static struct mt6797_cci_state state;
static void reset(unsigned int index,u32 other)
{
 hz=golden_hz[index]*1000000;vote=golden_uv[index]*1000;peer=other;uv=vote>peer?vote:peer;
 state=(struct mt6797_cci_state){hz,vote,true,false};
 calls=fail_at=ignore_at=fences=0;partial=false;trace[0]=0;boot_hz=boot_uv=0;fail_errno=-EIO;
}
static void blocked(void)
{
 assert(state.faulted && fences==1);unsigned int before=calls;
 assert(mt6797_cci_transition(&state,&io,NULL,0)==-EHOSTDOWN && calls==before);
 assert(mt6797_cci_adopt(&state,&io,NULL)==-EHOSTDOWN && calls==before);
}
int main(void)
{
 unsigned int policies=0,paths=0,failures=0;
 for(unsigned int i=0;i<16;i++) {
  assert(mt6797_cci_opps[i].hz==golden_hz[i]*1000000);
  assert(mt6797_cci_opps[i].uv==golden_uv[i]*1000);
 }
 const u32 big[]={750000000,1500000000,2600000000U};
 for(unsigned int mask=1;mask<8;mask++)
 for(unsigned int a=0;a<16;a++) for(unsigned int b=0;b<16;b++)
 for(unsigned int c=0;c<3;c++) {
  struct mt6797_cci_cpu cpus[]={cpu0[a],cpu1[b],{big[c],UINT_MAX,true}};
  u32 fastest=0,budget=0;
  for(unsigned int j=0;j<3;j++) {
   cpus[j].available=(mask>>j)&1;
   if(!cpus[j].available) continue;
   if(cpus[j].hz>fastest) fastest=cpus[j].hz;
   if(j<2 && cpus[j].uv>budget) budget=cpus[j].uv;
  }
  unsigned int expected=15;
  for(unsigned int j=0;j<16;j++) if((uint64_t)golden_hz[j]*2000000>=fastest) { expected=j;break; }
  if(golden_uv[expected]*1000>budget) budget=golden_uv[expected]*1000;
  for(unsigned int j=0;j<16;j++) if(golden_uv[j]*1000<=budget) expected=j;
  assert(mt6797_cci_select(cpus)==(int)expected);policies++;
 }
 struct mt6797_cci_cpu cpus[]={{221000000,770000,true},{325000000,770000,true},{750000000,1200000,true}};
 assert(mt6797_cci_select(cpus)==3); /* Fixed A72 forces 416 MHz / 900 mV. */
 cpus[2].available=false;assert(mt6797_cci_select(cpus)==0);
 cpus[0].uv=1100000;assert(mt6797_cci_select(cpus)==9); /* Voltage-dominated 819 MHz. */
 cpus[0].available=cpus[1].available=false;assert(mt6797_cci_select(cpus)==-ENODATA);
 cpus[2]=(struct mt6797_cci_cpu){UINT_MAX,0,true};assert(mt6797_cci_select(cpus)==15);
 cpus[0]=(struct mt6797_cci_cpu){1,UINT_MAX,true};assert(mt6797_cci_select(cpus)==-EINVAL);
 assert(mt6797_cci_select(NULL)==-EINVAL);
 for(unsigned int shared=0;shared<2;shared++)
 for(unsigned int a=0;a<16;a++) for(unsigned int b=0;b<16;b++) {
  reset(a,shared?1200000:0);assert(!mt6797_cci_transition(&state,&io,NULL,b));
  assert(state.hz==golden_hz[b]*1000000 && state.vote_uv==golden_uv[b]*1000 && !fences);
  assert(!strcmp(trace,a==b?"hv":a<b?"hvVvHhv":"hvHhVv"));
  unsigned int n=calls;char normal[64];strcpy(normal,trace);paths++;
  for(unsigned int i=1;i<=n;i++) for(unsigned int part=0;part<2;part++) {
   reset(a,shared?1200000:0);fail_at=i;partial=part;
   assert(mt6797_cci_transition(&state,&io,NULL,b)==(i<=2?-EIO:-EUCLEAN));
   assert(calls==i && state.hz==golden_hz[a]*1000000 && state.vote_uv==golden_uv[a]*1000);
   if(i<=2) assert(!fences);else blocked();failures++;
  }
  for(unsigned int i=1;i<=n;i++) if(normal[i-1]=='H' || normal[i-1]=='V') {
   reset(a,shared?1200000:0);ignore_at=i;
   int ret=mt6797_cci_transition(&state,&io,NULL,b);
   /* A shared rail can mask an ignored vote; provider verifies the vote write. */
   if(ret) { assert(ret==-EUCLEAN);blocked(); }
   else assert(hz==golden_hz[b]*1000000 && uv>=golden_uv[b]*1000);
  }
 }
 unsigned int adopted=0;
 for(unsigned int a=0;a<16;a++) for(u32 supply=770000;supply<=1200000;supply+=10000) {
  reset(a,0);uv=vote=supply;state.ready=false;boot_hz=hz;boot_uv=uv;
  int expected=-1;
  for(unsigned int i=0;i<16;i++)
   if(golden_hz[i]<=golden_hz[a] && golden_uv[i]*1000<=supply) expected=i;
  assert(expected>=0);
  assert(!mt6797_cci_adopt(&state,&io,NULL));
  assert(state.ready && !state.faulted && state.hz==golden_hz[expected]*1000000 &&
         state.vote_uv==golden_uv[expected]*1000);adopted++;
 }
 /* Unlisted firmware rate: no voltage reduction before verified 611 MHz. */
 reset(6,0);hz=629999328;state.ready=false;
 assert(!mt6797_cci_adopt(&state,&io,NULL));
 assert(state.ready && hz==611000000 && state.vote_uv==1000000 && !strcmp(trace,"hvVvHhVv"));
 for(unsigned int i=1;i<=8;i++) for(unsigned int part=0;part<2;part++) {
  reset(6,0);hz=629999328;state.ready=false;fail_at=i;partial=part;
  assert(mt6797_cci_adopt(&state,&io,NULL)==(i<=2?-EIO:-EUCLEAN));
  assert(!state.ready && calls==i);if(i>2)blocked();failures++;
 }
 for(unsigned int i=1;i<=2;i++) {
  reset(6,0);fail_at=i;fail_errno=-EUCLEAN;
  assert(mt6797_cci_transition(&state,&io,NULL,6)==-EUCLEAN);blocked();failures++;
  reset(6,0);state.ready=false;fail_at=i;fail_errno=-EUCLEAN;
  assert(mt6797_cci_adopt(&state,&io,NULL)==-EUCLEAN);blocked();failures++;
 }
 reset(6,0);assert(mt6797_cci_adopt(&state,&io,NULL)==-EALREADY && !calls);
 reset(6,0);state.ready=false;hz=100000000;
 assert(mt6797_cci_adopt(&state,&io,NULL)==-ERANGE && !fences && calls==2);
 reset(6,0);hz++;assert(mt6797_cci_transition(&state,&io,NULL,6)==-EUCLEAN);blocked();
 reset(6,0);uv=990000;assert(mt6797_cci_transition(&state,&io,NULL,6)==-EUCLEAN);blocked();
 reset(6,0);uv=1210000;assert(mt6797_cci_transition(&state,&io,NULL,6)==-EUCLEAN);blocked();
 reset(6,0);assert(mt6797_cci_transition(&state,&io,NULL,16)==-EINVAL && !calls);
 state.ready=false;assert(mt6797_cci_transition(&state,&io,NULL,6)==-EAGAIN && !calls);
 printf("PASS CCI policy: %u CPU combinations, %u ordered paths, %u injected failures, %u boot states and shared votes\n",policies,paths,failures,adopted);
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-cci-policy-') as tmp:
    c,exe=Path(tmp,'test.c'),Path(tmp,'test')
    c.write_text(prelude+src+arrays+main)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
