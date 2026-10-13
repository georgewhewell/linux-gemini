#!/usr/bin/env python3
"""Execute actual ring IO/runtime serializers against bounded synthetic memory."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
shim=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32; typedef uint8_t u8;
#define __iomem
#define BIT(n) (1U<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define min(a,b) ((a)<(b)?(a):(b))
#define cpu_to_le32(a) (a)
#define le32_to_cpu(a) (a)
static unsigned reads,writes,barriers;
static u8 smem[0x200000];
static void range(const void *p,size_t n){assert((const u8 *)p>=smem&&(const u8 *)p+n<=smem+sizeof(smem));}
static u32 readl(const void *p){u32 v;range(p,4);reads++;memcpy(&v,p,4);return v;}
static void writel(u32 v,void *p){range(p,4);writes++;memcpy(p,&v,4);}
static void memcpy_fromio(void *d,const void *s,size_t n){range(s,n);reads++;memcpy(d,s,n);}
static void memcpy_toio(void *d,const void *s,size_t n){range(d,n);writes++;memcpy(d,s,n);}
#define mb() (barriers++)
#define rmb() mb()
#define wmb() mb()
static void put_unaligned_le32(u32 v,void *p){memcpy(p,&v,4);}
static u32 get32(const void *p){u32 v;memcpy(&v,p,4);return v;}
'''
tests=r'''
static void init(unsigned q,struct md3_ring_layout *l){
 assert(!md3_ring_layout(q,l));memset(smem,0,sizeof(smem));u8 *p=smem+l->offset;
 writel(0xee0000ee,p);writel(0xee0000ee,p+4);writel(l->rx,p+16);writel(l->tx,p+28);
 writel(0xff0000ff,p+32+l->rx+l->tx);writel(0xff0000ff,p+36+l->rx+l->tx);
 reads=writes=barriers=0;
}
/* Independent scalar oracle for wrapped byte placement, never calls the
 * implementation's copy helper or pointer arithmetic. */
static void packet(u8 *ring,u32 size,u32 pos,const u8 *data,u32 len){
 u8 gold[4200]={0};put_unaligned_le32(0xaabbaabb,gold);put_unaligned_le32(len,gold+4);
 memcpy(gold+8,data,len);u32 total=((len+7)/8)*8+16;
 put_unaligned_le32(0xccddeeff,gold+total-8);put_unaligned_le32(0xccddeeff,gold+total-4);
 for(u32 i=0;i<total;i++)ring[(pos+i)%size]=gold[i];
}
int main(void){
 struct md3_ring_layout l;u8 data[4096],out[4096];unsigned cases=0;
 for(unsigned i=0;i<sizeof(data);i++)data[i]=(u8)(i*71+3);
 assert(md3_ring_layout(8,&l)==-EINVAL);
 for(unsigned q=0;q<8;q++)for(unsigned lenid=0;lenid<6;lenid++)for(unsigned wrap=0;wrap<4;wrap++){
  const unsigned sizes[]={16,17,31,3456,3476,4096};unsigned len=sizes[lenid],total=((len+7)/8)*8+16;
  init(q,&l);u8 *p=smem+l.offset;unsigned pos=wrap==0?0:wrap==1?l.tx-8:wrap==2?l.tx-16:l.tx-4096;
  writel(pos,p+20);writel(pos,p+24);unsigned old=writes;
  assert(md3_ring_transmit(smem,q,data,len)==(int)len&&writes>old&&barriers);
  assert(readl(p+24)==(pos+total)%l.tx&&readl(p+20)==pos);
  u8 gold[160000]={0};packet(gold,l.tx,pos,data,len);
  for(unsigned i=0;i<8+len;i++)assert(p[32+l.rx+(pos+i)%l.tx]==gold[(pos+i)%l.tx]);
  for(unsigned i=total-8;i<total;i++)assert(p[32+l.rx+(pos+i)%l.tx]==gold[(pos+i)%l.tx]);
  pos=wrap==0?0:wrap==1?l.rx-8:wrap==2?l.rx-16:l.rx-4096;
  packet(p+32,l.rx,pos,data,len);writel(pos,p+8);writel((pos+total)%l.rx,p+12);
  old=writes;assert(md3_ring_receive(smem,q,out,len-1)==-EMSGSIZE&&writes==old);
  assert(md3_ring_receive(smem,q,out,sizeof(out))==(int)len&&!memcmp(out,data,len));
  assert(readl(p+8)==(pos+total)%l.rx&&md3_ring_receive(smem,q,out,sizeof(out))==0);cases++;
 }
 for(unsigned fault=0;fault<11;fault++){
  init(4,&l);u8 *p=smem+l.offset;packet(p+32,l.rx,0,data,16);writel(32,p+12);
  if(fault==0)writel(0,p);if(fault==1)writel(0,p+36+l.rx+l.tx);
  if(fault==2)writel(l.rx+8,p+16);if(fault==3)writel(l.rx,p+12);if(fault==4)writel(1,p+8);
  if(fault==5)writel(8,p+12);if(fault==6)writel(0,p+32);if(fault==7)writel(UINT32_MAX,p+36);
  if(fault==8)writel(15,p+36);if(fault==9)packet(p+32,l.rx,0,data,100);if(fault==10)writel(0,p+56);
  unsigned before=writes;assert(md3_ring_receive(smem,4,out,sizeof(out))==-EPROTO&&writes==before);cases++;
 }
 init(4,&l);u8 *p=smem+l.offset;writel(24,p+20);unsigned before=writes;
 assert(md3_ring_transmit(smem,4,data,16)==-EAGAIN&&writes==before);
 const u32 words[16]={50463234,50529026,50528771,196610,0,768,3,0,0,0,0,0,0,0,0,0};
 u8 query[64],rt[2048],ap[MD3_QUERY_FRAME_SIZE];size_t used=0;memcpy(query,words,64);
 assert(!md3_runtime_build(query,rt,sizeof(rt),&used,ap,0,2,true)&&used==584&&sizeof(ap)==116);
 size_t pos=0;unsigned mandatory=0;
 for(unsigned i=0;i<64;i++){
  assert(rt[pos]==i);u32 n=get32(rt+pos+4);bool owned=i==0||i==1||i==2||i==4||i==9||i==12;
  assert(rt[pos+1]==(owned?2:query[i]?1:0));if(owned)mandatory++;
  if(i==1)assert(n==8&&get32(rt+pos+8)==0x40400000&&get32(rt+pos+12)==0x10000);
  if(i==2)assert(n==8&&get32(rt+pos+8)==0x40419000&&get32(rt+pos+12)==0xb6950);
  if(i==4)assert(n==8&&get32(rt+pos+8)==0x40200000&&get32(rt+pos+12)==0x200000);
  if(i==9)assert(n==16&&get32(rt+pos+8)==0&&get32(rt+pos+12)==0);
  if(i==12)assert(n==16&&get32(rt+pos+8)==5);
  if(i==14)assert(rt[pos+1]==1&&n==0);pos+=8+n;
 }
 assert(mandatory==6&&pos==used&&get32(ap+4)==116&&get32(ap+8)==1&&ap[20]==3);
 assert(get32(ap+16)==0x43434349&&get32(ap+112)==0x43434349);
 assert(get32(ap+84)==1&&get32(ap+88)==0x40410000&&get32(ap+92)==0x800);
 assert(get32(ap+96)==0x40410800&&get32(ap+100)==0x800&&get32(ap+104)==0x40400000&&get32(ap+108)==0x200000);
 for(unsigned fault=0;fault<5;fault++){
  memcpy(query,words,64);memset(rt,0xa5,sizeof(rt));memset(ap,0xa5,sizeof(ap));used=123;
  if(fault==0)query[14]=2;if(fault==1)query[0]=0x12;if(fault==2)query[40]=5;
  assert(md3_runtime_build(query,rt,sizeof(rt),&used,ap,0,fault==3?1:2,fault!=4)<0);
  assert(used==123);for(unsigned i=0;i<sizeof(rt);i++)assert(rt[i]==0xa5);for(unsigned i=0;i<sizeof(ap);i++)assert(ap[i]==0xa5);
 }

 for(unsigned q=0;q<8;q++){
  init(q,&l);u8 *p=smem+l.offset,*flow=smem+0xcf940;unsigned attempts=0;
  writel(0x464c4f57,flow);writel(BIT(q),flow+8);
  assert(!md3_ring_resume_needed(smem,q,&attempts));
  writel(0x4354524c,flow+12);writel(l.rx/2+8,p+12);
  assert(!md3_ring_resume_needed(smem,q,&attempts));writel(l.rx/2,p+12);
  assert(md3_ring_resume_needed(smem,q,&attempts)==1&&!attempts);
  attempts=1;assert(!md3_ring_resume_needed(smem,q,&attempts));writel(l.rx/8,p+12);
  assert(md3_ring_resume_needed(smem,q,&attempts)==1&&attempts==1);
  attempts=UINT32_MAX;writel(0,p+12);assert(md3_ring_resume_needed(smem,q,&attempts)==1);
  writel(0,flow+8);assert(!md3_ring_resume_needed(smem,q,&attempts)&&!attempts);
  writel(BIT(q),flow+8);writel(l.rx,p+12);assert(md3_ring_resume_needed(smem,q,&attempts)==-EPROTO);
  writel(0,p+12);writel(123,flow+12);assert(md3_ring_resume_needed(smem,q,&attempts)==-EPROTO);
  cases++;
 }
 printf("PASS actual MD3 rings: %u wrap/boundary/corruption cases; actual163 mandatory runtime/optional refusal; exact AP query116; no publication on rejected input\n",cases);
}
'''
headers=[(root/'drivers/soc/mediatek'/n).read_text() for n in ['mt6797-md3-ring.h','mt6797-md3-runtime.h']]
code=shim+'\n'.join(headers)+tests
mutations={
 'fabricated-flow-support': ('if (!tail)', 'if (false && !tail)'),
 'ignored-MD-busy': ('if (!(readl(flow + 8) & BIT(queue)))', 'if (false)'),
 'unknown-mandatory-success': ('return -EOPNOTSUPP;', 'return 0;'),
 'SCP-false-admission': ('} else if (mask) {\n\t\t\tmask = 1;', '} else if (mask) {\n\t\t\tmask = 2;'),
 'missing-footer-guard': ('if (le32_to_cpu(footer[0]) != 0xccddeeff ||','if (false && le32_to_cpu(footer[0]) != 0xccddeeff ||'),
 'publish-before-full-frame': ('if (total > available)','if (false && total > available)'),
}
with tempfile.TemporaryDirectory(prefix='md3-ring-runtime-') as d:
 p=Path(d)
 for name,mutation in [('actual',None),*mutations.items()]:
  src=code
  if mutation:
   a,b=mutation;assert src.count(a)==1,name;src=src.replace(a,b)
  (p/'test.c').write_text(src)
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-parentheses','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
  r=subprocess.run([str(p/'test')],capture_output=True,text=True,timeout=30)
  if mutation:assert r.returncode!=0,name;print('REJECTED',name)
  else:
   if r.returncode:print(r.stderr)
   r.check_returncode();print(r.stdout,end='')
