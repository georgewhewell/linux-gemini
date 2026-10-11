#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the native MSDC clock/request callbacks with failed providers."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/mmc/host/mtk-sd.c").read_text()


def function(name, optional=False):
    match = re.search(r"^static (?:inline )?[^;\n]*\b" + name + r"\([^;]*?\)\n\{.*?^}\n", source, re.M | re.S)
    if not match and not optional:
        raise RuntimeError("Missing native callback: " + name)
    return match.group(0) if match else ""


updated = bool(re.search(r"^static int msdc_set_mclk", source, re.M))
code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t u8;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define WARN_ON(v) assert(!(v))
#define spin_lock_irqsave(l,f) do {(void)(l); (f)=0;} while (0)
#define spin_unlock_irqrestore(l,f) do {(void)(l); (void)(f);} while (0)
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define MMC_RSP_PRESENT BIT(0)
#define MMC_RSP_136 BIT(1)
#define MMC_RSP_R1B_NO_CRC 99
#define mmc_resp_type(c) ((c)->flags)
#define MMC_POWER_OFF 0
#define MMC_POWER_UP 1
#define MMC_POWER_ON 2
#define MMC_TIMING_LEGACY 0
#define MMC_TIMING_MMC_HS 1
#define MMC_TIMING_UHS_DDR50 2
#define MMC_TIMING_MMC_DDR52 3
#define MMC_TIMING_MMC_HS400 4
#define MMC_ERR_CMD_TIMEOUT 0
#define MMC_ERR_CMD_CRC 1
#define MMC_ERR_DAT_TIMEOUT 2
#define MMC_ERR_DAT_CRC 3
#define MMC_ERR_AUTO_CMD 4
#define MMC_ERR_ADMA 5
#define MMC_ERR_REQ_TIMEOUT 10
#define MMC_ERR_CTRL_TIMEOUT 13
'''
code += "\n".join(re.findall(r"^#define[^\n]*(?:\\\n[^\n]*)*", source, re.M)) + "\n"
for name in ("cmd_ints_mask", "data_ints_mask"):
    code += re.search(r"^static const u32 " + name + r" = [^;]+;\n", source, re.M).group(0)
for name in ("msdc_save_para", "msdc_tune_para", "mtk_mmc_compatible"):
    code += re.search(r"^struct " + name + r" \{.*?^};\n", source, re.M | re.S).group(0)
code += r'''
struct mmc_host;
struct msdc_host;
struct mmc_card { int unused; };
struct mmc_command { int error; unsigned flags, opcode, arg, resp[4]; struct mmc_data *data; };
struct mmc_data { int error; unsigned bytes_xfered, blocks, blksz, host_cookie, sg_count, sg_len;
                 void *sg; struct mmc_command *stop; };
struct mmc_request { struct mmc_command *cmd, *sbc, *stop; struct mmc_data *data; };
struct mmc_ios { unsigned clock, timing, power_mode, bus_width, vdd; };
struct mmc_cqe_ops {
 int (*cqe_enable)(struct mmc_host *, struct mmc_card *);
 int (*cqe_request)(struct mmc_host *, struct mmc_request *);
 void (*cqe_post_req)(struct mmc_host *, struct mmc_request *);
 void (*cqe_off)(struct mmc_host *);
};
struct device { struct mmc_host *data; };
struct clk { int leases, id; };
struct clk_bulk_data { struct clk *clk; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct mmc_host { struct msdc_host *priv; unsigned actual_clock, err_stats[15];
 struct mmc_ios ios; struct {void *vmmc,*vqmmc;} supply;
 struct mmc_card *card; const struct mmc_cqe_ops *cqe_ops; };
struct msdc_host {
 struct device *dev; const struct mtk_mmc_compatible *dev_comp; void *base,*top_base;
 struct clk *h_clk,*bus_clk,*src_clk,*src_clk_cg,*crypto_clk;
 struct clk_bulk_data bulk_clks[MSDC_NR_CLOCKS];
 bool clocks_on,src_clk_cg_on,needs_hw_init,hsq_en,vqmmc_enabled,hs400_tuning;
 int clock_error,error,lock,irq; unsigned mclk,src_clk_freq,timeout_ns,timeout_clks,hs400_cmd_int_delay;
 unsigned char timing;
 struct mmc_cqe_ops cqe_ops; const struct mmc_cqe_ops *cqe_delegate;
 struct msdc_save_para save_para; struct msdc_tune_para def_tune_para,saved_tune_para;
 struct mmc_request *mrq; struct mmc_command *cmd; struct mmc_data *data;
 struct delayed_work req_timeout; void *pinctrl,*pins_eint,*pins_uhs;
};
static struct mmc_host mmc;
static struct msdc_host host;
static struct device dev;
static struct mtk_mmc_compatible compat;
static struct clk clocks[8];
static u32 regs[256];
static unsigned io, maps, unmaps, commands, completions, finalized, resets, tunes;
static unsigned poll_calls, unbounded, fail_poll, fail_poll2, stuck_mask, drop_mask;
static unsigned cfg_generation, accepted_generation, unmasked, hsq_resumes, irq_enables, nexts;
static unsigned delegate_enables, delegate_requests, buswidths, init_fail_poll;
static int enable_call, fail_enable, fail_enable2, delegate_error;
static bool forbidden_io, sdio, queued, hsq_suspended;
static struct mmc_host *mmc_from_priv(struct msdc_host *h) {assert(h==&host); return &mmc;}
static struct msdc_host *mmc_priv(struct mmc_host *m) {return m->priv;}
static struct mmc_host *dev_get_drvdata(struct device *d) {return d->data;}
static void mmc_debugfs_err_stats_inc(struct mmc_host *m,int stat) {m->err_stats[stat]++;}
static u32 readl(void *p) {
 assert(!forbidden_io); io++;
 u32 val=*(u32 *)p;
 if(p==(char *)host.base+MSDC_CFG) val|=MSDC_CFG_CKSTB;
 if(p==(char *)host.base+MSDC_DMA_CTRL) val&=~MSDC_DMA_CTRL_STOP;
 if(p==(char *)host.base+MSDC_DMA_CFG) val&=~MSDC_DMA_CFG_STS;
 return val;
}
static void writel(u32 val,void *p) {
 assert(!forbidden_io); io++;
 if(p==(char *)host.base+MSDC_CFG) {
  val=(val&~drop_mask)|(*(u32 *)p&drop_mask); cfg_generation++;
 }
 *(u32 *)p=val;
}
#define readl_poll_timeout(addr,val,cond,delay,timeout) ({ \
 unsigned call=++poll_calls; int result=-ETIMEDOUT; \
 if(!(timeout)) unbounded++; else assert((timeout)==20000 && (delay)==1); \
 (val)=readl(addr); \
 if(call==fail_poll || call==fail_poll2) (val)&=~MSDC_CFG_CKSTB; \
 (val)&=~stuck_mask; \
 if(cond) { result=0; accepted_generation=cfg_generation; } result; })
#define readl_poll_timeout_atomic readl_poll_timeout
static int clk_prepare_enable(struct clk *c) {
 if(!c)return 0;
 int call=++enable_call;
 if(call==fail_enable || call==fail_enable2)return -EACCES;
 c->leases++; return 0;
}
static void clk_disable_unprepare(struct clk *c) {
 if(!c)return;
 assert(c->leases>0); c->leases--;
}
static int clk_bulk_prepare_enable(int n,struct clk_bulk_data *c) {
 for(int i=0;i<n;i++) {
  int ret=clk_prepare_enable(c[i].clk);
  if(ret) {while(i--)clk_disable_unprepare(c[i].clk); return ret;}
 } return 0;
}
static void clk_bulk_disable_unprepare(int n,struct clk_bulk_data *c) {
 while(n--)clk_disable_unprepare(c[n].clk);
}
static void msdc_set_timeout(struct msdc_host *h,u64 ns,u64 clks) {
 (void)h;(void)ns;(void)clks;tunes++;
}
static void msdc_new_tx_setting(struct msdc_host *h) {(void)h;tunes++;}
static int dma_map_sg(struct device *d,void *sg,unsigned n,int dir) {
 (void)d;(void)sg;(void)dir;maps++;return n;
}
static void dma_unmap_sg(struct device *d,void *sg,unsigned n,int dir) {
 (void)d;(void)sg;(void)n;(void)dir;unmaps++;
}
static int mmc_get_dma_dir(struct mmc_data *d) {(void)d;return 0;}
static bool mmc_hsq_finalize_request(struct mmc_host *m,struct mmc_request *r) {
 (void)m;(void)r;if(!queued)return false;queued=false;finalized++;return true;
}
static void mmc_request_done(struct mmc_host *m,struct mmc_request *r) {(void)m;(void)r;completions++;}
static bool mmc_card_mmc(struct mmc_card *c) {(void)c;return true;}
static bool mmc_op_tuning(unsigned op) {(void)op;return false;}
static void msdc_start_command(struct msdc_host *h,struct mmc_request *r,struct mmc_command *c) {
 (void)h;(void)r;(void)c;commands++;
}
static void msdc_cmd_next(struct msdc_host *h,struct mmc_request *r,struct mmc_command *c) {
 (void)h;(void)r;(void)c;nexts++;
}
static void msdc_data_xfer_next(struct msdc_host *h,struct mmc_request *r) {(void)h;(void)r;nexts++;}
static void msdc_reset_hw(struct msdc_host *h) {(void)h;resets++;}
static void msdc_set_buswidth(struct msdc_host *h,unsigned width) {(void)h;(void)width;buswidths++;}
static int mmc_regulator_set_ocr(struct mmc_host *m,void *r,unsigned v) {(void)m;(void)r;(void)v;return 0;}
static int regulator_enable(void *r) {(void)r;return 0;}
static void regulator_disable(void *r) {(void)r;}
static bool sdio_irq_claimed(struct mmc_host *m) {(void)m;return sdio;}
static void __msdc_enable_sdio_irq(struct msdc_host *h,int on) {
 (void)h;if(on) {assert(accepted_generation==cfg_generation);unmasked++;}
}
static void enable_irq(int n) {(void)n;irq_enables++;}
static void disable_irq(int n) {(void)n;}
static int pinctrl_select_state(void *p,void *s) {(void)p;(void)s;return 0;}
static void mmc_hsq_resume(struct mmc_host *m) {(void)m;hsq_suspended=false;hsq_resumes++;}
static void mmc_hsq_suspend(struct mmc_host *m) {(void)m;hsq_suspended=true;}
static void msdc_init_hw(struct msdc_host *h) {
 assert(h->clocks_on && h->src_clk_cg_on && !h->clock_error);resets++;
 if(init_fail_poll)fail_poll=poll_calls+1;
}
'''
code += "#define UPDATED " + str(int(updated)) + "\n"
names = ["sdr_set_bits", "sdr_clr_bits", "sdr_set_field", "msdc_prepare_data", "msdc_data_prepared",
         "msdc_unprepare_data", "msdc_gate_clock", "msdc_clock_mask", "msdc_wait_clock", "msdc_ungate_clock",
         "msdc_clock_ready", "msdc_apply_clock", "msdc_set_mclk", "msdc_auto_cmd_done", "msdc_cmd_done",
         "msdc_ops_request", "msdc_pre_req", "msdc_post_req", "msdc_ops_cqe_enable", "msdc_ops_cqe_request",
         "msdc_install_cqe_ops", "msdc_data_xfer_done", "msdc_request_timeout", "msdc_set_ios_clock",
         "msdc_ops_set_ios", "msdc_save_reg", "msdc_restore_reg", "msdc_runtime_suspend", "msdc_runtime_resume"]
optional = {"msdc_clock_mask", "msdc_wait_clock", "msdc_clock_ready", "msdc_apply_clock",
            "msdc_ops_cqe_enable", "msdc_ops_cqe_request", "msdc_install_cqe_ops", "msdc_set_ios_clock"}
for name in names:
    code += function(name, name in optional)
code += r'''
static void reset_case(int bits,bool acquired) {
 memset(&mmc,0,sizeof(mmc));memset(&host,0,sizeof(host));memset(&dev,0,sizeof(dev));
 memset(&compat,0,sizeof(compat));memset(clocks,0,sizeof(clocks));memset(regs,0,sizeof(regs));
 io=maps=unmaps=commands=completions=finalized=resets=tunes=0;
 poll_calls=unbounded=fail_poll=fail_poll2=stuck_mask=drop_mask=0;
 cfg_generation=accepted_generation=unmasked=hsq_resumes=irq_enables=nexts=0;
 delegate_enables=delegate_requests=buswidths=init_fail_poll=0;
 enable_call=fail_enable=fail_enable2=delegate_error=0;
 forbidden_io=sdio=queued=hsq_suspended=false;
 mmc.priv=&host;dev.data=&mmc;host.dev=&dev;host.base=regs;host.dev_comp=&compat;
 compat.clk_div_bits=bits;compat.pad_tune_reg=0xf0;
 host.h_clk=&clocks[0];host.bus_clk=&clocks[1];host.src_clk=&clocks[2];
 host.src_clk_cg=&clocks[3];host.crypto_clk=&clocks[4];
 for(int i=0;i<8;i++){clocks[i].id=i;clocks[i].leases=acquired;}
 for(int i=0;i<MSDC_NR_CLOCKS;i++)host.bulk_clks[i].clk=&clocks[i+5];
 host.clocks_on=host.src_clk_cg_on=acquired;
 host.src_clk_freq=384000000;host.mclk=25000000;mmc.actual_clock=24000000;
 mmc.ios=(struct mmc_ios){.clock=25000000,.timing=MMC_TIMING_LEGACY};
 mmc.supply.vmmc=mmc.supply.vqmmc=(void *)(intptr_t)-ENOENT;
 regs[MSDC_CFG/4]=BIT(31)|MSDC_CFG_MODE|MSDC_CFG_PIO|MSDC_CFG_CKPDN|MSDC_CFG_CKSTB|(4U<<8);
 regs[MSDC_INTEN/4]=0xa55a;host.save_para.msdc_cfg=regs[MSDC_CFG/4];
}
static int set_clock(unsigned timing,unsigned hz) {
#if UPDATED
 return msdc_set_mclk(&host,timing,hz);
#else
 msdc_set_mclk(&host,timing,hz);return host.clock_error;
#endif
}
static void assert_leases(int n) {for(int i=0;i<8;i++)assert(clocks[i].leases==n);}
static void test_write_rejection(void) {
 for(int bits=8;bits<=12;bits+=4) {
  reset_case(bits,true);u32 original=regs[MSDC_CFG/4];drop_mask=MSDC_CFG_CKDIV;
  assert(set_clock(MMC_TIMING_MMC_HS,52000000)==-EIO);
  assert(host.clock_error==-EIO && host.mclk==25000000 && host.timing==MMC_TIMING_LEGACY);
  assert(mmc.actual_clock==24000000 && tunes==0 && regs[MSDC_INTEN/4]==0xa55a);
  assert(regs[MSDC_CFG/4]==original && unbounded==0);assert_leases(1);
#if UPDATED
  drop_mask=0;assert(set_clock(MMC_TIMING_MMC_HS,52000000)==0);
  assert(host.clock_error==0 && host.mclk==52000000 && mmc.actual_clock==48000000);
  assert((regs[MSDC_CFG/4]&(BIT(31)|MSDC_CFG_PIO|MSDC_CFG_MODE))==(original&(BIT(31)|MSDC_CFG_PIO|MSDC_CFG_MODE)));
  drop_mask=bits==8?MSDC_CFG_HS400_CK_MODE:MSDC_CFG_HS400_CK_MODE_EXTRA;
  assert(set_clock(MMC_TIMING_MMC_HS400,192000000)==-EIO);
  assert(host.timing==MMC_TIMING_MMC_HS && mmc.actual_clock==48000000);
  msdc_gate_clock(&host);assert_leases(0);
#endif
 }
 puts("MSDC_MASKED_CLOCK_REJECTION_PASS");
}
#if UPDATED
static void test_acquire(void) {
 for(int point=1;point<=8;point++) {
  reset_case(8,false);fail_enable=point;
  assert(msdc_ungate_clock(&host)==-EACCES);assert_leases(0);
  assert(!host.clocks_on && !host.src_clk_cg_on && host.clock_error==-EACCES);
  msdc_gate_clock(&host);assert_leases(0); /* Probe release label owns no failed acquire. */
  fail_enable=0;assert(msdc_ungate_clock(&host)==0);assert_leases(1);
  assert(set_clock(MMC_TIMING_LEGACY,25000000)==0 && !host.clock_error);
  msdc_gate_clock(&host);assert_leases(0);msdc_gate_clock(&host);assert_leases(0);
 }
 reset_case(12,false);fail_poll=1;
 assert(msdc_ungate_clock(&host)==-ETIMEDOUT);assert_leases(0);
 assert(mmc.err_stats[MMC_ERR_CTRL_TIMEOUT]==1 && unbounded==0);
}
static void test_clock_rollback(void) {
 for(int failed=1;failed<=2;failed++) {
  reset_case(8,true);fail_enable=failed;
  if(failed==2)fail_poll=1;
  assert(set_clock(MMC_TIMING_MMC_HS,52000000)==(failed==1?-EACCES:-ETIMEDOUT));
  assert(host.mclk==25000000 && host.timing==MMC_TIMING_LEGACY && tunes==0);
  if(failed==1)assert_leases(1);
  else {assert(!host.src_clk_cg_on && !mmc.actual_clock);assert(!(regs[MSDC_CFG/4]&MSDC_CFG_CKPDN));}
  msdc_gate_clock(&host);assert_leases(0);
 }
 reset_case(8,true);fail_enable=1;fail_enable2=2;
 assert(set_clock(MMC_TIMING_MMC_HS,52000000)==-EACCES);
 assert(!host.src_clk_cg_on && !mmc.actual_clock && !regs[MSDC_INTEN/4]);
 msdc_gate_clock(&host);assert_leases(0);
 reset_case(8,true);fail_poll=1;fail_poll2=2;
 assert(set_clock(MMC_TIMING_MMC_HS,52000000)==-ETIMEDOUT);
 assert(mmc.err_stats[MMC_ERR_CTRL_TIMEOUT]==2 && host.clock_error==-ETIMEDOUT);
 assert(!mmc.actual_clock && !regs[MSDC_INTEN/4]);msdc_gate_clock(&host);assert_leases(0);
}
static int delegate_enable(struct mmc_host *m,struct mmc_card *c) {(void)m;(void)c;delegate_enables++;return delegate_error;}
static int delegate_request(struct mmc_host *m,struct mmc_request *r) {(void)m;(void)r;delegate_requests++;return delegate_error;}
static void delegate_post(struct mmc_host *m,struct mmc_request *r) {(void)m;(void)r;}
static void delegate_off(struct mmc_host *m) {(void)m;}
static void test_requests(void) {
 struct mmc_command cmd={0};struct mmc_data data={.sg_len=1};
 struct mmc_request request={.cmd=&cmd,.data=&data};
 struct mmc_cqe_ops engine={.cqe_enable=delegate_enable,.cqe_request=delegate_request,
                         .cqe_post_req=delegate_post,.cqe_off=delegate_off};
 reset_case(8,true);mmc.cqe_ops=&engine;msdc_install_cqe_ops(&host);
 assert(host.cqe_delegate==&engine && mmc.cqe_ops==&host.cqe_ops);
 assert(mmc.cqe_ops->cqe_post_req==delegate_post && mmc.cqe_ops->cqe_off==delegate_off);
 assert(mmc.cqe_ops->cqe_enable(&mmc,NULL)==0 && mmc.cqe_ops->cqe_request(&mmc,&request)==0);
 delegate_error=-ENOSPC;assert(mmc.cqe_ops->cqe_request(&mmc,&request)==-ENOSPC);
 msdc_pre_req(&mmc,&request);assert(maps==1 && data.host_cookie==(MSDC_PREPARE_FLAG|MSDC_ASYNC_FLAG));
 host.clock_error=-ETIMEDOUT;host.hsq_en=true;queued=true;
 forbidden_io=true;unsigned writes=io;
 msdc_ops_request(&mmc,&request);assert(finalized==1 && !completions && !commands && io==writes);
 assert(cmd.error==-ETIMEDOUT && data.error==-ETIMEDOUT && !data.bytes_xfered);
 assert(unmaps==0 && data.host_cookie==(MSDC_PREPARE_FLAG|MSDC_ASYNC_FLAG));
 msdc_post_req(&mmc,&request,0);assert(unmaps==1 && !data.host_cookie);
 msdc_pre_req(&mmc,&request);assert(maps==1 && !data.host_cookie);
 msdc_ops_request(&mmc,&request);assert(completions==1 && finalized==1 && !commands);
 assert(mmc.cqe_ops->cqe_request(&mmc,&request)==-ETIMEDOUT);
 assert(mmc.cqe_ops->cqe_enable(&mmc,NULL)==-ETIMEDOUT);
 assert(delegate_requests==2 && delegate_enables==1);
 forbidden_io=false;host.hsq_en=false;
 assert(set_clock(MMC_TIMING_MMC_HS,52000000)==0);
 host.needs_hw_init=true;forbidden_io=true;
 assert(mmc.cqe_ops->cqe_request(&mmc,&request)==-EIO);
 msdc_ops_request(&mmc,&request);assert(completions==2 && !commands);
 forbidden_io=false;host.needs_hw_init=false;
 msdc_ops_request(&mmc,&request);assert(commands==1 && maps==2);
 msdc_unprepare_data(&host,&data);assert(unmaps==2);msdc_gate_clock(&host);assert_leases(0);
}
static void test_resume_and_power(void) {
 reset_case(8,false);sdio=true;host.hsq_en=true;hsq_suspended=true;host.pins_eint=&dev;fail_poll=2;
 assert(msdc_runtime_resume(&dev)==-ETIMEDOUT);
 assert(!unmasked && !irq_enables && !hsq_resumes && hsq_suspended);assert_leases(0);
 fail_poll=0;assert(msdc_runtime_resume(&dev)==0 && !host.clock_error);
 assert(unmasked==1 && irq_enables==1 && hsq_resumes==1);assert_leases(1);
 assert(msdc_runtime_suspend(&dev)==0 && hsq_suspended);assert_leases(0);
 reset_case(8,true);host.clock_error=-EIO;mmc.supply.vmmc=&dev;
 struct mmc_ios ios={.clock=0,.power_mode=MMC_POWER_UP};
 msdc_ops_set_ios(&mmc,&ios);assert(host.needs_hw_init && host.clock_error==-EIO && !resets && !buswidths);
 ios.power_mode=MMC_POWER_ON;ios.clock=400000;
 msdc_ops_set_ios(&mmc,&ios);assert(resets==1 && !host.needs_hw_init && !host.clock_error);
 assert(mmc.actual_clock==400000 && host.mclk==400000 && buswidths==1);
 msdc_gate_clock(&host);assert_leases(0);
 reset_case(8,true);host.clock_error=-EIO;mmc.supply.vmmc=&dev;ios.clock=0;ios.power_mode=MMC_POWER_UP;
 msdc_ops_set_ios(&mmc,&ios);init_fail_poll=1;ios.clock=400000;ios.power_mode=MMC_POWER_ON;
 msdc_ops_set_ios(&mmc,&ios);assert(host.needs_hw_init && host.clock_error==-ETIMEDOUT && !mmc.actual_clock);
 assert(host.mclk==0 && !buswidths && msdc_clock_ready(&host)==-ETIMEDOUT);
 init_fail_poll=fail_poll=0;msdc_ops_set_ios(&mmc,&ios);
 assert(!host.needs_hw_init && !host.clock_error && host.mclk==400000);
 msdc_gate_clock(&host);assert_leases(0);
}
static void test_data_errors(void) {
 unsigned faults[]={MSDC_INT_DMA_BDCSERR,MSDC_INT_DMA_GPDCSERR,MSDC_INT_DMA_PROTECT,
                    MSDC_INT_DATCRCERR,MSDC_INT_DATTMO};
 for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);i++)for(int complete=0;complete<2;complete++) {
  reset_case(8,true);struct mmc_command cmd={0};struct mmc_data data={.blocks=2,.blksz=512};
  struct mmc_request request={.cmd=&cmd,.data=&data};host.data=&data;
  msdc_data_xfer_done(&host,faults[i]|(complete?MSDC_INT_XFER_COMPL:0),&request,&data);
  assert(data.error==(i<3?-EIO:i==3?-EILSEQ:-ETIMEDOUT) && !data.bytes_xfered && !host.data);
  assert(nexts==1 && host.error==REQ_DAT_ERR && resets==1);
  assert(mmc.err_stats[i<3?MMC_ERR_ADMA:i==3?MMC_ERR_DAT_CRC:MMC_ERR_DAT_TIMEOUT]==1);
  if(i<3)assert(!mmc.err_stats[MMC_ERR_DAT_CRC] && !mmc.err_stats[MMC_ERR_DAT_TIMEOUT]);
  msdc_data_xfer_done(&host,faults[i],&request,&data);assert(nexts==1); /* Duplicate IRQ. */
  msdc_gate_clock(&host);assert_leases(0);
 }
 reset_case(8,true);struct mmc_command cmd={0},stop={.error=-EIO};
 struct mmc_data data={.blocks=2,.blksz=512};struct mmc_request request={.cmd=&cmd,.data=&data};
 host.data=&data;msdc_data_xfer_done(&host,MSDC_INT_XFER_COMPL,&request,&data);
 assert(data.bytes_xfered==1024 && !data.error && !resets);
 data.stop=&stop;data.bytes_xfered=0;host.data=&data;
 msdc_data_xfer_done(&host,MSDC_INT_XFER_COMPL,&request,&data);
 assert(!data.bytes_xfered && stop.error==-EIO && !mmc.err_stats[MMC_ERR_DAT_CRC]);
 msdc_gate_clock(&host);assert_leases(0);
}
static void test_command_counters(void) {
 unsigned faults[]={MSDC_INT_RSPCRCERR,MSDC_INT_CMDTMO};
 for(int i=0;i<2;i++) {
  reset_case(8,true);struct mmc_command cmd={0};struct mmc_request request={.cmd=&cmd};host.cmd=&cmd;
  assert(msdc_cmd_done(&host,faults[i],&request,&cmd));
  assert(cmd.error==(i?-ETIMEDOUT:-EILSEQ));assert(mmc.err_stats[i?MMC_ERR_CMD_TIMEOUT:MMC_ERR_CMD_CRC]==1);
  assert(msdc_cmd_done(&host,faults[i],&request,&cmd));assert(mmc.err_stats[i?MMC_ERR_CMD_TIMEOUT:MMC_ERR_CMD_CRC]==1);
  memset(&cmd,0,sizeof(cmd));assert(msdc_auto_cmd_done(&host,i?MSDC_INT_ACMDTMO:MSDC_INT_ACMDCRCERR,&cmd));
  assert(mmc.err_stats[MMC_ERR_AUTO_CMD]==1);msdc_gate_clock(&host);assert_leases(0);
 }
 reset_case(8,true);struct mmc_command cmd={0};struct mmc_request request={.cmd=&cmd};host.cmd=&cmd;host.mrq=&request;
 msdc_request_timeout(&host.req_timeout.work);
 assert(mmc.err_stats[MMC_ERR_REQ_TIMEOUT]==1 && mmc.err_stats[MMC_ERR_CMD_TIMEOUT]==1);
 msdc_gate_clock(&host);assert_leases(0);
}
#endif
int main(void) {
 test_write_rejection();
#if UPDATED
 test_acquire();test_clock_rollback();test_requests();test_resume_and_power();test_data_errors();test_command_counters();
 puts("MSDC_CLOCK_LIFETIME_AND_DATA_ERRORS_PASS");
#endif
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="msdc-clock-host-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(code)
    compiler = shlex.split(os.environ.get("HOSTCC", "cc"))
    subprocess.run([*compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                    "-O1", "-g", "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    *shlex.split(os.environ.get("HOSTCFLAGS", "")),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    print("MSDC_NATIVE_CALLBACK_BUILD_PASS", flush=True)
    subprocess.run([str(path / "test")], check=True)
