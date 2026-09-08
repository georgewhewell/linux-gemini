#!/usr/bin/env python3
"""Execute numeric rejection capture/snapshot and actual sysfs formatter."""
import os
from pathlib import Path
import subprocess
import tempfile
r=Path(__file__).resolve().parents[2]
source=(r/'drivers/net/wwan/mediatek/mtk_mt6797_ccci.c').read_text()
start=source.index('static ssize_t rx_rejected_show(');end=source.index('\nstatic DEVICE_ATTR(rx_rejected',start)
show=source[start:end]
header=(r/'drivers/net/wwan/mediatek/mtk_mt6797_ccci_rx_rejected.h').read_text()
shim=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
typedef pthread_mutex_t spinlock_t;
#define spin_lock_irqsave(p,f) do{(f)=0;pthread_mutex_lock(p);}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);pthread_mutex_unlock(p);}while(0)
#define GFP_KERNEL 0
#define PAGE_SIZE 4096
'''
adapters=r'''
struct mt6797_ccci {struct mt6797_rx_rejected rx_rejected;};
static struct mt6797_ccci c={.rx_rejected.lock=PTHREAD_MUTEX_INITIALIZER};
struct device{int unused;};struct device_attribute{int unused;};
static void *dev_get_drvdata(struct device *d){(void)d;return &c;}
static int fail_alloc,allocations;
static void *kzalloc(size_t n,int flags){(void)flags;if(fail_alloc)return NULL;allocations++;return calloc(1,n);}
static void kfree(void *p){if(p){allocations--;free(p);}}
static int sysfs_emit_at(char *buf,int at,const char *fmt,...){
 assert(at>=0&&at<PAGE_SIZE);va_list ap;va_start(ap,fmt);int n=vsnprintf(buf+at,PAGE_SIZE-at,fmt,ap);va_end(ap);assert(n>=0&&n<PAGE_SIZE-at);return n;
}
#define sysfs_emit(buf,...) sysfs_emit_at(buf,0,__VA_ARGS__)
'''
tests=r'''
static void *writer(void *unused){
 (void)unused;u8 f[24]={0};f[16]=0x34;f[17]=0x12;
 for(unsigned i=0;i<5000;i++)mt6797_rx_rejected_record(&c.rx_rejected,14,1,f,sizeof(f),-EOPNOTSUPP);
 return NULL;
}
int main(void){
 struct mt6797_rx_rejected_snapshot snap;u8 f[24]={0};f[16]=0x34;f[17]=0x12;
 char page[4096];assert(rx_rejected_show(NULL,NULL,page)>0&&!strcmp(page,"total=0 count=0\n"));
 for(unsigned n=0;n<20;n++){
  mt6797_rx_rejected_record(&c.rx_rejected,14,1,f,n,-EPROTO);
  mt6797_rx_rejected_snapshot(&c.rx_rejected,&snap);assert(!snap.entry[snap.count-1].op_valid&&!snap.entry[snap.count-1].op);
 }
 mt6797_rx_rejected_record(&c.rx_rejected,14,1,f,24,-EOPNOTSUPP);
 mt6797_rx_rejected_record(&c.rx_rejected,32,1,f,24,-EOPNOTSUPP);
 mt6797_rx_rejected_record(&c.rx_rejected,2,0,f,24,-EOPNOTSUPP);
 mt6797_rx_rejected_snapshot(&c.rx_rejected,&snap);assert(snap.total==23&&snap.count==16);
 for(unsigned i=0;i<16;i++)assert(snap.entry[i].sequence==8+i);
 assert(snap.entry[13].op_valid&&snap.entry[13].op==0x1234&&snap.entry[14].op_valid&&snap.entry[14].channel==32);
 assert(!snap.entry[15].op_valid&&!snap.entry[15].op);
 memset(f,255,sizeof(f));assert(snap.entry[13].op==0x1234);
 struct mt6797_rx_rejected_snapshot old=snap;
 pthread_t thread;assert(!pthread_create(&thread,NULL,writer,NULL));
 for(unsigned i=0;i<5000;i++){
  mt6797_rx_rejected_snapshot(&c.rx_rejected,&snap);assert(snap.count==16);
  for(unsigned j=0;j<16;j++)assert(snap.entry[j].sequence==snap.total-15+j);
 }
 assert(!pthread_join(thread,NULL));assert(old.total==23&&old.entry[13].op==0x1234);
 mt6797_rx_rejected_snapshot(&c.rx_rejected,&snap);assert(snap.total==5023);
 for(unsigned i=0;i<16;i++)c.rx_rejected.entry[i]=(struct mt6797_rx_rejected_entry){UINT64_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,SIZE_MAX,INT32_MIN,true};
 ssize_t n=rx_rejected_show(NULL,NULL,page);assert(n>0&&n<4096&&page[n-1]=='\n');
 unsigned rows=0;for(char *p=page;*p;p++)if(*p=='\n')rows++;assert(rows==17&&strstr(page,"op_valid=1 op=ffffffff"));
 fail_alloc=1;assert(rx_rejected_show(NULL,NULL,page)==-ENOMEM&&!allocations);
 puts("PASS rejected RX: bounded/truncated/unknown-op metadata, immutable copies, concurrent snapshots, wrap ordering, actual sysfs output bounds and allocation failure");
}
'''
assert 'DEVICE_ATTR(rx_rejected, 0400, rx_rejected_show, NULL)' in source
start=source.index('static void mt6797_ccci_dispatch_rx(');end=source.index('static int mt6797_ccci_rx_poll(',start);dispatch=source[start:end]
assert dispatch.count('mt6797_rx_rejected_record(')==3
assert dispatch.index('if (frame.channel != MTK_CCCI_UART2_RX && if_id < 0)')<dispatch.index('if (frame.data1 != skb->len')
for family in ['rpc','fs']:
 assert f'ret = mt6797_ccci_{family}_rx(ccci, skb, q);\n\t\tif (ret)\n\t\t\tmt6797_rx_rejected_record' in dispatch
with tempfile.TemporaryDirectory(prefix='gemini-rx-rejected-') as d:
 p=Path(d);(p/'test.c').write_text(shim+header+adapters+show+tests)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=15)
