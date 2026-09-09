#!/usr/bin/env python3
"""Production registration/stop/unregister with netdev/sysfs fakes.
The real kernel sysfs active-reader barrier is not simulated by these host tests.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile
base = runpy.run_path(str(Path(__file__).with_name('test-wifi-radio-tx-host.py')))
program = base['program'].split('int main(void) {')[0]
radio = (base['driver'] / 'radio.c').read_text()
program = program.replace('bool stopped; };', 'bool stopped; char name[16]; const void *netdev_ops; const struct attribute_group *sysfs_groups[5]; };')
program = program.replace('struct wiphy { int dummy; };', 'struct wiphy { unsigned interface_modes,signal_type,max_scan_ssids,n_cipher_suites; const u32 *cipher_suites; struct ieee80211_supported_band *bands[2]; u8 perm_addr[6]; };')
program = program.replace('struct wireless_dev { struct wiphy *wiphy; };', 'struct wireless_dev { struct wiphy *wiphy; unsigned iftype; struct net_device *netdev; };')
program = program.replace('struct ieee80211_supported_band { int dummy; };', 'struct ieee80211_supported_band { struct ieee80211_channel *channels; struct ieee80211_rate *bitrates; unsigned n_channels,n_bitrates; };')
program = program.replace('struct ieee80211_channel { int dummy; };', 'struct ieee80211_channel { unsigned band,hw_value,center_freq,max_power; };')
program = program.replace('struct ieee80211_rate { int dummy; };', 'struct ieee80211_rate { unsigned bitrate,hw_value; };')
program = program.replace('struct device_attribute { int unused; };', '''
struct attribute { unsigned mode; };
struct device_attribute { struct attribute attr; ssize_t (*show)(struct device *,struct device_attribute *,char *); void *store; };
struct attribute_group { struct attribute **attrs; };
#define DEVICE_ATTR_RO(name) struct device_attribute dev_attr_##name={.attr={0444},.show=name##_show}
''')
program += radio[radio.index('static DEVICE_ATTR_RO(tx_diagnostics);'):radio.index('static void mt6797_radio_purge_tx')]
program += r'''
#define NL80211_IFTYPE_STATION 2
#define CFG80211_SIGNAL_TYPE_MBM 1
#define WLAN_CIPHER_SUITE_CCMP 0x000fac04
#define IFNAMSIZ 16
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer)>(uintptr_t)-4096)
#define spin_lock_init(lock) (*(lock)=0)
#define mutex_init(lock) ((lock)->held=0)
#define init_completion(completion) ((completion)->done=false)
#define INIT_WORK(work,callback) do { (void)(work); (void)(callback); } while(0)
#define SET_NETDEV_DEV(netdev,parent) do { (void)(netdev); (void)(parent); } while(0)
static const int mt6797_radio_ops,mt6797_radio_netdev_ops;
static struct wiphy registered_wiphy;
static struct device parent;
static bool wiphy_alive,netdev_alive,sysfs_published;
static unsigned registration_failure,wiphy_frees,netdev_frees,wiphy_unregisters,netdev_unregisters;
static int mt6797_radio_stop(struct net_device *netdev);
static void mt6797_radio_scan_work(struct work_struct *work) { (void)work; }
static void mt6797_radio_connection_work(struct work_struct *work) { (void)work; }
static struct wiphy *wiphy_new(const void *ops,size_t size) {
 assert(ops==&mt6797_radio_ops && size==sizeof(r) && !wiphy_alive);
 if(registration_failure==1) return NULL;
 memset(&r,0,sizeof(r)); memset(&registered_wiphy,0,sizeof(registered_wiphy));
 wiphy_alive=true; return &registered_wiphy;
}
static void set_wiphy_dev(struct wiphy *wiphy,struct device *dev) { assert(wiphy==&registered_wiphy && dev==&parent); }
static unsigned ieee80211_channel_to_frequency(unsigned channel,unsigned band) { return band?5000+5*channel:2407+5*channel; }
static struct net_device *alloc_etherdev(unsigned private_size) {
 assert(!private_size && wiphy_alive && !netdev_alive);
 if(registration_failure==2) return NULL;
 memset(&dev,0,sizeof(dev)); dev.mtu=1500; netdev_alive=true; return &dev;
}
static void strscpy(char *to,const char *from,size_t n) { snprintf(to,n,"%s",from); }
static bool is_valid_ether_addr(const u8 *address) { return address[0] && !(address[0]&1); }
static void eth_hw_addr_set(struct net_device *netdev,const u8 *address) { memcpy(netdev->dev_addr,address,6); }
static void eth_hw_addr_random(struct net_device *netdev) { const u8 address[]={2,1,2,3,4,5}; memcpy(netdev->dev_addr,address,6); }
static void ether_addr_copy(u8 *to,const u8 *from) { memcpy(to,from,6); }
static void netif_carrier_off(struct net_device *netdev) { assert(netdev==&dev); }
static void skb_queue_head_init(struct sk_buff_head *queue) { memset(queue,0,sizeof(*queue)); }
static int wiphy_register(struct wiphy *wiphy) { assert(wiphy_alive && wiphy==&registered_wiphy); return registration_failure==3?-EIO:0; }
static int register_netdev(struct net_device *netdev) {
 assert(netdev_alive && netdev==&dev && netdev->sysfs_groups[0]==&mt6797_radio_tx_diag_group);
 assert(mt6797_radio_tx_diag_attrs[0]==&dev_attr_tx_diagnostics.attr && !mt6797_radio_tx_diag_attrs[1]);
 assert(dev_attr_tx_diagnostics.attr.mode==0444 && !dev_attr_tx_diagnostics.store);
 assert(dev_attr_tx_diagnostics.show==tx_diagnostics_show);
 sysfs_published=true; check_snapshot();
 if(registration_failure==4) { sysfs_published=false; return -EIO; }
 return 0;
}
static void flush_work(struct work_struct *work) {
 assert(!r.lock && !r.connection_lock.held);
 if(work==&r.connection_work) r.connection=NULL;
 else if(work==&r.tx_work) mt6797_radio_tx_work(work);
 else assert(work==&r.scan_work);
}
static void unregister_netdev(struct net_device *netdev) {
 assert(netdev_alive && sysfs_published && netdev==&dev);
 check_snapshot(); mt6797_radio_stop(netdev); check_snapshot();
 sysfs_published=false; netdev_unregisters++;
}
static void wiphy_unregister(struct wiphy *wiphy) {
 assert(wiphy_alive && !sysfs_published && wiphy==&registered_wiphy); wiphy_unregisters++;
}
static void free_netdev(struct net_device *netdev) {
 assert(netdev_alive && !sysfs_published && netdev==&dev && !r.tx_queue.length);
 netdev_alive=false; netdev_frees++;
}
static void wiphy_free(struct wiphy *wiphy) {
 assert(wiphy_alive && !netdev_alive && !sysfs_published && wiphy==&registered_wiphy);
 assert(!r.tx_diag.worker_active && !r.tx_diag.send_active && !r.tx_diag.credit_wait_active);
 wiphy_alive=false; wiphy_frees++;
}
'''
def function(name):
    match = re.search(r'^.*\b' + re.escape(name) + r'\([^;]*?\)\n\{', radio, re.M)
    assert match, name
    offset = match.end()
    depth = 1
    while depth:
        depth += (radio[offset] == '{') - (radio[offset] == '}')
        offset += 1
    return radio[match.start():offset] + '\n'
for name in ('mt6797_radio_abort','mt6797_radio_disconnect','mt6797_radio_stop',
             'mt6797_wifi_radio_register','mt6797_wifi_radio_unregister'):
    program += function(name)
program += r'''
static void registry_setup(unsigned failure) {
 setup(); assert(!wiphy_alive && !netdev_alive && !sysfs_published);
 registration_failure=failure; wiphy_frees=netdev_frees=wiphy_unregisters=netdev_unregisters=0;
}
int main(void) {
 struct mt6797_wifi_capability capability={.mac_address={2,6,7,8,9,10}};
 for(unsigned failure=1;failure<=4;failure++) {
  registry_setup(failure);
  struct mt6797_wifi_radio *out=mt6797_wifi_radio_register(&parent,&irq,NULL,&capability);
  assert(IS_ERR(out) && PTR_ERR(out)==(failure<=2?-ENOMEM:-EIO));
  assert(!wiphy_alive && !netdev_alive && !sysfs_published);
  assert(wiphy_frees==(failure>1) && netdev_frees==(failure>2));
  assert(wiphy_unregisters==(failure==4) && !netdev_unregisters);
 }
 registry_setup(0); test_time=8;
 assert(mt6797_wifi_radio_register(&parent,&irq,NULL,&capability)==&r);
 assert(r.tx_diag.lifetime_start_ns==8000000 && sysfs_published); check_snapshot();
 r.connection=&c; c.connected=c.authorized=true;
 submit(ETH_P_IP); submit(ETH_P_IP); check_snapshot();
 mt6797_radio_stop(&dev);
 assert(sysfs_published && !r.tx_queue.length && r.tx_diag.enqueued==2 && r.tx_diag.send_disconnected==2);
 check_snapshot();
 r.connection=&c; c.cancel=false; c.connected=true; submit(ETH_P_IP);
 mt6797_wifi_radio_unregister(&r);
 assert(!sysfs_published && !wiphy_alive && !netdev_alive);
 assert(wiphy_frees==1 && netdev_frees==1 && wiphy_unregisters==1 && netdev_unregisters==1);
 assert(r.tx_diag.enqueued==3 && r.tx_diag.send_disconnected==3);
 puts("PASS: production read-only sysfs registration, allocation/register failures, stop snapshot persistence, queued-send teardown and unregister-before-free ordering");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-radio-tx-diag-lifecycle-') as temporary:
    path = Path(temporary)
    (path/'test.c').write_text(program)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-g',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
