#!/usr/bin/env python3
"""Exercise actual clock callbacks with injected sample/decode/parent failures.

Checks observation does not retry hardware reads, leak an invalid sample, or
change clock framework return semantics. Hardware and decoder are test doubles;
this does not validate the register decoder or the device's clock rate.
"""
import os
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/clk/mediatek/clk-mt6797-cpu.c').read_text()
header = (root / 'drivers/clk/mediatek/clk-mt6797-cpu.h').read_text()

def extract(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

callbacks = '\n'.join(extract('static ' + signature) for signature in (
    'int mt6797_cpuclk_sample_decode(',
    'unsigned long mt6797_cpuclk_pll_recalc_rate(',
    'u8 mt6797_cpuclk_path_get_parent(',
    'unsigned long mt6797_cpuclk_path_recalc_rate(',
))
types = header[header.index('enum mt6797_cpuclk_domain'):header.index('struct mt6797_cpuclk_rate_plan')]
c = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
''' + types + r'''
struct mt6797_cpuclk { void *dev; };
struct clk_hw { unsigned long rate; };
struct mt6797_cpuclk_pll_hw {
 struct clk_hw hw; struct mt6797_cpuclk *provider;
 enum mt6797_cpuclk_domain domain;
};
struct mt6797_cpuclk_path_hw {
 struct clk_hw hw; struct mt6797_cpuclk *provider;
 enum mt6797_cpuclk_domain domain;
};
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
static int samples, decodes, sample_error, decode_error, missing, logs;
static unsigned long seen_ref;
static struct clk_hw parents[4];
static struct mt6797_cpuclk_state decoded;
static char message[1024];
static void dev_info_ratelimited(void *dev, const char *format, ...)
{
 va_list ap; (void)dev; logs++;
 va_start(ap,format); vsnprintf(message,sizeof(message),format,ap); va_end(ap);
}
static int mt6797_cpuclk_snapshot(struct mt6797_cpuclk *p,
 struct mt6797_cpuclk_snapshot *s)
{
 (void)p; samples++;
 /* Poison even a failed partial read: diagnostics must not print it. */
 memset(s,0xa5,sizeof(*s)); return sample_error;
}
static int mt6797_cpuclk_decode_state(enum mt6797_cpuclk_domain d,
 const struct mt6797_cpuclk_snapshot *s, unsigned long ref,
 struct mt6797_cpuclk_state *out)
{
 (void)d; assert(s->muxsel == 0xa5a5a5a5); decodes++; seen_ref=ref;
 if (decode_error) return decode_error;
 *out=decoded; return 0;
}
static struct clk_hw *clk_hw_get_parent_by_index(struct clk_hw *hw, u8 i)
{ (void)hw; assert(i<4); return i==missing ? NULL : &parents[i]; }
static unsigned long clk_hw_get_rate(struct clk_hw *hw) { return hw->rate; }
static unsigned long mt6797_cpuclk_apply_divider(unsigned long rate, u8 div)
{ assert(div==2); return rate/2; }
''' + callbacks + r'''
static void reset(void)
{
 samples=decodes=logs=sample_error=decode_error=0; missing=-1;
 message[0]=0; seen_ref=0;
 parents[0].rate=26000000; parents[2].rate=1000000000;
 decoded=(struct mt6797_cpuclk_state){.armpll_rate=2000000000,
  .source=MT6797_CPUCLK_ARMPLL,.divider=2};
}
int main(void)
{
 struct mt6797_cpuclk provider={0};
 struct mt6797_cpuclk_pll_hw pll={.provider=&provider,.domain=MT6797_CPUCLK_MP2};
 struct mt6797_cpuclk_path_hw path={.provider=&provider,.domain=MT6797_CPUCLK_MP2};
 for (int callback=0; callback<3; callback++) {
  for (int failure=0; failure<3; failure++) {
   reset();
   if (failure==1) sample_error=-EIO;
   if (failure==2) decode_error=-ERANGE;
   unsigned long result;
   if (callback==0) result=mt6797_cpuclk_pll_recalc_rate(&pll.hw,26000000);
   else if (callback==1) result=mt6797_cpuclk_path_get_parent(&path.hw);
   else result=mt6797_cpuclk_path_recalc_rate(&path.hw,123);
   assert(samples==1 && decodes==(failure!=1));
   if (failure) {
    assert(result==(callback==1 ? (u8)-EINVAL : 0)); assert(logs==1);
    assert(strstr(message,"domain=2"));
    assert(strstr(message,"ref_hz=26000000"));
    if (failure==1) {
     assert(strstr(message,"stage=snapshot status=-5"));
     assert(!strstr(message,"a5a5") && !strstr(message,"mux="));
    } else {
     assert(strstr(message,"stage=decode status=-34"));
     assert(strstr(message,"mux=a5a5a5a5"));
     assert(strstr(message,"mp2con1=a5a5a5a5"));
    }
   } else {
    assert(result==(callback==0 ? 2000000000UL : callback==1 ? 1 : 1000000000UL));
    assert(logs==0 && seen_ref==26000000);
   }
  }
 }
 reset(); missing=0;
 assert(mt6797_cpuclk_path_get_parent(&path.hw)==(u8)-EINVAL);
 assert(samples==0 && decodes==0 && strstr(message,"stage=reference-parent"));
 reset(); missing=0;
 assert(mt6797_cpuclk_path_recalc_rate(&path.hw,123)==0);
 assert(samples==0 && decodes==0 && strstr(message,"stage=reference-parent"));
 reset(); decoded.source=MT6797_CPUCLK_MAINPLL;
 assert(mt6797_cpuclk_path_recalc_rate(&path.hw,123)==500000000);
 assert(samples==1 && decodes==1 && logs==0);
 reset(); decoded.source=MT6797_CPUCLK_MAINPLL; missing=2;
 assert(mt6797_cpuclk_path_recalc_rate(&path.hw,123)==0);
 assert(samples==1 && decodes==1 && strstr(message,"stage=selected-parent source=2"));
 reset(); decode_error=-EINVAL;
 assert(mt6797_cpuclk_pll_recalc_rate(&pll.hw,0)==0);
 assert(samples==1 && decodes==1 && seen_ref==0);
 assert(strstr(message,"stage=decode status=-22 ref_hz=0"));
 puts("mt6797-cpuclk-observation: pass");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(c)
    subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror',
                    '-o', str(exe), str(src)], check=True)
    subprocess.run([str(exe)], check=True)
