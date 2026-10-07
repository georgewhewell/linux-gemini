#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute all three calibrated-DVFS rail admission paths in microvolts.

EEM voltage conversion and unrelated provenance providers are boundary
substitutes; the EEM table builder, handoff checker, snapshot table matcher,
shared rail check, constants and data structures are production code.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
headers=root/'include/linux/soc/mediatek'
handoff=(headers/'mt6797-dvfsp-handoff.h').read_text()
eem=(headers/'mt6797-eem-calibration.h').read_text()
readback=(headers/'mt6797-eem-readback.h').read_text()
rail=(headers/'mt6797-cpu-rails.h').read_text()
rail=re.sub(r'^#include .*\n','',rail,flags=re.M)
def extract(text,signature,end='\n}\n'):
    start=text.index(signature)
    return text[start:text.index(end,start)+len(end)]
def definitions(text):
    result = []
    lines = iter(text.splitlines())
    for line in lines:
        if not line.startswith('#define MT6797_'):
            continue
        result.append(line)
        while line.rstrip().endswith('\\'):
            line = next(lines)
            result.append(line)
    return '\n'.join(result)
structs='\n'.join(extract(handoff,'struct '+name+' {','\n};') for name in (
    'mt6797_dvfsp_state_provenance','mt6797_dvfsp_calibration_table_entry',
    'mt6797_dvfsp_calibration_cluster','mt6797_dvfsp_calibration_state'))
structs+='\n'+extract(handoff,'enum mt6797_dvfsp_state_cluster_id {','\n};')
structs+='\n'+extract(readback,'struct mt6797_eem_readback_bank {','\n};')
structs+='\n'+extract(readback,'struct mt6797_eem_readback {','\n};')
structs+='\n'+extract(eem,'struct mt6797_eem_calibration_input {','\n};')
thermal=(root/'drivers/thermal/mediatek/auxadc_thermal.c').read_text()
owner=(root/'drivers/soc/mediatek/mt6797-dvfsp-handoff.c').read_text()
snapshot=(root/'drivers/soc/mediatek/mt6797-dvfsp-state-snapshot.c').read_text()
functions='\n'.join((
    extract(thermal,'int mt6797_eem_calibration_build('),
    extract(owner,'static int\nmt6797_dvfsp_calibration_state_check('),
    extract(snapshot,'static int mt6797_dvfsp_snapshot_table_match('),
))
prelude=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) ((UINT32_MAX<<(l)) & (UINT32_MAX>>(31-(h))))
struct mt6797_dvfsp_ptp_state { int unused; };
struct mt6797_dvfsp_ppm_snapshot { int unused; };
'''
boundary=r'''
static int mt6797_eem_calibration_ptp_state_check(const struct mt6797_dvfsp_ptp_state *p)
{ return p?0:-EPROTO; }
static int mt6797_eem_calibration_ppm_check(const struct mt6797_eem_calibration_input *p)
{ return p->ppm_snapshot?0:-EPROTO; }
static int mt6797_dvfsp_state_provenance_check(const struct mt6797_dvfsp_state_provenance *p)
{ return p && p->abi==MT6797_DVFSP_STATE_PROVENANCE_ABI?0:-EPROTO; }
static int mt6797_eem_calibration_provenance_check(const struct mt6797_dvfsp_state_provenance *p)
{ return mt6797_dvfsp_state_provenance_check(p); }
static int mt6797_eem_calibration_row_voltage(const struct mt6797_eem_calibration_input *input,
 const struct mt6797_eem_readback_bank *bank,unsigned int bank_index,unsigned int row,u32 *uv)
{ (void)bank;*uv=input->record_vproc_uv[bank_index][row];return 0; }
'''
main=r'''
static struct mt6797_eem_readback readback={
 .abi=MT6797_EEM_READBACK_ABI,
 .bank_mask=MT6797_DVFSP_CALIBRATION_STATE_BANK_ALL,
 .temperature_millic=40000,
};
static struct mt6797_dvfsp_ptp_state ptp;
static struct mt6797_dvfsp_ppm_snapshot ppm;
static struct mt6797_eem_calibration_input input={
 .readback=&readback,.ptp_state=&ptp,.ppm_snapshot=&ppm,
 .temperature_millic=40000,.thermal_generation=1,.clock_owner_generation=1,
 .rail_owner_generation=1,.source_generation=1,
 .provenance={.abi=MT6797_DVFSP_STATE_PROVENANCE_ABI},
};
static unsigned int cases;
static void check(u32 vproc,u32 vsram,bool valid)
{
 struct mt6797_dvfsp_calibration_state state={
  .abi=MT6797_DVFSP_CALIBRATION_STATE_ABI,
  .phase=MT6797_DVFSP_CALIBRATION_STATE_PHASE_MON,
  .bank_mask=MT6797_DVFSP_CALIBRATION_STATE_BANK_ALL,
  .thermal_generation=1,.clock_owner_generation=1,.rail_owner_generation=1,.source_generation=1,
  .provenance={.abi=MT6797_DVFSP_STATE_PROVENANCE_ABI},
 };
 const u32 bank_ids[]={0,3,4,5};
 assert(mt6797_cpu_rails_valid(vproc,vsram)==valid);
 for(unsigned int bank=0;bank<MT6797_EEM_CALIBRATION_BANKS;bank++) {
  input.vmin_uv[bank]=1;input.vmax_uv[bank]=UINT32_MAX;
  readback.bank[bank].bank=bank_ids[bank];
  readback.bank[bank].eem_enable=MT6797_EEM_CALIBRATION_MON_ENABLE;
  for(unsigned int row=0;row<MT6797_EEM_CALIBRATION_ROWS;row++) {
   input.frequency_pct[bank][row]=100-row*4;
   input.frequency_khz[bank][row]=1600000-row*10000;
   input.ppm_limit_khz[bank][row]=2000000;
   input.record_vproc_uv[bank][row]=vproc;
   input.vsram_uv[bank][row]=vsram;
   if(!(row%2))readback.bank[bank].frequency_pct[row/2]=input.frequency_pct[bank][row];
  }
  state.cluster[bank].table_count=1;
  state.cluster[bank].table[0]=(struct mt6797_dvfsp_calibration_table_entry){100000,vproc,vsram,2000000};
  const struct mt6797_dvfsp_calibration_table_entry *matched=(void *)1;
  assert(mt6797_dvfsp_snapshot_table_match(&state.cluster[bank],100000,&matched)==(valid?0:-EPROTO));
  assert(valid ? matched==&state.cluster[bank].table[0] : matched==NULL);
 }
 assert(mt6797_dvfsp_calibration_state_check(&state)==(valid?0:-EPROTO));
 assert(mt6797_eem_calibration_build(&input,&state)==(valid?0:-EPROTO));
 if(valid) {
  assert(!mt6797_dvfsp_calibration_state_check(&state));
  for(unsigned int bank=0;bank<4;bank++)
   for(unsigned int row=0;row<16;row++) {
    assert(state.cluster[bank].table[row].vproc_uv==vproc);
    assert(state.cluster[bank].table[row].vsram_uv==vsram);
   }
 }
 cases++;
}
int main(void)
{
 static const struct {u32 vproc,vsram;bool valid;} vectors[]={
  {770000,1000000,true}, /* SRAM floor: 230 mV, not 23 mV */
  {1000000,1100000,true}, /* nominal 100 mV offset */
  {1200000,1200000,true}, /* SRAM ceiling: equality is allowed */
  {1000000,1000000,true},
  {700000,1000000,true}, /* exact 300 mV boundary */
  {699999,1000000,false},
  {900000,1200000,true},
  {899999,1200000,false},
  {1000001,1000000,false},
  {1200001,1200000,false},
  {900000,999999,false},
  {1000000,1200001,false},
  {800000,820000,false}, /* formerly accepted by snapshot only */
  {0,1000000,false},{1000000,0,false},
  {UINT32_MAX,1000000,false},{UINT32_MAX,UINT32_MAX,false},
  {1000000,UINT32_MAX,false},
 };
 for(unsigned int i=0;i<sizeof(vectors)/sizeof(vectors[0]);i++)
  check(vectors[i].vproc,vectors[i].vsram,vectors[i].valid);
 /* Every DA9214 selector in the vendor 0.77-1.2 V CPU range, with
  * nominal SRAM offset, floor/ceiling and 25 mV CPULDO round-up. */
 for(u32 vp=770000;vp<=1200000;vp+=10000) {
  u32 vs=((vp+100000+24999)/25000)*25000;
  if(vs<1000000)vs=1000000;
  if(vs>1200000)vs=1200000;
  check(vp,vs,true);
 }
 /* Exhaust a surrounding grid against an independent signed difference. */
 for(u32 vp=650000;vp<=1250000;vp+=10000)
 for(u32 vs=975000;vs<=1225000;vs+=25000) {
  int64_t delta=(int64_t)vs-vp;
  check(vp,vs,vs>=1000000 && vs<=1200000 && delta>=0 && delta<=300000);
 }
 printf("PASS %u rail pairs through EEM build, handoff admission and snapshot matching\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-rail-admission-') as tmp:
    src,exe=Path(tmp,'test.c'),Path(tmp,'test')
    src.write_text(prelude+definitions(handoff)+'\n'+definitions(eem)+'\n'+
                   definitions(readback)+'\n'+rail+structs+boundary+functions+main)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=undefined','-fno-sanitize-recover=all',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
