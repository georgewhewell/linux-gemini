#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute DA921x voltage transactions and DA9214 settling, including rollback."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/regulator/da9213-legacy-regulator.c').read_text()
header = (root / 'drivers/regulator/da9213-legacy-transaction.h').read_text()
header = re.sub(r'^#include .*\n', '', header, flags=re.M)

def extract(signature, end='\n}\n'):
    start = source.index(signature)
    return source[start:source.index(end, start) + len(end)]

types = '\n'.join(extract('struct ' + name + ' {', '\n};') for name in
                  ('da9213_legacy_variant', 'da9213_legacy'))
types += '\n' + '\n'.join(extract('static const struct da9213_legacy_variant ' + name,
                                  '\n};') for name in
                           ('da9213_legacy_da9213', 'da9213_legacy_da9214',
                            'da9213_legacy_da9215'))
types += '\n' + extract('static const u8 da9213_legacy_vsel_reg', '\n};')
functions = '\n'.join(extract(signature) for signature in (
    'static int da9213_legacy_io_read(', 'static int da9213_legacy_io_write(',
    'static void da9213_legacy_io_settle('))
functions += '\n' + extract('static const struct da9213_legacy_transaction_io da9213_legacy_io', '\n};')
functions += '\n' + '\n'.join(extract(signature) for signature in (
    'int da9213_legacy_update_bits_transaction(',
    'static int da9213_legacy_update_primary(',
    'static int da9213_legacy_set_voltage_sel('))
prelude = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
#define max(a,b) ((a)>(b)?(a):(b))
#define DA9213_LEGACY_BUCKA 0
#define DA9213_LEGACY_BUCKB 1
struct mutex { int held; };
struct i2c_client { int unused; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=1; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=0; }
'''
model = r'''
static struct i2c_client client;
static struct da9213_legacy chip={.client=&client};
struct regulator_dev { unsigned int id; };
static void *rdev_get_drvdata(struct regulator_dev *r) { (void)r; return &chip; }
static unsigned int rdev_get_id(struct regulator_dev *r) { return r->id; }
static u8 value, target_reg;
static unsigned int reads,writes,delays,expected_delay,settles;
static unsigned int fail_read,fail_write,ignore_write,partial_write;
static char trace[32];
static void event(char c)
{ unsigned int n=strlen(trace); assert(n+1<sizeof(trace)); trace[n]=c;trace[n+1]=0; }
static int da9213_legacy_raw_read(struct i2c_client *c,u8 reg,u8 *out)
{
 assert(c==&client && reg==target_reg && chip.lock.held);
 event('R'); reads++;
 if(reads==fail_read) return -EIO;
 *out=value; return 0;
}
static int da9213_legacy_raw_write(struct i2c_client *c,u8 reg,u8 v)
{
 assert(c==&client && reg==target_reg && chip.lock.held);
 event('W'); writes++;
 if(writes!=ignore_write && (writes!=fail_write || partial_write)) value=v;
 return writes==fail_write ? -EREMOTEIO : 0;
}
static void udelay(unsigned int us)
{
 assert(chip.lock.held && settles && us==expected_delay);
 event('D'); delays++;
}
'''
main = r'''
static void reset(unsigned int old,unsigned int next,unsigned int buck,
                  const struct da9213_legacy_variant *variant)
{
 assert(!chip.lock.held); chip.variant=variant;
 target_reg=buck ? 0xd9 : 0xd7; value=old|0x80;
 reads=writes=delays=fail_read=fail_write=ignore_write=partial_write=0;
 trace[0]=0;
 unsigned int steps=abs((int)next-(int)old);
 expected_delay=steps>25 ? steps : 25;
 settles=steps && variant==&da9213_legacy_da9214;
}
int main(void)
{
 const struct da9213_legacy_variant *variants[]={&da9213_legacy_da9213,
  &da9213_legacy_da9214,&da9213_legacy_da9215};
 unsigned int pairs=0;
 for(unsigned int v=0;v<3;v++) for(unsigned int buck=0;buck<2;buck++)
 for(unsigned int a=0;a<128;a++) for(unsigned int b=0;b<128;b++) {
  struct regulator_dev r={.id=buck};
  reset(a,b,buck,variants[v]);
  assert(!da9213_legacy_set_voltage_sel(&r,b));
  assert(value==(b|0x80) && !chip.lock.held);
  assert(writes==(a!=b) && delays==settles);
  assert(!strcmp(trace,a==b ? "R" : settles ? "RWDR" : "RWR"));
  pairs++;
 }
 struct regulator_dev r={.id=0};
 /* Golden full-span delay: 127 * 10 mV at 10 mV/us = 127 us. */
 reset(0,127,0,&da9213_legacy_da9214); assert(expected_delay==127);
 assert(!da9213_legacy_set_voltage_sel(&r,127) && delays==1);
 for(unsigned int partial=0;partial<2;partial++) {
  reset(70,90,0,&da9213_legacy_da9214);fail_write=1;partial_write=partial;
  assert(da9213_legacy_set_voltage_sel(&r,90)==-EREMOTEIO);
  assert(value==(70|0x80) && delays==2 && !strcmp(trace,"RWDWDR"));
 }
 reset(70,90,0,&da9213_legacy_da9214);fail_read=1;
 assert(da9213_legacy_set_voltage_sel(&r,90)==-EIO && !writes && !delays);
 reset(70,90,0,&da9213_legacy_da9214);fail_read=2;
 assert(da9213_legacy_set_voltage_sel(&r,90)==-EIO);
 assert(value==(70|0x80) && delays==2 && !strcmp(trace,"RWDRWDR"));
 reset(70,90,0,&da9213_legacy_da9214);ignore_write=1;
 assert(da9213_legacy_set_voltage_sel(&r,90)==-EIO && delays==2);
 reset(70,90,0,&da9213_legacy_da9214);ignore_write=1;fail_write=2;
 assert(da9213_legacy_set_voltage_sel(&r,90)==-EUCLEAN && delays==2);
 reset(70,90,0,&da9213_legacy_da9214);ignore_write=1;fail_read=3;
 assert(da9213_legacy_set_voltage_sel(&r,90)==-EUCLEAN && delays==2);
 reset(70,90,0,&da9213_legacy_da9214);
 assert(da9213_legacy_set_voltage_sel(&r,128)==-EINVAL && !reads && !writes);
 /* Enable-register writes are not voltage transitions. */
 reset(0,1,0,&da9213_legacy_da9214);target_reg=0x5d;
 assert(!da9213_legacy_update_primary(&chip,0x5d,1,1) && !delays);
 assert(!strcmp(trace,"RWR"));
 printf("PASS %u DA921x selector pairs, DA9214 settling and verified/uncertain rollback\n",pairs);
}
'''
with tempfile.TemporaryDirectory(prefix='da9214-settling-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(prelude + header + types + model + functions + main)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
