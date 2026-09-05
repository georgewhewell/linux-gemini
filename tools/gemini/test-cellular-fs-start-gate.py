#!/usr/bin/env python3
"""Execute actual cold-probe first-write gates; verify unchanged CCCI timer."""
import os
import hashlib
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
owner=(root/'drivers/soc/mediatek/mt6797-md1-cold-owner.c').read_text()
probe=owner[owner.index('static int mt6797_md1_cold_owner_probe('):]
early=probe[probe.index('\tret = mt6797_md1_fs_provider_require('):probe.index('\tif (!mt6797_md1_lk_handoff_image_loaded()')]
branch=probe[probe.index('"mediatek,stage-mainline-firmware-load"'):]
firmware=branch[branch.index('\t\tret = mtk_wdt_require_boot_recovery('):branch.index('\t\tplan = loaded_plan;')]
initial=probe[probe.index('\t/* Vendor CCCI: final LK remap'):probe.index('\tret = mt6797_md1_cold_owner_post_vector_evidence_get(')]
assert probe.index('mt6797_md1_fs_provider_require(')<probe.index('devm_kzalloc(')
assert firmware.index('mt6797_md1_fs_provider_claim(')<firmware.index('mt6797_md1_firmware_load(')
assert initial.index('mt6797_md1_fs_provider_claim(')<initial.index('mt6797_md1_cold_apply_initial_mpu(')
ccci=(root/'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text()
def timer_tail(s):
    start=s.index('\tret = mt6797_md1_hs_arm(')
    tail=s[start:s.index('\nstatic void mt6797_ccci_remove(',start)]
    # GETATTR adds a process-context workqueue before enabling NAPI. Exclude
    # only that exact independently-tested allocation block, not timer logic.
    allocation='''\tccci->fs_attr_wq = alloc_ordered_workqueue("mt6797-fs-attr", 0);
\tif (!ccci->fs_attr_wq)
\t\treturn -ENOMEM;
\tret = devm_add_action_or_reset(&pdev->dev, mt6797_ccci_fs_attr_destroy, ccci);
\tif (ret)
\t\treturn ret;
'''
    assert tail.count(allocation)==1
    return tail.replace(allocation,'')
# Independently checked against b71187c5; works in a Nix source without .git.
assert hashlib.sha256(timer_tail(ccci).encode()).hexdigest() == 'e53fb1ac6d6436a6dfac203c6327dfad560a033e6aee084c2267de1104559b7a'
assert '#define MT6797_CCCI_HANDSHAKE_TIMEOUT\t(30 * HZ)' in ccci
shim=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define EPROBE_DEFER 517
struct kstatfs {long f_bsize;uint64_t f_blocks,f_bfree;long f_frsize;};
struct mt6797_md1_fs_stats {u32 block_size,blocks,free_blocks;};
struct device {int unused;};struct platform_device {struct device dev;};
static int writes,claims,watchdogs,fail_write,cancel_on_watchdog;
static int events[8];
'''
adapter=r'''
static struct mt6797_fs_state state;
static int mt6797_md1_fs_provider_require(struct device *d){(void)d;return mt6797_fs_require(&state);}
static int mt6797_md1_fs_provider_claim(struct device *d){(void)d;claims++;return mt6797_fs_claim(&state);}
static int mtk_wdt_require_boot_recovery(struct device *d){(void)d;watchdogs++;
 if(cancel_on_watchdog)assert(!mt6797_fs_cancel(&state));
 return 0;}
#define dev_err_probe(d,ret,...) (ret)
static int write_step(int event){assert(state.stage==MT6797_FS_CONSUMED&&watchdogs);events[writes++]=event;
 return writes==fail_write?-EIO:0;}
#define mt6797_md1_firmware_load(...) write_step(1)
#define mt6797_md1_cold_apply_initial_mpu(...) write_step(2)
#define mt6797_md1_cold_owner_run(...) write_step(3)
#define mt6797_md1_cold_wdt_run(...) write_step(4)
#define mt6797_md1_cold_apply_stage1_mpu(...) write_step(5)
#define mt6797_md1_cold_boot_vector_release(...) write_step(6)
static int run(bool mainline) {
 struct platform_device p={0},*pdev=&p;
 bool fs_claimed=false,mpu_started=false;int ret;
'''
ending=r'''
 (void)mpu_started;return 0;
out_fatal:
 (void)mpu_started;return ret;
}
static void reset(unsigned stage){state=(struct mt6797_fs_state){.stage=stage};
 writes=claims=watchdogs=fail_write=cancel_on_watchdog=0;}
int main(void){
 struct kstatfs fs={4096,10,5,4096};struct mt6797_md1_fs_stats stats;
 assert(!mt6797_fs_stats_convert(&fs,&stats));
 reset(MT6797_FS_EMPTY);assert(!mt6797_fs_begin(&state)&&!mt6797_fs_publish(&state));
 assert(!mt6797_fs_reprobe(&state));
 for(unsigned mainline=0;mainline<2;mainline++){
  reset(MT6797_FS_EMPTY);assert(run(mainline)==-EPROBE_DEFER&&!writes&&!claims&&!watchdogs);
  reset(MT6797_FS_INSTALLING);assert(run(mainline)==-EPROBE_DEFER&&!writes&&!claims);
  reset(MT6797_FS_READY);cancel_on_watchdog=1;assert(run(mainline)==-EALREADY&&!writes&&claims==1);
  reset(MT6797_FS_READY);assert(!run(mainline));assert(claims==1&&writes==(mainline?6:5));
  for(int i=0;i<writes;i++)assert(events[i]==i+(mainline?1:2));
  assert(mt6797_fs_cancel(&state)==-EBUSY&&run(mainline)==-EALREADY);
  for(int failure=1;failure<=(mainline?6:5);failure++){
   reset(MT6797_FS_READY);fail_write=failure;assert(run(mainline)==-EIO&&writes==failure&&claims==1);
   assert(state.stage==MT6797_FS_CONSUMED&&run(mainline)==-EALREADY&&writes==failure);
  }
 }
 puts("PASS actual cold first-write gates: deferred no-write, cancel-before-claim, single claim firmware/LK paths, permanent failure at every stage, MPU/power/WDT/GO order; existing 30s queue-start timer byte-identical");
}
'''
code=shim+(root/'drivers/soc/mediatek/mt6797-md1-fs-state.h').read_text()+adapter+early
code+='\tif (mainline) {\n'+firmware+'\t}\n'+initial+ending
with tempfile.TemporaryDirectory(prefix='gemini-fs-start-gate-') as d:
    p=Path(d);(p/'test.c').write_text(code)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
