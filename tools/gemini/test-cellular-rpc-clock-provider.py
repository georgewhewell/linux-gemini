#!/usr/bin/env python3
"""Run the actual cold-owner AFC provider with register and lease adapters."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'drivers/soc/mediatek/mt6797-md1-cold-owner.c').read_text()
a = source.index('int mt6797_md1_cold_owner_rf_clock(')
b = source.index('EXPORT_SYMBOL_GPL(mt6797_md1_cold_owner_rf_clock)', a)
shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint32_t u32;
struct device {int id;};
struct regmap {u32 command,afc;};
struct mt6797_md1_cold_owner_backend {
 struct regmap *spm; bool cldma_leased,rf_afc_applied;u32 rf_afc_first,rf_afc_latest;
};
static struct regmap map;
static struct device consumer,other;
static struct mt6797_md1_cold_owner_backend owner;
static struct mt6797_md1_cold_owner_backend *mt6797_md1_handoff_backend;
static struct device *mt6797_md1_cldma_consumer;
static int mt6797_md1_handoff_status,mt6797_md1_cold_owner_fatal_latch;
static int mt6797_md1_post_vector_evidence_lock,locked,operations,writes,fail_at,fatal_at;
static bool mismatch;
#define atomic_read(p) (*(p))
#define atomic_set(p,v) (*(p)=(v))
static void mutex_lock(int *p){(void)p;assert(!locked);locked=1;}
static void mutex_unlock(int *p){(void)p;assert(locked);locked=0;}
static int access_reg(void){assert(locked);operations++;
 if(operations==fatal_at)mt6797_md1_cold_owner_fatal_latch=1;
 return operations==fail_at?-EIO:0;}
static int regmap_read(struct regmap *p,u32 reg,unsigned *v){assert(p==&map);int r=access_reg();if(r)return r;
 assert(reg==0x404||reg==0x414);*v=reg==0x404?p->command:p->afc;return 0;}
static int regmap_write(struct regmap *p,u32 reg,u32 value){assert(p==&map&&reg==0x414);writes++;
 int r=access_reg();if(r)return r;if(!mismatch)p->afc=value;return 0;}
static void reset(void){map=(struct regmap){6,0x02a80008};owner=(struct mt6797_md1_cold_owner_backend){.spm=&map,.cldma_leased=true};
 mt6797_md1_handoff_backend=&owner;mt6797_md1_cldma_consumer=&consumer;
 mt6797_md1_handoff_status=mt6797_md1_cold_owner_fatal_latch=locked=operations=writes=fail_at=fatal_at=0;mismatch=false;}
'''
tests = r'''
int main(void){u8 sw[4];
 reset();assert(!mt6797_md1_cold_owner_rf_clock(&consumer,0x12345678,sw));
 assert(writes==1&&operations==4&&owner.rf_afc_applied&&owner.rf_afc_first==0x12345678&&owner.rf_afc_latest==0x12345678);
 assert(!memcmp(sw,(u8[]){1,0,1,1},4));
 assert(!mt6797_md1_cold_owner_rf_clock(&consumer,0xabcdef01,sw));
 assert(writes==1&&operations==6&&owner.rf_afc_first==0x12345678&&owner.rf_afc_latest==0xabcdef01&&map.afc==0x12345678);
 /* Every register failure latches terminal state, no second write attempt. */
 for(int n=1;n<=4;n++){reset();memset(sw,0x55,4);fail_at=n;
  assert(mt6797_md1_cold_owner_rf_clock(&consumer,0x12345678,sw)==-EIO);
  assert(mt6797_md1_cold_owner_fatal_latch&&!owner.rf_afc_applied&&!owner.rf_afc_latest&&!locked);
  assert(!memcmp(sw,(u8[]){0x55,0x55,0x55,0x55},4));
  int old=operations;assert(mt6797_md1_cold_owner_rf_clock(&consumer,1,sw)==-ESHUTDOWN&&operations==old);
 }
 for(int n=0;n<6;n++){reset();if(n==0)mt6797_md1_handoff_status=-ESHUTDOWN;
  if(n==1)mt6797_md1_handoff_backend=NULL;if(n==2)mt6797_md1_cldma_consumer=&other;
  if(n==3)owner.cldma_leased=false;if(n==4)mt6797_md1_cold_owner_fatal_latch=1;
  assert(mt6797_md1_cold_owner_rf_clock(n==5?&other:&consumer,1,sw)==-ESHUTDOWN);
  assert(!operations&&!writes&&!locked);
 }
 reset();assert(mt6797_md1_cold_owner_rf_clock(NULL,1,sw)==-EINVAL);
 assert(mt6797_md1_cold_owner_rf_clock(&consumer,1,NULL)==-EINVAL&&!operations);
 for(unsigned cmd=0;cmd<8;cmd++)if(cmd!=6){reset();map.command=cmd;
  assert(mt6797_md1_cold_owner_rf_clock(&consumer,1,sw)==-EUCLEAN&&!writes&&mt6797_md1_cold_owner_fatal_latch);}
 reset();map.command=0xfffffff6;assert(!mt6797_md1_cold_owner_rf_clock(&consumer,1,sw));assert(map.command==0xfffffff6);
 reset();mismatch=true;assert(mt6797_md1_cold_owner_rf_clock(&consumer,1,sw)==-EIO&&writes==1&&!owner.rf_afc_applied);
 reset();assert(!mt6797_md1_cold_owner_rf_clock(&consumer,1,sw));map.afc=2;
 assert(mt6797_md1_cold_owner_rf_clock(&consumer,3,sw)==-EUCLEAN&&writes==1&&owner.rf_afc_latest==1);
 /* IRQ fatal before write and during write/readback prevents success. */
 for(int n=1;n<=4;n++){reset();fatal_at=n;assert(mt6797_md1_cold_owner_rf_clock(&consumer,1,sw)==-ESHUTDOWN);
  assert(!owner.rf_afc_latest&&!locked);if(n<=2)assert(!writes);else assert(writes==1);}
 puts("PASS actual AFC provider: lease guards, modem command ownership, first-only write, latest retention, every I/O failure, drift/readback/fatal, no retries or output on failure");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-rf-provider-') as directory:
    p=Path(directory);(p/'test.c').write_text(shim+source[a:b]+tests)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
