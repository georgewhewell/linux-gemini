#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the production A72 shutdown core with firmware/regulator boundaries."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
body='\n'.join(re.sub(r'^#include .*\n','',(root/p).read_text(),flags=re.M) for p in (
    'drivers/soc/mediatek/mt6797-a72-power.h',
    'drivers/soc/mediatek/mt6797-a72-power-core.c'))
prelude=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
typedef uint32_t u32;
#define current get_current()
#define BIT(n) (1U<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define static_assert _Static_assert
'''
main=r'''
struct fake {
 struct mt6797_a72_off_sample live;
 unsigned int cpu, calls, fail, corrupt, field, fences, disables, firmware;
 bool effective;
 char trace[20];
};
static int event(struct fake *f,char code)
{ assert(f->calls<19);f->trace[f->calls++]=code;return f->calls==f->fail?-EIO:0; }
static int sample(void *context,struct mt6797_a72_off_sample *out)
{
 struct fake *f=context;int ret=event(f,'S');if(ret)return ret;*out=f->live;
 if(f->calls==f->corrupt) switch(f->field) {
 case 0:out->online_mask|=f->cpu==8?1:2;break;
 case 1:out->power_status^=BIT(7);break;
 case 2:out->power_status_second^=BIT(6);break;
 case 3:out->power_status|=BIT(17);break;
 case 4:out->power_status_second|=BIT(17);break;
 case 5:out->ext_buck_iso^=BIT(1);break;
 case 6:out->mp2_pwr_con^=BIT(0);break;
 case 7:out->mp2_pwr_con^=BIT(4);break;
 case 8:out->idvfs_control|=BIT(0);break;
 case 9:out->vproc_enabled^=1;break;
 case 10:out->vproc_enabled=-EIO;break;
 default:assert(0);
 }
 return 0;
}
static int dcm(void *context)
{ struct fake *f=context;assert(f->cpu==8 && !f->live.online_mask && !f->firmware);return event(f,'D'); }
static int kill(void *context,unsigned int cpu)
{
 struct fake *f=context;assert(f->cpu==cpu && !(f->live.online_mask&BIT(cpu-8)));
 assert(f->live.vproc_enabled==1 && !f->disables);f->firmware++;
 int ret=event(f,'F');if(ret && !f->effective)return ret;
 u32 mask=cpu==8?BIT(7)|BIT(6)|BIT(17):BIT(6);
 f->live.power_status&=~mask;f->live.power_status_second&=~mask;
 if(cpu==8){f->live.mp2_pwr_con=BIT(4);f->live.ext_buck_iso|=BIT(1);}
 return ret;
}
static int disable(void *context)
{
 struct fake *f=context;assert(f->cpu==8 && f->firmware==1 && !f->live.online_mask);
 assert(!(f->live.power_status&(BIT(7)|BIT(6)|BIT(17))));
 assert(!(f->live.power_status_second&(BIT(7)|BIT(6)|BIT(17))));
 assert(f->live.ext_buck_iso&BIT(1));assert(!f->disables++);
 int ret=event(f,'V');if(!ret || f->effective)f->live.vproc_enabled=0;return ret;
}
static void fence(void *context) { struct fake *f=context;f->fences++; }
static const struct mt6797_a72_off_io io={sample,dcm,kill,disable,fence};
static struct fake reset(unsigned int cpu)
{
 struct fake f={.cpu=cpu};
 f.live=(struct mt6797_a72_off_sample){.online_mask=cpu==9?1:0,
  .power_status=BIT(7)|BIT(17)|(cpu==9?BIT(6):0),
  .power_status_second=BIT(7)|BIT(17)|(cpu==9?BIT(6):0),
  .mp2_pwr_con=BIT(0)|BIT(2),.vproc_enabled=1};
 return f;
}
static void terminal(struct fake *f,struct mt6797_a72_off_state *state)
{
 assert(state->faulted && !state->completed && state->error && f->fences==1);
 unsigned int calls=f->calls;
 assert(mt6797_a72_finish_cpu_off(state,&io,f,f->cpu)==-EUCLEAN);
 assert(f->calls==calls && f->fences==1);
}
int main(void)
{
 unsigned int faults=0,corruptions=0;
 for(unsigned int cpu=8;cpu<=9;cpu++) {
  struct fake f=reset(cpu);struct mt6797_a72_off_state state={0};
  assert(!mt6797_a72_finish_cpu_off(&state,&io,&f,cpu));
  assert(!strcmp(f.trace,cpu==8?"SDFSSVSS":"SFSS"));
  assert(state.completed && !state.faulted && state.cluster_off==(cpu==8));
  assert(state.final.vproc_enabled==(cpu==9));assert(f.disables==(cpu==8));
  unsigned int total=f.calls;
  assert(mt6797_a72_finish_cpu_off(&state,&io,&f,cpu)==-EUCLEAN && f.calls==total);
  for(unsigned int call=1;call<=total;call++)for(unsigned int effect=0;effect<2;effect++) {
   f=reset(cpu);f.fail=call;f.effective=effect;state=(struct mt6797_a72_off_state){0};
   assert(mt6797_a72_finish_cpu_off(&state,&io,&f,cpu)==-EUCLEAN);
   assert(f.calls==call);terminal(&f,&state);faults++;
  }
  for(unsigned int call=(cpu==8?4:3);call<=total;call++) {
   if(cpu==8 && call==6)continue;
   for(unsigned int field=0;field<=10;field++) {
    if(cpu==9 && (field==3 || field==4))continue;
    f=reset(cpu);f.corrupt=call;f.field=field;state=(struct mt6797_a72_off_state){0};
    assert(mt6797_a72_finish_cpu_off(&state,&io,&f,cpu)==-EUCLEAN);
    assert(f.calls==call);terminal(&f,&state);
    if(call<(cpu==8?6:5))assert(!f.disables);
    corruptions++;
   }
  }
  /* Preflight must never query firmware for a target still logically online. */
  for(unsigned int mask=0;mask<4;mask++) {
   if(mask==(cpu==9?1U:0U))continue;
   f=reset(cpu);f.live.online_mask=mask;state=(struct mt6797_a72_off_state){0};
   assert(mt6797_a72_finish_cpu_off(&state,&io,&f,cpu)==-EUCLEAN);
   assert(f.calls==1 && !f.firmware && !f.disables);terminal(&f,&state);
  }
  for(unsigned int field=0;field<3;field++) {
   f=reset(cpu);state=(struct mt6797_a72_off_state){0};
   if(field==0)f.live.idvfs_control=1;else f.live.vproc_enabled=field==1?0:-EIO;
   assert(mt6797_a72_finish_cpu_off(&state,&io,&f,cpu)==-EUCLEAN);
   assert(f.calls==1 && !f.firmware && !f.disables);terminal(&f,&state);
  }
  /* Other clusters' acknowledgements must not be treated as MP2 state. */
  f=reset(cpu);state=(struct mt6797_a72_off_state){0};
  f.live.power_status|=BIT(22);f.live.power_status_second|=BIT(21);
  f.live.ext_buck_iso|=BIT(0);f.live.idvfs_control|=BIT(12);
  assert(!mt6797_a72_finish_cpu_off(&state,&io,&f,cpu));
  assert(state.final.ext_buck_iso&BIT(0));
 }
 struct fake f=reset(8);struct mt6797_a72_off_state state={0};
 for(unsigned int cpu=0;cpu<12;cpu++)if(cpu!=8 && cpu!=9)
  assert(mt6797_a72_finish_cpu_off(&state,&io,&f,cpu)==-EINVAL && !f.calls);
 struct mt6797_a72_off_io invalid=io;invalid.firmware_kill=NULL;
 assert(mt6797_a72_finish_cpu_off(&state,&invalid,&f,8)==-EINVAL && !f.calls);
 assert(mt6797_a72_finish_cpu_off(NULL,&io,&f,8)==-EINVAL && !f.calls);
 printf("PASS A72 shutdown: two golden sequences, %u call failures, %u corrupt readbacks, live-target refusal and permanent fencing\n",faults,corruptions);
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-a72-off-') as tmp:
    src,exe=Path(tmp,'test.c'),Path(tmp,'test')
    src.write_text(prelude+body+main)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=undefined','-fno-sanitize-recover=all',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
