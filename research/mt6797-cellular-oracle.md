# MT6797 MD1 hand-off and next oracle

This note is tied to vendor tree `/home/grw/src/gemini-linux-kernel-3.18`
at `8cfe6596` and mainline experiment tree
`/home/grw/src/linux-gemini-cellular-owned-next-20260831`.

## What LK proves

LK passes an image/tag-table result, not an execution result. The vendor kernel
locates `ccci,modem_info_v2`, parses the table, then separately enters its modem
start path:

> `drivers/misc/mediatek/ccci_util/ccci_util_lib_fo.c:960-964`
>
> `raw_ptr = ... of_get_flat_dt_prop(..., "ccci,modem_info_v2", NULL);`
>
> `if (raw_ptr != NULL) {`
>
> `        if (lk_info_parsing_v2(raw_ptr) == 1) /* No md enabled in LK */`

The MD record's zero error marks the environment ready:

> `drivers/misc/mediatek/ccci_util/ccci_util_lib_fo.c:731-742`
>
> `lk_load_img_err_no[md_id] = (int)curr->errno;`
>
> `if (lk_load_img_err_no[md_id] == 0)`
>
> `        s_g_md_env_rdy_flag |= 1<<md_id;`

The same code reports image loading as successful (`:1009-1018`), but contains
no MD1 release or handshake test. Therefore the verified Gemini values
`MD1=0xb4000000/0x08000000` and `SMEM=0x88000000/0x00600000` prove that Linux
has a coherent LK-loaded candidate. They do not prove that MD1 ran.

## Active vendor power and release path

The old `base/power/mt6797/mt_spm_mtcmos.c` is not the Gemini power oracle:

> `drivers/misc/mediatek/base/power/mt6797/Makefile:42`
>
> `#obj-y += mt_spm_mtcmos.o`

The built CCF power owner is `clk-mt6797-pg.c`:

> `drivers/clk/mediatek/Makefile:33`
>
> `obj-$(CONFIG_COMMON_CLK_MT6797) += clk-mt6797.o clk-mt6797-pll.o clk-mt6797-pg.o`

> `drivers/clk/mediatek/clk-mt6797-pg.c:131-157`
>
> `#define PWR_STATUS            SPM_REG(0x0180)`
>
> `#define PWR_STATUS_2ND        SPM_REG(0x0184)`
>
> `#define MD1_PWR_CON           SPM_REG(0x0320)`
>
> `#define MD_EXT_BUCK_ISO       SPM_REG(0x0390)`

Its exact MD1 on sequence clears external buck isolation, requests both power
acks, clears clock/isolation/SRAM power-down, releases the six-bit bus fence,
then releases reset (`clk-mt6797-pg.c:525-566`). The fence is:

> `drivers/clk/mediatek/clk-mt6797-pg.c:192-198`
>
> `#define MD1_PROT_MASK ((0x1 << 3)|(0x1 << 4)|(0x1 << 5)|`
>
> `                         (0x1 << 7)|(0x1 << 10)|(0x1 << 16))`

This matters because the existing mainline `mt6797-md1-cold-owner.c` used the
unbuilt file's bits 24--28 and LTE isolation flow. It is now quarantined behind
`depends on BROKEN`; it is not a hardware candidate.

After power/PLL setup, the only vendor boot-vector release is:

> `drivers/misc/mediatek/eccci/mt6797/cldma_platform.c:860-869`
>
> `cldma_write32(md_ctrl->md_boot_slave_En, 0, 1);`

The enclosing order is explicit:

> `drivers/misc/mediatek/eccci/modem_cldma.c:2145-2170`
>
> `ccci_reset_ccif_hw(...);`
>
> `ret = md_cd_power_on(md);`
>
> `md_cd_let_md_go(md);`
>
> `wdt_enable_irq(md);`
>
> `ccif_enable_irq(md);`

## Execution and handshake oracles

The strongest read-only next discriminator is the AP-owned SPM/infracfg
snapshot implemented in `mt6797-md1-boot-status-observe.c`. It reads once and
writes nothing:

- SPM `0x180/0x184` bit 0: the two independent MD1 power acknowledgements.
- SPM `0x320`: both power requests, reset, isolation, clock and SRAM state.
- SPM `0x390` bit 0: external buck isolation.
- infracfg `0x220/0x228`: the active six-bit MD1 bus fence.
- infracfg `0xf0c`: raw MD source-clock request state.

It classifies `cold-off`, `powered-domain-reset-held`,
`powered-domain-reset-deasserted-boot-vector-unknown`, or `inconsistent`, and
separately reports whether the raw state is consistent with the active vendor
power-gate path's cold state. Power-domain `PWR_RST_B` is not
`MD_BOOT_VECTOR_EN`; neither powered state proves boot-vector release or
execution. This changes the next decision without making any power, reset,
clock, IRQ, DMA, shared-memory, SIM or RF request.

The AP-visible boot words are also a safe raw execution hint. Vendor code makes
two dummy reads and reports the third:

> `drivers/misc/mediatek/eccci/mt6797/cldma_platform.c:360-378`
>
> `cldma_read32(md_boot_stats0, 0); /* dummy read */`
>
> `CCCI_NOTICE_LOG(..., "md_boot_stats0:0x%X\n", cldma_read32(...));`

I could not determine a source-backed semantic table for their values. Zero is
therefore not labelled "not executing" and nonzero is not labelled "ready".

Normal readiness is not an AP_CCIF bit. The real vendor acceptance oracle is a
CCCI control packet received over CLDMA: `MD_INIT_START_BOOT` plus
`MD_INIT_CHK_ID` is HS1, followed by `MD_NORMAL_BOOT` as HS2
(`eccci/port_ctlmsg.c:26-40`), and the FSM requires HS1 then HS2 before READY
(`eccci/ccci_fsm.c:209-231`). AP_CCIF `RCHNUM` is used for exception and peer
wake channels (`eccci/modem_cldma.c:1916-1941`), so an all-zero CCIF snapshot
cannot disprove normal modem execution.

## Acceptance values for the next boot

Collect the single `read-only MD1 ownership snapshot` log line together with
the existing three boot-status samples and CCIF snapshot.

- `state=cold-off active-pg-cold-consistent=1`: implement a new
  active-CCF-derived, separately gated start transaction; do not reuse the
  quarantined owner.
- `state=powered-domain-reset-held`: investigate who acquired power and why the
  domain reset remains held before issuing any write.
- `state=powered-domain-reset-deasserted-boot-vector-unknown`: add a passive
  bounded boot-word time series. Do not infer boot-vector release, execution or
  handshake, and do not replay power.
- `state=inconsistent`: stop. Preserve all raw values and reboot before another
  MD experiment.

No result from this observer authorizes `MD_BOOT_VECTOR_EN`, CCIF ACK, CLDMA,
shared-memory, SIM, RF, modem-partition, GPT, or flash writes.
