#!/usr/bin/env python3
"""Actual passive HIF getter: lock ordering, contention, snapshot and bounds."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(os.environ.get('GEMINI_KERNEL_SOURCE', Path(__file__).resolve().parents[2]))
driver = root / 'drivers/net/wireless/mediatek/mt6797'
radio = (driver / 'radio.c').read_text()
match = re.search(r'^static ssize_t hif_diagnostics_show\([^;]*?\)\n\{', radio, re.M)
assert match
end = match.end()
depth = 1
while depth:
    depth += (radio[end] == '{') - (radio[end] == '}')
    end += 1
getter = radio[match.start():end]
stats = re.search(r'struct mt6797_wifi_hif_pump_stats \{.*?\n\};',
                  (driver / 'hif_pump.h').read_text(), re.S).group()
program = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef uint64_t u64; typedef uint8_t u8;
#define MT6797_WIFI_HIF_TX_QUEUES 16
#define PAGE_SIZE 4096
'''+stats+r'''
struct mutex { pthread_mutex_t native; unsigned rank; };
struct mt6797_wifi_hif_irq { struct mutex lifecycle_lock,rx_lock; struct mt6797_wifi_hif_pump_stats pump_stats; };
struct mt6797_wifi_radio { struct mt6797_wifi_hif_irq *irq; struct { u64 lifetime_start_ns; } tx_diag; };
struct wiphy { struct mt6797_wifi_radio *radio; };
struct wireless_dev { struct wiphy *wiphy; };
struct device { int unused; };
struct net_device { struct device dev; struct wireless_dev *ieee80211_ptr; };
struct device_attribute { int unused; };
#define to_net_dev(dev) ((struct net_device *)(dev))
#define wiphy_priv(wiphy) ((wiphy)->radio)
static struct mt6797_wifi_hif_irq irq;
static struct mt6797_wifi_radio radio={.irq=&irq,.tx_diag={1234567}};
static struct wiphy wiphy={&radio};
static struct wireless_dev wireless={&wiphy};
static struct net_device netdev={.ieee80211_ptr=&wireless};
static _Thread_local unsigned held,seen;
static atomic_bool life_taken;
static bool mutate_after_snapshot;
static void mutex_lock(struct mutex *m) {
 assert(held==m->rank-1);
 assert(!pthread_mutex_lock(&m->native));held++;seen|=1U<<(m->rank-1);
 if(m->rank==1)atomic_store(&life_taken,true);
}
static void mutex_unlock(struct mutex *m) {
 assert(held==m->rank);held--;assert(!pthread_mutex_unlock(&m->native));
}
static ssize_t sysfs_emit(char *buf,const char *format,...) {
 assert(!held && seen==3);seen=0;
 if(mutate_after_snapshot)memset(&irq.pump_stats,0,sizeof(irq.pump_stats));
 va_list args;va_start(args,format);int n=vsnprintf(buf,PAGE_SIZE,format,args);va_end(args);
 assert(n>0 && n<PAGE_SIZE);return n;
}
'''+getter+r'''
static char output[PAGE_SIZE];
static unsigned long long field(const char *name) {
 char needle[64];snprintf(needle,sizeof(needle),"%s=",name);const char *p=strstr(output,needle);
 assert(p && (p==output || p[-1]=='\n'));unsigned long long v;assert(sscanf(p+strlen(needle),"%llu",&v)==1);return v;
}
static void fill(u64 n,bool pending) {
 memset(&irq.pump_stats,0,sizeof(irq.pump_stats));irq.pump_stats.interrupts=n;
 irq.pump_stats.rx_packets=n;irq.pump_stats.rx_dropped=n;irq.pump_stats.rx_budget_yields=n;irq.pump_stats.rx_pending=pending;
}
static void check(u64 n,bool pending) {
 assert(field("version")==1 && field("lifetime_start_ns")==1234567);
 for(unsigned i=0;i<4;i++){const char *names[]={"interrupts","rx_packets","rx_dropped","rx_budget_yields"};assert(field(names[i])==n);}
 assert(field("rx_pending")==pending);
 unsigned lines=0;for(char *p=output;*p;p++)lines+=*p=='\n';assert(lines==7);
}
static void *reader(void *unused) {
 (void)unused;assert(hif_diagnostics_show(&netdev.dev,NULL,output)>0);return NULL;
}
int main(void) {
 irq.lifecycle_lock.rank=1;irq.rx_lock.rank=2;
 assert(!pthread_mutex_init(&irq.lifecycle_lock.native,NULL));assert(!pthread_mutex_init(&irq.rx_lock.native,NULL));
 for(unsigned pending=0;pending<2;pending++) {
  fill(UINT64_MAX,pending);struct mt6797_wifi_hif_pump_stats before=irq.pump_stats;
  reader(NULL);check(UINT64_MAX,pending);assert(!memcmp(&before,&irq.pump_stats,sizeof(before)));
 }
 /* Formatting uses the captured values after the owner locks are released. */
 fill(97,true);mutate_after_snapshot=true;reader(NULL);check(97,true);mutate_after_snapshot=false;
 /* Start/stop/reset owns lifecycle; a snapshot waits for that owner. */
 pthread_t thread;assert(!pthread_mutex_lock(&irq.lifecycle_lock.native));fill(1,false);
 assert(!pthread_create(&thread,NULL,reader,NULL));fill(41,true);
 assert(!pthread_mutex_unlock(&irq.lifecycle_lock.native));assert(!pthread_join(thread,NULL));check(41,true);
 /* RX never needs lifecycle: it can finish while the snapshot holds lifecycle
  * and waits for rx_lock. This is the same order used by start/stop.
  */
 atomic_store(&life_taken,false);assert(!pthread_mutex_lock(&irq.rx_lock.native));fill(2,false);
 assert(!pthread_create(&thread,NULL,reader,NULL));
 for(unsigned wait=0;!atomic_load(&life_taken);wait++){assert(wait<10000);usleep(100);}
 assert(pthread_mutex_trylock(&irq.lifecycle_lock.native)==EBUSY);fill(73,false);
 assert(!pthread_mutex_unlock(&irq.rx_lock.native));assert(!pthread_join(thread,NULL));check(73,false);
 assert(!pthread_mutex_destroy(&irq.rx_lock.native));assert(!pthread_mutex_destroy(&irq.lifecycle_lock.native));
 puts("PASS: actual passive HIF getter, lifecycle->RX order, start/stop and RX contention, immutable snapshots, pending states and 64-bit/page bounds");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-hif-diagnostics-') as temporary:
    path = Path(temporary)
    (path/'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', '-pthread', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=20)
