#!/usr/bin/env python3
"""Production beacon admission and PM wait/send policy with synchronous fakes.
The separate vendor codec and AIS runtime tests validate the mocked boundaries.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile
base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-worker-host.py')))
program = base['program'].split('enum stage {')[0]
program = program.replace('static ktime_t ktime_get(void) { return 100; }',
                         'static ktime_t now=100; static ktime_t ktime_get(void) { return now; }')
driver = base['driver']
program = program.replace('struct mt6797_wifi_hif_irq { int lifecycle_lock; bool attached; };',
                         'struct mt6797_wifi_hif_irq { int lifecycle_lock,mode; bool attached; };')
program += r'''
#define WLAN_EID_TIM 5
#define MT6797_WIFI_HIF_IRQ_RUNTIME 2
#define MT6797_WIFI_MCR_WTDR1 0x34
#define dev_info(...) do {} while(0)
static struct mt6797_wifi_radio r;
static struct mt6797_radio_connection c;
static struct mt6797_wifi_hif_irq irq;
static struct net_device dev={.dev_addr={2,1,2,3,4,5}};
static struct cfg80211_bss bss={.bssid={2,6,7,8,9,10}};
static unsigned writes,releases,fences,waits;
static unsigned long last_timeout;
static int action,wait_action;
static bool cancel_during_send;
static u8 frame[42];
static struct mt6797_wifi_rx_view view;
static int mt6797_wifi_hif_irq_mode(struct mt6797_wifi_hif_irq *h) { return h->mode; }
static unsigned long msecs_to_jiffies(unsigned n) { return n; }
static int mt6797_radio_beacon(struct mt6797_wifi_radio *,const struct mt6797_wifi_rx_view *);
static unsigned long wait_for_completion_timeout(struct completion *completion,unsigned long timeout) {
 assert(r.connection_lock.held && !irq.lifecycle_lock && completion==&c.pm_completion);
 waits++; last_timeout=timeout;
 if(wait_action==0) mt6797_radio_beacon(&r,&view);
 if(wait_action==1) { c.cancel=true; complete(completion); }
 if(wait_action==3) complete(completion);
 if(wait_action==4) { mt6797_radio_beacon(&r,&view); now=5000; }
 return completion->done;
}
int mt6797_wifi_ais_pm_build(const struct mt6797_wifi_ais_bss_handle *b,
 const struct mt6797_wifi_ais_sta_handle *s,const struct mt6797_wifi_ais_pm_params *p,
 bool connected,u8 sequence,u8 *command,size_t capacity) {
 assert(b==&c.ais.runtime.bss && s==&c.ais.runtime.sta && sequence && capacity==20);
 if(connected)assert(p->association_id==123 && p->dtim_period==3 && p->beacon_interval==100);
 memset(command,0,capacity); command[4]=connected?0x16:0x17;
 return 0;
}
static int mt6797_wifi_hif_runtime_send(struct mt6797_wifi_hif_irq *h,u32 address,const u8 *packet,size_t length) {
 assert(h==&irq && irq.lifecycle_lock && r.connection_lock.held && address==0x34);
 assert((packet[4]==0x16 && length==20)||(packet[4]==0x17 && length==12));
 writes++; if(cancel_during_send)c.cancel=true;
 return action==1?-EIO:0;
}
static void mt6797_wifi_hif_runtime_fail(struct mt6797_wifi_hif_irq *h,int error) {
 assert(h==&irq && error<0); h->mode=3; fences++;
}
int mt6797_wifi_ais_hif_release_channel(struct mt6797_wifi_ais_hif *ais) {
 assert(ais==&c.ais && c.pm_started && r.connection_lock.held && !irq.lifecycle_lock);
 releases++;
 if(action==2) { mt6797_wifi_hif_runtime_fail(&irq,-EIO); return -EIO; }
 return 0;
}
'''
join = (driver/'ais_join.c').read_text()
program += join[join.index('int mt6797_wifi_ais_pm_beacon_parse('):join.index('int mt6797_wifi_ais_pm_build(')]
metadata = (driver/'rx_metadata.c').read_text()
for name in ['MT6797_WIFI_RX_5GHZ_CHANNEL_BIAS','MT6797_WIFI_RX_MAX_REGULAR_5GHZ_CHANNEL']:
    program += re.search(r'^#define\s+'+name+r'\s+[^\n]+',metadata,re.M)[0]+'\n'
program += metadata[metadata.index('int mt6797_wifi_rx_channel_decode('):metadata.index('int mt6797_wifi_rx_metadata_decode(')]
for name in ['mt6797_radio_connection_cancelled','mt6797_radio_beacon','mt6797_radio_pm_send','mt6797_radio_finish_pm']:
    program += base['function'](name)
program += r'''
static void setup(void) {
 memset(&r,0,sizeof(r)); memset(&c,0,sizeof(c)); memset(&irq,0,sizeof(irq));
 writes=releases=fences=waits=0; action=wait_action=0; cancel_during_send=false;
 now=100;
 r.connection=&c; r.netdev=&dev; r.irq=&irq; r.connection_lock.held=1; r.command_sequence=255;
 irq.mode=2; c.bss=&bss; c.connected=true; c.link_attached=true;
 c.join.negotiated.channel=(struct mt6797_wifi_ais_channel){1,6};
 c.join.negotiated.association_id=123; c.ais.channel_deadline=5000;
 memset(frame,0,sizeof(frame)); frame[0]=0x80;
 memset(frame+4,255,6); memcpy(frame+10,bss.bssid,6); memcpy(frame+16,bss.bssid,6);
 frame[32]=100; frame[36]=5; frame[37]=4; frame[38]=1; frame[39]=3;
 view=(struct mt6797_wifi_rx_view){.payload=frame,.payload_length=42,.channel=6};
}
static void rejected(void) {
 assert(!mt6797_radio_beacon(&r,&view));
 assert(!c.beacon_seen && !c.pm_completion.done && !writes && !releases && !r.lock);
}
int main(void) {
 setup(); assert(!mt6797_radio_finish_pm(&r));
 assert(writes==1 && releases==1 && !fences && c.pm_started && c.beacon_seen);
 assert(last_timeout==1500 && r.command_sequence==3 && !irq.lifecycle_lock);
 frame[39]=4; assert(!mt6797_radio_beacon(&r,&view)); assert(c.pm.dtim_period==3); /* Duplicate immutable. */
 c.cancel=true; assert(!mt6797_radio_pm_send(&r,false)); /* Abort after cancellation remains valid. */
 assert(writes==2 && !c.pm_started && !irq.lifecycle_lock);
 setup(); memcpy(frame+4,dev.dev_addr,6); assert(!mt6797_radio_finish_pm(&r));
 assert(writes==1 && releases==1);
 setup(); c.ais.channel_deadline=150; assert(!mt6797_radio_finish_pm(&r)); assert(last_timeout==50);
 setup(); c.ais.channel_deadline=99; assert(mt6797_radio_finish_pm(&r)==-ETIMEDOUT && !waits && !writes);
 setup(); wait_action=2; assert(mt6797_radio_finish_pm(&r)==-ETIMEDOUT && !writes && !fences);
 setup(); wait_action=1; assert(mt6797_radio_finish_pm(&r)==-ECANCELED && !writes && !fences);
 setup(); wait_action=3; assert(mt6797_radio_finish_pm(&r)==-EPROTO && !writes && !fences);
 setup(); wait_action=4; assert(mt6797_radio_finish_pm(&r)==-ETIMEDOUT && !writes && !fences);
 setup(); action=1; assert(mt6797_radio_finish_pm(&r)==-EIO && writes==1 && fences==1 && !releases && !c.pm_started);
 assert(mt6797_radio_pm_send(&r,true)==-ENOTCONN && writes==1); /* No retry after unknown send. */
 setup(); action=2; assert(mt6797_radio_finish_pm(&r)==-EIO && writes==1 && fences==1 && releases==1 && c.pm_started);
 assert(mt6797_radio_pm_send(&r,false)==-ENOTCONN && writes==1);
 setup(); cancel_during_send=true; assert(mt6797_radio_finish_pm(&r)==-ECANCELED && writes==1 && !releases && c.pm_started);
 setup(); frame[10]^=4; rejected();
 setup(); frame[16]^=4; rejected();
 setup(); frame[4]=2; rejected();
 setup(); view.channel=11; rejected();
 setup(); view.channel=36; rejected();
 setup(); view.channel=0; rejected();
 setup(); c.cancel=true; rejected();
 setup(); r.stopping=true; rejected();
 setup(); c.connected=false; rejected();
 setup(); r.connection=NULL; rejected();
 setup(); frame[39]=0; rejected();
 setup(); frame[0]=0x50; rejected();
 setup(); view.payload_length=37; rejected();
 setup(); view.payload=NULL; rejected();
 puts("PASS: production beacon ownership/channel/duplicate admission, bounded PM wait, cancellation, connected/abort ordering, command wrap, uncertain-send fencing and channel-release failure");
}
'''
if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='gemini-radio-pm-') as temporary:
        path=Path(temporary)
        (path/'test.c').write_text(program)
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-g',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)
