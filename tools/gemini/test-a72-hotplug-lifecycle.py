#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute native A72 lifecycle callbacks and transaction core with I/O doubles."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'drivers/soc/mediatek/mt6797-a72-power.c').read_text()
def block(start, end):
    return s[s.index(start):s.index(end, s.index(start))]
def function(name):
    a=s.index(name+'(')
    return 'static int '+s[a:s.index('\n}\n',a)+3]
core='\n'.join(re.sub(r'^#include .*\n','',(root/f).read_text(),flags=re.M) for f in (
    'drivers/soc/mediatek/mt6797-a72-power.h',
    'drivers/soc/mediatek/mt6797-a72-power-core.c'))
struct=block('struct mt6797_a72_preflight {','static DEFINE_MUTEX(')
live=block('static int mt6797_a72_snapshot(', 'static int mt6797_a72_preflight_parse_cpus(')
off=block('struct mt6797_a72_off_context {','static int\nmt6797_a72_cpu_on_prepare(')
callbacks='\n'.join(function(n) for n in ('mt6797_a72_cpu_on_prepare','mt6797_a72_cpu_on_abort','mt6797_a72_cpu_online_commit'))
c=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64;
#define BIT(n) (1U<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define static_assert _Static_assert
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,x) ((v)=(x))
#define EXPORT_SYMBOL_GPL(x)
#define MT6797_A72_CPU_COUNT 2
#define MT6797_A72_FIRST_CPU 8
#define MT6797_A72_IDVFS_CONTROL_REG 0x10222470U
#define MT6797_A72_IDVFS_FENCE_REG 0x102224a0U
#define dev_info(...) ((void)0)
typedef unsigned cpumask_t;
struct device {int tag;}; struct clk {int tag;}; struct regulator {int tag;};
struct reset_control {int tag;}; struct regmap {int tag;};
struct notifier_block {int tag;}; struct hlist_node {int tag;};
struct mutex {bool held;};
struct mt6797_sram_sample {u32 control,calibration;};
#define hlist_entry(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
''' + core + struct + r'''
static struct mutex mt6797_a72_owner_lock;
static struct mt6797_a72_preflight owner,*mt6797_a72_owner;
static struct device dev;static struct clk clk;static struct regulator vp,vs;
static struct reset_control resetctl;static struct regmap spm;
static unsigned online,running,calls,fail_at,firmwares,dcms,rail_disables,sram_starts;
static bool effect,reset_asserted,sram_armed,clock_saved,clock_running,dcm_on;
static u32 power,status,status2,iso,idvfs,sram_control,sram_cal;
static int vproc_enabled;
static int io(void) {return ++calls==fail_at?-EIO:0;}
static void mutex_lock(struct mutex *m){assert(!m->held);m->held=true;}
static void mutex_unlock(struct mutex *m){assert(m->held);m->held=false;}
static void lockdep_assert_cpus_held(void){}
static bool cpu_online(unsigned c){assert(c<10);return !!(online&BIT(c));}
static bool cpumask_test_cpu(unsigned c,const cpumask_t *m){return !!(*m&BIT(c));}
static bool cpumask_intersects(const cpumask_t *a,const cpumask_t *b){return !!(*a&*b);}
#define cpu_online_mask (&online)
static unsigned raw_smp_processor_id(void){return running;}
static u32 read_cpuid_id(void){return MT6797_A72_R0P1_MIDR;}
static bool arm64_late_cpu_profile_ready(void){return true;}
static u32 *reg(u32 r){switch(r){case 0x188:return &status;case 0x18c:return &status2;
 case 0x218:return &power;case 0x290:return &iso;default:assert(0);return NULL;}}
static int regmap_read(struct regmap *m,u32 r,u32 *v){assert(m==&spm);int e=io();if(!e)*v=*reg(r);return e;}
static int regmap_update_bits(struct regmap *m,u32 r,u32 mask,u32 v){assert(m==&spm);int e=io();if(!e||effect)*reg(r)=(*reg(r)&~mask)|(v&mask);return e;}
static int mt6797_a72_secure_read(u32 r,u32 *v){int e=io();assert(r==MT6797_A72_IDVFS_CONTROL_REG||r==MT6797_A72_IDVFS_FENCE_REG);if(!e)*v=r==MT6797_A72_IDVFS_CONTROL_REG?idvfs:0x00ff1100;return e;}
static int regulator_is_enabled(struct regulator *r){assert(r==&vp);int e=io();return e?:vproc_enabled;}
static int regulator_get_voltage(struct regulator *r){int e=io();return e?e:r==&vp?1000000:sram_control?1100000:1050000;}
static int regulator_enable(struct regulator *r){assert(r==&vp);int e=io();if(!e||effect)vproc_enabled=1;return e;}
static int regulator_disable(struct regulator *r){assert(r==&vp);int e=io();rail_disables++;
 assert(!(status&(BIT(7)|BIT(6)|BIT(17))) && !(status2&(BIT(7)|BIT(6)|BIT(17))) && (iso&BIT(1)));
 if(!e||effect)vproc_enabled=0;return e;}
static int reset_control_status(struct reset_control *r){assert(r==&resetctl);int e=io();return e?:reset_asserted;}
static int reset_control_assert(struct reset_control *r){assert(r==&resetctl);int e=io();if(!e||effect)reset_asserted=true;return e;}
static int reset_control_deassert(struct reset_control *r){assert(r==&resetctl);int e=io();if(!e||effect)reset_asserted=false;return e;}
static void usleep_range(unsigned a,unsigned b){assert(b==a+50);}
static int mt6797_sram_read_boot_sample(struct device *d,struct regulator *r,struct mt6797_sram_sample *v)
{assert(d==&dev&&r==&vs);int e=io();if(!e)*v=(struct mt6797_sram_sample){sram_control,sram_cal};return e;}
static int mt6797_sram_prepare_cpu8(struct device *d,struct regulator *r,unsigned c,struct mt6797_sram_sample *v)
{assert(d==&dev&&r==&vs&&c==8&&!cpu_online(8)&&!cpu_online(9));assert(!owner.started||sram_armed);
 int e=io();sram_starts++;sram_armed=false;if(!e||effect){sram_control=0x4008fb;sram_cal=0x8967;*v=(struct mt6797_sram_sample){sram_control,sram_cal};}return e;}
static int mt6797_sram_record_cpu8_off(struct device *d,struct regulator *r)
{assert(d==&dev&&r==&vs&&!vproc_enabled&&!status&&!status2&&(iso&BIT(1)));int e=io();if(!e)sram_armed=true;return e;}
static int mt6797_cpuclk_check_mp2_boot(struct device *d,struct clk *c)
{assert(d==&dev&&c==&clk);int e=io();return e?:clock_running?-EBUSY:0;}
static int mt6797_cpuclk_check_mp2_restart(struct device *d,struct clk *c)
{assert(d==&dev&&c==&clk);int e=io();return e?:!clock_saved||clock_running?-EBUSY:0;}
static int mt6797_cpuclk_set_mp2_sync_dcm(struct device *d,struct clk *c,bool on)
{assert(d==&dev&&c==&clk&&clock_running);int e=io();dcms++;
 if(!e||effect){dcm_on=on;clock_saved=!on;}return e;}
''' + live + off + callbacks + r'''
static int firmware(unsigned cpu)
{
 assert(!cpu_online(cpu));if(cpu==8)assert(!dcm_on && clock_saved);
 int e=io();firmwares++;if(e&&!effect)return e;
 status&=~BIT(cpu==8?7:6);status2=status;
 if(cpu==8){status=status2=0;iso|=BIT(1);power=BIT(4);clock_running=false;}
 return e;
}
static void reset(void)
{
 memset(&owner,0,sizeof(owner));mt6797_a72_owner=&owner;
 owner.dev=&dev;owner.mp2_clk=&clk;owner.vproc_big=&vp;owner.sram_big=&vs;
 owner.pwrap_reset=&resetctl;owner.spm=&spm;owner.cpu_mask=BIT(8)|BIT(9);
 assert(!mt6797_a72_build_plan(&owner.plan));
 online=BIT(0)|BIT(4);running=0;calls=fail_at=firmwares=dcms=rail_disables=sram_starts=0;
 effect=reset_asserted=sram_armed=clock_saved=clock_running=dcm_on=false;
 status=status2=idvfs=sram_control=sram_cal=0;iso=BIT(1);power=BIT(4);vproc_enabled=0;
}
static int prepare(unsigned cpu){return mt6797_a72_cpu_on_prepare(cpu,&owner.cpuhp_node);}
static void up(unsigned cpu)
{
 running=0;assert(!prepare(cpu));online|=BIT(cpu);status|=BIT(17)|BIT(cpu==8?7:6);status2=status;
 clock_running=true;power=BIT(0)|BIT(2);running=cpu;
 assert(!mt6797_a72_cpu_online_commit(cpu,&owner.online_node));running=0;
}
static void arm(unsigned cpu)
{running=cpu;assert(!mt6797_a72_cpu_off_prepare(cpu,&owner.online_node));
 assert(!mt6797_a72_power_cpu_disable_admit(cpu));online&=~BIT(cpu);running=0;}
static void down(unsigned cpu)
{arm(cpu);assert(!mt6797_a72_power_cpu_kill(cpu,firmware));
 assert(!mt6797_a72_cpu_on_abort(cpu,&owner.cpuhp_node));}
static void clean(void){assert(!owner.operation_lock.held&&!mt6797_a72_owner_lock.held);}
static void pair(void){reset();up(8);up(9);}
int main(void)
{
 pair();unsigned saved=calls;running=8;
 assert(mt6797_a72_cpu_off_prepare(8,&owner.online_node)==-EBUSY);
 assert(calls==saved&&!owner.off_armed[0]);running=9;
 assert(!mt6797_a72_cpu_off_prepare(9,&owner.online_node));
 assert(!mt6797_a72_cpu_online_commit(9,&owner.online_node));
 assert(calls==saved&&!owner.off_armed[1]&&!owner.faulted);clean();
 down(9);up(9);down(9);down(8);
 for(int i=0;i<3;i++){up(8);up(9);down(9);down(8);clean();}
 assert(!vproc_enabled&&sram_armed&&owner.off[0].completed&&!owner.faulted);
 for(unsigned cpu=8;cpu<=9;cpu++) {
  pair();if(cpu==8)down(9);arm(cpu);calls=0;
  assert(!mt6797_a72_power_cpu_kill(cpu,firmware));unsigned count=calls;
  for(unsigned n=1;n<=count;n++)for(unsigned applied=0;applied<2;applied++) {
   pair();if(cpu==8)down(9);arm(cpu);calls=0;fail_at=n;effect=applied;
   assert(mt6797_a72_power_cpu_kill(cpu,firmware)==-EUCLEAN);
   assert(owner.faulted&&owner.off[cpu-8].faulted);clean();
   saved=calls;assert(mt6797_a72_power_cpu_kill(cpu,firmware)==-EUCLEAN);
   assert(calls==saved);
  }
  printf("PASS CPU%u native shutdown: %u I/O boundaries, errors before/after effect\n",cpu,count);
 }
 pair();down(9);down(8);calls=0;assert(!prepare(8));unsigned count=calls;
 for(unsigned n=1;n<=count;n++)for(unsigned applied=0;applied<2;applied++) {
  pair();down(9);down(8);calls=0;fail_at=n;effect=applied;
  assert(prepare(8)<0);assert(!owner.cpu8_armed);clean();
  if(owner.faulted){saved=calls;assert(prepare(8)==-EUCLEAN);assert(calls==saved);}
 }
 printf("PASS native warm startup: %u I/O boundaries, errors before/after effect\n",count);
 pair();saved=calls;assert(mt6797_a72_power_cpu_kill(9,firmware)==-EUCLEAN);
 assert(owner.faulted&&calls==saved);clean();
 reset();assert(mt6797_a72_power_cpu_disable_admit(8)==-EPERM);
 assert(mt6797_a72_power_cpu_disable_admit(4)==-EOPNOTSUPP);
 mt6797_a72_owner=NULL;assert(mt6797_a72_power_cpu_disable_admit(8)==-EPERM);
 puts("PASS native cold/warm cycles, CPU9 re-add, unplug veto cancellation, peer ordering, live-target refusal and vote lifetime");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-a72-lifecycle-') as tmp:
    src,exe=Path(tmp,'test.c'),Path(tmp,'test')
    src.write_text(c)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',
                    '-Wno-sign-compare','-Wno-misleading-indentation','-fsanitize=undefined',
                    str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
