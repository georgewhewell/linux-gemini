#!/usr/bin/env python3
"""Execute actual A72 VPROC apply/rollback branches with regulator vote doubles."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'drivers/soc/mediatek/mt6797-a72-power.c').read_text()
def branch(function):
    at=s.index(function+'('); at=s.index('case MT6797_A72_VPROC_ENABLE:',at)
    end=s.index('\n\tcase ',at+1)
    return s[at+len('case MT6797_A72_VPROC_ENABLE:'):end]
c=r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
struct mt6797_a72_preflight {bool vproc_vote_owned,vproc_vote_added;int *vproc_big;};
static int votes,enables,disables,enable_error,disable_error;
static int regulator_enable(int *r){assert(r==&votes);enables++;if(enable_error)return enable_error;votes++;return 0;}
static int regulator_disable(int *r){assert(r==&votes && votes>0);disables++;if(disable_error)return disable_error;votes--;return 0;}
'''
c+='static int apply(struct mt6797_a72_preflight *preflight){int ret;'+branch('mt6797_a72_apply')+'}\n'
c+='static int rollback(struct mt6797_a72_preflight *preflight){int ret;'+branch('mt6797_a72_rollback')+'}\n'
c+=r'''
int main(void){
 struct mt6797_a72_preflight p={.vproc_big=&votes};
 assert(apply(&p)==0 && votes==1 && p.vproc_vote_added);assert(rollback(&p)==0 && votes==0 && !p.vproc_vote_owned);
 enable_error=-EUCLEAN;assert(apply(&p)==-EUCLEAN);assert(rollback(&p)==0 && disables==1 && votes==0);
 enable_error=0;p.vproc_vote_owned=true;votes=1;unsigned calls=enables;
 assert(apply(&p)==0 && enables==calls);assert(rollback(&p)==0 && votes==1 && disables==1);
 p.vproc_vote_added=true;disable_error=-EIO;assert(rollback(&p)==-EIO && p.vproc_vote_owned && p.vproc_vote_added);
 puts("PASS actual A72 inherited/added/failed enable vote handling");
}
'''
with tempfile.TemporaryDirectory() as d:
 src,exe=Path(d,'t.c'),Path(d,'t');src.write_text(c)
 subprocess.run([os.environ.get('HOSTCC','cc'),'-Wall','-Wextra','-Werror','-Wno-sign-compare',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
