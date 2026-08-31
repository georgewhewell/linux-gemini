// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: Xudong Chen <xudong.chen@mediatek.com>
 */

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/i2c-mt65xx-gemini-ledger.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>
#include <linux/units.h>
#include <linux/uaccess.h>

#define I2C_RS_TRANSFER			(1 << 4)
#define I2C_ARB_LOST			(1 << 3)
#define I2C_HS_NACKERR			(1 << 2)
#define I2C_ACKERR			(1 << 1)
#define I2C_TRANSAC_COMP		(1 << 0)
#define I2C_TRANSAC_START		(1 << 0)
#define I2C_RS_MUL_CNFG			(1 << 15)
#define I2C_RS_MUL_TRIG			(1 << 14)
#define I2C_DCM_DISABLE			0x0000
#define I2C_IO_CONFIG_OPEN_DRAIN	0x0003
#define I2C_IO_CONFIG_PUSH_PULL		0x0000
#define I2C_SOFT_RST			0x0001
#define I2C_HANDSHAKE_RST		0x0020
#define I2C_FIFO_ADDR_CLR		0x0001
#define I2C_DELAY_LEN			0x0002
#define I2C_ST_START_CON		0x8001
#define I2C_FS_START_CON		0x1800
#define I2C_TIME_CLR_VALUE		0x0000
#define I2C_TIME_DEFAULT_VALUE		0x0003
#define I2C_WRRD_TRANAC_VALUE		0x0002
#define I2C_RD_TRANAC_VALUE		0x0001
#define I2C_WRRD_PACKED_TX_MAX		0x00ff
#define I2C_WRRD_PACKED_RX_MAX		0x001f
#define I2C_FIFO_RX_COUNT_MASK		0x00f0
#define I2C_FIFO_RX_COUNT_SHIFT		4
#define I2C_SCL_MIS_COMP_VALUE		0x0000
#define I2C_CHN_CLR_FLAG		0x0000
#define I2C_RELIABILITY		0x0010
#define I2C_DMAACK_ENABLE		0x0008

#define I2C_DMA_CON_TX			0x0000
#define I2C_DMA_CON_RX			0x0001
#define I2C_DMA_ASYNC_MODE		0x0004
#define I2C_DMA_SKIP_CONFIG		0x0010
#define I2C_DMA_DIR_CHANGE		0x0200
#define I2C_DMA_START_EN		0x0001
#define I2C_DMA_INT_FLAG_NONE		0x0000
#define I2C_DMA_CLR_FLAG		0x0000
#define I2C_DMA_WARM_RST		0x0001
#define I2C_DMA_HARD_RST		0x0002
#define I2C_DMA_HANDSHAKE_RST		0x0004

#define MAX_SAMPLE_CNT_DIV		8
#define MAX_STEP_CNT_DIV		64
#define MAX_CLOCK_DIV_8BITS		256
#define MAX_CLOCK_DIV_5BITS		32
#define MAX_HS_STEP_CNT_DIV		8
#define I2C_STANDARD_MODE_BUFFER	(1000 / 3)
#define I2C_FAST_MODE_BUFFER		(300 / 3)
#define I2C_FAST_MODE_PLUS_BUFFER	(20 / 3)

#define I2C_CONTROL_RS                  (0x1 << 1)
#define I2C_CONTROL_DMA_EN              (0x1 << 2)
#define I2C_CONTROL_CLK_EXT_EN          (0x1 << 3)
#define I2C_CONTROL_DIR_CHANGE          (0x1 << 4)
#define I2C_CONTROL_ACKERR_DET_EN       (0x1 << 5)
#define I2C_CONTROL_TRANSFER_LEN_CHANGE (0x1 << 6)
#define I2C_CONTROL_DMAACK_EN           (0x1 << 8)
#define I2C_CONTROL_ASYNC_MODE          (0x1 << 9)
#define I2C_CONTROL_WRAPPER             (0x1 << 0)

#define I2C_DRV_NAME		"i2c-mt65xx"

/**
 * enum i2c_mt65xx_clks - Clocks enumeration for MT65XX I2C
 *
 * @I2C_MT65XX_CLK_MAIN: main clock for i2c bus
 * @I2C_MT65XX_CLK_DMA:  DMA clock for i2c via DMA
 * @I2C_MT65XX_CLK_PMIC: PMIC clock for i2c from PMIC
 * @I2C_MT65XX_CLK_ARB:  Arbitrator clock for i2c
 * @I2C_MT65XX_CLK_MAX:  Number of supported clocks
 */
enum i2c_mt65xx_clks {
	I2C_MT65XX_CLK_MAIN = 0,
	I2C_MT65XX_CLK_DMA,
	I2C_MT65XX_CLK_PMIC,
	I2C_MT65XX_CLK_ARB,
	I2C_MT65XX_CLK_MAX
};

static const char * const i2c_mt65xx_clk_ids[I2C_MT65XX_CLK_MAX] = {
	"main", "dma", "pmic", "arb"
};

enum DMA_REGS_OFFSET {
	OFFSET_INT_FLAG = 0x0,
	OFFSET_INT_EN = 0x04,
	OFFSET_EN = 0x08,
	OFFSET_RST = 0x0c,
	OFFSET_CON = 0x18,
	OFFSET_TX_MEM_ADDR = 0x1c,
	OFFSET_RX_MEM_ADDR = 0x20,
	OFFSET_TX_LEN = 0x24,
	OFFSET_RX_LEN = 0x28,
	OFFSET_TX_4G_MODE = 0x54,
	OFFSET_RX_4G_MODE = 0x58,
};

enum i2c_trans_st_rs {
	I2C_TRANS_STOP = 0,
	I2C_TRANS_REPEATED_START,
};

enum mtk_trans_op {
	I2C_MASTER_WR = 1,
	I2C_MASTER_RD,
	I2C_MASTER_WRRD,
};

enum mtk_i2c_wrrd_len_mode {
	MTK_I2C_WRRD_LEN_AUX,
	MTK_I2C_WRRD_LEN_PACKED_8_5,
};

#ifdef CONFIG_I2C_MT65XX_ORION_DIAGNOSTIC
#define MTK_I2C_ORION_MODE_COUNT	3
#define MTK_I2C_ORION_REGISTER_COUNT	3
#define MTK_I2C_ORION_SAMPLE_COUNT	\
	(MTK_I2C_ORION_MODE_COUNT * MTK_I2C_ORION_REGISTER_COUNT)

enum mtk_i2c_orion_mode {
	MTK_I2C_ORION_MODE_NONE,
	MTK_I2C_ORION_MODE_AUX_DMA,
	MTK_I2C_ORION_MODE_PACKED_DMA,
	MTK_I2C_ORION_MODE_PACKED_FIFO,
};

enum mtk_i2c_orion_state {
	MTK_I2C_ORION_READY,
	MTK_I2C_ORION_RUNNING,
	MTK_I2C_ORION_DONE,
};

struct mtk_i2c_orion_sample {
	enum mtk_i2c_orion_mode mode;
	u8 reg;
	u8 prefill;
	u8 value;
	int ret;
	bool use_dma;
	u16 transfer_len;
	u16 transfer_len_aux;
	u16 transac_len;
	u16 control;
	u16 irq_stat;
	u16 fifo_count;
	u32 dma_en_pre;
	u32 dma_con_pre;
	u32 dma_int_flag_pre;
	u32 dma_tx_len_pre;
	u32 dma_rx_len_pre;
	u32 dma_en_irq;
	u32 dma_int_flag_irq;
	u32 dma_en_post;
	u32 dma_con_post;
	u32 dma_int_flag_post;
	u32 dma_tx_len_post;
	u32 dma_rx_len_post;
};

struct mtk_i2c_orion_diag {
	struct mutex lock; /* Serializes one-shot state and result reads. */
	struct mtk_i2c_orion_sample samples[MTK_I2C_ORION_SAMPLE_COUNT];
	struct mtk_i2c_orion_sample *active_sample;
	enum mtk_i2c_orion_state state;
	enum mtk_i2c_orion_mode mode;
	bool reset_pending;
	int run_error;
	unsigned int attempted;
	unsigned int completed;
	unsigned int retries_before;
	unsigned int retries_during;
	unsigned int retries_after;
};
#endif

#if defined(CONFIG_I2C_MT65XX_QUASAR_DIAGNOSTIC) || \
	defined(CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC)
#define MTK_I2C_QUASAR_PASS_COUNT	2
#ifdef CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC
#define MTK_I2C_QUASAR_REGISTER_COUNT	7
#else
#define MTK_I2C_QUASAR_REGISTER_COUNT	3
#endif
#define MTK_I2C_QUASAR_SAMPLE_COUNT	\
	(MTK_I2C_QUASAR_PASS_COUNT * MTK_I2C_QUASAR_REGISTER_COUNT)

enum mtk_i2c_quasar_state {
	MTK_I2C_QUASAR_READY,
	MTK_I2C_QUASAR_RUNNING,
	MTK_I2C_QUASAR_DONE,
};

struct mtk_i2c_quasar_snapshot {
	u16 transfer_len;
	u16 transfer_len_aux;
	u16 transac_len;
	u16 control;
	u16 start;
	u16 intr_stat;
	u16 fifo_stat;
	u32 dma_en;
	u32 dma_con;
	u32 dma_int_flag;
	u32 dma_tx_len;
	u32 dma_rx_len;
};

struct mtk_i2c_quasar_sample {
	struct mtk_i2c_quasar_snapshot pre;
	struct mtk_i2c_quasar_snapshot irq;
	struct mtk_i2c_quasar_snapshot post;
	struct mtk_i2c_quasar_snapshot drained;
	unsigned int pass;
	unsigned int index;
	u16 addr;
	u8 reg;
	u8 expected;
	u8 prefill;
	u8 value;
	int ret;
	int validation_error;
	bool programmed;
	bool transport_completed;
	bool value_validated;
	bool use_dma;
	u16 irq_stat;
	u16 fifo_stat;
	u16 fifo_count;
	u16 fifo_count_drained;
};

struct mtk_i2c_quasar_diag {
	struct mutex lock; /* Serializes one-shot state and result reads. */
	struct mtk_i2c_quasar_sample samples[MTK_I2C_QUASAR_SAMPLE_COUNT];
	struct mtk_i2c_quasar_sample *active_sample;
	enum mtk_i2c_quasar_state state;
	int run_error;
	unsigned int attempted;
	unsigned int transport_completed;
	unsigned int value_validated;
	unsigned int failure_pass;
	unsigned int failure_index;
	bool mismatch;
	unsigned int stability_validated;
	unsigned int retries_before;
	unsigned int retries_during;
	unsigned int retries_after;
	int init_attempts_before;
	int init_attempts_after;
	int init_successes_before;
	int init_successes_after;
	int transfer_attempts_before;
	int transfer_attempts_after;
	int dma_starts_before;
	int dma_starts_after;
	int nonzero_starts_before;
	int nonzero_starts_after;
	int irqs_before;
	int irqs_after;
};
#endif

enum I2C_REGS_OFFSET {
	OFFSET_DATA_PORT,
	OFFSET_SLAVE_ADDR,
	OFFSET_INTR_MASK,
	OFFSET_INTR_STAT,
	OFFSET_CONTROL,
	OFFSET_TRANSFER_LEN,
	OFFSET_TRANSAC_LEN,
	OFFSET_DELAY_LEN,
	OFFSET_TIMING,
	OFFSET_START,
	OFFSET_EXT_CONF,
	OFFSET_FIFO_STAT,
	OFFSET_FIFO_THRESH,
	OFFSET_FIFO_ADDR_CLR,
	OFFSET_IO_CONFIG,
	OFFSET_RSV_DEBUG,
	OFFSET_HS,
	OFFSET_SOFTRESET,
	OFFSET_DCM_EN,
	OFFSET_MULTI_DMA,
	OFFSET_PATH_DIR,
	OFFSET_DEBUGSTAT,
	OFFSET_DEBUGCTRL,
	OFFSET_TRANSFER_LEN_AUX,
	OFFSET_CLOCK_DIV,
	OFFSET_LTIMING,
	OFFSET_SCL_HIGH_LOW_RATIO,
	OFFSET_HS_SCL_HIGH_LOW_RATIO,
	OFFSET_SCL_MIS_COMP_POINT,
	OFFSET_STA_STO_AC_TIMING,
	OFFSET_HS_STA_STO_AC_TIMING,
	OFFSET_SDA_TIMING,
};

static const u16 mt_i2c_regs_v1[] = {
	[OFFSET_DATA_PORT] = 0x0,
	[OFFSET_SLAVE_ADDR] = 0x4,
	[OFFSET_INTR_MASK] = 0x8,
	[OFFSET_INTR_STAT] = 0xc,
	[OFFSET_CONTROL] = 0x10,
	[OFFSET_TRANSFER_LEN] = 0x14,
	[OFFSET_TRANSAC_LEN] = 0x18,
	[OFFSET_DELAY_LEN] = 0x1c,
	[OFFSET_TIMING] = 0x20,
	[OFFSET_START] = 0x24,
	[OFFSET_EXT_CONF] = 0x28,
	[OFFSET_FIFO_STAT] = 0x30,
	[OFFSET_FIFO_THRESH] = 0x34,
	[OFFSET_FIFO_ADDR_CLR] = 0x38,
	[OFFSET_IO_CONFIG] = 0x40,
	[OFFSET_RSV_DEBUG] = 0x44,
	[OFFSET_HS] = 0x48,
	[OFFSET_SOFTRESET] = 0x50,
	[OFFSET_DCM_EN] = 0x54,
	[OFFSET_PATH_DIR] = 0x60,
	[OFFSET_DEBUGSTAT] = 0x64,
	[OFFSET_DEBUGCTRL] = 0x68,
	[OFFSET_TRANSFER_LEN_AUX] = 0x6c,
	[OFFSET_CLOCK_DIV] = 0x70,
	[OFFSET_SCL_HIGH_LOW_RATIO] = 0x74,
	[OFFSET_HS_SCL_HIGH_LOW_RATIO] = 0x78,
	[OFFSET_SCL_MIS_COMP_POINT] = 0x7C,
	[OFFSET_STA_STO_AC_TIMING] = 0x80,
	[OFFSET_HS_STA_STO_AC_TIMING] = 0x84,
	[OFFSET_SDA_TIMING] = 0x88,
};

static const u16 mt_i2c_regs_v2[] = {
	[OFFSET_DATA_PORT] = 0x0,
	[OFFSET_SLAVE_ADDR] = 0x4,
	[OFFSET_INTR_MASK] = 0x8,
	[OFFSET_INTR_STAT] = 0xc,
	[OFFSET_CONTROL] = 0x10,
	[OFFSET_TRANSFER_LEN] = 0x14,
	[OFFSET_TRANSAC_LEN] = 0x18,
	[OFFSET_DELAY_LEN] = 0x1c,
	[OFFSET_TIMING] = 0x20,
	[OFFSET_START] = 0x24,
	[OFFSET_EXT_CONF] = 0x28,
	[OFFSET_LTIMING] = 0x2c,
	[OFFSET_HS] = 0x30,
	[OFFSET_IO_CONFIG] = 0x34,
	[OFFSET_FIFO_ADDR_CLR] = 0x38,
	[OFFSET_SDA_TIMING] = 0x3c,
	[OFFSET_TRANSFER_LEN_AUX] = 0x44,
	[OFFSET_CLOCK_DIV] = 0x48,
	[OFFSET_SOFTRESET] = 0x50,
	[OFFSET_MULTI_DMA] = 0x8c,
	[OFFSET_SCL_MIS_COMP_POINT] = 0x90,
	[OFFSET_DEBUGSTAT] = 0xe4,
	[OFFSET_DEBUGCTRL] = 0xe8,
	[OFFSET_FIFO_STAT] = 0xf4,
	[OFFSET_FIFO_THRESH] = 0xf8,
	[OFFSET_DCM_EN] = 0xf88,
};

static const u16 mt_i2c_regs_v3[] = {
	[OFFSET_DATA_PORT] = 0x0,
	[OFFSET_INTR_MASK] = 0x8,
	[OFFSET_INTR_STAT] = 0xc,
	[OFFSET_CONTROL] = 0x10,
	[OFFSET_TRANSFER_LEN] = 0x14,
	[OFFSET_TRANSAC_LEN] = 0x18,
	[OFFSET_DELAY_LEN] = 0x1c,
	[OFFSET_TIMING] = 0x20,
	[OFFSET_START] = 0x24,
	[OFFSET_EXT_CONF] = 0x28,
	[OFFSET_LTIMING] = 0x2c,
	[OFFSET_HS] = 0x30,
	[OFFSET_IO_CONFIG] = 0x34,
	[OFFSET_FIFO_ADDR_CLR] = 0x38,
	[OFFSET_SDA_TIMING] = 0x3c,
	[OFFSET_TRANSFER_LEN_AUX] = 0x44,
	[OFFSET_CLOCK_DIV] = 0x48,
	[OFFSET_SOFTRESET] = 0x50,
	[OFFSET_MULTI_DMA] = 0x8c,
	[OFFSET_SCL_MIS_COMP_POINT] = 0x90,
	[OFFSET_SLAVE_ADDR] = 0x94,
	[OFFSET_DEBUGSTAT] = 0xe4,
	[OFFSET_DEBUGCTRL] = 0xe8,
	[OFFSET_FIFO_STAT] = 0xf4,
	[OFFSET_FIFO_THRESH] = 0xf8,
	[OFFSET_DCM_EN] = 0xf88,
};

struct mtk_i2c_compatible {
	const struct i2c_adapter_quirks *quirks;
	const u16 *regs;
	enum mtk_i2c_wrrd_len_mode wrrd_len_mode;
	u8 fifo_size;
	unsigned char pmic_i2c: 1;
	unsigned char dcm: 1;
	unsigned char auto_restart: 1;
	unsigned char timing_adjust: 1;
	unsigned char dma_sync: 1;
	unsigned char ltiming_adjust: 1;
	unsigned char apdma_sync: 1;
	unsigned char max_dma_support;
};

struct mtk_i2c_ac_timing {
	u16 htiming;
	u16 ltiming;
	u16 hs;
	u16 ext;
	u16 inter_clk_div;
	u16 scl_hl_ratio;
	u16 hs_scl_hl_ratio;
	u16 sta_stop;
	u16 hs_sta_stop;
	u16 sda_timing;
};

struct mtk_i2c {
	struct i2c_adapter adap;	/* i2c host adapter */
	struct device *dev;
	struct completion msg_complete;
	struct i2c_timings timing_info;
	struct mt6797_dvfsp_handoff *dvfsp_handoff;
	bool handoff_runtime_pm_link;
	atomic_t init_attempts;
	atomic_t init_successes;
	atomic_t transfer_attempts;
	atomic_t dma_start_count;
	atomic_t nonzero_start_count;
	atomic_t irq_count;
	atomic_t clock_ungated_checks;
	atomic_t clock_gated_checks;
	atomic_t clock_validation_failures;
	atomic_t suspend_checks;
	atomic_t resume_checks;
	atomic_t resume_failures;
#ifdef CONFIG_I2C_MT65XX_GEMINI_LIFECYCLE_ORACLE
	atomic_t oracle_combined_pointer_reads;
	atomic_t oracle_primary_pointer_reads;
	atomic_t oracle_page2_pointer_reads;
	atomic_t oracle_write_only_messages;
	atomic_t oracle_register_data_write_messages;
	atomic_t oracle_other_transfers;
	atomic_t oracle_other_address_transfers;
#endif

	/* set in i2c probe */
	void __iomem *base;		/* i2c base addr */
	void __iomem *pdmabase;		/* dma base address*/
	struct clk_bulk_data clocks[I2C_MT65XX_CLK_MAX]; /* clocks for i2c */
	bool have_pmic;			/* can use i2c pins from PMIC */
	bool use_push_pull;		/* IO config push-pull mode */

	u16 irq_stat;			/* interrupt status */
	unsigned int clk_src_div;
	unsigned int speed_hz;		/* The speed in transfer */
	enum mtk_trans_op op;
	u16 timing_reg;
	u16 high_speed_reg;
	u16 ltiming_reg;
	unsigned char auto_restart;
	bool ignore_restart_irq;
	struct mtk_i2c_ac_timing ac_timing;
	const struct mtk_i2c_compatible *dev_comp;
#ifdef CONFIG_I2C_MT65XX_ORION_DIAGNOSTIC
	struct mtk_i2c_orion_diag orion;
#endif
#if defined(CONFIG_I2C_MT65XX_QUASAR_DIAGNOSTIC) || \
	defined(CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC)
	struct mtk_i2c_quasar_diag quasar;
#endif
};

static atomic_t mtk_i2c_handoff_probe_attempts = ATOMIC_INIT(0);

/**
 * struct i2c_spec_values:
 * @min_low_ns: min LOW period of the SCL clock
 * @min_su_sta_ns: min set-up time for a repeated START condition
 * @max_hd_dat_ns: max data hold time
 * @min_su_dat_ns: min data set-up time
 */
struct i2c_spec_values {
	unsigned int min_low_ns;
	unsigned int min_su_sta_ns;
	unsigned int max_hd_dat_ns;
	unsigned int min_su_dat_ns;
};

static const struct i2c_spec_values standard_mode_spec = {
	.min_low_ns = 4700 + I2C_STANDARD_MODE_BUFFER,
	.min_su_sta_ns = 4700 + I2C_STANDARD_MODE_BUFFER,
	.max_hd_dat_ns = 3450 - I2C_STANDARD_MODE_BUFFER,
	.min_su_dat_ns = 250 + I2C_STANDARD_MODE_BUFFER,
};

static const struct i2c_spec_values fast_mode_spec = {
	.min_low_ns = 1300 + I2C_FAST_MODE_BUFFER,
	.min_su_sta_ns = 600 + I2C_FAST_MODE_BUFFER,
	.max_hd_dat_ns = 900 - I2C_FAST_MODE_BUFFER,
	.min_su_dat_ns = 100 + I2C_FAST_MODE_BUFFER,
};

static const struct i2c_spec_values fast_mode_plus_spec = {
	.min_low_ns = 500 + I2C_FAST_MODE_PLUS_BUFFER,
	.min_su_sta_ns = 260 + I2C_FAST_MODE_PLUS_BUFFER,
	.max_hd_dat_ns = 400 - I2C_FAST_MODE_PLUS_BUFFER,
	.min_su_dat_ns = 50 + I2C_FAST_MODE_PLUS_BUFFER,
};

static const struct i2c_adapter_quirks mt6577_i2c_quirks = {
	.flags = I2C_AQ_COMB_WRITE_THEN_READ,
	.max_num_msgs = 1,
	.max_write_len = 255,
	.max_read_len = 255,
	.max_comb_1st_msg_len = 255,
	.max_comb_2nd_msg_len = 31,
};

static const struct i2c_adapter_quirks mt6797_idvfs_i2c_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN | I2C_AQ_COMB_WRITE_THEN_READ,
	.max_num_msgs = 1,
	.max_write_len = 255,
	.max_read_len = 255,
	.max_comb_1st_msg_len = 255,
	.max_comb_2nd_msg_len = 31,
};

static const struct i2c_adapter_quirks mt7622_i2c_quirks = {
	.max_num_msgs = 255,
};

static const struct i2c_adapter_quirks mt8183_i2c_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN,
};

static const struct mtk_i2c_compatible mt2712_compat = {
	.regs = mt_i2c_regs_v1,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_AUX,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 1,
	.timing_adjust = 1,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 33,
};

static const struct mtk_i2c_compatible mt6577_compat = {
	.quirks = &mt6577_i2c_quirks,
	.regs = mt_i2c_regs_v1,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_PACKED_8_5,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 0,
	.timing_adjust = 0,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 32,
};

static const struct mtk_i2c_compatible mt6589_compat = {
	.quirks = &mt6577_i2c_quirks,
	.regs = mt_i2c_regs_v1,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_PACKED_8_5,
	.pmic_i2c = 1,
	.dcm = 0,
	.auto_restart = 0,
	.timing_adjust = 0,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 32,
};

static const struct mtk_i2c_compatible mt7622_compat = {
	.quirks = &mt7622_i2c_quirks,
	.regs = mt_i2c_regs_v1,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_AUX,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 1,
	.timing_adjust = 0,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 32,
};

static const struct mtk_i2c_compatible mt8168_compat = {
	.regs = mt_i2c_regs_v1,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_AUX,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 1,
	.timing_adjust = 1,
	.dma_sync = 1,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 33,
};

static const struct mtk_i2c_compatible mt7981_compat = {
	.regs = mt_i2c_regs_v3,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_AUX,
	.pmic_i2c = 0,
	.dcm = 0,
	.auto_restart = 1,
	.timing_adjust = 1,
	.dma_sync = 1,
	.ltiming_adjust = 1,
	.max_dma_support = 33
};

static const struct mtk_i2c_compatible mt7986_compat = {
	.quirks = &mt7622_i2c_quirks,
	.regs = mt_i2c_regs_v1,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_AUX,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 1,
	.timing_adjust = 0,
	.dma_sync = 1,
	.ltiming_adjust = 0,
	.max_dma_support = 32,
};

static const struct mtk_i2c_compatible mt8173_compat = {
	.regs = mt_i2c_regs_v1,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_AUX,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 1,
	.timing_adjust = 0,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 33,
};

static const struct mtk_i2c_compatible mt6797_idvfs_compat = {
	.quirks = &mt6797_idvfs_i2c_quirks,
	.regs = mt_i2c_regs_v1,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_PACKED_8_5,
	.fifo_size = 8,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 1,
	.timing_adjust = 0,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 33,
};

static const struct mtk_i2c_compatible mt8183_compat = {
	.quirks = &mt8183_i2c_quirks,
	.regs = mt_i2c_regs_v2,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_AUX,
	.pmic_i2c = 0,
	.dcm = 0,
	.auto_restart = 1,
	.timing_adjust = 1,
	.dma_sync = 1,
	.ltiming_adjust = 1,
	.apdma_sync = 0,
	.max_dma_support = 33,
};

static const struct mtk_i2c_compatible mt8186_compat = {
	.regs = mt_i2c_regs_v2,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_AUX,
	.pmic_i2c = 0,
	.dcm = 0,
	.auto_restart = 1,
	.timing_adjust = 1,
	.dma_sync = 0,
	.ltiming_adjust = 1,
	.apdma_sync = 0,
	.max_dma_support = 36,
};

static const struct mtk_i2c_compatible mt8188_compat = {
	.regs = mt_i2c_regs_v3,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_AUX,
	.pmic_i2c = 0,
	.dcm = 0,
	.auto_restart = 1,
	.timing_adjust = 1,
	.dma_sync = 0,
	.ltiming_adjust = 1,
	.apdma_sync = 1,
	.max_dma_support = 36,
};

static const struct mtk_i2c_compatible mt8192_compat = {
	.quirks = &mt8183_i2c_quirks,
	.regs = mt_i2c_regs_v2,
	.wrrd_len_mode = MTK_I2C_WRRD_LEN_AUX,
	.pmic_i2c = 0,
	.dcm = 0,
	.auto_restart = 1,
	.timing_adjust = 1,
	.dma_sync = 1,
	.ltiming_adjust = 1,
	.apdma_sync = 1,
	.max_dma_support = 36,
};

static const struct of_device_id mtk_i2c_of_match[] = {
	{ .compatible = "mediatek,mt2712-i2c", .data = &mt2712_compat },
	{ .compatible = "mediatek,mt6577-i2c", .data = &mt6577_compat },
	{ .compatible = "mediatek,mt6589-i2c", .data = &mt6589_compat },
	{ .compatible = "mediatek,mt6797-idvfs-i2c",
	  .data = &mt6797_idvfs_compat },
	{ .compatible = "mediatek,mt6797-i2c", .data = &mt8173_compat },
	{ .compatible = "mediatek,mt7622-i2c", .data = &mt7622_compat },
	{ .compatible = "mediatek,mt7981-i2c", .data = &mt7981_compat },
	{ .compatible = "mediatek,mt7986-i2c", .data = &mt7986_compat },
	{ .compatible = "mediatek,mt8168-i2c", .data = &mt8168_compat },
	{ .compatible = "mediatek,mt8173-i2c", .data = &mt8173_compat },
	{ .compatible = "mediatek,mt8183-i2c", .data = &mt8183_compat },
	{ .compatible = "mediatek,mt8186-i2c", .data = &mt8186_compat },
	{ .compatible = "mediatek,mt8188-i2c", .data = &mt8188_compat },
	{ .compatible = "mediatek,mt8192-i2c", .data = &mt8192_compat },
	{}
};
MODULE_DEVICE_TABLE(of, mtk_i2c_of_match);

static u16 mtk_i2c_readw(struct mtk_i2c *i2c, enum I2C_REGS_OFFSET reg)
{
	return readw(i2c->base + i2c->dev_comp->regs[reg]);
}

static void mtk_i2c_writew(struct mtk_i2c *i2c, u16 val,
			   enum I2C_REGS_OFFSET reg)
{
	writew(val, i2c->base + i2c->dev_comp->regs[reg]);
}

static void mtk_i2c_init_hw(struct mtk_i2c *i2c)
{
	u16 control_reg;
	u16 intr_stat_reg;
	u16 ext_conf_val;

	if (i2c->dvfsp_handoff)
		atomic_inc(&i2c->init_attempts);

	mtk_i2c_writew(i2c, I2C_CHN_CLR_FLAG, OFFSET_START);
	intr_stat_reg = mtk_i2c_readw(i2c, OFFSET_INTR_STAT);
	mtk_i2c_writew(i2c, intr_stat_reg, OFFSET_INTR_STAT);

	if (i2c->dev_comp->apdma_sync) {
		writel(I2C_DMA_WARM_RST, i2c->pdmabase + OFFSET_RST);
		udelay(10);
		writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_RST);
		udelay(10);
		writel(I2C_DMA_HANDSHAKE_RST | I2C_DMA_HARD_RST,
		       i2c->pdmabase + OFFSET_RST);
		mtk_i2c_writew(i2c, I2C_HANDSHAKE_RST | I2C_SOFT_RST,
			       OFFSET_SOFTRESET);
		udelay(10);
		writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_RST);
		mtk_i2c_writew(i2c, I2C_CHN_CLR_FLAG, OFFSET_SOFTRESET);
	} else {
		writel(I2C_DMA_HARD_RST, i2c->pdmabase + OFFSET_RST);
		udelay(50);
		writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_RST);
		mtk_i2c_writew(i2c, I2C_SOFT_RST, OFFSET_SOFTRESET);
	}

	/* Set ioconfig */
	if (i2c->use_push_pull)
		mtk_i2c_writew(i2c, I2C_IO_CONFIG_PUSH_PULL, OFFSET_IO_CONFIG);
	else
		mtk_i2c_writew(i2c, I2C_IO_CONFIG_OPEN_DRAIN, OFFSET_IO_CONFIG);

	if (i2c->dev_comp->dcm)
		mtk_i2c_writew(i2c, I2C_DCM_DISABLE, OFFSET_DCM_EN);

	mtk_i2c_writew(i2c, i2c->timing_reg, OFFSET_TIMING);
	mtk_i2c_writew(i2c, i2c->high_speed_reg, OFFSET_HS);
	if (i2c->dev_comp->ltiming_adjust)
		mtk_i2c_writew(i2c, i2c->ltiming_reg, OFFSET_LTIMING);

	if (i2c->speed_hz <= I2C_MAX_STANDARD_MODE_FREQ)
		ext_conf_val = I2C_ST_START_CON;
	else
		ext_conf_val = I2C_FS_START_CON;

	if (i2c->dev_comp->timing_adjust) {
		ext_conf_val = i2c->ac_timing.ext;
		mtk_i2c_writew(i2c, i2c->ac_timing.inter_clk_div,
			       OFFSET_CLOCK_DIV);
		mtk_i2c_writew(i2c, I2C_SCL_MIS_COMP_VALUE,
			       OFFSET_SCL_MIS_COMP_POINT);
		mtk_i2c_writew(i2c, i2c->ac_timing.sda_timing,
			       OFFSET_SDA_TIMING);

		if (i2c->dev_comp->ltiming_adjust) {
			mtk_i2c_writew(i2c, i2c->ac_timing.htiming,
				       OFFSET_TIMING);
			mtk_i2c_writew(i2c, i2c->ac_timing.hs, OFFSET_HS);
			mtk_i2c_writew(i2c, i2c->ac_timing.ltiming,
				       OFFSET_LTIMING);
		} else {
			mtk_i2c_writew(i2c, i2c->ac_timing.scl_hl_ratio,
				       OFFSET_SCL_HIGH_LOW_RATIO);
			mtk_i2c_writew(i2c, i2c->ac_timing.hs_scl_hl_ratio,
				       OFFSET_HS_SCL_HIGH_LOW_RATIO);
			mtk_i2c_writew(i2c, i2c->ac_timing.sta_stop,
				       OFFSET_STA_STO_AC_TIMING);
			mtk_i2c_writew(i2c, i2c->ac_timing.hs_sta_stop,
				       OFFSET_HS_STA_STO_AC_TIMING);
		}
	}
	mtk_i2c_writew(i2c, ext_conf_val, OFFSET_EXT_CONF);

	/* If use i2c pin from PMIC mt6397 side, need set PATH_DIR first */
	if (i2c->have_pmic)
		mtk_i2c_writew(i2c, I2C_CONTROL_WRAPPER, OFFSET_PATH_DIR);

	control_reg = I2C_CONTROL_ACKERR_DET_EN |
		      I2C_CONTROL_CLK_EXT_EN | I2C_CONTROL_DMA_EN;
	if (i2c->dev_comp->dma_sync)
		control_reg |= I2C_CONTROL_DMAACK_EN | I2C_CONTROL_ASYNC_MODE;

	mtk_i2c_writew(i2c, control_reg, OFFSET_CONTROL);
	mtk_i2c_writew(i2c, I2C_DELAY_LEN, OFFSET_DELAY_LEN);

	if (i2c->dvfsp_handoff)
		atomic_inc(&i2c->init_successes);
}

static const struct i2c_spec_values *mtk_i2c_get_spec(unsigned int speed)
{
	if (speed <= I2C_MAX_STANDARD_MODE_FREQ)
		return &standard_mode_spec;
	else if (speed <= I2C_MAX_FAST_MODE_FREQ)
		return &fast_mode_spec;
	else
		return &fast_mode_plus_spec;
}

static int mtk_i2c_max_step_cnt(unsigned int target_speed)
{
	if (target_speed > I2C_MAX_FAST_MODE_PLUS_FREQ)
		return MAX_HS_STEP_CNT_DIV;
	else
		return MAX_STEP_CNT_DIV;
}

static int mtk_i2c_get_clk_div_restri(struct mtk_i2c *i2c,
				      unsigned int sample_cnt)
{
	int clk_div_restri = 0;

	if (i2c->dev_comp->ltiming_adjust == 0)
		return 0;

	if (sample_cnt == 1) {
		if (i2c->ac_timing.inter_clk_div == 0)
			clk_div_restri = 0;
		else
			clk_div_restri = 1;
	} else {
		if (i2c->ac_timing.inter_clk_div == 0)
			clk_div_restri = -1;
		else if (i2c->ac_timing.inter_clk_div == 1)
			clk_div_restri = 0;
		else
			clk_div_restri = 1;
	}

	return clk_div_restri;
}

/*
 * Check and Calculate i2c ac-timing
 *
 * Hardware design:
 * sample_ns = (HZ_PER_GHZ * (sample_cnt + 1)) / clk_src
 * xxx_cnt_div =  spec->min_xxx_ns / sample_ns
 *
 * Sample_ns is rounded down for xxx_cnt_div would be greater
 * than the smallest spec.
 * The sda_timing is chosen as the middle value between
 * the largest and smallest.
 */
static int mtk_i2c_check_ac_timing(struct mtk_i2c *i2c,
				   unsigned int clk_src,
				   unsigned int check_speed,
				   unsigned int step_cnt,
				   unsigned int sample_cnt)
{
	const struct i2c_spec_values *spec;
	unsigned int su_sta_cnt, low_cnt, high_cnt, max_step_cnt;
	unsigned int sda_max, sda_min, max_sta_cnt = 0x3f;
	unsigned int clk_ns, sample_ns;

	if (!i2c->dev_comp->timing_adjust)
		return 0;

	if (i2c->dev_comp->ltiming_adjust)
		max_sta_cnt = 0x100;

	spec = mtk_i2c_get_spec(check_speed);

	sample_ns = div_u64(1ULL * HZ_PER_GHZ * (sample_cnt + 1), clk_src);
	if (i2c->dev_comp->ltiming_adjust)
		clk_ns = HZ_PER_GHZ / clk_src;
	else
		clk_ns = sample_ns / 2;

	su_sta_cnt = DIV_ROUND_UP(spec->min_su_sta_ns +
				  i2c->timing_info.scl_int_delay_ns, clk_ns);
	if (su_sta_cnt > max_sta_cnt)
		return -1;

	low_cnt = DIV_ROUND_UP(spec->min_low_ns, sample_ns);
	max_step_cnt = mtk_i2c_max_step_cnt(check_speed);
	if ((2 * step_cnt) > low_cnt && low_cnt < max_step_cnt) {
		if (low_cnt > step_cnt) {
			high_cnt = 2 * step_cnt - low_cnt;
		} else {
			high_cnt = step_cnt;
			low_cnt = step_cnt;
		}
	} else {
		return -2;
	}

	sda_max = spec->max_hd_dat_ns / sample_ns;
	if (sda_max > low_cnt)
		sda_max = 0;

	sda_min = DIV_ROUND_UP(spec->min_su_dat_ns, sample_ns);
	if (sda_min < low_cnt)
		sda_min = 0;

	if (sda_min > sda_max)
		return -3;

	if (check_speed > I2C_MAX_FAST_MODE_PLUS_FREQ) {
		if (i2c->dev_comp->ltiming_adjust) {
			i2c->ac_timing.hs = I2C_TIME_DEFAULT_VALUE |
				(sample_cnt << 12) | (high_cnt << 8);
			i2c->ac_timing.ltiming &= ~GENMASK(15, 9);
			i2c->ac_timing.ltiming |= (sample_cnt << 12) |
				(low_cnt << 9);
			i2c->ac_timing.ext &= ~GENMASK(7, 1);
			i2c->ac_timing.ext |= (su_sta_cnt << 1) | (1 << 0);
		} else {
			i2c->ac_timing.hs_scl_hl_ratio = (1 << 12) |
				(high_cnt << 6) | low_cnt;
			i2c->ac_timing.hs_sta_stop = (su_sta_cnt << 8) |
				su_sta_cnt;
		}
		i2c->ac_timing.sda_timing &= ~GENMASK(11, 6);
		i2c->ac_timing.sda_timing |= (1 << 12) |
			((sda_max + sda_min) / 2) << 6;
	} else {
		if (i2c->dev_comp->ltiming_adjust) {
			i2c->ac_timing.htiming = (sample_cnt << 8) | (high_cnt);
			i2c->ac_timing.ltiming = (sample_cnt << 6) | (low_cnt);
			i2c->ac_timing.ext = (su_sta_cnt << 8) | (1 << 0);
		} else {
			i2c->ac_timing.scl_hl_ratio = (1 << 12) |
				(high_cnt << 6) | low_cnt;
			i2c->ac_timing.sta_stop = (su_sta_cnt << 8) |
				su_sta_cnt;
		}

		i2c->ac_timing.sda_timing = (1 << 12) |
			(sda_max + sda_min) / 2;
	}

	return 0;
}

/*
 * Calculate i2c port speed
 *
 * Hardware design:
 * i2c_bus_freq = parent_clk / (clock_div * 2 * sample_cnt * step_cnt)
 * clock_div: fixed in hardware, but may be various in different SoCs
 *
 * The calculation want to pick the highest bus frequency that is still
 * less than or equal to i2c->speed_hz. The calculation try to get
 * sample_cnt and step_cn
 */
static int mtk_i2c_calculate_speed(struct mtk_i2c *i2c, unsigned int clk_src,
				   unsigned int target_speed,
				   unsigned int *timing_step_cnt,
				   unsigned int *timing_sample_cnt)
{
	unsigned int step_cnt;
	unsigned int sample_cnt;
	unsigned int max_step_cnt;
	unsigned int base_sample_cnt = MAX_SAMPLE_CNT_DIV;
	unsigned int base_step_cnt;
	unsigned int opt_div;
	unsigned int best_mul;
	unsigned int cnt_mul;
	int ret = -EINVAL;
	int clk_div_restri = 0;

	if (target_speed > I2C_MAX_HIGH_SPEED_MODE_FREQ)
		target_speed = I2C_MAX_HIGH_SPEED_MODE_FREQ;

	max_step_cnt = mtk_i2c_max_step_cnt(target_speed);
	base_step_cnt = max_step_cnt;
	/* Find the best combination */
	opt_div = DIV_ROUND_UP(clk_src >> 1, target_speed);
	best_mul = MAX_SAMPLE_CNT_DIV * max_step_cnt;

	/* Search for the best pair (sample_cnt, step_cnt) with
	 * 0 < sample_cnt < MAX_SAMPLE_CNT_DIV
	 * 0 < step_cnt < max_step_cnt
	 * sample_cnt * step_cnt >= opt_div
	 * optimizing for sample_cnt * step_cnt being minimal
	 */
	for (sample_cnt = 1; sample_cnt <= MAX_SAMPLE_CNT_DIV; sample_cnt++) {
		clk_div_restri = mtk_i2c_get_clk_div_restri(i2c, sample_cnt);
		step_cnt = DIV_ROUND_UP(opt_div + clk_div_restri, sample_cnt);
		cnt_mul = step_cnt * sample_cnt;
		if (step_cnt > max_step_cnt)
			continue;

		if (cnt_mul < best_mul) {
			ret = mtk_i2c_check_ac_timing(i2c, clk_src,
				target_speed, step_cnt - 1, sample_cnt - 1);
			if (ret)
				continue;

			best_mul = cnt_mul;
			base_sample_cnt = sample_cnt;
			base_step_cnt = step_cnt;
			if (best_mul == (opt_div + clk_div_restri))
				break;
		}
	}

	if (ret)
		return -EINVAL;

	sample_cnt = base_sample_cnt;
	step_cnt = base_step_cnt;

	if ((clk_src / (2 * (sample_cnt * step_cnt - clk_div_restri))) >
		target_speed) {
		/* In this case, hardware can't support such
		 * low i2c_bus_freq
		 */
		dev_dbg(i2c->dev, "Unsupported speed (%uhz)\n",	target_speed);
		return -EINVAL;
	}

	*timing_step_cnt = step_cnt - 1;
	*timing_sample_cnt = sample_cnt - 1;

	return 0;
}

static void mtk_i2c_set_speed(struct mtk_i2c *i2c, unsigned int parent_clk)
{
	unsigned int clk_src;
	unsigned int step_cnt;
	unsigned int sample_cnt;
	unsigned int l_step_cnt;
	unsigned int l_sample_cnt;
	unsigned int target_speed;
	unsigned int clk_div;
	unsigned int max_clk_div;
	int ret;

	target_speed = i2c->speed_hz;
	parent_clk /= i2c->clk_src_div;

	if (i2c->dev_comp->timing_adjust && i2c->dev_comp->ltiming_adjust)
		max_clk_div = MAX_CLOCK_DIV_5BITS;
	else if (i2c->dev_comp->timing_adjust)
		max_clk_div = MAX_CLOCK_DIV_8BITS;
	else
		max_clk_div = 1;

	for (clk_div = 1; clk_div <= max_clk_div; clk_div++) {
		clk_src = parent_clk / clk_div;
		i2c->ac_timing.inter_clk_div = clk_div - 1;

		if (target_speed > I2C_MAX_FAST_MODE_PLUS_FREQ) {
			/* Set master code speed register */
			ret = mtk_i2c_calculate_speed(i2c, clk_src,
						      I2C_MAX_FAST_MODE_FREQ,
						      &l_step_cnt,
						      &l_sample_cnt);
			if (ret < 0)
				continue;

			i2c->timing_reg = (l_sample_cnt << 8) | l_step_cnt;

			/* Set the high speed mode register */
			ret = mtk_i2c_calculate_speed(i2c, clk_src,
						      target_speed, &step_cnt,
						      &sample_cnt);
			if (ret < 0)
				continue;

			i2c->high_speed_reg = I2C_TIME_DEFAULT_VALUE |
					(sample_cnt << 12) | (step_cnt << 8);

			if (i2c->dev_comp->ltiming_adjust)
				i2c->ltiming_reg =
					(l_sample_cnt << 6) | l_step_cnt |
					(sample_cnt << 12) | (step_cnt << 9);
		} else {
			ret = mtk_i2c_calculate_speed(i2c, clk_src,
						      target_speed, &l_step_cnt,
						      &l_sample_cnt);
			if (ret < 0)
				continue;

			i2c->timing_reg = (l_sample_cnt << 8) | l_step_cnt;

			/* Disable the high speed transaction */
			i2c->high_speed_reg = I2C_TIME_CLR_VALUE;

			if (i2c->dev_comp->ltiming_adjust)
				i2c->ltiming_reg =
					(l_sample_cnt << 6) | l_step_cnt;
		}

		break;
	}
}

static void i2c_dump_register(struct mtk_i2c *i2c)
{
	dev_dbg(i2c->dev, "SLAVE_ADDR: 0x%x, INTR_MASK: 0x%x\n",
		mtk_i2c_readw(i2c, OFFSET_SLAVE_ADDR),
		mtk_i2c_readw(i2c, OFFSET_INTR_MASK));
	dev_dbg(i2c->dev, "INTR_STAT: 0x%x, CONTROL: 0x%x\n",
		mtk_i2c_readw(i2c, OFFSET_INTR_STAT),
		mtk_i2c_readw(i2c, OFFSET_CONTROL));
	dev_dbg(i2c->dev, "TRANSFER_LEN: 0x%x, TRANSAC_LEN: 0x%x\n",
		mtk_i2c_readw(i2c, OFFSET_TRANSFER_LEN),
		mtk_i2c_readw(i2c, OFFSET_TRANSAC_LEN));
	dev_dbg(i2c->dev, "DELAY_LEN: 0x%x, HTIMING: 0x%x\n",
		mtk_i2c_readw(i2c, OFFSET_DELAY_LEN),
		mtk_i2c_readw(i2c, OFFSET_TIMING));
	dev_dbg(i2c->dev, "START: 0x%x, EXT_CONF: 0x%x\n",
		mtk_i2c_readw(i2c, OFFSET_START),
		mtk_i2c_readw(i2c, OFFSET_EXT_CONF));
	dev_dbg(i2c->dev, "HS: 0x%x, IO_CONFIG: 0x%x\n",
		mtk_i2c_readw(i2c, OFFSET_HS),
		mtk_i2c_readw(i2c, OFFSET_IO_CONFIG));
	dev_dbg(i2c->dev, "DCM_EN: 0x%x, TRANSFER_LEN_AUX: 0x%x\n",
		mtk_i2c_readw(i2c, OFFSET_DCM_EN),
		mtk_i2c_readw(i2c, OFFSET_TRANSFER_LEN_AUX));
	dev_dbg(i2c->dev, "CLOCK_DIV: 0x%x, FIFO_STAT: 0x%x\n",
		mtk_i2c_readw(i2c, OFFSET_CLOCK_DIV),
		mtk_i2c_readw(i2c, OFFSET_FIFO_STAT));
	dev_dbg(i2c->dev, "DEBUGCTRL : 0x%x, DEBUGSTAT: 0x%x\n",
		mtk_i2c_readw(i2c, OFFSET_DEBUGCTRL),
		mtk_i2c_readw(i2c, OFFSET_DEBUGSTAT));
	if (i2c->dev_comp->regs == mt_i2c_regs_v2) {
		dev_dbg(i2c->dev, "LTIMING: 0x%x, MULTI_DMA: 0x%x\n",
			mtk_i2c_readw(i2c, OFFSET_LTIMING),
			mtk_i2c_readw(i2c, OFFSET_MULTI_DMA));
	}
	dev_dbg(i2c->dev, "\nDMA_INT_FLAG: 0x%x, DMA_INT_EN: 0x%x\n",
		readl(i2c->pdmabase + OFFSET_INT_FLAG),
		readl(i2c->pdmabase + OFFSET_INT_EN));
	dev_dbg(i2c->dev, "DMA_EN: 0x%x, DMA_CON: 0x%x\n",
		readl(i2c->pdmabase + OFFSET_EN),
		readl(i2c->pdmabase + OFFSET_CON));
	dev_dbg(i2c->dev, "DMA_TX_MEM_ADDR: 0x%x, DMA_RX_MEM_ADDR: 0x%x\n",
		readl(i2c->pdmabase + OFFSET_TX_MEM_ADDR),
		readl(i2c->pdmabase + OFFSET_RX_MEM_ADDR));
	dev_dbg(i2c->dev, "DMA_TX_LEN: 0x%x, DMA_RX_LEN: 0x%x\n",
		readl(i2c->pdmabase + OFFSET_TX_LEN),
		readl(i2c->pdmabase + OFFSET_RX_LEN));
	dev_dbg(i2c->dev, "DMA_TX_4G_MODE: 0x%x, DMA_RX_4G_MODE: 0x%x",
		readl(i2c->pdmabase + OFFSET_TX_4G_MODE),
		readl(i2c->pdmabase + OFFSET_RX_4G_MODE));
}

static enum mtk_i2c_wrrd_len_mode
mtk_i2c_get_wrrd_len_mode(struct mtk_i2c *i2c)
{
#ifdef CONFIG_I2C_MT65XX_ORION_DIAGNOSTIC
	if (i2c->orion.mode == MTK_I2C_ORION_MODE_AUX_DMA)
		return MTK_I2C_WRRD_LEN_AUX;
	if (i2c->orion.mode == MTK_I2C_ORION_MODE_PACKED_DMA ||
	    i2c->orion.mode == MTK_I2C_ORION_MODE_PACKED_FIFO)
		return MTK_I2C_WRRD_LEN_PACKED_8_5;
#endif

	return i2c->dev_comp->wrrd_len_mode;
}

static bool mtk_i2c_wrrd_len_valid(struct mtk_i2c *i2c,
				   const struct i2c_msg *msgs)
{
	if (mtk_i2c_get_wrrd_len_mode(i2c) == MTK_I2C_WRRD_LEN_AUX)
		return true;

	return msgs->len && (msgs + 1)->len &&
	       msgs->len <= I2C_WRRD_PACKED_TX_MAX &&
	       (msgs + 1)->len <= I2C_WRRD_PACKED_RX_MAX;
}

static int mtk_i2c_program_wrrd_len(struct mtk_i2c *i2c,
				    const struct i2c_msg *msgs)
{
	u16 transfer_len;

	if (mtk_i2c_get_wrrd_len_mode(i2c) == MTK_I2C_WRRD_LEN_AUX) {
		mtk_i2c_writew(i2c, msgs->len, OFFSET_TRANSFER_LEN);
		mtk_i2c_writew(i2c, (msgs + 1)->len,
			       OFFSET_TRANSFER_LEN_AUX);
		return 0;
	}

	if (!mtk_i2c_wrrd_len_valid(i2c, msgs))
		return -EOPNOTSUPP;

	transfer_len = msgs->len | ((msgs + 1)->len << 8);
	mtk_i2c_writew(i2c, transfer_len, OFFSET_TRANSFER_LEN);

	return 0;
}

static bool mtk_i2c_should_use_dma(struct mtk_i2c *i2c,
				   const struct i2c_msg *msgs)
{
#ifdef CONFIG_I2C_MT65XX_ORION_DIAGNOSTIC
	if (i2c->orion.mode == MTK_I2C_ORION_MODE_AUX_DMA ||
	    i2c->orion.mode == MTK_I2C_ORION_MODE_PACKED_DMA)
		return true;
	if (i2c->orion.mode == MTK_I2C_ORION_MODE_PACKED_FIFO)
		return false;
#endif

	if (!i2c->dev_comp->fifo_size ||
	    msgs->len > i2c->dev_comp->fifo_size)
		return true;

	return i2c->op == I2C_MASTER_WRRD &&
	       (msgs + 1)->len > i2c->dev_comp->fifo_size;
}

#ifdef CONFIG_I2C_MT65XX_ORION_DIAGNOSTIC
static void mtk_i2c_orion_record_programmed(struct mtk_i2c *i2c,
					    bool use_dma)
{
	struct mtk_i2c_orion_sample *sample =
		READ_ONCE(i2c->orion.active_sample);

	if (!sample)
		return;

	sample->use_dma = use_dma;
	sample->transfer_len = mtk_i2c_readw(i2c, OFFSET_TRANSFER_LEN);
	sample->transfer_len_aux =
		mtk_i2c_readw(i2c, OFFSET_TRANSFER_LEN_AUX);
	sample->transac_len = mtk_i2c_readw(i2c, OFFSET_TRANSAC_LEN);
	sample->control = mtk_i2c_readw(i2c, OFFSET_CONTROL);
	sample->dma_en_pre = readl(i2c->pdmabase + OFFSET_EN);
	sample->dma_con_pre = readl(i2c->pdmabase + OFFSET_CON);
	sample->dma_int_flag_pre =
		readl(i2c->pdmabase + OFFSET_INT_FLAG);
	sample->dma_tx_len_pre = readl(i2c->pdmabase + OFFSET_TX_LEN);
	sample->dma_rx_len_pre = readl(i2c->pdmabase + OFFSET_RX_LEN);
}

static void mtk_i2c_orion_record_irq(struct mtk_i2c *i2c, u16 irq_stat)
{
	struct mtk_i2c_orion_sample *sample =
		READ_ONCE(i2c->orion.active_sample);

	if (!sample)
		return;

	sample->irq_stat |= irq_stat;
	sample->dma_en_irq = readl(i2c->pdmabase + OFFSET_EN);
	sample->dma_int_flag_irq =
		readl(i2c->pdmabase + OFFSET_INT_FLAG);
}

static void mtk_i2c_orion_record_post(struct mtk_i2c *i2c)
{
	struct mtk_i2c_orion_sample *sample =
		READ_ONCE(i2c->orion.active_sample);

	if (!sample)
		return;

	sample->dma_en_post = readl(i2c->pdmabase + OFFSET_EN);
	sample->dma_con_post = readl(i2c->pdmabase + OFFSET_CON);
	sample->dma_int_flag_post =
		readl(i2c->pdmabase + OFFSET_INT_FLAG);
	sample->dma_tx_len_post = readl(i2c->pdmabase + OFFSET_TX_LEN);
	sample->dma_rx_len_post = readl(i2c->pdmabase + OFFSET_RX_LEN);
}

static void mtk_i2c_orion_record_fifo(struct mtk_i2c *i2c, u16 fifo_count)
{
	struct mtk_i2c_orion_sample *sample =
		READ_ONCE(i2c->orion.active_sample);

	if (sample)
		sample->fifo_count = fifo_count;
}
#else
static void mtk_i2c_orion_record_programmed(struct mtk_i2c *i2c,
					    bool use_dma)
{
}

static void mtk_i2c_orion_record_irq(struct mtk_i2c *i2c, u16 irq_stat)
{
}

static void mtk_i2c_orion_record_post(struct mtk_i2c *i2c)
{
}

static void mtk_i2c_orion_record_fifo(struct mtk_i2c *i2c, u16 fifo_count)
{
}
#endif

#if defined(CONFIG_I2C_MT65XX_QUASAR_DIAGNOSTIC) || \
	defined(CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC)
static void
mtk_i2c_quasar_snapshot(struct mtk_i2c *i2c,
			struct mtk_i2c_quasar_snapshot *snapshot)
{
	snapshot->transfer_len = mtk_i2c_readw(i2c, OFFSET_TRANSFER_LEN);
	snapshot->transfer_len_aux =
		mtk_i2c_readw(i2c, OFFSET_TRANSFER_LEN_AUX);
	snapshot->transac_len = mtk_i2c_readw(i2c, OFFSET_TRANSAC_LEN);
	snapshot->control = mtk_i2c_readw(i2c, OFFSET_CONTROL);
	snapshot->start = mtk_i2c_readw(i2c, OFFSET_START);
	snapshot->intr_stat = mtk_i2c_readw(i2c, OFFSET_INTR_STAT);
	snapshot->fifo_stat = mtk_i2c_readw(i2c, OFFSET_FIFO_STAT);
	snapshot->dma_en = readl(i2c->pdmabase + OFFSET_EN);
	snapshot->dma_con = readl(i2c->pdmabase + OFFSET_CON);
	snapshot->dma_int_flag =
		readl(i2c->pdmabase + OFFSET_INT_FLAG);
	snapshot->dma_tx_len = readl(i2c->pdmabase + OFFSET_TX_LEN);
	snapshot->dma_rx_len = readl(i2c->pdmabase + OFFSET_RX_LEN);
}

static void mtk_i2c_quasar_record_programmed(struct mtk_i2c *i2c,
					      bool use_dma)
{
	struct mtk_i2c_quasar_sample *sample =
		smp_load_acquire(&i2c->quasar.active_sample);

	if (!sample)
		return;

	mtk_i2c_quasar_snapshot(i2c, &sample->pre);
	sample->use_dma = use_dma;
	sample->programmed = true;
}

static void mtk_i2c_quasar_record_irq(struct mtk_i2c *i2c, u16 irq_stat)
{
	struct mtk_i2c_quasar_sample *sample =
		smp_load_acquire(&i2c->quasar.active_sample);

	if (!sample)
		return;

	sample->irq_stat |= irq_stat;
	mtk_i2c_quasar_snapshot(i2c, &sample->irq);
}

static void mtk_i2c_quasar_record_post(struct mtk_i2c *i2c)
{
	struct mtk_i2c_quasar_sample *sample =
		smp_load_acquire(&i2c->quasar.active_sample);

	if (sample)
		mtk_i2c_quasar_snapshot(i2c, &sample->post);
}

static void mtk_i2c_quasar_record_fifo(struct mtk_i2c *i2c, u16 fifo_stat,
					u16 fifo_count)
{
	struct mtk_i2c_quasar_sample *sample =
		smp_load_acquire(&i2c->quasar.active_sample);

	if (!sample)
		return;

	sample->fifo_stat = fifo_stat;
	sample->fifo_count = fifo_count;
}

static void mtk_i2c_quasar_record_drained(struct mtk_i2c *i2c)
{
	struct mtk_i2c_quasar_sample *sample =
		smp_load_acquire(&i2c->quasar.active_sample);

	if (!sample)
		return;

	mtk_i2c_quasar_snapshot(i2c, &sample->drained);
	sample->fifo_count_drained =
		(sample->drained.fifo_stat & I2C_FIFO_RX_COUNT_MASK) >>
		I2C_FIFO_RX_COUNT_SHIFT;
}
#else
static void mtk_i2c_quasar_record_programmed(struct mtk_i2c *i2c,
					      bool use_dma)
{
}

static void mtk_i2c_quasar_record_irq(struct mtk_i2c *i2c, u16 irq_stat)
{
}

static void mtk_i2c_quasar_record_post(struct mtk_i2c *i2c)
{
}

static void mtk_i2c_quasar_record_fifo(struct mtk_i2c *i2c, u16 fifo_stat,
					u16 fifo_count)
{
}

static void mtk_i2c_quasar_record_drained(struct mtk_i2c *i2c)
{
}
#endif

static void mtk_i2c_prepare_pio(struct mtk_i2c *i2c,
				const struct i2c_msg *msgs)
{
	int i;

	if (i2c->op == I2C_MASTER_RD)
		return;

	for (i = 0; i < msgs->len; i++)
		writel(msgs->buf[i],
		       i2c->base + i2c->dev_comp->regs[OFFSET_DATA_PORT]);
}

static int mtk_i2c_complete_pio(struct mtk_i2c *i2c,
				struct i2c_msg *msgs)
{
	struct i2c_msg *rx_msg;
	u16 fifo_count;
	u16 fifo_stat;
	int i;

	if (i2c->op == I2C_MASTER_WR)
		return 0;

	rx_msg = i2c->op == I2C_MASTER_WRRD ? msgs + 1 : msgs;
	fifo_stat = mtk_i2c_readw(i2c, OFFSET_FIFO_STAT);
	fifo_count = (fifo_stat &
		      I2C_FIFO_RX_COUNT_MASK) >> I2C_FIFO_RX_COUNT_SHIFT;
	mtk_i2c_orion_record_fifo(i2c, fifo_count);
	mtk_i2c_quasar_record_fifo(i2c, fifo_stat, fifo_count);
	if (fifo_count != rx_msg->len) {
		dev_err(i2c->dev,
			"FIFO count %u does not match receive length %u\n",
			fifo_count, rx_msg->len);
		return -EIO;
	}

	for (i = 0; i < rx_msg->len; i++)
		rx_msg->buf[i] = (u8)readl(i2c->base +
			i2c->dev_comp->regs[OFFSET_DATA_PORT]);

	mtk_i2c_quasar_record_drained(i2c);
	return 0;
}

static int mtk_i2c_do_transfer(struct mtk_i2c *i2c, struct i2c_msg *msgs,
			       int num, int left_num)
{
	u16 addr_reg;
	u16 start_reg;
	u16 control_reg;
	u16 restart_flag = 0;
	u16 dma_sync = 0;
	u32 reg_4g_mode;
	u32 reg_dma_reset;
	u8 *dma_rd_buf = NULL;
	u8 *dma_wr_buf = NULL;
	dma_addr_t rpaddr = 0;
	dma_addr_t wpaddr = 0;
	bool use_dma;
	int ret;

	i2c->irq_stat = 0;
	use_dma = mtk_i2c_should_use_dma(i2c, msgs);

	if (i2c->auto_restart)
		restart_flag = I2C_RS_TRANSFER;

	reinit_completion(&i2c->msg_complete);

	if (i2c->dev_comp->apdma_sync &&
	    i2c->op != I2C_MASTER_WRRD && num > 1) {
		mtk_i2c_writew(i2c, 0x00, OFFSET_DEBUGCTRL);
		writel(I2C_DMA_HANDSHAKE_RST | I2C_DMA_WARM_RST,
		       i2c->pdmabase + OFFSET_RST);

		ret = readw_poll_timeout(i2c->pdmabase + OFFSET_RST,
					 reg_dma_reset,
					 !(reg_dma_reset & I2C_DMA_WARM_RST),
					 0, 100);
		if (ret) {
			dev_err(i2c->dev, "DMA warm reset timeout\n");
			return -ETIMEDOUT;
		}

		writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_RST);
		mtk_i2c_writew(i2c, I2C_HANDSHAKE_RST, OFFSET_SOFTRESET);
		mtk_i2c_writew(i2c, I2C_CHN_CLR_FLAG, OFFSET_SOFTRESET);
		mtk_i2c_writew(i2c, I2C_RELIABILITY | I2C_DMAACK_ENABLE,
			       OFFSET_DEBUGCTRL);
	}

	control_reg = mtk_i2c_readw(i2c, OFFSET_CONTROL) &
			~(I2C_CONTROL_DIR_CHANGE | I2C_CONTROL_RS |
			  I2C_CONTROL_DMA_EN);
	if (use_dma)
		control_reg |= I2C_CONTROL_DMA_EN;
	if ((i2c->speed_hz > I2C_MAX_FAST_MODE_PLUS_FREQ) || (left_num >= 1))
		control_reg |= I2C_CONTROL_RS;

	if (i2c->op == I2C_MASTER_WRRD)
		control_reg |= I2C_CONTROL_DIR_CHANGE | I2C_CONTROL_RS;

	mtk_i2c_writew(i2c, control_reg, OFFSET_CONTROL);

	addr_reg = i2c_8bit_addr_from_msg(msgs);
	mtk_i2c_writew(i2c, addr_reg, OFFSET_SLAVE_ADDR);

	/* Clear interrupt status */
	mtk_i2c_writew(i2c, restart_flag | I2C_HS_NACKERR | I2C_ACKERR |
			    I2C_ARB_LOST | I2C_TRANSAC_COMP, OFFSET_INTR_STAT);

	mtk_i2c_writew(i2c, I2C_FIFO_ADDR_CLR, OFFSET_FIFO_ADDR_CLR);

	/* Enable interrupt */
	mtk_i2c_writew(i2c, restart_flag | I2C_HS_NACKERR | I2C_ACKERR |
			    I2C_ARB_LOST | I2C_TRANSAC_COMP, OFFSET_INTR_MASK);

	/* Set transfer and transaction len */
	if (i2c->op == I2C_MASTER_WRRD) {
		ret = mtk_i2c_program_wrrd_len(i2c, msgs);
		if (ret)
			return ret;
		mtk_i2c_writew(i2c, I2C_WRRD_TRANAC_VALUE, OFFSET_TRANSAC_LEN);
	} else {
		mtk_i2c_writew(i2c, msgs->len, OFFSET_TRANSFER_LEN);
		mtk_i2c_writew(i2c, num, OFFSET_TRANSAC_LEN);
	}

	if (i2c->dev_comp->apdma_sync) {
		dma_sync = I2C_DMA_SKIP_CONFIG | I2C_DMA_ASYNC_MODE;
		if (i2c->op == I2C_MASTER_WRRD)
			dma_sync |= I2C_DMA_DIR_CHANGE;
	}

	/* Prepare buffer data to start transfer */
	if (!use_dma) {
		mtk_i2c_prepare_pio(i2c, msgs);
	} else if (i2c->op == I2C_MASTER_RD) {
		writel(I2C_DMA_INT_FLAG_NONE, i2c->pdmabase + OFFSET_INT_FLAG);
		writel(I2C_DMA_CON_RX | dma_sync, i2c->pdmabase + OFFSET_CON);

		dma_rd_buf = i2c_get_dma_safe_msg_buf(msgs, 1);
		if (!dma_rd_buf)
			return -ENOMEM;

		rpaddr = dma_map_single(i2c->dev, dma_rd_buf,
					msgs->len, DMA_FROM_DEVICE);
		if (dma_mapping_error(i2c->dev, rpaddr)) {
			i2c_put_dma_safe_msg_buf(dma_rd_buf, msgs, false);

			return -ENOMEM;
		}

		if (i2c->dev_comp->max_dma_support > 32) {
			reg_4g_mode = upper_32_bits(rpaddr);
			writel(reg_4g_mode, i2c->pdmabase + OFFSET_RX_4G_MODE);
		}

		writel((u32)rpaddr, i2c->pdmabase + OFFSET_RX_MEM_ADDR);
		writel(msgs->len, i2c->pdmabase + OFFSET_RX_LEN);
	} else if (i2c->op == I2C_MASTER_WR) {
		writel(I2C_DMA_INT_FLAG_NONE, i2c->pdmabase + OFFSET_INT_FLAG);
		writel(I2C_DMA_CON_TX | dma_sync, i2c->pdmabase + OFFSET_CON);

		dma_wr_buf = i2c_get_dma_safe_msg_buf(msgs, 1);
		if (!dma_wr_buf)
			return -ENOMEM;

		wpaddr = dma_map_single(i2c->dev, dma_wr_buf,
					msgs->len, DMA_TO_DEVICE);
		if (dma_mapping_error(i2c->dev, wpaddr)) {
			i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, false);

			return -ENOMEM;
		}

		if (i2c->dev_comp->max_dma_support > 32) {
			reg_4g_mode = upper_32_bits(wpaddr);
			writel(reg_4g_mode, i2c->pdmabase + OFFSET_TX_4G_MODE);
		}

		writel((u32)wpaddr, i2c->pdmabase + OFFSET_TX_MEM_ADDR);
		writel(msgs->len, i2c->pdmabase + OFFSET_TX_LEN);
	} else {
		writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_INT_FLAG);
		writel(I2C_DMA_CLR_FLAG | dma_sync, i2c->pdmabase + OFFSET_CON);

		dma_wr_buf = i2c_get_dma_safe_msg_buf(msgs, 1);
		if (!dma_wr_buf)
			return -ENOMEM;

		wpaddr = dma_map_single(i2c->dev, dma_wr_buf,
					msgs->len, DMA_TO_DEVICE);
		if (dma_mapping_error(i2c->dev, wpaddr)) {
			i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, false);

			return -ENOMEM;
		}

		dma_rd_buf = i2c_get_dma_safe_msg_buf((msgs + 1), 1);
		if (!dma_rd_buf) {
			dma_unmap_single(i2c->dev, wpaddr,
					 msgs->len, DMA_TO_DEVICE);

			i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, false);

			return -ENOMEM;
		}

		rpaddr = dma_map_single(i2c->dev, dma_rd_buf,
					(msgs + 1)->len,
					DMA_FROM_DEVICE);
		if (dma_mapping_error(i2c->dev, rpaddr)) {
			dma_unmap_single(i2c->dev, wpaddr,
					 msgs->len, DMA_TO_DEVICE);

			i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, false);
			i2c_put_dma_safe_msg_buf(dma_rd_buf, (msgs + 1), false);

			return -ENOMEM;
		}

		if (i2c->dev_comp->max_dma_support > 32) {
			reg_4g_mode = upper_32_bits(wpaddr);
			writel(reg_4g_mode, i2c->pdmabase + OFFSET_TX_4G_MODE);

			reg_4g_mode = upper_32_bits(rpaddr);
			writel(reg_4g_mode, i2c->pdmabase + OFFSET_RX_4G_MODE);
		}

		writel((u32)wpaddr, i2c->pdmabase + OFFSET_TX_MEM_ADDR);
		writel((u32)rpaddr, i2c->pdmabase + OFFSET_RX_MEM_ADDR);
		writel(msgs->len, i2c->pdmabase + OFFSET_TX_LEN);
		writel((msgs + 1)->len, i2c->pdmabase + OFFSET_RX_LEN);
	}

	mtk_i2c_orion_record_programmed(i2c, use_dma);
	mtk_i2c_quasar_record_programmed(i2c, use_dma);
	if (use_dma) {
		if (i2c->dvfsp_handoff)
			atomic_inc(&i2c->dma_start_count);
		writel(I2C_DMA_START_EN, i2c->pdmabase + OFFSET_EN);
	}

	if (!i2c->auto_restart) {
		start_reg = I2C_TRANSAC_START;
	} else {
		start_reg = I2C_TRANSAC_START | I2C_RS_MUL_TRIG;
		if (left_num >= 1)
			start_reg |= I2C_RS_MUL_CNFG;
	}
	if (i2c->dvfsp_handoff)
		atomic_inc(&i2c->nonzero_start_count);
	mtk_i2c_writew(i2c, start_reg, OFFSET_START);

	ret = wait_for_completion_timeout(&i2c->msg_complete,
					  i2c->adap.timeout);
	mtk_i2c_orion_record_post(i2c);
	mtk_i2c_quasar_record_post(i2c);

	/* Clear interrupt mask */
	mtk_i2c_writew(i2c, ~(restart_flag | I2C_HS_NACKERR | I2C_ACKERR |
			    I2C_ARB_LOST | I2C_TRANSAC_COMP), OFFSET_INTR_MASK);

	if (use_dma && i2c->op == I2C_MASTER_WR) {
		dma_unmap_single(i2c->dev, wpaddr,
				 msgs->len, DMA_TO_DEVICE);

		i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, true);
	} else if (use_dma && i2c->op == I2C_MASTER_RD) {
		dma_unmap_single(i2c->dev, rpaddr,
				 msgs->len, DMA_FROM_DEVICE);

		i2c_put_dma_safe_msg_buf(dma_rd_buf, msgs, true);
	} else if (use_dma) {
		dma_unmap_single(i2c->dev, wpaddr, msgs->len,
				 DMA_TO_DEVICE);
		dma_unmap_single(i2c->dev, rpaddr, (msgs + 1)->len,
				 DMA_FROM_DEVICE);

		i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, true);
		i2c_put_dma_safe_msg_buf(dma_rd_buf, (msgs + 1), true);
	}

	if (i2c->dev_comp == &mt6797_idvfs_compat &&
	    i2c->irq_stat & I2C_ARB_LOST) {
		dev_dbg(i2c->dev, "addr: %x, arbitration lost\n", msgs->addr);
		mtk_i2c_init_hw(i2c);
		return -EAGAIN;
	}

	if (ret == 0) {
		dev_dbg(i2c->dev, "addr: %x, transfer timeout\n", msgs->addr);
		i2c_dump_register(i2c);
		mtk_i2c_init_hw(i2c);
		return -ETIMEDOUT;
	}

	if (i2c->irq_stat & (I2C_HS_NACKERR | I2C_ACKERR)) {
		dev_dbg(i2c->dev, "addr: %x, transfer ACK error\n", msgs->addr);
		mtk_i2c_init_hw(i2c);
		return -ENXIO;
	}

	if (i2c->dev_comp == &mt6797_idvfs_compat &&
	    i2c->irq_stat != I2C_TRANSAC_COMP) {
		dev_dbg(i2c->dev, "addr: %x, incomplete transaction: 0x%x\n",
			msgs->addr, i2c->irq_stat);
		mtk_i2c_init_hw(i2c);
		return -EIO;
	}

	if (!use_dma) {
		ret = mtk_i2c_complete_pio(i2c, msgs);
		if (ret) {
			mtk_i2c_init_hw(i2c);
			return ret;
		}
	}

	return 0;
}

#ifdef CONFIG_I2C_MT65XX_GEMINI_LIFECYCLE_ORACLE
static void
mtk_i2c_record_lifecycle_oracle(struct mtk_i2c *i2c,
				struct i2c_msg *msgs, int num)
{
	bool primary;
	bool page2;
	int i;

	if (i2c->dev_comp != &mt6797_idvfs_compat)
		return;

	if (num <= 0) {
		atomic_inc(&i2c->oracle_other_transfers);
		return;
	}

	primary = msgs[0].addr == 0x68;
	page2 = msgs[0].addr == 0x69;
	if (num == 2 && (primary || page2) &&
	    !(msgs[0].flags & I2C_M_RD) && msgs[0].len == 1 &&
	    msgs[1].addr == msgs[0].addr && (msgs[1].flags & I2C_M_RD) &&
	    msgs[1].len == 1) {
		atomic_inc(&i2c->oracle_combined_pointer_reads);
		if (primary)
			atomic_inc(&i2c->oracle_primary_pointer_reads);
		else
			atomic_inc(&i2c->oracle_page2_pointer_reads);
	} else {
		atomic_inc(&i2c->oracle_other_transfers);
	}

	for (i = 0; i < num; i++) {
		if (!(msgs[i].flags & I2C_M_RD)) {
			if (num == 1)
				atomic_inc(&i2c->oracle_write_only_messages);
			if (msgs[i].len > 1)
				atomic_inc(&i2c->oracle_register_data_write_messages);
		}
		if (msgs[i].addr != 0x68 && msgs[i].addr != 0x69)
			atomic_inc(&i2c->oracle_other_address_transfers);
	}
}
#else
static void
mtk_i2c_record_lifecycle_oracle(struct mtk_i2c *i2c,
				struct i2c_msg *msgs, int num)
{
}
#endif

static int mtk_i2c_transfer(struct i2c_adapter *adap,
			    struct i2c_msg msgs[], int num)
{
	int ret;
	int lease_ret;
	int left_num = num;
	bool write_then_read_en = false;
	bool lease_active = false;
	struct mtk_i2c *i2c = i2c_get_adapdata(adap);
	struct mt6797_dvfsp_i2c6_lease lease = { };

	mtk_i2c_record_lifecycle_oracle(i2c, msgs, num);
	if (i2c->dvfsp_handoff) {
		atomic_inc(&i2c->transfer_attempts);
		ret = mt6797_dvfsp_handoff_require_ready(
			i2c->dvfsp_handoff);
		if (ret)
			return ret;
		ret = mt6797_dvfsp_handoff_begin_i2c6_transfer(
			i2c->dvfsp_handoff, &lease);
		if (ret)
			return ret;
		lease_active = true;
	}

	if (i2c->adap.bus_regulator) {
		ret = regulator_enable(i2c->adap.bus_regulator);
		if (ret)
			goto err_lease;
	}

	ret = clk_bulk_enable(I2C_MT65XX_CLK_MAX, i2c->clocks);
	if (ret)
		goto err_regulator;

#ifdef CONFIG_I2C_MT65XX_ORION_DIAGNOSTIC
	if (i2c->orion.reset_pending) {
		mtk_i2c_init_hw(i2c);
		i2c->orion.reset_pending = false;
	}
#endif

	i2c->auto_restart = i2c->dev_comp->auto_restart;

	/* checking if we can skip restart and optimize using WRRD mode */
	if (i2c->auto_restart && num == 2) {
		if (!(msgs[0].flags & I2C_M_RD) && (msgs[1].flags & I2C_M_RD) &&
		    msgs[0].addr == msgs[1].addr &&
		    mtk_i2c_wrrd_len_valid(i2c, msgs)) {
			i2c->auto_restart = 0;
			write_then_read_en = true;
		}
	}

	if (i2c->auto_restart && num >= 2 &&
		i2c->speed_hz > I2C_MAX_FAST_MODE_PLUS_FREQ)
		/* ignore the first restart irq after the master code,
		 * otherwise the first transfer will be discarded.
		 */
		i2c->ignore_restart_irq = true;
	else
		i2c->ignore_restart_irq = false;

	while (left_num--) {
		if (!msgs->buf) {
			dev_dbg(i2c->dev, "data buffer is NULL.\n");
			ret = -EINVAL;
			goto err_exit;
		}

		if (msgs->flags & I2C_M_RD)
			i2c->op = I2C_MASTER_RD;
		else
			i2c->op = I2C_MASTER_WR;

		if (write_then_read_en) {
			/* combined two messages into one transaction */
			i2c->op = I2C_MASTER_WRRD;
			left_num--;
		}

		ret = mtk_i2c_do_transfer(i2c, msgs, num, left_num);
		if (ret < 0)
			goto err_exit;

		if (i2c->op == I2C_MASTER_WRRD)
			msgs += 2;
		else
			msgs++;
	}
	/* the return value is number of executed messages */
	ret = num;

err_exit:
	clk_bulk_disable(I2C_MT65XX_CLK_MAX, i2c->clocks);
err_regulator:
	if (i2c->adap.bus_regulator)
		regulator_disable(i2c->adap.bus_regulator);

err_lease:
	if (lease_active) {
		lease_ret = mt6797_dvfsp_handoff_end_i2c6_transfer(
			i2c->dvfsp_handoff, &lease, ret);
		if (!ret && lease_ret)
			ret = lease_ret;
	}

	return ret;
}

static irqreturn_t mtk_i2c_irq(int irqno, void *dev_id)
{
	struct mtk_i2c *i2c = dev_id;
	u16 restart_flag = i2c->auto_restart ? I2C_RS_TRANSFER : 0;
	u16 intr_stat;

	if (i2c->dvfsp_handoff)
		atomic_inc(&i2c->irq_count);

	intr_stat = mtk_i2c_readw(i2c, OFFSET_INTR_STAT);
	mtk_i2c_orion_record_irq(i2c, intr_stat);
	mtk_i2c_quasar_record_irq(i2c, intr_stat);
	mtk_i2c_writew(i2c, intr_stat, OFFSET_INTR_STAT);

	/*
	 * when occurs ack error, i2c controller generate two interrupts
	 * first is the ack error interrupt, then the complete interrupt
	 * i2c->irq_stat need keep the two interrupt value.
	 */
	i2c->irq_stat |= intr_stat;

	if (i2c->ignore_restart_irq && (i2c->irq_stat & restart_flag)) {
		i2c->ignore_restart_irq = false;
		i2c->irq_stat = 0;
		if (i2c->dvfsp_handoff)
			atomic_inc(&i2c->nonzero_start_count);
		mtk_i2c_writew(i2c, I2C_RS_MUL_CNFG | I2C_RS_MUL_TRIG |
				    I2C_TRANSAC_START, OFFSET_START);
	} else {
		if (i2c->irq_stat & (I2C_TRANSAC_COMP | restart_flag))
			complete(&i2c->msg_complete);
	}

	return IRQ_HANDLED;
}

static u32 mtk_i2c_functionality(struct i2c_adapter *adap)
{
	if (i2c_check_quirks(adap, I2C_AQ_NO_ZERO_LEN))
		return I2C_FUNC_I2C |
			(I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
	else
		return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm mtk_i2c_algorithm = {
	.xfer = mtk_i2c_transfer,
	.functionality = mtk_i2c_functionality,
};

static int mtk_i2c_parse_dt(struct device_node *np, struct mtk_i2c *i2c)
{
	int ret;

	ret = of_property_read_u32(np, "clock-frequency", &i2c->speed_hz);
	if (ret < 0)
		i2c->speed_hz = I2C_MAX_STANDARD_MODE_FREQ;

	ret = of_property_read_u32(np, "clock-div", &i2c->clk_src_div);
	if (ret < 0)
		return ret;

	if (i2c->clk_src_div == 0)
		return -EINVAL;

	i2c->have_pmic = of_property_read_bool(np, "mediatek,have-pmic");
	i2c->use_push_pull =
		of_property_read_bool(np, "mediatek,use-push-pull");

	i2c_parse_fw_timings(i2c->dev, &i2c->timing_info, true);

	return 0;
}

static ssize_t handoff_status_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mtk_i2c *i2c = dev_get_drvdata(dev);
	bool ready;
	ssize_t len;

	ready = !mt6797_dvfsp_handoff_require_ready(i2c->dvfsp_handoff);

	len = sysfs_emit(buf,
			 "handoff=%s probe_attempts=%d init_attempts=%d "
		"init_successes=%d clock_ungated_checks=%d "
		"clock_gated_checks=%d clock_validation_failures=%d "
		"runtime_pm_link=%d clock_domains=i2c-appm,ap-dma "
		"transfer_attempts=%d dma_starts=%d nonzero_starts=%d "
		"irq_count=%d suspend_checks=%d resume_checks=%d "
		"resume_failures=%d\n",
		ready ? "ready" : "not-ready",
		atomic_read(&mtk_i2c_handoff_probe_attempts),
		atomic_read(&i2c->init_attempts),
		atomic_read(&i2c->init_successes),
		atomic_read(&i2c->clock_ungated_checks),
		atomic_read(&i2c->clock_gated_checks),
		atomic_read(&i2c->clock_validation_failures),
		i2c->handoff_runtime_pm_link,
		atomic_read(&i2c->transfer_attempts),
		atomic_read(&i2c->dma_start_count),
		atomic_read(&i2c->nonzero_start_count),
		atomic_read(&i2c->irq_count),
		atomic_read(&i2c->suspend_checks),
		atomic_read(&i2c->resume_checks),
		atomic_read(&i2c->resume_failures));
#ifdef CONFIG_I2C_MT65XX_GEMINI_LIFECYCLE_ORACLE
	len += sysfs_emit_at(buf, len, "oracle_combined_pointer_reads=%d\n",
			     atomic_read(&i2c->oracle_combined_pointer_reads));
	len += sysfs_emit_at(buf, len, "oracle_primary_pointer_reads=%d\n",
			     atomic_read(&i2c->oracle_primary_pointer_reads));
	len += sysfs_emit_at(buf, len, "oracle_page2_pointer_reads=%d\n",
			     atomic_read(&i2c->oracle_page2_pointer_reads));
	len += sysfs_emit_at(buf, len, "oracle_write_only_messages=%d\n",
			     atomic_read(&i2c->oracle_write_only_messages));
	len += sysfs_emit_at(buf, len,
			     "oracle_register_data_write_messages=%d\n",
			     atomic_read(&i2c->oracle_register_data_write_messages));
	len += sysfs_emit_at(buf, len, "oracle_other_transfers=%d\n",
			     atomic_read(&i2c->oracle_other_transfers));
	len += sysfs_emit_at(buf, len,
			     "oracle_other_address_transfers=%d\n",
			     atomic_read(&i2c->oracle_other_address_transfers));
#endif
	return len;
}
static DEVICE_ATTR_RO(handoff_status);

static struct attribute *mtk_i2c_handoff_attrs[] = {
	&dev_attr_handoff_status.attr,
	NULL,
};

static const struct attribute_group mtk_i2c_handoff_group = {
	.attrs = mtk_i2c_handoff_attrs,
};

#ifdef CONFIG_I2C_MT65XX_ORION_DIAGNOSTIC
#define MTK_I2C_ORION_RESULT_SIZE	8192

static const u8 mtk_i2c_orion_registers[MTK_I2C_ORION_REGISTER_COUNT] = {
	0x05, 0x06, 0x47,
};

static const u8 mtk_i2c_orion_prefills[MTK_I2C_ORION_REGISTER_COUNT] = {
	0xa5, 0x5a, 0x3c,
};

static const enum mtk_i2c_orion_mode
mtk_i2c_orion_modes[MTK_I2C_ORION_MODE_COUNT] = {
	MTK_I2C_ORION_MODE_PACKED_FIFO,
	MTK_I2C_ORION_MODE_PACKED_DMA,
	MTK_I2C_ORION_MODE_AUX_DMA,
};

static const char *mtk_i2c_orion_mode_name(enum mtk_i2c_orion_mode mode)
{
	switch (mode) {
	case MTK_I2C_ORION_MODE_AUX_DMA:
		return "aux-dma";
	case MTK_I2C_ORION_MODE_PACKED_DMA:
		return "packed-dma";
	case MTK_I2C_ORION_MODE_PACKED_FIFO:
		return "packed-fifo";
	default:
		return "none";
	}
}

static const char *mtk_i2c_orion_state_name(enum mtk_i2c_orion_state state)
{
	switch (state) {
	case MTK_I2C_ORION_READY:
		return "ready";
	case MTK_I2C_ORION_RUNNING:
		return "running";
	case MTK_I2C_ORION_DONE:
		return "done";
	default:
		return "invalid";
	}
}

static int mtk_i2c_orion_child_present(struct device *dev, void *data)
{
	return 1;
}

static int mtk_i2c_orion_run(struct mtk_i2c *i2c)
{
	unsigned int mode_index;
	unsigned int reg_index;
	int ret = 0;

	i2c_lock_bus(&i2c->adap, I2C_LOCK_ROOT_ADAPTER);

	if (device_for_each_child(&i2c->adap.dev, NULL,
				  mtk_i2c_orion_child_present)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	if (mt6797_dvfsp_handoff_require_ready(i2c->dvfsp_handoff)) {
		ret = -EHOSTDOWN;
		goto out_unlock;
	}

	if (atomic_read(&i2c->transfer_attempts) ||
	    atomic_read(&i2c->dma_start_count) ||
	    atomic_read(&i2c->nonzero_start_count) ||
	    atomic_read(&i2c->irq_count)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	i2c->orion.retries_before = i2c->adap.retries;
	i2c->adap.retries = 0;
	i2c->orion.retries_during = i2c->adap.retries;

	for (mode_index = 0; mode_index < MTK_I2C_ORION_MODE_COUNT;
	     mode_index++) {
		i2c->orion.mode = mtk_i2c_orion_modes[mode_index];
		i2c->orion.reset_pending = true;

		for (reg_index = 0;
		     reg_index < MTK_I2C_ORION_REGISTER_COUNT;
		     reg_index++) {
			struct mtk_i2c_orion_sample *sample;
			struct i2c_msg msgs[2];
			u8 pointer = mtk_i2c_orion_registers[reg_index];
			u8 value = mtk_i2c_orion_prefills[reg_index];

			sample = &i2c->orion.samples[i2c->orion.attempted++];
			memset(sample, 0, sizeof(*sample));
			sample->mode = i2c->orion.mode;
			sample->reg = pointer;
			sample->prefill = value;
			sample->value = value;
			sample->fifo_count = 0xffff;

			msgs[0].addr = 0x69;
			msgs[0].flags = 0;
			msgs[0].len = 1;
			msgs[0].buf = &pointer;
			msgs[1].addr = 0x69;
			msgs[1].flags = I2C_M_RD;
			msgs[1].len = 1;
			msgs[1].buf = &value;

			WRITE_ONCE(i2c->orion.active_sample, sample);
			ret = __i2c_transfer(&i2c->adap, msgs, ARRAY_SIZE(msgs));
			WRITE_ONCE(i2c->orion.active_sample, NULL);
			sample->ret = ret;
			sample->value = value;
			if (ret != 2) {
				if (ret >= 0)
					ret = -EIO;
				goto out_restore_retries;
			}
			i2c->orion.completed++;
		}
	}
	ret = 0;

out_restore_retries:
	i2c->adap.retries = i2c->orion.retries_before;
	i2c->orion.retries_after = i2c->adap.retries;
out_unlock:
	WRITE_ONCE(i2c->orion.active_sample, NULL);
	i2c->orion.mode = MTK_I2C_ORION_MODE_NONE;
	i2c->orion.reset_pending = false;
	i2c_unlock_bus(&i2c->adap, I2C_LOCK_ROOT_ADAPTER);

	return ret;
}

static ssize_t mtk_i2c_orion_read(struct file *file, char __user *user_buf,
				  size_t count, loff_t *ppos)
{
	struct mtk_i2c *i2c = file->private_data;
	char *buf;
	unsigned int i;
	int len = 0;
	ssize_t ret;

	buf = kzalloc(MTK_I2C_ORION_RESULT_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&i2c->orion.lock);
	len += scnprintf(buf + len, MTK_I2C_ORION_RESULT_SIZE - len,
		"candidate=Orion state=%s one_shot=%s run_error=%d ",
		mtk_i2c_orion_state_name(i2c->orion.state),
		i2c->orion.state == MTK_I2C_ORION_READY ? "unused" : "consumed",
		i2c->orion.run_error);
	len += scnprintf(buf + len, MTK_I2C_ORION_RESULT_SIZE - len,
		"attempted=%u completed=%u address=0x69 ",
		i2c->orion.attempted, i2c->orion.completed);
	len += scnprintf(buf + len, MTK_I2C_ORION_RESULT_SIZE - len,
		"transfer_attempts=%d dma_starts=%d nonzero_starts=%d irqs=%d ",
		atomic_read(&i2c->transfer_attempts),
		atomic_read(&i2c->dma_start_count),
		atomic_read(&i2c->nonzero_start_count),
		atomic_read(&i2c->irq_count));
	len += scnprintf(buf + len, MTK_I2C_ORION_RESULT_SIZE - len,
		"retries_before=%u retries_during=%u retries_after=%u ",
		i2c->orion.retries_before,
		i2c->orion.retries_during, i2c->orion.retries_after);
	len += scnprintf(buf + len, MTK_I2C_ORION_RESULT_SIZE - len,
		"registers=05,06,47 modes=packed-fifo,packed-dma,aux-dma\n");

	for (i = 0; i < i2c->orion.attempted; i++) {
		const struct mtk_i2c_orion_sample *sample =
			&i2c->orion.samples[i];

		len += scnprintf(buf + len,
			MTK_I2C_ORION_RESULT_SIZE - len,
			"sample=%u mode=%s register=%02x pre=%02x post=%02x ",
			i, mtk_i2c_orion_mode_name(sample->mode),
			sample->reg, sample->prefill, sample->value);
		len += scnprintf(buf + len,
			MTK_I2C_ORION_RESULT_SIZE - len,
			"ret=%d engine=%s transfer_len=%04x transfer_len_aux=%04x ",
			sample->ret, sample->use_dma ? "dma" : "fifo",
			sample->transfer_len, sample->transfer_len_aux);
		len += scnprintf(buf + len,
			MTK_I2C_ORION_RESULT_SIZE - len,
			"transac_len=%04x control=%04x irq_stat=%04x fifo_count=%u ",
			sample->transac_len, sample->control,
			sample->irq_stat, sample->fifo_count);
		len += scnprintf(buf + len,
			MTK_I2C_ORION_RESULT_SIZE - len,
			"dma_en_pre=%08x dma_con_pre=%08x dma_int_flag_pre=%08x ",
			sample->dma_en_pre, sample->dma_con_pre,
			sample->dma_int_flag_pre);
		len += scnprintf(buf + len,
			MTK_I2C_ORION_RESULT_SIZE - len,
			"dma_tx_len_pre=%08x dma_rx_len_pre=%08x dma_en_irq=%08x ",
			sample->dma_tx_len_pre, sample->dma_rx_len_pre,
			sample->dma_en_irq);
		len += scnprintf(buf + len,
			MTK_I2C_ORION_RESULT_SIZE - len,
			"dma_int_flag_irq=%08x dma_en_post=%08x dma_con_post=%08x ",
			sample->dma_int_flag_irq, sample->dma_en_post,
			sample->dma_con_post);
		len += scnprintf(buf + len,
			MTK_I2C_ORION_RESULT_SIZE - len,
			"dma_int_flag_post=%08x dma_tx_len_post=%08x dma_rx_len_post=%08x\n",
			sample->dma_int_flag_post,
			sample->dma_tx_len_post, sample->dma_rx_len_post);
	}
	mutex_unlock(&i2c->orion.lock);

	ret = simple_read_from_buffer(user_buf, count, ppos, buf, len);
	kfree(buf);

	return ret;
}

static ssize_t mtk_i2c_orion_write(struct file *file,
				   const char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	struct mtk_i2c *i2c = file->private_data;
	char command[4];
	int ret;

	if (*ppos || count != sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, user_buf, sizeof(command)))
		return -EFAULT;
	if (memcmp(command, "run\n", sizeof(command)))
		return -EINVAL;

	mutex_lock(&i2c->orion.lock);
	if (i2c->orion.state != MTK_I2C_ORION_READY) {
		ret = -EBUSY;
		goto out_unlock;
	}

	i2c->orion.state = MTK_I2C_ORION_RUNNING;
	ret = mtk_i2c_orion_run(i2c);
	i2c->orion.run_error = ret;
	i2c->orion.state = MTK_I2C_ORION_DONE;
	if (!ret) {
		*ppos += count;
		ret = count;
	}

out_unlock:
	mutex_unlock(&i2c->orion.lock);
	return ret;
}

static const struct file_operations mtk_i2c_orion_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mtk_i2c_orion_read,
	.write = mtk_i2c_orion_write,
	.llseek = default_llseek,
};

static int mtk_i2c_orion_init(struct mtk_i2c *i2c)
{
	struct device_node *target;
	struct dentry *file;
	int ret;

	if (i2c->dev_comp != &mt6797_idvfs_compat)
		return 0;
	if (!i2c->dvfsp_handoff)
		return dev_err_probe(i2c->dev, -EINVAL,
				     "GEMINI_VEGA_DIAGNOSTIC_GATE missing DVFSP handoff\n");

	target = of_find_node_by_path("/i2c@1100e000");
	if (!target)
		return dev_err_probe(i2c->dev, -ENODEV,
				     "GEMINI_VEGA_DIAGNOSTIC_GATE target node unavailable\n");
	if (i2c->dev->of_node != target) {
		of_node_put(target);
		return dev_err_probe(i2c->dev, -EINVAL,
				     "GEMINI_VEGA_DIAGNOSTIC_GATE node identity mismatch\n");
	}
	of_node_put(target);

	if (!i2c->adap.debugfs)
		return dev_err_probe(i2c->dev, -ENODEV,
				     "GEMINI_VEGA_DIAGNOSTIC_GATE adapter debugfs unavailable\n");

	mutex_init(&i2c->orion.lock);
	i2c->orion.state = MTK_I2C_ORION_READY;
	i2c->orion.retries_before = i2c->adap.retries;
	i2c->orion.retries_during = i2c->adap.retries;
	i2c->orion.retries_after = i2c->adap.retries;
	file = debugfs_create_file("orion-run-all", 0600, i2c->adap.debugfs,
				   i2c, &mtk_i2c_orion_fops);
	if (IS_ERR_OR_NULL(file)) {
		ret = file ? PTR_ERR(file) : -ENOMEM;
		return dev_err_probe(i2c->dev, ret,
				     "GEMINI_VEGA_DIAGNOSTIC_GATE debugfs creation failed\n");
	}

	dev_info(i2c->dev,
		 "GEMINI_ORION_DIAGNOSTIC state=ready one_shot=unused\n");
	return 0;
}
#else
static int mtk_i2c_orion_init(struct mtk_i2c *i2c)
{
	return 0;
}
#endif

#if defined(CONFIG_I2C_MT65XX_QUASAR_DIAGNOSTIC) || \
	defined(CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC)
#ifdef CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC
#define MTK_I2C_QUASAR_RESULT_SIZE	32768
#else
#define MTK_I2C_QUASAR_RESULT_SIZE	16384
#endif

#ifdef CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC
static const u16
mtk_i2c_quasar_addresses[MTK_I2C_QUASAR_REGISTER_COUNT] = {
	0x69, 0x69, 0x69, 0x68, 0x68, 0x68, 0x68,
};

static const u8
mtk_i2c_quasar_registers[MTK_I2C_QUASAR_REGISTER_COUNT] = {
	0x05, 0x06, 0x47, 0xd3, 0x5e, 0xd9, 0xda,
};

static const u8
mtk_i2c_quasar_expected[MTK_I2C_QUASAR_REGISTER_COUNT] = {
	0xd9, 0xd0, 0xc0, 0x1f, 0x00, 0x00, 0x00,
};

static const u8
mtk_i2c_quasar_prefills[MTK_I2C_QUASAR_PASS_COUNT]
			 [MTK_I2C_QUASAR_REGISTER_COUNT] = {
	{ 0xa5, 0x5a, 0x3c, 0x96, 0x69, 0xc3, 0x87 },
	{ 0x78, 0xb4, 0x4b, 0xd2, 0x2d, 0xe1, 0x1e },
};

#define MTK_I2C_QUASAR_CANDIDATE	"Gauss"
#define MTK_I2C_QUASAR_DEBUGFS_FILE	"fermi-run-native"
#define MTK_I2C_QUASAR_GATE		"GEMINI_FERMI_DIAGNOSTIC_GATE"
#define MTK_I2C_QUASAR_READY_MARKER	"GEMINI_FERMI_NATIVE_DIAGNOSTIC"
#define MTK_I2C_QUASAR_COUNTER_CONTRACT	"14,0,14,14"
#define mtk_i2c_quasar_read		mtk_i2c_fermi_read
#define mtk_i2c_quasar_write		mtk_i2c_fermi_write
#define mtk_i2c_quasar_fops		mtk_i2c_fermi_fops
#define mtk_i2c_quasar_init		mtk_i2c_fermi_init
#else
static const u16
mtk_i2c_quasar_addresses[MTK_I2C_QUASAR_REGISTER_COUNT] = {
	0x69, 0x69, 0x69,
};

static const u8
mtk_i2c_quasar_registers[MTK_I2C_QUASAR_REGISTER_COUNT] = {
	0x05, 0x06, 0x47,
};

static const u8
mtk_i2c_quasar_expected[MTK_I2C_QUASAR_REGISTER_COUNT] = {
	0xd9, 0xd0, 0xc0,
};

static const u8
mtk_i2c_quasar_prefills[MTK_I2C_QUASAR_PASS_COUNT]
			 [MTK_I2C_QUASAR_REGISTER_COUNT] = {
	{ 0xa5, 0x5a, 0x3c },
	{ 0x96, 0x69, 0xc3 },
};

#define MTK_I2C_QUASAR_CANDIDATE	"Quasar"
#define MTK_I2C_QUASAR_DEBUGFS_FILE	"quasar-run-native"
#define MTK_I2C_QUASAR_GATE		"GEMINI_QUASAR_DIAGNOSTIC_GATE"
#define MTK_I2C_QUASAR_READY_MARKER	"GEMINI_QUASAR_NATIVE_DIAGNOSTIC"
#define MTK_I2C_QUASAR_COUNTER_CONTRACT	"6,0,6,6"
#endif

static const char *
mtk_i2c_quasar_state_name(enum mtk_i2c_quasar_state state)
{
	switch (state) {
	case MTK_I2C_QUASAR_READY:
		return "ready";
	case MTK_I2C_QUASAR_RUNNING:
		return "running";
	case MTK_I2C_QUASAR_DONE:
		return "done";
	default:
		return "invalid";
	}
}

static int mtk_i2c_quasar_child_present(struct device *dev, void *data)
{
	return 1;
}

static void mtk_i2c_quasar_record_counters(struct mtk_i2c *i2c, bool before)
{
	struct mtk_i2c_quasar_diag *diag = &i2c->quasar;

	if (before) {
		diag->init_attempts_before = atomic_read(&i2c->init_attempts);
		diag->init_successes_before = atomic_read(&i2c->init_successes);
		diag->transfer_attempts_before =
			atomic_read(&i2c->transfer_attempts);
		diag->dma_starts_before = atomic_read(&i2c->dma_start_count);
		diag->nonzero_starts_before =
			atomic_read(&i2c->nonzero_start_count);
		diag->irqs_before = atomic_read(&i2c->irq_count);
	} else {
		diag->init_attempts_after = atomic_read(&i2c->init_attempts);
		diag->init_successes_after = atomic_read(&i2c->init_successes);
		diag->transfer_attempts_after =
			atomic_read(&i2c->transfer_attempts);
		diag->dma_starts_after = atomic_read(&i2c->dma_start_count);
		diag->nonzero_starts_after =
			atomic_read(&i2c->nonzero_start_count);
		diag->irqs_after = atomic_read(&i2c->irq_count);
	}
}

static int
mtk_i2c_quasar_validate_sample(struct mtk_i2c *i2c,
			       struct mtk_i2c_quasar_sample *sample,
			       unsigned int ordinal)
{
	struct mtk_i2c_quasar_diag *diag = &i2c->quasar;

	if (!sample->programmed ||
	    sample->use_dma ||
	    sample->pre.transfer_len != 0x0101 ||
	    sample->pre.transfer_len_aux ||
	    sample->pre.transac_len != I2C_WRRD_TRANAC_VALUE ||
	    sample->pre.control != 0x003a ||
	    sample->pre.control & I2C_CONTROL_DMA_EN ||
	    sample->irq_stat != I2C_TRANSAC_COMP ||
	    sample->irq.intr_stat != I2C_TRANSAC_COMP ||
	    sample->fifo_count != 1 ||
	    sample->pre.dma_en || sample->pre.dma_con ||
	    sample->pre.dma_int_flag || sample->pre.dma_tx_len ||
	    sample->pre.dma_rx_len ||
	    sample->irq.dma_en || sample->irq.dma_con ||
	    sample->irq.dma_int_flag || sample->irq.dma_tx_len ||
	    sample->irq.dma_rx_len ||
	    sample->post.dma_en || sample->post.dma_con ||
	    sample->post.dma_int_flag || sample->post.dma_tx_len ||
	    sample->post.dma_rx_len ||
	    sample->fifo_count_drained ||
	    sample->drained.dma_en || sample->drained.dma_con ||
	    sample->drained.dma_int_flag || sample->drained.dma_tx_len ||
	    sample->drained.dma_rx_len)
		return -EPROTO;

	mtk_i2c_quasar_record_counters(i2c, false);
	if (diag->init_attempts_after != diag->init_attempts_before ||
	    diag->init_successes_after != diag->init_successes_before ||
	    diag->transfer_attempts_after != ordinal ||
	    diag->dma_starts_after ||
	    diag->nonzero_starts_after != ordinal ||
	    diag->irqs_after != ordinal)
		return -EPROTO;

#ifdef CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC
	if (sample->value == sample->prefill)
		return -ENODATA;

	if (sample->index < 3) {
		if (sample->value != sample->expected)
			return -EUCLEAN;
	} else {
		if (sample->index == 3 && sample->value != sample->expected)
			return -EUCLEAN;
		if (sample->pass == 1) {
			if (sample->value !=
			    diag->samples[sample->index].value)
				return -EUCLEAN;
			diag->stability_validated++;
		}
	}
#else
	if (sample->value != sample->expected)
		return -EUCLEAN;
#endif

	return 0;
}

static int mtk_i2c_quasar_run(struct mtk_i2c *i2c)
{
	struct mtk_i2c_quasar_diag *diag = &i2c->quasar;
	unsigned int pass;
	unsigned int index;
	int ret = 0;

	i2c_lock_bus(&i2c->adap, I2C_LOCK_ROOT_ADAPTER);
	mtk_i2c_quasar_record_counters(i2c, true);
	mtk_i2c_quasar_record_counters(i2c, false);
	diag->retries_before = i2c->adap.retries;
	diag->retries_during = i2c->adap.retries;
	diag->retries_after = i2c->adap.retries;

	if (device_for_each_child(&i2c->adap.dev, NULL,
				  mtk_i2c_quasar_child_present)) {
		ret = -EBUSY;
		goto out_restore_retries;
	}

	if (mt6797_dvfsp_handoff_require_ready(i2c->dvfsp_handoff)) {
		ret = -EHOSTDOWN;
		goto out_restore_retries;
	}

	if (diag->transfer_attempts_before ||
	    diag->dma_starts_before ||
	    diag->nonzero_starts_before ||
	    diag->irqs_before) {
		ret = -EBUSY;
		goto out_restore_retries;
	}

	if (diag->init_attempts_before != 1 ||
	    diag->init_successes_before != 1 ||
	    diag->retries_before != 1 ||
	    i2c->dev_comp->wrrd_len_mode != MTK_I2C_WRRD_LEN_PACKED_8_5 ||
	    i2c->dev_comp->fifo_size != 8) {
		ret = -EPROTO;
		goto out_restore_retries;
	}

	i2c->adap.retries = 0;
	diag->retries_during = i2c->adap.retries;

	for (pass = 0; pass < MTK_I2C_QUASAR_PASS_COUNT; pass++) {
		for (index = 0; index < MTK_I2C_QUASAR_REGISTER_COUNT;
		     index++) {
			struct mtk_i2c_quasar_sample *sample;
			struct i2c_msg msgs[2];
			unsigned int ordinal;
			u8 pointer = mtk_i2c_quasar_registers[index];
			u8 value = mtk_i2c_quasar_prefills[pass][index];

			sample = &diag->samples[diag->attempted++];
			memset(sample, 0, sizeof(*sample));
			sample->pass = pass;
			sample->index = index;
			sample->addr = mtk_i2c_quasar_addresses[index];
			sample->reg = pointer;
			sample->expected = mtk_i2c_quasar_expected[index];
			sample->prefill = value;
			sample->value = value;
			sample->fifo_count = 0xffff;
			sample->fifo_count_drained = 0xffff;
			ordinal = diag->attempted;

			msgs[0].addr = sample->addr;
			msgs[0].flags = 0;
			msgs[0].len = 1;
			msgs[0].buf = &pointer;
			msgs[1].addr = sample->addr;
			msgs[1].flags = I2C_M_RD;
			msgs[1].len = 1;
			msgs[1].buf = &value;

			smp_store_release(&diag->active_sample, sample);
			ret = __i2c_transfer(&i2c->adap, msgs,
					     ARRAY_SIZE(msgs));
			smp_store_release(&diag->active_sample, NULL);
			sample->ret = ret;
			sample->value = value;
			if (ret != 2) {
				if (ret >= 0)
					ret = -EIO;
				sample->validation_error = ret;
				diag->failure_pass = pass;
				diag->failure_index = index;
				goto out_restore_retries;
			}
			sample->transport_completed = true;
			diag->transport_completed++;

			ret = mtk_i2c_quasar_validate_sample(i2c, sample,
							     ordinal);
			sample->validation_error = ret;
			if (ret) {
				diag->failure_pass = pass;
				diag->failure_index = index;
				diag->mismatch = ret == -EUCLEAN;
				goto out_restore_retries;
			}
			sample->value_validated = true;
			diag->value_validated++;
		}
	}

	mtk_i2c_quasar_record_counters(i2c, false);
	if (diag->attempted != MTK_I2C_QUASAR_SAMPLE_COUNT ||
	    diag->transport_completed != MTK_I2C_QUASAR_SAMPLE_COUNT ||
	    diag->value_validated != MTK_I2C_QUASAR_SAMPLE_COUNT ||
	    diag->transfer_attempts_after != MTK_I2C_QUASAR_SAMPLE_COUNT ||
	    diag->dma_starts_after ||
	    diag->nonzero_starts_after != MTK_I2C_QUASAR_SAMPLE_COUNT ||
	    diag->irqs_after != MTK_I2C_QUASAR_SAMPLE_COUNT ||
	    diag->init_attempts_after != diag->init_attempts_before ||
	    diag->init_successes_after != diag->init_successes_before) {
		ret = -EPROTO;
		goto out_restore_retries;
	}
#ifdef CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC
	if (diag->stability_validated != 4) {
		ret = -EPROTO;
		goto out_restore_retries;
	}
#endif
	ret = 0;

out_restore_retries:
	i2c->adap.retries = diag->retries_before;
	diag->retries_after = i2c->adap.retries;
	smp_store_release(&diag->active_sample, NULL);
	mtk_i2c_quasar_record_counters(i2c, false);
	i2c_unlock_bus(&i2c->adap, I2C_LOCK_ROOT_ADAPTER);

	return ret;
}

static int
mtk_i2c_quasar_emit_snapshot(char *buf, int len, const char *name,
			     const struct mtk_i2c_quasar_snapshot *snapshot)
{
	len += scnprintf(buf + len, MTK_I2C_QUASAR_RESULT_SIZE - len,
		"%s_transfer_len=%04x %s_transfer_len_aux=%04x "
		"%s_transac_len=%04x %s_control=%04x %s_start=%04x "
		"%s_intr_stat=%04x %s_fifo_stat=%04x ",
		name, snapshot->transfer_len, name, snapshot->transfer_len_aux,
		name, snapshot->transac_len, name, snapshot->control,
		name, snapshot->start, name, snapshot->intr_stat,
		name, snapshot->fifo_stat);
	len += scnprintf(buf + len, MTK_I2C_QUASAR_RESULT_SIZE - len,
		"%s_dma_en=%08x %s_dma_con=%08x %s_dma_int_flag=%08x "
		"%s_dma_tx_len=%08x %s_dma_rx_len=%08x ",
		name, snapshot->dma_en, name, snapshot->dma_con,
		name, snapshot->dma_int_flag, name, snapshot->dma_tx_len,
		name, snapshot->dma_rx_len);

	return len;
}

#ifdef CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC
static const char *
mtk_i2c_quasar_expected_kind(const struct mtk_i2c_quasar_sample *sample)
{
	if (sample->index < 3)
		return "exact";
	if (sample->index == 3)
		return "exact-d3-stable";
	return "stable";
}
#endif

static ssize_t mtk_i2c_quasar_read(struct file *file,
				   char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	struct mtk_i2c *i2c = file->private_data;
	struct mtk_i2c_quasar_diag *diag = &i2c->quasar;
	char *buf;
	unsigned int i;
	int len = 0;
	ssize_t ret;

	buf = kzalloc(MTK_I2C_QUASAR_RESULT_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&diag->lock);
#ifdef CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC
	len += scnprintf(buf + len, MTK_I2C_QUASAR_RESULT_SIZE - len,
		"candidate=%s state=%s one_shot=%s run_error=%d "
		"attempted=%u transport_completed=%u value_validated=%u "
		"addresses=0x69,0x68 passes=2 "
		"transfer_order=69:05,69:06,69:47,68:d3,68:5e,68:d9,68:da "
		"expected_signature=d9,d0,c0 topology_register=d3 "
		"d3_exact_mask=ff d3_exact_expected=1f "
		"stability_registers=d3,5e,d9,da stability_validated=%u ",
		MTK_I2C_QUASAR_CANDIDATE,
		mtk_i2c_quasar_state_name(diag->state),
		diag->state == MTK_I2C_QUASAR_READY ? "unused" : "consumed",
		diag->run_error, diag->attempted, diag->transport_completed,
		diag->value_validated, diag->stability_validated);
	len += scnprintf(buf + len, MTK_I2C_QUASAR_RESULT_SIZE - len,
		"prefills=a5,5a,3c,96,69,c3,87,78,b4,4b,d2,2d,e1,1e "
		"mode=none forced_length_mode=none forced_engine=none "
		"reset_pending=0 mismatch=%u "
		"failure_pass=%u failure_index=%u ",
		diag->mismatch, diag->failure_pass, diag->failure_index);
#else
	len += scnprintf(buf + len, MTK_I2C_QUASAR_RESULT_SIZE - len,
		"candidate=%s state=%s one_shot=%s run_error=%d "
		"attempted=%u transport_completed=%u value_validated=%u "
		"address=0x69 passes=2 "
		"registers=05,06,47 expected=d9,d0,c0 ",
		MTK_I2C_QUASAR_CANDIDATE,
		mtk_i2c_quasar_state_name(diag->state),
		diag->state == MTK_I2C_QUASAR_READY ? "unused" : "consumed",
		diag->run_error, diag->attempted, diag->transport_completed,
		diag->value_validated);
	len += scnprintf(buf + len, MTK_I2C_QUASAR_RESULT_SIZE - len,
		"prefills=a5,5a,3c,96,69,c3 mode=none "
		"forced_length_mode=none forced_engine=none reset_pending=0 mismatch=%u "
		"failure_pass=%u failure_index=%u ",
		diag->mismatch, diag->failure_pass, diag->failure_index);
#endif
	len += scnprintf(buf + len, MTK_I2C_QUASAR_RESULT_SIZE - len,
		"retries_before=%u retries_during=%u retries_after=%u ",
		diag->retries_before, diag->retries_during,
		diag->retries_after);
	len += scnprintf(buf + len, MTK_I2C_QUASAR_RESULT_SIZE - len,
		"init_attempts_before=%d init_attempts_after=%d "
		"init_successes_before=%d init_successes_after=%d ",
		diag->init_attempts_before, diag->init_attempts_after,
		diag->init_successes_before, diag->init_successes_after);
	len += scnprintf(buf + len, MTK_I2C_QUASAR_RESULT_SIZE - len,
		"transfer_attempts_before=%d transfer_attempts_after=%d "
		"dma_starts_before=%d dma_starts_after=%d ",
		diag->transfer_attempts_before, diag->transfer_attempts_after,
		diag->dma_starts_before, diag->dma_starts_after);
	len += scnprintf(buf + len, MTK_I2C_QUASAR_RESULT_SIZE - len,
		"nonzero_starts_before=%d nonzero_starts_after=%d "
		"irqs_before=%d irqs_after=%d "
		"success_counter_contract=%s\n",
		diag->nonzero_starts_before, diag->nonzero_starts_after,
		diag->irqs_before, diag->irqs_after,
		MTK_I2C_QUASAR_COUNTER_CONTRACT);

	for (i = 0; i < diag->attempted; i++) {
		const struct mtk_i2c_quasar_sample *sample =
			&diag->samples[i];

#ifdef CONFIG_I2C_MT65XX_FERMI_DIAGNOSTIC
		len += scnprintf(buf + len,
			MTK_I2C_QUASAR_RESULT_SIZE - len,
			"sample=%u pass=%u index=%u address=%02x "
			"register=%02x expected_kind=%s expected=%02x "
			"prefill=%02x value=%02x ret=%d "
			"validation_error=%d programmed=%u "
			"transport_completed=%u value_validated=%u "
			"engine=%s irq_stat=%04x ",
			i, sample->pass, sample->index, sample->addr,
			sample->reg, mtk_i2c_quasar_expected_kind(sample),
			sample->expected, sample->prefill, sample->value,
			sample->ret, sample->validation_error,
			sample->programmed, sample->transport_completed,
			sample->value_validated,
			sample->programmed ?
				(sample->use_dma ? "dma" : "fifo") :
				"unobserved",
			sample->irq_stat);
#else
		len += scnprintf(buf + len,
			MTK_I2C_QUASAR_RESULT_SIZE - len,
			"sample=%u pass=%u index=%u register=%02x "
			"expected=%02x prefill=%02x value=%02x ret=%d "
			"validation_error=%d programmed=%u "
			"transport_completed=%u value_validated=%u "
			"engine=%s irq_stat=%04x ",
			i, sample->pass, sample->index, sample->reg,
			sample->expected, sample->prefill, sample->value,
			sample->ret, sample->validation_error,
			sample->programmed, sample->transport_completed,
			sample->value_validated,
			sample->programmed ?
				(sample->use_dma ? "dma" : "fifo") :
				"unobserved",
			sample->irq_stat);
#endif
		len = mtk_i2c_quasar_emit_snapshot(buf, len, "pre",
						   &sample->pre);
		len = mtk_i2c_quasar_emit_snapshot(buf, len, "irq",
						   &sample->irq);
		len = mtk_i2c_quasar_emit_snapshot(buf, len, "post",
						   &sample->post);
		len = mtk_i2c_quasar_emit_snapshot(buf, len, "drained",
						   &sample->drained);
		len += scnprintf(buf + len,
			MTK_I2C_QUASAR_RESULT_SIZE - len,
			"fifo_stat=%04x fifo_count=%u "
			"fifo_count_drained=%u\n",
			sample->fifo_stat, sample->fifo_count,
			sample->fifo_count_drained);
	}
	mutex_unlock(&diag->lock);

	ret = simple_read_from_buffer(user_buf, count, ppos, buf, len);
	kfree(buf);

	return ret;
}

static ssize_t mtk_i2c_quasar_write(struct file *file,
				    const char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	struct mtk_i2c *i2c = file->private_data;
	struct mtk_i2c_quasar_diag *diag = &i2c->quasar;
	char command[4];
	int ret;

	if (*ppos || count != sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, user_buf, sizeof(command)))
		return -EFAULT;
	if (memcmp(command, "run\n", sizeof(command)))
		return -EINVAL;

	mutex_lock(&diag->lock);
	if (diag->state != MTK_I2C_QUASAR_READY) {
		ret = -EBUSY;
		goto out_unlock;
	}

	diag->state = MTK_I2C_QUASAR_RUNNING;
	ret = mtk_i2c_quasar_run(i2c);
	diag->run_error = ret;
	diag->state = MTK_I2C_QUASAR_DONE;
	if (!ret) {
		*ppos += count;
		ret = count;
	}

out_unlock:
	mutex_unlock(&diag->lock);
	return ret;
}

static const struct file_operations mtk_i2c_quasar_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mtk_i2c_quasar_read,
	.write = mtk_i2c_quasar_write,
	.llseek = default_llseek,
};

static int mtk_i2c_quasar_init(struct mtk_i2c *i2c)
{
	struct mtk_i2c_quasar_diag *diag = &i2c->quasar;
	struct device_node *target;
	struct dentry *file;
	int ret;

	if (i2c->dev_comp != &mt6797_idvfs_compat)
		return 0;
	if (!i2c->dvfsp_handoff)
		return dev_err_probe(i2c->dev, -EINVAL,
				     MTK_I2C_QUASAR_GATE
				     " missing DVFSP handoff\n");

	target = of_find_node_by_path("/i2c@1100e000");
	if (!target)
		return dev_err_probe(i2c->dev, -ENODEV,
				     MTK_I2C_QUASAR_GATE
				     " target node unavailable\n");
	if (i2c->dev->of_node != target) {
		of_node_put(target);
		return dev_err_probe(i2c->dev, -EINVAL,
				     MTK_I2C_QUASAR_GATE
				     " node identity mismatch\n");
	}
	of_node_put(target);

	if (!i2c->adap.debugfs)
		return dev_err_probe(i2c->dev, -ENODEV,
				     MTK_I2C_QUASAR_GATE
				     " adapter debugfs unavailable\n");

	mutex_init(&diag->lock);
	diag->state = MTK_I2C_QUASAR_READY;
	diag->failure_pass = MTK_I2C_QUASAR_PASS_COUNT;
	diag->failure_index = MTK_I2C_QUASAR_REGISTER_COUNT;
	diag->retries_before = i2c->adap.retries;
	diag->retries_during = i2c->adap.retries;
	diag->retries_after = i2c->adap.retries;
	mtk_i2c_quasar_record_counters(i2c, true);
	mtk_i2c_quasar_record_counters(i2c, false);
	file = debugfs_create_file(MTK_I2C_QUASAR_DEBUGFS_FILE, 0600,
				   i2c->adap.debugfs, i2c,
				   &mtk_i2c_quasar_fops);
	if (IS_ERR_OR_NULL(file)) {
		ret = file ? PTR_ERR(file) : -ENOMEM;
		return dev_err_probe(i2c->dev, ret,
				     MTK_I2C_QUASAR_GATE
				     " debugfs creation failed\n");
	}

	dev_info(i2c->dev,
		 MTK_I2C_QUASAR_READY_MARKER " state=ready "
		 "one_shot=unused mode=none forced_length_mode=none "
		 "forced_engine=none reset_pending=0\n");
	return 0;
}
#else
static int mtk_i2c_quasar_init(struct mtk_i2c *i2c)
{
	return 0;
}
#endif

static int mtk_i2c_probe(struct platform_device *pdev)
{
	int ret = 0;
	int post_ret = 0;
	struct mtk_i2c *i2c;
	int i, irq, speed_clk;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->dev = &pdev->dev;

	if (of_find_property(pdev->dev.of_node, "access-controllers", NULL)) {
		atomic_inc(&mtk_i2c_handoff_probe_attempts);
		atomic_set(&i2c->init_attempts, 0);
		atomic_set(&i2c->init_successes, 0);
		atomic_set(&i2c->transfer_attempts, 0);
		atomic_set(&i2c->dma_start_count, 0);
		atomic_set(&i2c->nonzero_start_count, 0);
		atomic_set(&i2c->irq_count, 0);
		atomic_set(&i2c->clock_ungated_checks, 0);
		atomic_set(&i2c->clock_gated_checks, 0);
		atomic_set(&i2c->clock_validation_failures, 0);
		atomic_set(&i2c->suspend_checks, 0);
		atomic_set(&i2c->resume_checks, 0);
		atomic_set(&i2c->resume_failures, 0);
#ifdef CONFIG_I2C_MT65XX_GEMINI_LIFECYCLE_ORACLE
		atomic_set(&i2c->oracle_combined_pointer_reads, 0);
		atomic_set(&i2c->oracle_primary_pointer_reads, 0);
		atomic_set(&i2c->oracle_page2_pointer_reads, 0);
		atomic_set(&i2c->oracle_write_only_messages, 0);
		atomic_set(&i2c->oracle_register_data_write_messages, 0);
		atomic_set(&i2c->oracle_other_transfers, 0);
		atomic_set(&i2c->oracle_other_address_transfers, 0);
#endif
		i2c->dvfsp_handoff =
			mt6797_dvfsp_handoff_get(&pdev->dev);
		if (IS_ERR(i2c->dvfsp_handoff))
			return dev_err_probe(
				&pdev->dev, PTR_ERR(i2c->dvfsp_handoff),
				"GEMINI_MT6797_I2C6_GUARD handoff=denied "
				"probe_attempts=%d reason=supplier-unavailable\n",
				atomic_read(
					&mtk_i2c_handoff_probe_attempts));
		i2c->handoff_runtime_pm_link = true;

		ret = mt6797_dvfsp_handoff_require_ready(
			i2c->dvfsp_handoff);
		if (ret)
			return dev_err_probe(
				&pdev->dev, ret,
				"GEMINI_MT6797_I2C6_GUARD handoff=denied "
				"probe_attempts=%d reason=supplier-not-ready\n",
				atomic_read(
					&mtk_i2c_handoff_probe_attempts));

		dev_pm_set_driver_flags(&pdev->dev,
					DPM_FLAG_NO_DIRECT_COMPLETE);
	}

	i2c->base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	i2c->pdmabase = devm_platform_get_and_ioremap_resource(pdev, 1, NULL);
	if (IS_ERR(i2c->pdmabase))
		return PTR_ERR(i2c->pdmabase);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	init_completion(&i2c->msg_complete);

	i2c->dev_comp = of_device_get_match_data(&pdev->dev);
	i2c->adap.dev.of_node = pdev->dev.of_node;
	i2c->adap.dev.parent = &pdev->dev;
	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &mtk_i2c_algorithm;
	i2c->adap.quirks = i2c->dev_comp->quirks;
	i2c->adap.timeout = 2 * HZ;
	i2c->adap.retries = 1;
	i2c->adap.bus_regulator = devm_regulator_get_optional(&pdev->dev, "vbus");
	if (IS_ERR(i2c->adap.bus_regulator)) {
		if (PTR_ERR(i2c->adap.bus_regulator) == -ENODEV)
			i2c->adap.bus_regulator = NULL;
		else
			return PTR_ERR(i2c->adap.bus_regulator);
	}

	ret = mtk_i2c_parse_dt(pdev->dev.of_node, i2c);
	if (ret)
		return -EINVAL;

	if (i2c->have_pmic && !i2c->dev_comp->pmic_i2c)
		return -EINVAL;

	/* Fill in clk-bulk IDs */
	for (i = 0; i < I2C_MT65XX_CLK_MAX; i++)
		i2c->clocks[i].id = i2c_mt65xx_clk_ids[i];

	/* Get clocks one by one, some may be optional */
	i2c->clocks[I2C_MT65XX_CLK_MAIN].clk = devm_clk_get(&pdev->dev, "main");
	if (IS_ERR(i2c->clocks[I2C_MT65XX_CLK_MAIN].clk)) {
		dev_err(&pdev->dev, "cannot get main clock\n");
		return PTR_ERR(i2c->clocks[I2C_MT65XX_CLK_MAIN].clk);
	}

	i2c->clocks[I2C_MT65XX_CLK_DMA].clk = devm_clk_get(&pdev->dev, "dma");
	if (IS_ERR(i2c->clocks[I2C_MT65XX_CLK_DMA].clk)) {
		dev_err(&pdev->dev, "cannot get dma clock\n");
		return PTR_ERR(i2c->clocks[I2C_MT65XX_CLK_DMA].clk);
	}

	i2c->clocks[I2C_MT65XX_CLK_ARB].clk = devm_clk_get_optional(&pdev->dev, "arb");
	if (IS_ERR(i2c->clocks[I2C_MT65XX_CLK_ARB].clk))
		return PTR_ERR(i2c->clocks[I2C_MT65XX_CLK_ARB].clk);

	i2c->clocks[I2C_MT65XX_CLK_PMIC].clk = devm_clk_get_optional(&pdev->dev, "pmic");
	if (IS_ERR(i2c->clocks[I2C_MT65XX_CLK_PMIC].clk)) {
		dev_err(&pdev->dev, "cannot get pmic clock\n");
		return PTR_ERR(i2c->clocks[I2C_MT65XX_CLK_PMIC].clk);
	}

	if (i2c->have_pmic) {
		if (!i2c->clocks[I2C_MT65XX_CLK_PMIC].clk) {
			dev_err(&pdev->dev, "cannot get pmic clock\n");
			return -ENODEV;
		}
		speed_clk = I2C_MT65XX_CLK_PMIC;
	} else {
		speed_clk = I2C_MT65XX_CLK_MAIN;
	}

	strscpy(i2c->adap.name, I2C_DRV_NAME, sizeof(i2c->adap.name));

	mtk_i2c_set_speed(i2c, clk_get_rate(i2c->clocks[speed_clk].clk));

	if (i2c->dev_comp->max_dma_support > 32) {
		ret = dma_set_mask(&pdev->dev,
				DMA_BIT_MASK(i2c->dev_comp->max_dma_support));
		if (ret) {
			dev_err(&pdev->dev, "dma_set_mask return error.\n");
			return ret;
		}
	}

	ret = clk_bulk_prepare_enable(I2C_MT65XX_CLK_MAX, i2c->clocks);
	if (ret) {
		dev_err(&pdev->dev, "clock enable failed!\n");
		return ret;
	}
	mtk_i2c_init_hw(i2c);

	if (i2c->dvfsp_handoff) {
		atomic_inc(&i2c->clock_ungated_checks);
		ret = mt6797_dvfsp_handoff_validate_clock(
			i2c->dvfsp_handoff, false);
		if (ret)
			atomic_inc(&i2c->clock_validation_failures);
	}
	clk_bulk_disable(I2C_MT65XX_CLK_MAX, i2c->clocks);

	if (i2c->dvfsp_handoff) {
		atomic_inc(&i2c->clock_gated_checks);
		post_ret = mt6797_dvfsp_handoff_validate_clock(
			i2c->dvfsp_handoff, true);
		if (post_ret)
			atomic_inc(&i2c->clock_validation_failures);
	}
	if (ret || post_ret) {
		if (!ret)
			ret = post_ret;
		goto err_bulk_unprepare;
	}

	ret = devm_request_irq(&pdev->dev, irq, mtk_i2c_irq,
			       IRQF_NO_SUSPEND | IRQF_TRIGGER_NONE,
			       dev_name(&pdev->dev), i2c);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Request I2C IRQ %d fail\n", irq);
		goto err_bulk_unprepare;
	}

	i2c_set_adapdata(&i2c->adap, i2c);
	ret = i2c_add_adapter(&i2c->adap);
	if (ret)
		goto err_bulk_unprepare;

	platform_set_drvdata(pdev, i2c);
	ret = mtk_i2c_orion_init(i2c);
	if (ret)
		goto err_del_adapter;
	ret = mtk_i2c_quasar_init(i2c);
	if (ret)
		goto err_del_adapter;

	if (i2c->dvfsp_handoff) {
		ret = devm_device_add_group(&pdev->dev,
					    &mtk_i2c_handoff_group);
		if (ret)
			goto err_del_adapter;

		dev_info(
			&pdev->dev,
			"GEMINI_MT6797_I2C6_GUARD handoff=ready "
			"probe_attempts=%d init_attempts=%d "
			"init_successes=%d clock_ungated_checks=%d "
			"clock_gated_checks=%d "
			"clock_validation_failures=%d "
			"runtime_pm_link=%d "
			"clock_domains=i2c-appm,ap-dma "
			"transfer_attempts=%d dma_starts=%d "
			"nonzero_starts=%d irq_count=%d\n",
			atomic_read(&mtk_i2c_handoff_probe_attempts),
			atomic_read(&i2c->init_attempts),
			atomic_read(&i2c->init_successes),
			atomic_read(&i2c->clock_ungated_checks),
			atomic_read(&i2c->clock_gated_checks),
			atomic_read(&i2c->clock_validation_failures),
			i2c->handoff_runtime_pm_link,
			atomic_read(&i2c->transfer_attempts),
			atomic_read(&i2c->dma_start_count),
			atomic_read(&i2c->nonzero_start_count),
			atomic_read(&i2c->irq_count));
	}

	return 0;

err_del_adapter:
	platform_set_drvdata(pdev, NULL);
	i2c_del_adapter(&i2c->adap);
err_bulk_unprepare:
	clk_bulk_unprepare(I2C_MT65XX_CLK_MAX, i2c->clocks);

	return ret;
}

static void mtk_i2c_remove(struct platform_device *pdev)
{
	struct mtk_i2c *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);

	clk_bulk_unprepare(I2C_MT65XX_CLK_MAX, i2c->clocks);
}

static int mtk_i2c_suspend_late(struct device *dev)
{
	struct mtk_i2c *i2c = dev_get_drvdata(dev);

	if (!i2c->dvfsp_handoff)
		return 0;

	atomic_inc(&i2c->suspend_checks);
	if (!mt6797_dvfsp_handoff_is_ready_atomic(i2c->dvfsp_handoff))
		return -EHOSTDOWN;

	i2c_mark_adapter_suspended(&i2c->adap);
	clk_bulk_unprepare(I2C_MT65XX_CLK_MAX, i2c->clocks);

	return 0;
}

static int mtk_i2c_resume_early(struct device *dev)
{
	int ret;
	int post_ret;
	struct mtk_i2c *i2c = dev_get_drvdata(dev);

	if (!i2c->dvfsp_handoff)
		return 0;

	atomic_inc(&i2c->resume_checks);
	if (!mt6797_dvfsp_handoff_is_ready_atomic(i2c->dvfsp_handoff)) {
		atomic_inc(&i2c->resume_failures);
		return -EHOSTDOWN;
	}

	ret = clk_bulk_prepare_enable(I2C_MT65XX_CLK_MAX, i2c->clocks);
	if (ret) {
		atomic_inc(&i2c->resume_failures);
		return ret;
	}

	mtk_i2c_init_hw(i2c);

	atomic_inc(&i2c->clock_ungated_checks);
	ret = mt6797_dvfsp_handoff_validate_clock_pm(
		i2c->dvfsp_handoff, false);
	if (ret)
		atomic_inc(&i2c->clock_validation_failures);

	clk_bulk_disable(I2C_MT65XX_CLK_MAX, i2c->clocks);

	atomic_inc(&i2c->clock_gated_checks);
	post_ret = mt6797_dvfsp_handoff_validate_clock_pm(
		i2c->dvfsp_handoff, true);
	if (post_ret)
		atomic_inc(&i2c->clock_validation_failures);
	if (ret || post_ret) {
		if (!ret)
			ret = post_ret;
		goto err_unprepare;
	}

	i2c_mark_adapter_resumed(&i2c->adap);

	return 0;

err_unprepare:
	atomic_inc(&i2c->resume_failures);
	clk_bulk_unprepare(I2C_MT65XX_CLK_MAX, i2c->clocks);
	return ret;
}

static int mtk_i2c_suspend_noirq(struct device *dev)
{
	struct mtk_i2c *i2c = dev_get_drvdata(dev);

	if (i2c->dvfsp_handoff)
		return 0;

	i2c_mark_adapter_suspended(&i2c->adap);
	clk_bulk_unprepare(I2C_MT65XX_CLK_MAX, i2c->clocks);

	return 0;
}

static int mtk_i2c_resume_noirq(struct device *dev)
{
	struct mtk_i2c *i2c = dev_get_drvdata(dev);
	int ret;

	if (i2c->dvfsp_handoff)
		return 0;

	ret = clk_bulk_prepare_enable(I2C_MT65XX_CLK_MAX, i2c->clocks);
	if (ret) {
		dev_err(dev, "clock enable failed!\n");
		return ret;
	}

	mtk_i2c_init_hw(i2c);
	clk_bulk_disable(I2C_MT65XX_CLK_MAX, i2c->clocks);
	i2c_mark_adapter_resumed(&i2c->adap);

	return 0;
}

static const struct dev_pm_ops mtk_i2c_pm = {
	LATE_SYSTEM_SLEEP_PM_OPS(mtk_i2c_suspend_late,
				 mtk_i2c_resume_early)
	NOIRQ_SYSTEM_SLEEP_PM_OPS(mtk_i2c_suspend_noirq,
				  mtk_i2c_resume_noirq)
};

static struct platform_driver mtk_i2c_driver = {
	.probe = mtk_i2c_probe,
	.remove = mtk_i2c_remove,
	.driver = {
		.name = I2C_DRV_NAME,
		.pm = pm_sleep_ptr(&mtk_i2c_pm),
		.of_match_table = mtk_i2c_of_match,
	},
};

module_platform_driver(mtk_i2c_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek I2C Bus Driver");
MODULE_AUTHOR("Xudong Chen <xudong.chen@mediatek.com>");
