#!/usr/bin/env python3
"""Compile the actual pure kernel header; no device or kernel activation."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/thermal/mediatek/mt6351_calibration.h"

static struct mt6351_calibration decode(unsigned vts, unsigned temp,
                                      unsigned slope, unsigned neg, unsigned id)
{
    u16 words[2] = { (vts & 511) << 7 | temp << 1 | 1,
                    vts >> 9 | neg << 4 | slope << 5 | id << 11 };
    struct mt6351_calibration c;
    assert(!mt6351_calibration_decode(words, &c));
    assert(c.vts == vts && c.degc == temp && c.slope_negative == neg);
    assert(c.slope == (id ? slope : 0));
    return c;
}

int main(void)
{
    /* Independently specified integer-division golden values:
     * VTS1600 -> 703mV; raw1500 -> 659mV.
     * sign+: 703000000/1718 + 25000 - 659000000/1610 = 24880
     * sign-: 703000000/1478 + 25000 - 659000000/1586 = 85132
     * genuine zero: VTS155 ->68mV, raw224 ->98mV, temp38, slope1:
     * 68000000/1608 + 19000 - 98000000/1599 = 0.
     */
    const struct { unsigned v,t,s,n,id,r; int err,mc; } vectors[] = {
        {1600,50,0,0,1,1600,0,25000},
        {1600,50,12,0,1,1500,0,24880},
        {1600,50,12,1,1,1500,0,85132},
        {1600,50,63,1,0,1600,0,25000},
        {155,38,1,0,1,224,0,0},
        {0,38,0,0,1,0,0,19000},
        {8191,60,63,1,1,4095,-ERANGE,0},
        {1600,50,0,0,1,4095,-ERANGE,0},
        {1600,50,0,0,1,4096,-ERANGE,0},
        {1600,50,0,0,1,~0U,-ERANGE,0},
    };
    for (unsigned i=0; i<sizeof(vectors)/sizeof(*vectors); ++i) {
        struct mt6351_calibration c = decode(vectors[i].v, vectors[i].t,
            vectors[i].s, vectors[i].n, vectors[i].id);
        s32 out = 1234567;
        assert(mt6351_calibration_to_mc(&c, vectors[i].r, &out) == vectors[i].err);
        assert(out == (vectors[i].err ? 1234567 : vectors[i].mc));
    }
    for (unsigned temp=0; temp<64; ++temp) {
        u16 w[2] = {temp << 1 | 1, 0};
        struct mt6351_calibration c, before;
        memset(&c, 0xa5, sizeof(c)); before=c;
        int err=mt6351_calibration_decode(w, &c);
        assert(err == ((temp<38 || temp>60) ? -ERANGE : 0));
        if (err) assert(!memcmp(&c, &before, sizeof(c)));
        w[0] &= ~1;
        c=before;
        assert(mt6351_calibration_decode(w, &c) == -ENODATA);
        assert(!memcmp(&c, &before, sizeof(c)));
    }
    for (unsigned v=0; v<8192; ++v)
        for (unsigned sign=0; sign<2; ++sign)
            for (unsigned id=0; id<2; ++id)
                (void)decode(v, 50, v%64, sign, id);
    struct mt6351_calibration valid=decode(1600,50,0,0,1);
    for (unsigned field=0; field<5; ++field) {
        struct mt6351_calibration c=valid;
        s32 out=1234567;
        if (field==0) c.vts=8192;
        if (field==1) c.degc=0;
        if (field==2) c.degc=255;
        if (field==3) c.slope=255;
        if (field==4) c.slope_negative=255;
        assert(mt6351_calibration_to_mc(&c,1600,&out)==-ERANGE);
        assert(out==1234567);
    }
    s64 out=123;
    assert(mt6351_calibration_muldiv(S64_MAX,2,1,&out)==-EOVERFLOW && out==123);
    assert(mt6351_calibration_muldiv(S64_MIN,1,-1,&out)==-EOVERFLOW && out==123);
    assert(mt6351_calibration_muldiv(1,1,0,&out)==-EINVAL && out==123);
    assert(!mt6351_calibration_muldiv(-10,1,3,&out) && out==-3);
    assert(!mt6351_calibration_muldiv(S64_MAX,1,1,&out) && out==S64_MAX);
    puts("PASS: actual MT6351 C decode/conversion, goldens, zero, errors and overflow");
}
'''

with tempfile.TemporaryDirectory(prefix="mt6351-calibration-") as temp:
    work = Path(temp)
    (work / "linux").mkdir()
    headers = {
        "types.h": "#include <stdint.h>\ntypedef uint16_t u16; typedef uint8_t u8; typedef int64_t s64; typedef int32_t s32;\n#define S64_MIN INT64_MIN\n#define S64_MAX INT64_MAX\n",
        "errno.h": "#define ENODATA 61\n#define ERANGE 34\n#define EINVAL 22\n#define EOVERFLOW 75\n",
        "math64.h": "#define div64_s64(a,b) ((a)/(b))\n",
        "overflow.h": "#define check_mul_overflow __builtin_mul_overflow\n#define check_add_overflow __builtin_add_overflow\n",
    }
    for name, content in headers.items():
        (work / "linux" / name).write_text(content)
    (work / "test.c").write_text(TEST)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-O2", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "-I", str(work), "-I", str(ROOT), str(work / "test.c"),
                    "-o", str(work / "test")], check=True)
    subprocess.run([str(work / "test")], check=True)
