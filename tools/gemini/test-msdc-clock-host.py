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
reset_updated = bool(re.search(r"^static int msdc_reset_hw", source, re.M))
code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t u8;
#define __iomem
#define BIT(n) (1U << (n))
#define HZ 100
#define FIELD_PREP(m,v) (((u32)(v) << __builtin_ctz(m)) & (m))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
static void debug_nop(void *d,const char *fmt,...) {(void)d;(void)fmt;}
#define dev_dbg(...) debug_nop(__VA_ARGS__)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define WARN_ON(v) assert(!(v))
#define spin_lock_irqsave(l,f) do {(void)(l); (f)=0;} while (0)
#define spin_unlock_irqrestore(l,f) do {(void)(l); (void)(f);} while (0)
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define MMC_RSP_PRESENT BIT(0)
#define MMC_RSP_136 BIT(1)
#define MMC_RSP_R1B_NO_CRC 99
#define MMC_CAP2_NO_SDIO BIT(0)
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
                 unsigned flags; void *sg; struct mmc_command *stop; };
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
typedef int wait_queue_head_t;
struct cqhci_host { bool enabled; int quiesce_error,task_error; };
struct mmc_host { struct msdc_host *priv; unsigned actual_clock, err_stats[15];
 unsigned caps2;
 struct mmc_ios ios; struct {void *vmmc,*vqmmc;} supply;
 struct mmc_card *card; const struct mmc_cqe_ops *cqe_ops; };
struct msdc_host {
 struct device *dev; const struct mtk_mmc_compatible *dev_comp; void *base,*top_base;
 struct clk *h_clk,*bus_clk,*src_clk,*src_clk_cg,*crypto_clk;
 struct clk_bulk_data bulk_clks[MSDC_NR_CLOCKS];
 bool clocks_on,src_clk_cg_on,needs_hw_init,hsq_en,vqmmc_enabled,hs400_tuning;
 bool dma_active,reset_done,recovery_pending,recovery_running,recovery_attempted,internal_cd;
 int reset_error,quiesce_wait;
 struct mmc_request *recovery_mrq; struct cqhci_host *cq_host;
 int clock_error,error,lock,irq; unsigned mclk,src_clk_freq,timeout_ns,timeout_clks,hs400_cmd_int_delay;
 unsigned char timing;
 struct mmc_cqe_ops cqe_ops; const struct mmc_cqe_ops *cqe_delegate;
 struct msdc_save_para save_para; struct msdc_tune_para def_tune_para,saved_tune_para;
 struct mmc_request *mrq; struct mmc_command *cmd; struct mmc_data *data;
 int dma; struct delayed_work req_timeout; void *pinctrl,*pins_eint,*pins_uhs,*reset;
 unsigned tuning_step;
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
static unsigned delegate_enables, delegate_requests, buswidths, init_fail_poll, scheduled, wakeups, waits, sync_cancel, quiesce_calls;
static int enable_call, fail_enable, fail_enable2, delegate_error;
static bool forbidden_io, sdio, queued, hsq_suspended, terminal_order;
static bool stuck_reset,stuck_fifo,stuck_stop,stuck_dma,reset_revalidate;
static int reset_assert_error,reset_deassert_error,quiesce_result;
static jmp_buf blocked;
static struct mmc_host *mmc_from_priv(struct msdc_host *h) {assert(h==&host); return &mmc;}
static struct msdc_host *mmc_priv(struct mmc_host *m) {return m->priv;}
static struct mmc_host *dev_get_drvdata(struct device *d) {return d->data;}
static void mmc_debugfs_err_stats_inc(struct mmc_host *m,int stat) {m->err_stats[stat]++;}
static u32 readl(void *p) {
 assert(!forbidden_io); io++;
 u32 val=*(u32 *)p;
 if(p==(char *)host.base+MSDC_CFG) {
  val|=MSDC_CFG_CKSTB;
  if(!stuck_reset)val&=~MSDC_CFG_RST;
 }
 if(p==(char *)host.base+MSDC_FIFOCS && !stuck_fifo)val&=~MSDC_FIFOCS_CLR;
 if(p==(char *)host.base+MSDC_DMA_CTRL && !stuck_stop)val&=~MSDC_DMA_CTRL_STOP;
 if(p==(char *)host.base+MSDC_DMA_CFG) {
  if(stuck_dma)val|=MSDC_DMA_CFG_STS;
  else val&=~MSDC_DMA_CFG_STS;
 }
 if(init_fail_poll && resets && p==(char *)host.base+compat.pad_tune_reg)
  fail_poll=poll_calls+1;
 *(u32 *)p=val;
 return val;
}
static void writel(u32 val,void *p) {
 assert(!forbidden_io); io++;
 if(p==(char *)host.base+MSDC_CFG) {
  val=(val&~drop_mask)|(*(u32 *)p&drop_mask); cfg_generation++;
  if(val&MSDC_CFG_RST)resets++;
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
 (void)m;(void)r;if(!queued)return false;
 assert(!host.dma_active);if(terminal_order)assert(!host.mrq && (host.reset_done || host.reset_error));queued=false;finalized++;return true;
}
static void mmc_request_done(struct mmc_host *m,struct mmc_request *r) {(void)m;(void)r;if(terminal_order)assert(!host.mrq && !host.dma_active && (host.reset_done || host.reset_error));completions++;}
static bool mmc_card_mmc(struct mmc_card *c) {(void)c;return true;}
static bool mmc_op_tuning(unsigned op) {return op==19;}
static void msdc_start_command(struct msdc_host *h,struct mmc_request *r,struct mmc_command *c) {
 (void)h;(void)r;(void)c;commands++;
}
static void msdc_cmd_next(struct msdc_host *h,struct mmc_request *r,struct mmc_command *c) {
 (void)h;(void)r;(void)c;nexts++;
}
static void msdc_data_xfer_next(struct msdc_host *h,struct mmc_request *r) {(void)h;(void)r;nexts++;}
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
static int reset_control_assert(void *r) {(void)r;return reset_assert_error;}
static int reset_control_deassert(void *r) {(void)r;return reset_deassert_error;}
static void usleep_range(int a,int b) {(void)a;(void)b;}
static void mod_delayed_work(void *w,struct delayed_work *d,unsigned t) {
 (void)w;(void)d;(void)t;scheduled++;
}
static void cancel_delayed_work(struct delayed_work *d) {(void)d;}
static void cancel_delayed_work_sync(struct delayed_work *d) {(void)d;sync_cancel++;}
static void wake_up_all(int *w) {(void)w;wakeups++;}
#define wait_event(q,condition) do {(void)(q); if(!(condition)) {waits++;longjmp(blocked,1);}} while(0)
static void msdc_dma_setup(struct msdc_host *h,int *d,struct mmc_data *data) {(void)h;(void)d;(void)data;}
static bool mmc_op_multi(unsigned op) {(void)op;return true;}
static bool msdc_cmd_is_ready(struct msdc_host *h,struct mmc_request *r,struct mmc_command *c) {(void)h;(void)r;(void)c;return true;}
static u32 msdc_cmd_prepare_raw_cmd(struct msdc_host *h,struct mmc_request *r,struct mmc_command *c) {(void)h;(void)r;(void)c;commands++;return 1;}
#define MMC_DATA_READ 1
static void *system_percpu_wq;
static int cqhci_deactivate(struct mmc_host *m) {(void)m;quiesce_calls++;if(!quiesce_result && host.cq_host)host.cq_host->task_error=0;return quiesce_result;}
static void msdc_track_cmd_data(struct msdc_host *h,struct mmc_command *c) {(void)h;(void)c;}
static void msdc_recheck_sdio_irq(struct msdc_host *m) {(void)m;}
'''
code += "#define UPDATED " + str(int(updated)) + "\n"
code += "#define RESET_UPDATED " + str(int(reset_updated)) + "\n"
names = ["sdr_set_bits", "sdr_clr_bits", "sdr_set_field", "sdr_get_field", "msdc_reset_hw", "msdc_stop_dma",
         "msdc_defer_request", "msdc_prepare_data", "msdc_data_prepared",
         "msdc_unprepare_data", "msdc_gate_clock", "msdc_clock_mask", "msdc_wait_clock", "msdc_ungate_clock",
         "msdc_clock_ready", "msdc_apply_clock", "msdc_set_mclk", "msdc_auto_cmd_done", "msdc_cmd_done",
         "msdc_ops_request", "msdc_pre_req", "msdc_post_req", "msdc_ops_cqe_enable", "msdc_ops_cqe_request",
         "msdc_install_cqe_ops", "msdc_request_done", "msdc_data_xfer_done", "msdc_request_timeout", "msdc_init_hw", "msdc_set_ios_clock",
         "msdc_ops_set_ios", "msdc_cqe_quiesce", "msdc_wait_for_quiescence", "msdc_save_reg", "msdc_restore_reg", "msdc_runtime_suspend", "msdc_runtime_resume"]
optional = {"msdc_clock_mask", "msdc_wait_clock", "msdc_clock_ready", "msdc_apply_clock",
            "msdc_ops_cqe_enable", "msdc_ops_cqe_request", "msdc_install_cqe_ops", "msdc_set_ios_clock",
            "msdc_stop_dma", "msdc_defer_request", "msdc_cqe_quiesce", "msdc_wait_for_quiescence"}
for name in names:
    code += function(name, name in optional)
if reset_updated:
    for name in ("msdc_start_data", "msdc_start_command", "msdc_cmd_next", "msdc_data_xfer_next"):
        body = function(name)
        for called in ("msdc_start_data", "msdc_start_command", "msdc_cmd_next", "msdc_data_xfer_next"):
            body = re.sub(r"\b" + called + r"\b", "native_" + called, body)
        code += body
code += r'''
static void reset_case(int bits,bool acquired) {
 memset(&mmc,0,sizeof(mmc));memset(&host,0,sizeof(host));memset(&dev,0,sizeof(dev));
 memset(&compat,0,sizeof(compat));memset(clocks,0,sizeof(clocks));memset(regs,0,sizeof(regs));
 io=maps=unmaps=commands=completions=finalized=resets=tunes=0;
 poll_calls=unbounded=fail_poll=fail_poll2=stuck_mask=drop_mask=0;
 cfg_generation=accepted_generation=unmasked=hsq_resumes=irq_enables=nexts=0;
 delegate_enables=delegate_requests=buswidths=init_fail_poll=0;
 scheduled=wakeups=waits=sync_cancel=quiesce_calls=0;
 enable_call=fail_enable=fail_enable2=delegate_error=0;
 forbidden_io=sdio=queued=hsq_suspended=terminal_order=false;
 stuck_reset=stuck_fifo=stuck_stop=stuck_dma=reset_revalidate=false;
 reset_assert_error=reset_deassert_error=quiesce_result=0;
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

static void test_bounded_reset(void) {
 reset_case(8,true);stuck_reset=true;
#if RESET_UPDATED
 assert(msdc_reset_hw(&host)==-ETIMEDOUT);
#else
 msdc_reset_hw(&host);
#endif
 assert(unbounded==0); /* Old callback compiles, then rejects this actual failed-reset path. */
#if RESET_UPDATED
 assert(host.reset_error==-ETIMEDOUT && host.needs_hw_init && !host.reset_done);
 assert(mmc.err_stats[MMC_ERR_CTRL_TIMEOUT]==1);
 reset_case(8,true);stuck_fifo=true;
 assert(msdc_reset_hw(&host)==-ETIMEDOUT && !host.reset_done && unbounded==0);
 reset_case(8,true);host.reset=&dev;reset_assert_error=-EACCES;
 assert(msdc_init_hw(&host)==-EACCES && !resets && host.needs_hw_init);
 reset_assert_error=0;reset_deassert_error=-EIO;
 assert(msdc_init_hw(&host)==-EIO && !resets);
 reset_deassert_error=0;assert(msdc_init_hw(&host)==0 && !host.reset_error);
#endif
 puts("MSDC_BOUNDED_RESET_PASS");
}
#if RESET_UPDATED
static void expect_retained_teardown(void) {
 if(!setjmp(blocked)) {msdc_wait_for_quiescence(&host);assert(!"unsafe teardown returned");}
}
static void test_idle_zero_clock_teardown(void) {
 /* Card shutdown powers off before the platform shutdown callback. */
 reset_case(8,true);
 struct mmc_ios ios={.clock=0,.power_mode=MMC_POWER_OFF};
 msdc_ops_set_ios(&mmc,&ios);
 assert(!host.mclk && !mmc.actual_clock && !(regs[MSDC_CFG/4]&MSDC_CFG_CKPDN));
 unsigned writes=io,polls=poll_calls;
 msdc_wait_for_quiescence(&host);
 assert(io==writes && poll_calls==polls && !scheduled && sync_cancel==1);
 assert(!waits && !quiesce_calls);assert_leases(1);
 /* A missing source lease cannot turn proved idle into a new fatal owner. */
 clk_disable_unprepare(host.src_clk_cg);host.src_clk_cg_on=false;
 forbidden_io=true;msdc_wait_for_quiescence(&host);
 assert(io==writes && !scheduled && sync_cancel==2 && !waits);
 forbidden_io=false;msdc_gate_clock(&host);assert_leases(0);
 forbidden_io=true;msdc_wait_for_quiescence(&host);
 assert(io==writes && !scheduled && sync_cancel==3 && !waits);
 /* Enabled CQE still needs the checked native proof even without a legacy request. */
 reset_case(8,true);struct cqhci_host cq={.enabled=true};host.cq_host=&cq;
 forbidden_io=true;msdc_wait_for_quiescence(&host);
 assert(quiesce_calls==1 && sync_cancel==1 && !waits && !scheduled && !io);
 forbidden_io=false;msdc_gate_clock(&host);assert_leases(0);
 puts("MSDC_IDLE_ZERO_CLOCK_TEARDOWN_PASS");
}
static void test_retained_dma(void) {
 for(int stop=0;stop<2;stop++)for(int async=0;async<2;async++) {
  reset_case(8,true);struct mmc_command cmd={0};
  struct mmc_data data={.sg_len=1,.blocks=2,.blksz=512};
  struct mmc_request request={.cmd=&cmd,.data=&data};
  host.mrq=&request;host.data=&data;host.dma_active=true;host.hsq_en=queued=async;
  if(async)msdc_pre_req(&mmc,&request);else msdc_prepare_data(&host,&data);
  stuck_stop=stop;stuck_dma=!stop;
  msdc_data_xfer_done(&host,MSDC_INT_XFER_COMPL,&request,&data);
  assert(data.error==-ETIMEDOUT && !data.bytes_xfered && !host.data && !host.cmd);
  assert(host.mrq==&request && host.recovery_mrq==&request && host.recovery_pending && host.dma_active);
  assert(!completions && !finalized && !unmaps && scheduled==1 && maps==1 && !regs[MSDC_INTEN/4]);
  unsigned writes=io;msdc_data_xfer_done(&host,MSDC_INT_XFER_COMPL,&request,&data);
  assert(io==writes && scheduled==1);msdc_post_req(&mmc,&request,0);assert(!unmaps);
  assert(msdc_runtime_suspend(&dev)==-EBUSY && msdc_runtime_resume(&dev)==-EBUSY);assert_leases(1);
  struct mmc_ios ios={.clock=52000000,.timing=MMC_TIMING_MMC_HS,.power_mode=MMC_POWER_ON};
  msdc_ops_set_ios(&mmc,&ios);assert(host.reset_error==-ETIMEDOUT && !resets && io==writes);
  assert(set_clock(MMC_TIMING_MMC_HS,52000000)==-EBUSY && io==writes);
  /* The single existing watchdog attempt fails without releasing anything. */
  msdc_request_timeout(&host.req_timeout.work);
  assert(host.recovery_attempted && !host.recovery_pending && host.recovery_mrq==&request);
  assert(!completions && !finalized && !unmaps && scheduled==1);assert_leases(1);
  writes=io;msdc_request_timeout(&host.req_timeout.work);assert(io==writes && scheduled==1);
  expect_retained_teardown();
  assert(waits==1 && !sync_cancel && !unmaps && !completions && scheduled==1);assert_leases(1);
  /* A later explicit proof can release ownership; no periodic retry service. */
  stuck_dma=stuck_stop=false;host.recovery_pending=true;terminal_order=true;
  msdc_request_timeout(&host.req_timeout.work);
  assert(!host.dma_active && !host.mrq && !host.recovery_mrq && !host.recovery_pending);
  assert(completions==(unsigned)!async && finalized==(unsigned)async);
  assert(host.reset_error==-ETIMEDOUT && host.needs_hw_init); /* Idle != healthy. */
  if(async)assert(!unmaps);else assert(unmaps==1);
  msdc_post_req(&mmc,&request,0);assert(unmaps==1 && !data.host_cookie);
  msdc_post_req(&mmc,&request,0);assert(unmaps==1);
  msdc_wait_for_quiescence(&host);assert(sync_cancel==1);
  terminal_order=false;assert(msdc_set_ios_clock(&host,&ios)==0 && !host.reset_error && !host.needs_hw_init);
  msdc_gate_clock(&host);assert_leases(0);
 }
 /* Even a healthy active DMA owner cannot be reset by POWER_UP/set_ios. */
 reset_case(8,true);host.dma_active=true;mmc.supply.vmmc=&dev;
 struct mmc_ios active_ios={.clock=400000,.power_mode=MMC_POWER_UP};
 forbidden_io=true;msdc_ops_set_ios(&mmc,&active_ios);assert(!resets && !buswidths && !io);
 forbidden_io=false;
 /* Successful first watchdog recovery completes once, after reset/idle proof. */
 reset_case(8,true);struct mmc_command cmd={0};struct mmc_data data={.sg_len=1};
 struct mmc_request request={.cmd=&cmd,.data=&data};host.mrq=&request;host.data=&data;host.dma_active=true;
 msdc_prepare_data(&host,&data);stuck_dma=true;
 msdc_data_xfer_done(&host,MSDC_INT_DATTMO|MSDC_INT_DMA_PROTECT,&request,&data);
 assert(mmc.err_stats[MMC_ERR_DAT_TIMEOUT]==1 && mmc.err_stats[MMC_ERR_ADMA]==1);
 stuck_dma=false;terminal_order=true;
 msdc_request_timeout(&host.req_timeout.work);
 assert(completions==1 && unmaps==1 && !host.mrq && !host.recovery_pending);
 unsigned writes=io;msdc_request_timeout(&host.req_timeout.work);
 assert(io==writes && completions==1 && unmaps==1);
 msdc_data_xfer_done(&host,MSDC_INT_DATTMO,&request,&data);
 assert(io==writes && completions==1 && unmaps==1);
 msdc_gate_clock(&host);assert_leases(0);
}
static void test_command_reset_and_cqe_owner(void) {
 reset_case(8,true);struct mmc_command cmd={0};struct mmc_request request={.cmd=&cmd};
 host.mrq=&request;host.error=REQ_CMD_EIO;stuck_reset=true;terminal_order=true;
 msdc_request_done(&host,&request);
 assert(completions==1 && cmd.error==-ETIMEDOUT && !host.mrq && host.needs_hw_init && unbounded==0);
 forbidden_io=true;assert(msdc_clock_ready(&host)==-ETIMEDOUT);
 forbidden_io=false;terminal_order=false;msdc_gate_clock(&host);assert_leases(0);
 /* Failed pre-command FIFO reset must not issue ARG/CMD or start data. */
 reset_case(8,true);memset(&cmd,0,sizeof(cmd));host.mrq=&request;
 regs[MSDC_FIFOCS/4]=1;stuck_fifo=true;terminal_order=true;
 native_msdc_start_command(&host,&request,&cmd);
 assert(!commands && completions==1 && cmd.error==-ETIMEDOUT && !host.mrq);
 assert(!regs[SDC_ARG/4] && !regs[SDC_CMD/4]);
 /* Reset failure prevents following SBC/STOP or DMA producer. */
 reset_case(8,true);host.reset_error=-EIO;host.mrq=&request;cmd.error=0;
 native_msdc_cmd_next(&host,&request,&cmd);assert(completions==1 && !commands && !host.dma_active);
 host.mrq=&request;native_msdc_data_xfer_next(&host,&request);assert(completions==2 && !commands);
 /* The existing tuning response-CRC exception must not reset arriving data. */
 reset_case(8,true);cmd.opcode=19;cmd.error=0;host.mrq=&request;host.cmd=&cmd;
 assert(msdc_cmd_done(&host,MSDC_INT_RSPCRCERR,&request,&cmd));
 assert(!resets && cmd.error==-EILSEQ);
 struct mmc_data tuning_data={.flags=MMC_DATA_READ};cmd.data=&tuning_data;
 native_msdc_cmd_next(&host,&request,&cmd);assert(host.data==&tuning_data && host.dma_active && !resets);
 cmd.data=NULL;
 /* Native CQE qcnt may be zero: STOP/STS still owns descriptor lifetime. */
 reset_case(8,true);stuck_dma=true;
 assert(msdc_cqe_quiesce(&mmc,false,0)==-ETIMEDOUT && host.dma_active && host.recovery_pending);
 expect_retained_teardown();
 assert(waits==1 && !sync_cancel);assert_leases(1);
 msdc_request_timeout(&host.req_timeout.work);assert(scheduled==1 && host.recovery_attempted);
 stuck_dma=false;assert(msdc_cqe_quiesce(&mmc,true,0)==0 && !host.dma_active && host.reset_error);
 assert(host.needs_hw_init);msdc_wait_for_quiescence(&host);assert(sync_cancel==1);
 msdc_gate_clock(&host);assert_leases(0);
 /* Rejected CQHCI producer disable schedules native recovery without MMIO. */
 reset_case(8,true);struct cqhci_host cq={.enabled=true,.quiesce_error=-EIO};host.cq_host=&cq;
 assert(msdc_cqe_quiesce(&mmc,false,-EIO)==-EIO);forbidden_io=true;
 assert(host.dma_active && host.recovery_pending && scheduled==1 && io==1 && !resets && !regs[MSDC_DMA_CTRL/4]);
 assert(msdc_runtime_resume(&dev)==-EBUSY && msdc_runtime_suspend(&dev)==-EBUSY);assert_leases(1);
 quiesce_result=-EIO;msdc_request_timeout(&host.req_timeout.work);
 assert(host.recovery_attempted && !host.recovery_pending && host.dma_active && io==1 && scheduled==1);
 msdc_request_timeout(&host.req_timeout.work);assert(io==1 && scheduled==1);
 expect_retained_teardown();assert(waits==1 && !sync_cancel);assert_leases(1);
 forbidden_io=false;cq.quiesce_error=0;assert(msdc_cqe_quiesce(&mmc,true,0)==0);
 quiesce_result=0;msdc_wait_for_quiescence(&host);assert(sync_cancel==1);
 msdc_gate_clock(&host);assert_leases(0);
 /* Only verified foreground initialization + accepted task clear clears that latch. */
 reset_case(8,true);cq=(struct cqhci_host){.enabled=true,.task_error=-EIO};host.cq_host=&cq;
 assert(msdc_clock_ready(&host));quiesce_result=-EIO;
 struct mmc_ios ios={.clock=400000,.power_mode=MMC_POWER_ON};
 msdc_ops_set_ios(&mmc,&ios);assert(host.needs_hw_init && cq.task_error==-EIO && msdc_clock_ready(&host));
 quiesce_result=0;msdc_ops_set_ios(&mmc,&ios);
 assert(!host.needs_hw_init && !cq.task_error && !host.reset_error && !msdc_clock_ready(&host));
 msdc_gate_clock(&host);assert_leases(0);
 reset_case(8,false);cq=(struct cqhci_host){.enabled=true,.task_error=-EIO};host.cq_host=&cq;
 quiesce_result=-EIO;assert(msdc_runtime_resume(&dev)==-EIO && cq.task_error && !host.clocks_on);assert_leases(0);
 quiesce_result=0;assert(msdc_runtime_resume(&dev)==0 && !cq.task_error && !host.needs_hw_init);
 msdc_gate_clock(&host);assert_leases(0);
 puts("MSDC_RECOVERY_MAPPING_AND_TEARDOWN_PASS");
}
#endif
int main(void) {
 (void)blocked;(void)system_percpu_wq;
 test_bounded_reset();test_write_rejection();
#if UPDATED
 test_acquire();test_clock_rollback();test_requests();test_resume_and_power();test_data_errors();test_command_counters();
 puts("MSDC_CLOCK_LIFETIME_AND_DATA_ERRORS_PASS");
#if RESET_UPDATED
 test_idle_zero_clock_teardown();test_retained_dma();test_command_reset_and_cqe_owner();
#endif
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

# Shared CQHCI owns slots/descriptors; the optional callback supplies native proof.
cq_source = (root / "drivers/mmc/host/cqhci-core.c").read_text()
cq_header = (root / "drivers/mmc/host/cqhci.h").read_text()
checked_cqe = "static int cqhci_quiesce(" in cq_source
cq_code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define pr_err(...) ((void)0)
#define pr_debug(...) ((void)0)
#define pr_warn(...) ((void)0)
#define WARN_ON(v) assert(!(v))
#define EXPORT_SYMBOL(x)
#define READ_ONCE(x) (x)
static void acquire_lock(int *l);
static void release_lock(int *l);
#define spin_lock_irqsave(l,f) do {acquire_lock(l);(f)=0;} while(0)
#define spin_unlock_irqrestore(l,f) do {release_lock(l);(void)(f);} while(0)
#define wmb() ((void)0)
#define MMC_DATA_READ 1
#define DMA_FROM_DEVICE 0
#define DMA_TO_DEVICE 1
'''
for body in (cq_header, cq_source):
    lines = iter(body.splitlines())
    for line in lines:
        if line.startswith("#define"):
            definition = line
            while line.endswith("\\"):
                line = next(lines)
                definition += "\n" + line
            cq_code += definition + "\n"
cq_code += r'''
struct mmc_host;
struct cqhci_host;
struct mmc_data {int error;unsigned bytes_xfered,flags,sg_len;void *sg;};
struct mmc_command {int error;unsigned arg;};
struct mmc_request {struct mmc_command *cmd;struct mmc_data *data;unsigned tag;};
struct mmc_card {struct {bool cmdq_en;}ext_csd;unsigned rca;};
struct cqhci_slot {struct mmc_request *mrq;unsigned flags;};
struct cqhci_host_ops {void (*disable)(struct mmc_host *,bool);
 void (*pre_enable)(struct mmc_host *);void (*post_disable)(struct mmc_host *);
 void (*enable)(struct mmc_host *);int (*quiesce)(struct mmc_host *,bool,int);};
struct mmc_host {void *cqe_private;bool cqe_on;};
struct cqhci_host {struct mmc_host *mmc;struct cqhci_host_ops *ops;
 bool enabled,activated,recovery_halt,quiesce_halt;int quiesce_error,task_error,lock,wait_queue;
 unsigned rca;int num_slots,qcnt;struct cqhci_slot slot[32];
 size_t data_size,desc_size;void *trans_desc_base,*desc_base;
 uintptr_t trans_desc_dma_base,desc_dma_base;};
static struct mmc_host mmc;
static struct cqhci_host host;
static struct cqhci_host_ops ops;
static u32 regs[128];
static unsigned frees,done,unmaps,maps,disable_calls,post_calls,quiesce_calls,waits,doorbells,builds,wakes,unsafe_enables;
static bool native_active,drop_disable,halt_ok,clear_ok,async_proof,inject_recovery,inject_enable;
static int quiesce_result;
static jmp_buf blocked;
static int cqhci_deactivate(struct mmc_host *m);
static void cqhci_recovery_start(struct mmc_host *m);
static void acquire_lock(int *l) {
 if(inject_recovery){inject_recovery=false;host.recovery_halt=true;cqhci_recovery_start(host.mmc);}
 assert(!*l);*l=1;
}
static void release_lock(int *l) {
 assert(*l==1);*l=0;
 if(inject_enable){inject_enable=false;host.recovery_halt=true;cqhci_recovery_start(host.mmc);}
}
static void cqhci_post_req(struct mmc_host *m,struct mmc_request *r);
static u32 cqhci_readl(struct cqhci_host *h,unsigned reg) {(void)h;return regs[reg/4];}
static void cqhci_writel(struct cqhci_host *h,u32 v,unsigned reg) {
 (void)h;if(reg==CQHCI_CFG && drop_disable)v|=CQHCI_ENABLE;
 if(reg==CQHCI_TDBR){assert(!host.recovery_halt && !host.quiesce_error);doorbells++;native_active=true;}
 regs[reg/4]=v;
}
static void __cqhci_enable(struct cqhci_host *h) {
 if(inject_enable && !h->lock){inject_enable=false;host.recovery_halt=true;cqhci_recovery_start(host.mmc);}
 if(h->ops->quiesce && h->recovery_halt)unsafe_enables++;
 builds++;h->activated=true;regs[CQHCI_CFG/4]|=CQHCI_ENABLE;
}
static int cqhci_host_alloc_tdl(struct cqhci_host *h) {(void)h;return 0;}
static bool cqhci_halted(struct cqhci_host *h) {(void)h;return halt_ok;}
static bool cqhci_halt(struct mmc_host *m,unsigned t) {(void)m;(void)t;return halt_ok;}
static bool cqhci_clear_all_tasks(struct mmc_host *m,unsigned t) {(void)m;(void)t;return clear_ok;}
static void cqhci_set_irqs(struct cqhci_host *h,unsigned mask) {(void)h;(void)mask;}
static void *mmc_dev(struct mmc_host *m) {return m;}
static void dmam_free_coherent(void *d,size_t s,void *p,uintptr_t a) {
 (void)d;(void)s;(void)p;(void)a;assert(!native_active);frees++;
}
static void dma_unmap_sg(void *d,void *sg,unsigned n,int dir) {(void)d;(void)sg;(void)n;(void)dir;assert(!native_active);unmaps++;}
static void mmc_cqe_request_done(struct mmc_host *m,struct mmc_request *r) {
 assert(!native_active && host.recovery_halt);done++;cqhci_post_req(m,r);
}
static void cqhci_prep_task_desc(struct mmc_request *r,struct cqhci_host *h,int t) {(void)r;(void)h;(void)t;}
static int cqhci_prep_tran_desc(struct mmc_request *r,struct cqhci_host *h,int t) {(void)r;(void)h;(void)t;maps++;return 0;}
static void cqhci_prep_dcmd_desc(struct mmc_host *m,struct mmc_request *r) {(void)m;(void)r;}
static void wake_up_all(int *w) {(void)w;wakes++;}
#define readx_poll_timeout(fn,arg,val,cond,delay,timeout) ({(void)(delay);(void)(timeout);(val)=fn(arg);(cond)?0:-ETIMEDOUT;})
static int native_quiesce(struct mmc_host *m,bool recovery,int producer_error) {
 (void)m;(void)recovery;quiesce_calls++;
 if(producer_error){assert(native_active);return producer_error;}
 assert(!(regs[CQHCI_CFG/4]&CQHCI_ENABLE));assert(host.recovery_halt && !host.lock);
 if(quiesce_result)return quiesce_result;
 native_active=false;return 0;
}
static void native_disable(struct mmc_host *m,bool r) {(void)m;(void)r;disable_calls++;}
static void native_post(struct mmc_host *m) {(void)m;assert(!native_active);post_calls++;}
/* Execute asynchronous native proof at the real wait boundary, or retain owner. */
#define wait_event(q,condition) do {(void)(q);if(!(condition)){waits++;if(async_proof){quiesce_result=0;assert(cqhci_deactivate(host.mmc)==0);}else longjmp(blocked,1);}assert(condition);}while(0)
'''
for name in ("__cqhci_disable", "cqhci_quiesce", "cqhci_deactivate", "cqhci_enable", "cqhci_read_ctl", "cqhci_off", "cqhci_disable",
             "cqhci_post_req", "cqhci_tag", "cqhci_request", "cqhci_recovery_start", "cqhci_error_from_flags",
             "cqhci_recover_mrq", "cqhci_recover_mrqs", "cqhci_recovery_finish"):
    match = re.search(r"^(?:static )?(?:inline )?[^;\n]*\b" + name + r"\([^;]*?\)\n\{.*?^}\n", cq_source, re.M | re.S)
    if not match:
        if name == "cqhci_quiesce":
            continue
        raise RuntimeError("Missing CQHCI callback: " + name)
    body = match.group(0)
    if name == "cqhci_deactivate":
        body = "static " + body
    cq_code += body
cq_code += "#define CHECKED_CQE " + str(int(checked_cqe)) + "\n"
cq_code += r'''
static void reset_owner(bool checked) {
 memset(&mmc,0,sizeof(mmc));memset(&host,0,sizeof(host));memset(&ops,0,sizeof(ops));memset(regs,0,sizeof(regs));
 frees=done=unmaps=maps=disable_calls=post_calls=quiesce_calls=waits=doorbells=builds=wakes=unsafe_enables=0;
 native_active=drop_disable=async_proof=inject_recovery=inject_enable=false;halt_ok=clear_ok=true;quiesce_result=0;
 mmc.cqe_private=&host;mmc.cqe_on=true;host.mmc=&mmc;host.ops=&ops;
 host.enabled=host.activated=true;host.num_slots=32;
 host.desc_base=host.trans_desc_base=&host;regs[CQHCI_CFG/4]=CQHCI_ENABLE;
 ops.disable=native_disable;ops.post_disable=native_post;
 if(checked)ops.quiesce=native_quiesce;
}
static void expect_recovery_retained(void) {
 if(!setjmp(blocked)){cqhci_recovery_finish(&mmc);assert(!"unsafe CQE recovery published");}
}
int main(void) {
 /* This pre-fix callback compiles, then fails to reject unsafe native ownership. */
 reset_owner(true);native_active=true;quiesce_result=-ETIMEDOUT;
 assert(cqhci_deactivate(&mmc)==-ETIMEDOUT);
#if CHECKED_CQE
 assert(host.quiesce_error==-ETIMEDOUT && host.recovery_halt && !host.activated && !frees);
 cqhci_disable(&mmc);assert(host.enabled && !frees && host.desc_base && !post_calls);
 quiesce_result=0;assert(cqhci_deactivate(&mmc)==0 && !host.quiesce_error && !host.recovery_halt);
 cqhci_disable(&mmc);assert(!host.enabled && frees==2 && !host.desc_base && !host.trans_desc_base);
 /* Dropped generic CQHCI disable write cannot be mistaken for native proof. */
 reset_owner(true);native_active=true;drop_disable=true;
 assert(cqhci_deactivate(&mmc)==-EIO && quiesce_calls==1 && native_active);
 cqhci_disable(&mmc);assert(!frees && host.enabled && !post_calls);
 drop_disable=false;assert(cqhci_deactivate(&mmc)==0 && !native_active);cqhci_disable(&mmc);assert(frees==2);
 /* Real quiesce interposed at enable: old path could enable after proven stop. */
 reset_owner(true);host.activated=false;regs[CQHCI_CFG/4]=0;inject_enable=true;
 struct mmc_data enable_data={.sg_len=1};struct mmc_request enable_request={.data=&enable_data};
 assert(cqhci_request(&mmc,&enable_request)==-EBUSY);
 assert(unsafe_enables==0 && !(regs[CQHCI_CFG/4]&CQHCI_ENABLE) && !doorbells && unmaps==1);
 /* A request paused before enable cannot re-enable after recovery latches. */
 reset_owner(true);host.activated=false;regs[CQHCI_CFG/4]=0;
 quiesce_result=-ETIMEDOUT;inject_recovery=true;
 struct mmc_data race_data={.sg_len=1};struct mmc_request race_request={.data=&race_data};
 assert(cqhci_request(&mmc,&race_request)==-ETIMEDOUT);
 assert(!builds && !doorbells && !maps && host.recovery_halt);
 quiesce_result=0;assert(cqhci_deactivate(&mmc)==0 && !native_active);
 assert(cqhci_request(&mmc,&race_request)==-EBUSY && !builds && !doorbells);
 /* Conversely an already enabled/admitted request precedes disable and stays held. */
 reset_owner(true);host.activated=false;regs[CQHCI_CFG/4]=0;
 assert(cqhci_request(&mmc,&race_request)==0 && builds==1 && doorbells==1);
 host.recovery_halt=true;quiesce_result=-ETIMEDOUT;cqhci_recovery_start(&mmc);
 assert(host.qcnt==1 && !done && !unmaps);async_proof=true;
 cqhci_recovery_finish(&mmc);assert(done==1 && unmaps==1 && !native_active);
 /* A rejected disable remains latched through a rejected retry, even qcnt==0. */
 reset_owner(true);native_active=true;drop_disable=true;
 assert(cqhci_deactivate(&mmc)==-EIO);cqhci_disable(&mmc);
 assert(native_active && host.qcnt==0 && host.quiesce_error==-EIO && !frees && !done);
 assert(cqhci_deactivate(&mmc)==-EIO && !frees && host.enabled);
 drop_disable=false;assert(cqhci_deactivate(&mmc)==0 && !host.recovery_halt);
 cqhci_disable(&mmc);assert(frees==2 && !host.enabled);
 /* Held slots are completed only after the asynchronous native inactivity proof. */
 reset_owner(true);native_active=true;quiesce_result=-ETIMEDOUT;host.recovery_halt=true;
 struct mmc_data data={.sg_len=1,.bytes_xfered=1024};struct mmc_request request={.data=&data};
 host.slot[0].mrq=&request;host.slot[0].flags=CQHCI_HOST_TIMEOUT;host.qcnt=1;
 cqhci_recovery_start(&mmc);assert(host.quiesce_error && native_active);
 expect_recovery_retained();assert(waits==1 && host.qcnt==1 && !done && !unmaps && !frees);
 unsigned before=doorbells;assert(cqhci_request(&mmc,&request)==-ETIMEDOUT && doorbells==before && !maps);
 async_proof=true;cqhci_recovery_finish(&mmc);
 assert(!native_active && host.qcnt==0 && done==1 && unmaps==1 && !host.slot[0].mrq);
 assert(data.error==-ETIMEDOUT && !data.bytes_xfered && !host.activated && !host.recovery_halt);
 host.recovery_halt=true;cqhci_recovery_finish(&mmc);assert(done==1 && unmaps==1); /* Duplicate recovery. */
 assert(cqhci_request(&mmc,&request)==0 && doorbells==before+1 && builds==1);
 host.recovery_halt=true;native_active=false;cqhci_recovery_finish(&mmc);assert(done==2 && unmaps==2);
 /* Failure to clear task state blocks restart even after DMA can no longer write. */
 reset_owner(true);host.recovery_halt=true;clear_ok=false;cqhci_recovery_finish(&mmc);
 assert(host.task_error==-EIO && host.recovery_halt);
 assert(cqhci_request(&mmc,&request)==-EIO && !doorbells && !maps);
 struct mmc_card card={.ext_csd.cmdq_en=true};assert(cqhci_enable(&mmc,&card)==-EIO);
 assert(cqhci_deactivate(&mmc)==-EIO && host.task_error && host.recovery_halt);
 clear_ok=true;assert(cqhci_deactivate(&mmc)==0 && !host.task_error && !host.recovery_halt);
 /* Rejected producer clear plus one accepted async retry releases slots once. */
 reset_owner(true);native_active=true;drop_disable=true;host.recovery_halt=true;
 host.slot[0].mrq=&request;host.slot[0].flags=CQHCI_HOST_TIMEOUT;host.qcnt=1;
 cqhci_recovery_start(&mmc);expect_recovery_retained();
 assert(host.qcnt==1 && !done && !unmaps && !frees && host.quiesce_error==-EIO);
 drop_disable=false;async_proof=true;quiesce_result=-ETIMEDOUT;
 cqhci_recovery_finish(&mmc);assert(done==1 && unmaps==1 && host.qcnt==0);
 /* Initial checked-host enable uses the same producer serialization. */
 reset_owner(true);host.enabled=host.activated=false;regs[CQHCI_CFG/4]=0;
 assert(cqhci_enable(&mmc,&card)==0 && builds==1 && host.enabled);
 assert(cqhci_deactivate(&mmc)==0);cqhci_disable(&mmc);assert(frees==2);
 reset_owner(true);host.enabled=host.activated=false;regs[CQHCI_CFG/4]=0;
 inject_recovery=true;quiesce_result=-ETIMEDOUT;
 assert(cqhci_enable(&mmc,&card)==-ETIMEDOUT && !builds && frees==2 && !host.desc_base);
 /* Hosts lacking the checked native callback retain their original path. */
 reset_owner(false);regs[CQHCI_CTL/4]=CQHCI_HALT;cqhci_off(&mmc);
 assert(disable_calls==1 && post_calls==1 && !mmc.cqe_on && host.activated);
 cqhci_disable(&mmc);assert(frees==2 && !host.enabled && !quiesce_calls);
 reset_owner(false);host.recovery_halt=true;cqhci_recovery_finish(&mmc);
 assert(!host.recovery_halt && (regs[CQHCI_CFG/4]&CQHCI_ENABLE));
 puts("CQHCI_CHECKED_NATIVE_OWNERSHIP_PASS");
#endif
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="cqhci-owner-host-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(cq_code)
    subprocess.run([*compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                    "-O1", "-g", "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    *shlex.split(os.environ.get("HOSTCFLAGS", "")), str(path / "test.c"), "-o", str(path / "test")], check=True)
    print("CQHCI_NATIVE_CALLBACK_BUILD_PASS", flush=True)
    subprocess.run([str(path / "test")], check=True)
