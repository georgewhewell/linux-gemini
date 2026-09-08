#!/usr/bin/env python3
"""Run the actual FIFO pump, RX codecs, and existing KUnit cases with host shims.
Synthetic FIFO frames cover delivery/drop boundaries; no real IRQ concurrency.
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import tempfile
base = runpy.run_path(str(Path(__file__).with_name('test-wifi-data-rx-host.py')))
driver = base['driver']
program = base['program'].split('\nint main(void) {')[0]
for name in ['hif_regs.h', 'hif_pump.h', 'hif_pump.c']:
    program += re.sub(r'^#include .*\n', '', (driver / name).read_text(), flags=re.M)
program += r'''
static void put_unaligned_be16(u16 v,void *p) { u8 *b=p; b[0]=v>>8; b[1]=v; }
struct kunit { int unused; };
#define U64_MAX UINT64_MAX
#define KUNIT_ASSERT_EQ(t,a,b) assert((a)==(b))
#define KUNIT_EXPECT_EQ(t,a,b) assert((a)==(b))
#define KUNIT_EXPECT_PTR_EQ(t,a,b) assert((a)==(b))
#define KUNIT_EXPECT_TRUE(t,a) assert(a)
#define KUNIT_EXPECT_MEMEQ(t,a,b,n) assert(!memcmp(a,b,n))
'''
cases = []
for name in ['protocol_test.c', 'hif_pump_test.c']:
    src = (driver / name).read_text()
    cases.extend(re.findall(r'KUNIT_CASE\((\w+)\)', src))
    program += re.sub(r'^#include .*\n', '', src.split('static struct kunit_case')[0], flags=re.M)
program += r'''
static unsigned read_index, frames, events, drops;
static int sink_error, packet_error;
static size_t packet_length;
static int status_read(void *c,u32 *s) { (void)c; *s=MT6797_WIFI_WHISR_RX0_DONE; return 0; }
static int credits(void *c,u16 *v) { (void)c; memset(v,0,32); return 0; }
static int lengths_read(void *c,u16 *v) {
 (void)c; v[1]=0; v[0]=read_index==0?packet_length:read_index==1?8:0; return 0;
}
static int packet_read(void *c,unsigned port,u16 len,const u8 **out) {
 static const u8 event[]={8,0,0,0xe0,1,2,0,0}; (void)c; assert(!port);
 if(packet_error) return packet_error;
 if(read_index==0) { assert(len==packet_length); *out=packet; }
 else { assert(len==8); *out=event; }
 read_index++; return 0;
}
static bool expired(void *c) { (void)c; return false; }
static int frame_deliver(void *c,const struct mt6797_wifi_rx_view *v) {
 struct mt6797_wifi_data_rx d; (void)c;
 assert(!mt6797_wifi_data_rx_decode(v->packet,v->packet_length,&d));
 assert(d.length==18 && d.ethernet[12]==0x88 && d.ethernet[13]==0x8e && d.eapol);
 assert(!d.protected && d.ethernet[0]==2 && d.transmitter[0]==2);
 frames++; return sink_error;
}
static int event_deliver(void *c,const struct mt6797_wifi_event_view *v) {
 (void)c; assert(v->id==1); events++; return 0;
}
static void reject(void *c,const u8 *p,size_t n,int error) {
 (void)c; assert(p==packet && n==packet_length && error<0); drops++;
}
static int pump(struct mt6797_wifi_hif_pump_stats *s) {
 const struct mt6797_wifi_hif_pump_ops ops={
 .read_status=status_read,.read_tx_released=credits,.read_lengths=lengths_read,
 .read_packet=packet_read,.deliver_frame=frame_deliver,.deliver_event=event_deliver,
 .reject_packet=reject,.expired=expired}; u32 status;
 read_index=frames=events=drops=0; memset(s,0,sizeof(*s));
 return mt6797_wifi_hif_pump_run(&ops,s,&status);
}
int main(void) {
 struct kunit t={0}; struct mt6797_wifi_hif_pump_stats s;
'''
program += '\n'.join(f' {case}(&t);' for case in cases)
program += r'''
 for(unsigned native_frame=0;native_frame<2;native_frame++) {
  packet_length=native_frame?native(false,true,false,0,false):setup(false,true);
  assert(!pump(&s)); assert(frames==1 && events==1 && !drops && s.rx_packets==2);
  assert(s.ethernet_frames==!native_frame && s.ieee80211_frames==native_frame);
  sink_error=-EIO; assert(pump(&s)==-EIO && frames==1 && !events); sink_error=0;
 }
 /* A corrupt air frame or an unrelated report is drained, then the event
  * behind it is delivered. Both otherwise used to kill the transport.
  */
 for(unsigned mode=0;mode<2;mode++) {
  packet_length=setup(false,true);
  if(mode) put_unaligned_le16(0x2000,packet+2);
  else put_unaligned_le16(0xc002,packet+10);
  assert(!pump(&s)); assert(!frames && events==1 && drops==1 && s.rx_dropped==1);
 }
 /* Transport bounds and firmware event errors still fail closed. */
 packet_length=setup(false,true); put_unaligned_le16(packet_length+1,packet);
 assert(pump(&s)==-EMSGSIZE && !frames && !events && drops==1);
 packet_length=setup(false,true); packet_error=-EIO;
 assert(pump(&s)==-EIO && !frames && !events && !drops);
 puts("PASS: production protocol/pump KUnit cases plus FIFO native and translated EAPOL delivery, drain/drop continuation, sink failures and transport bounds");
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-rx-pump-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
