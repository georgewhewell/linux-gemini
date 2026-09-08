#!/usr/bin/env python3
"""Production netdev TX queue, worker and codec with skb/transport fakes.
No real workqueue/IRQ concurrency, radio transmission or firmware scheduling.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile
base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-worker-host.py')))
driver = base['driver']
program = base['program'].split('enum stage {')[0]
program = program.replace('struct sk_buff_head { unsigned length; };',
 'struct sk_buff; struct sk_buff_head { unsigned length; struct sk_buff *head,*tail; };')
program = program.replace('struct net_device { u8 dev_addr[6]; };',
 'struct net_device { u8 dev_addr[6]; struct wireless_dev *ieee80211_ptr; unsigned mtu,tx_packets,tx_bytes,tx_dropped; bool stopped; };')
program = program.replace('struct mt6797_wifi_hif_irq { int lifecycle_lock; bool attached; };',
 'struct mt6797_wifi_hif_irq { int lifecycle_lock; bool attached; struct completion tx_credit_completion; };')
program = program.replace('static ktime_t ktime_get(void) { return 100; }',
 'static ktime_t test_time; static ktime_t ktime_get(void) { return test_time; }')
program += r'''
#define dev_info_ratelimited(...) do {} while(0)
#define ETH_HLEN 14
#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806
#define ETH_P_PAE 0x888e
#define ETH_P_802_3_MIN 0x0600
#define ETH_P_8021Q 0x8100
#define ETH_P_8021AD 0x88a8
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
#define BIT(n) (1U<<(n))
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define check_add_overflow(a,b,p) __builtin_add_overflow(a,b,p)
#define MT6797_WIFI_MCR_WTDR1 0x34
#define MT6797_RADIO_TX_QUEUE_LIMIT 64U
#define MT6797_RADIO_TX_WAKE_THRESHOLD 32U
#define DEV_STATS_INC(d,n) ((d)->n++)
#define DEV_STATS_ADD(d,n,v) ((d)->n+=(v))
#define NETDEV_TX_OK 0
#define NETDEV_TX_BUSY 1
typedef int netdev_tx_t;
struct sk_buff { struct sk_buff *next; size_t len; u8 data[1600]; };
static struct mt6797_wifi_radio r;
static struct mt6797_wifi_hif_irq irq;
static struct mt6797_radio_connection c;
static struct net_device dev={.dev_addr={2,6,7,8,9,10},.mtu=1500};
static struct cfg80211_bss bss={.bssid={2,1,2,3,4,5}};
static unsigned calls,sends,freed,queued,fenced;
static int action;
static u8 captured[2352];
static struct mt6797_wifi_radio *wiphy_priv(struct wiphy *w) { (void)w; return &r; }
static void netif_stop_queue(struct net_device *d) { d->stopped=true; }
static void netif_wake_queue(struct net_device *d) { d->stopped=false; }
static unsigned skb_queue_len(struct sk_buff_head *q) { return q->length; }
static void skb_queue_tail(struct sk_buff_head *q,struct sk_buff *skb) {
 skb->next=NULL; if(q->tail) q->tail->next=skb; else q->head=skb; q->tail=skb; q->length++;
}
static struct sk_buff *skb_dequeue(struct sk_buff_head *q) {
 struct sk_buff *skb=q->head; if(!skb) return NULL;
 q->head=skb->next; if(!q->head) q->tail=NULL; q->length--; return skb;
}
static bool pskb_may_pull(struct sk_buff *skb,unsigned length) { return skb->len>=length; }
static int skb_linearize(struct sk_buff *skb) { (void)skb; return 0; }
static void dev_kfree_skb_any(struct sk_buff *skb) { freed++; free(skb); }
static void queue_work(int queue,struct work_struct *work) { (void)queue; (void)work; queued++; }
static void *kmalloc(size_t n,int allocation) { (void)allocation; return malloc(n); }
static void kfree_sensitive(void *p) { free(p); }
static u16 get_unaligned_be16(const void *p) { const u8 *b=p; return (u16)b[0]<<8|b[1]; }
static void put_unaligned_le16(u16 value,void *p) { u8 *b=p; b[0]=value; b[1]=value>>8; }
static ktime_t ktime_add_ms(ktime_t t,unsigned ms) { return t+ms; }
static int ktime_compare(ktime_t a,ktime_t b) { return a<b?-1:a>b; }
static unsigned long msecs_to_jiffies(unsigned ms) { return ms; }
static unsigned long wait_for_completion_timeout(struct completion *completion,unsigned long timeout) {
 (void)completion; assert(timeout==50 && irq.lifecycle_lock && r.connection_lock.held);
 test_time+=timeout; if(action==3) c.cancel=true; return 0;
}
static void mt6797_wifi_hif_runtime_fail(struct mt6797_wifi_hif_irq *i,int error) { assert(i==&irq && error); fenced++; c.cancel=true; }
static int mt6797_wifi_hif_runtime_send(struct mt6797_wifi_hif_irq *i,u32 address,const u8 *p,size_t length) {
 assert(i==&irq && address==MT6797_WIFI_MCR_WTDR1 && irq.lifecycle_lock && r.connection_lock.held);
 calls++;
 if(action==2 || action==3 || (action==1 && calls<=3)) return -EAGAIN;
 memcpy(captured,p,length); sends++; return 0;
}
'''
program += re.sub(r'^#include .*\n', '', (driver / 'protocol.c').read_text(), flags=re.M)
program += base['function']('mt6797_radio_eapol_pairwise_message')
for name in ['mt6797_radio_connection_cancelled', 'mt6797_radio_purge_tx',
             'mt6797_radio_tx_packet', 'mt6797_radio_tx_work', 'mt6797_radio_xmit']:
    program += base['function'](name)
program += r'''
static void setup(void) {
 memset(&r,0,sizeof(r)); memset(&irq,0,sizeof(irq)); memset(&c,0,sizeof(c));
 r.irq=&irq; r.netdev=&dev; r.connection=&c; dev.ieee80211_ptr=&r.wdev;
 c.bss=&bss; c.connected=c.authorized=c.pairwise_ready=true; c.pairwise_key.installed=true;
 c.join.negotiated.security=MT6797_WIFI_AIS_SECURITY_WPA2_PSK_CCMP;
 c.ais.runtime.sta.pairwise.index=3; c.ais.runtime.bss.own_mac_index=2;
 dev.tx_packets=dev.tx_bytes=dev.tx_dropped=0; dev.stopped=false;
 calls=sends=freed=queued=fenced=0; action=0; test_time=0;
}
static struct sk_buff *packet(u16 ether_type) {
 struct sk_buff *skb=calloc(1,sizeof(*skb)); skb->len=18;
 memcpy(skb->data,bss.bssid,6); memcpy(skb->data+6,dev.dev_addr,6);
 skb->data[12]=ether_type>>8; skb->data[13]=ether_type; return skb;
}
static struct sk_buff *eapol(u16 info,unsigned data_length) {
 struct sk_buff *skb=packet(ETH_P_PAE); u8 *p=skb->data+14;
 skb->len=113+data_length; p[0]=2; p[1]=3;
 p[2]=(95+data_length)>>8; p[3]=95+data_length; p[4]=2;
 p[5]=info>>8; p[6]=info; p[97]=data_length>>8; p[98]=data_length;
 return skb;
}
static void submit(u16 ether_type) { assert(mt6797_radio_xmit(packet(ether_type),&dev)==NETDEV_TX_OK); }
static void run(void) {
 mt6797_radio_tx_work(&r.tx_work);
 assert(!r.lock && !r.connection_lock.held && !irq.lifecycle_lock && !r.tx_queue.length);
}
int main(void) {
 setup(); submit(ETH_P_IP); run();
 assert(sends==1 && freed==1 && dev.tx_packets==1 && dev.tx_bytes==18 && !dev.tx_dropped);
 assert(captured[3]==0x20 && captured[4]==3 && captured[6]==0x80 && captured[7]==8);
 setup(); c.authorized=false; submit(ETH_P_IP); assert(freed==1 && !queued); run(); assert(!sends);
 setup(); c.authorized=false; c.pairwise_key.installed=c.pairwise_ready=false;
 submit(ETH_P_PAE); run(); assert(sends==1 && captured[3]==0x88 && !(captured[6]&0x80));
 assert(captured[11]==0x80 && !get_unaligned_le16(captured+26)); /* 1 Mbps default. */
 setup(); c.control_port_no_encrypt=true; submit(ETH_P_PAE); run(); assert(sends==1 && !(captured[6]&0x80));
 setup(); submit(ETH_P_PAE); run(); assert(sends==1 && (captured[6]&0x80));
 /* PTK may be ready before a plaintext M3's M4 is queued. Only that
  * pairwise reply may bypass encryption; group replies and data may not. */
 setup(); c.pairwise_reply_plaintext=true;
 assert(!mt6797_radio_xmit(eapol(0x030a,0),&dev)); run(); assert(sends==1 && !(captured[6]&0x80));
 assert(c.pairwise_reply_plaintext);
 assert(!mt6797_radio_xmit(eapol(0x030a,0),&dev)); run(); assert(sends==2 && !(captured[6]&0x80));
 setup(); c.pairwise_reply_plaintext=true;
 assert(!mt6797_radio_xmit(eapol(0x010a,0),&dev)); run(); assert(!(captured[6]&0x80)); /* AP omitted Secure. */
 setup(); c.pairwise_reply_plaintext=true;
 assert(!mt6797_radio_xmit(eapol(0x0302,0),&dev)); run(); assert(captured[6]&0x80); /* Group reply. */
 setup(); c.pairwise_reply_plaintext=true;
 assert(!mt6797_radio_xmit(eapol(0x010a,22),&dev)); run(); assert(captured[6]&0x80); /* M2 with RSN. */
 setup(); c.pairwise_reply_plaintext=true; submit(ETH_P_IP); run(); assert(captured[6]&0x80);
 setup(); assert(!mt6797_radio_xmit(eapol(0x030a,0),&dev)); run(); assert(captured[6]&0x80); /* Protected M3. */
 setup(); c.control_port_no_encrypt=true;
 assert(!mt6797_radio_xmit(eapol(0x030a,0),&dev)); run(); assert(!(captured[6]&0x80));
 setup(); struct sk_buff *key=eapol(0x13ca,0); u8 *body=key->data+14;
 assert(mt6797_radio_eapol_pairwise_message(body,99)==3);
 for(unsigned length=0;length<99;length++) assert(!mt6797_radio_eapol_pairwise_message(body,length));
 for(unsigned bit=0;bit<16;bit++) {
  if(!(0xec30U&(1U<<bit))) continue;
  unsigned info=0x13caU|(1U<<bit); body[5]=info>>8; body[6]=info;
  assert(!mt6797_radio_eapol_pairwise_message(body,99));
 }
 body[5]=0x13; body[6]=0xca;
 body[0]=3; assert(!mt6797_radio_eapol_pairwise_message(body,99)); body[0]=2;
 body[1]=0; assert(!mt6797_radio_eapol_pairwise_message(body,99)); body[1]=3;
 body[4]=254; assert(!mt6797_radio_eapol_pairwise_message(body,99)); body[4]=2;
 body[98]=1; assert(!mt6797_radio_eapol_pairwise_message(body,99)); body[98]=0;
 body[3]=96; assert(!mt6797_radio_eapol_pairwise_message(body,99));
 free(key);
 setup(); c.join.negotiated.security=MT6797_WIFI_AIS_SECURITY_OPEN; submit(ETH_P_IP); run(); assert(sends==1 && !(captured[6]&0x80));
 setup(); struct sk_buff *skb=packet(ETH_P_IP); memset(skb->data,255,6);
 assert(!mt6797_radio_xmit(skb,&dev)); run(); assert(sends==1 && captured[4]==3 && (captured[6]&0x80));
 setup(); skb=packet(ETH_P_PAE); skb->data[1]^=1;
 assert(!mt6797_radio_xmit(skb,&dev)); run(); assert(!sends && dev.tx_dropped==1);
 setup(); skb=packet(ETH_P_IP); skb->data[7]^=1;
 assert(!mt6797_radio_xmit(skb,&dev)); run(); assert(!sends && dev.tx_dropped==1);
 setup(); submit(ETH_P_8021Q); run(); assert(!sends && dev.tx_dropped==1);
 setup(); c.pairwise_ready=false; submit(ETH_P_IP); run(); assert(!sends && dev.tx_dropped==1);
 setup(); action=1; submit(ETH_P_IP); run(); assert(sends==1 && calls==4 && test_time==150);
 setup(); action=2; submit(ETH_P_IP); run(); assert(!sends && fenced==1 && test_time==1000 && dev.tx_dropped==1);
 setup(); action=3; submit(ETH_P_IP); run(); assert(!sends && !fenced && test_time==50 && dev.tx_dropped==1);
 setup();
 for(unsigned i=0;i<64;i++) submit(ETH_P_IP);
 assert(r.tx_queue.length==64 && dev.stopped);
 skb=packet(ETH_P_IP); assert(mt6797_radio_xmit(skb,&dev)==NETDEV_TX_BUSY && !freed); free(skb);
 run(); assert(sends==64 && freed==64 && !dev.stopped);
 setup(); submit(ETH_P_IP); submit(ETH_P_PAE); c.cancel=true;
 mt6797_radio_purge_tx(&r); assert(freed==2 && dev.tx_dropped==2); run(); assert(!sends);
 puts("PASS: production TX queue/worker/codec: port/key/encryption policy, plaintext-M3 reply and retransmit policy, bounded EAPOL classifier, AP EAPOL, multicast via PTK, fixed 1Mbps, credit retries/deadline/cancellation, queue backpressure and teardown ownership");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-radio-tx-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
