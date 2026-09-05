// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: Hanyi Wu <hanyi.wu@mediatek.com>
 *         Sascha Hauer <s.hauer@pengutronix.de>
 *         Dawei Chien <dawei.chien@mediatek.com>
 *         Louis Yu <louis.yu@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/thermal.h>
#include <linux/reset.h>
#include <linux/math64.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-eem-calibration.h>
#include <linux/soc/mediatek/mt6797-eem-readback.h>

#include "../thermal_hwmon.h"
#include "auxadc_thermal_internal.h"

/* AUXADC Registers */
#define AUXADC_CON0_V		0x000
#define AUXADC_CON1_SET_V	0x008
#define AUXADC_CON1_CLR_V	0x00c
#define AUXADC_CON2_V		0x010
#define AUXADC_DATA(channel)	(0x14 + (channel) * 4)

#define APMIXED_SYS_TS_CON0	0x600
#define APMIXED_SYS_TS_CON1	0x604

/* Thermal Controller Registers */
#define TEMP_MONCTL0		0x000
#define TEMP_MONCTL1		0x004
#define TEMP_MONCTL2		0x008
#define TEMP_MONIDET0		0x014
#define TEMP_MONIDET1		0x018
#define TEMP_MSRCTL0		0x038
#define TEMP_MSRCTL1		0x03c
#define TEMP_AHBPOLL		0x040
#define TEMP_AHBTO		0x044
#define TEMP_ADCPNP0		0x048
#define TEMP_ADCPNP1		0x04c
#define TEMP_ADCPNP2		0x050
#define TEMP_ADCPNP3		0x0b4

#define TEMP_ADCMUX		0x054
#define TEMP_ADCEN		0x060
#define TEMP_PNPMUXADDR		0x064
#define TEMP_ADCMUXADDR		0x068
#define TEMP_ADCENADDR		0x074
#define TEMP_ADCVALIDADDR	0x078
#define TEMP_ADCVOLTADDR	0x07c
#define TEMP_RDCTRL		0x080
#define TEMP_ADCVALIDMASK	0x084
#define TEMP_ADCVOLTAGESHIFT	0x088
#define TEMP_ADCWRITECTRL	0x08c
#define TEMP_MSR0		0x090
#define TEMP_MSR1		0x094
#define TEMP_MSR2		0x098
#define TEMP_MSR3		0x0B8

#define TEMP_SPARE0		0x0f0

#define TEMP_ADCPNP0_1          0x148
#define TEMP_ADCPNP1_1          0x14c
#define TEMP_ADCPNP2_1          0x150
#define TEMP_MSR0_1             0x190
#define TEMP_MSR1_1             0x194
#define TEMP_MSR2_1             0x198
#define TEMP_ADCPNP3_1          0x1b4
#define TEMP_MSR3_1             0x1B8

#define PTPCORESEL		0x400
#define MT6797_THERMAL_AHB_STATUS	0x418
#define MT6797_PERIODIC_PAUSE_MASK	0x10e
#define MT6797_FIRST_SAMPLE_ATTEMPTS	100

/* MT6797 EEM/PTP registers share thermal_base and PTPCORESEL. */
#define MT6797_EEM_FREQPCT30		0x218
#define MT6797_EEM_FREQPCT74		0x21c
#define MT6797_EEM_DCVALUES		0x240
#define MT6797_EEM_AGEVALUES		0x244
#define MT6797_EEM_VOP30		0x248
#define MT6797_EEM_VOP74		0x24c
#define MT6797_EEM_TEMP		0x250
#define MT6797_EEM_INTSTS		0x254
#define MT6797_EEM_INTSTS_RAW		0x258
#define MT6797_EEM_INTEN		0x25c
#define MT6797_EEM_THERM_INTSTS	0x404
#define MT6797_EEM_MOD_INTSTS		0x408
#define MT6797_EEMCORESEL_MASK		GENMASK(2, 0)

#define MT6797_EEM_EEMEN		0x238

#define TEMP_MONCTL1_PERIOD_UNIT(x)	((x) & 0x3ff)

#define TEMP_MONCTL2_FILTER_INTERVAL(x)	(((x) & 0x3ff) << 16)
#define TEMP_MONCTL2_SENSOR_INTERVAL(x)	((x) & 0x3ff)

#define TEMP_AHBPOLL_ADC_POLL_INTERVAL(x)	(x)

#define TEMP_ADCWRITECTRL_ADC_PNP_WRITE		BIT(0)
#define TEMP_ADCWRITECTRL_ADC_MUX_WRITE		BIT(1)

#define TEMP_ADCVALIDMASK_VALID_HIGH		BIT(5)
#define TEMP_ADCVALIDMASK_VALID_POS(bit)	(bit)

/* MT8173 thermal sensors */
#define MT8173_TS1	0
#define MT8173_TS2	1
#define MT8173_TS3	2
#define MT8173_TS4	3
#define MT8173_TSABB	4

/* AUXADC channel 11 is used for the temperature sensors */
#define MT8173_TEMP_AUXADC_CHANNEL	11

/* The total number of temperature sensors in the MT8173 */
#define MT8173_NUM_SENSORS		5

/* The number of banks in the MT8173 */
#define MT8173_NUM_ZONES		4

/* The number of sensing points per bank */
#define MT8173_NUM_SENSORS_PER_ZONE	4

/* The number of controller in the MT8173 */
#define MT8173_NUM_CONTROLLER		1

/* The calibration coefficient of sensor  */
#define MT8173_CALIBRATION	165

/* Valid temperatures range */
#define MT8173_TEMP_MIN		-20000
#define MT8173_TEMP_MAX		150000

/*
 * Layout of the fuses providing the calibration data
 * These macros could be used for MT8183, MT8173, MT2701, and MT2712.
 * MT8183 has 6 sensors and needs 6 VTS calibration data.
 * MT8173 has 5 sensors and needs 5 VTS calibration data.
 * MT2701 has 3 sensors and needs 3 VTS calibration data.
 * MT2712 has 4 sensors and needs 4 VTS calibration data.
 */
#define CALIB_BUF0_VALID_V1		BIT(0)
#define CALIB_BUF1_ADC_GE_V1(x)		(((x) >> 22) & 0x3ff)
#define CALIB_BUF0_VTS_TS1_V1(x)	(((x) >> 17) & 0x1ff)
#define CALIB_BUF0_VTS_TS2_V1(x)	(((x) >> 8) & 0x1ff)
#define CALIB_BUF1_VTS_TS3_V1(x)	(((x) >> 0) & 0x1ff)
#define CALIB_BUF2_VTS_TS4_V1(x)	(((x) >> 23) & 0x1ff)
#define CALIB_BUF2_VTS_TS5_V1(x)	(((x) >> 5) & 0x1ff)
#define CALIB_BUF2_VTS_TSABB_V1(x)	(((x) >> 14) & 0x1ff)
#define CALIB_BUF0_DEGC_CALI_V1(x)	(((x) >> 1) & 0x3f)
#define CALIB_BUF0_O_SLOPE_V1(x)	(((x) >> 26) & 0x3f)
#define CALIB_BUF0_O_SLOPE_SIGN_V1(x)	(((x) >> 7) & 0x1)
#define CALIB_BUF1_ID_V1(x)		(((x) >> 9) & 0x1)

/*
 * Layout of the fuses providing the calibration data
 * These macros could be used for MT7622.
 */
#define CALIB_BUF0_ADC_OE_V2(x)		(((x) >> 22) & 0x3ff)
#define CALIB_BUF0_ADC_GE_V2(x)		(((x) >> 12) & 0x3ff)
#define CALIB_BUF0_DEGC_CALI_V2(x)	(((x) >> 6) & 0x3f)
#define CALIB_BUF0_O_SLOPE_V2(x)	(((x) >> 0) & 0x3f)
#define CALIB_BUF1_VTS_TS1_V2(x)	(((x) >> 23) & 0x1ff)
#define CALIB_BUF1_VTS_TS2_V2(x)	(((x) >> 14) & 0x1ff)
#define CALIB_BUF1_VTS_TSABB_V2(x)	(((x) >> 5) & 0x1ff)
#define CALIB_BUF1_VALID_V2(x)		(((x) >> 4) & 0x1)
#define CALIB_BUF1_O_SLOPE_SIGN_V2(x)	(((x) >> 3) & 0x1)

/*
 * Layout of the fuses providing the calibration data
 * These macros can be used for MT7981 and MT7986.
 */
#define CALIB_BUF0_ADC_GE_V3(x)		(((x) >> 0) & 0x3ff)
#define CALIB_BUF0_DEGC_CALI_V3(x)	(((x) >> 20) & 0x3f)
#define CALIB_BUF0_O_SLOPE_V3(x)	(((x) >> 26) & 0x3f)
#define CALIB_BUF1_VTS_TS1_V3(x)	(((x) >> 0) & 0x1ff)
#define CALIB_BUF1_VTS_TS2_V3(x)	(((x) >> 21) & 0x1ff)
#define CALIB_BUF1_VTS_TSABB_V3(x)	(((x) >> 9) & 0x1ff)
#define CALIB_BUF1_VALID_V3(x)		(((x) >> 18) & 0x1)
#define CALIB_BUF1_O_SLOPE_SIGN_V3(x)	(((x) >> 19) & 0x1)
#define CALIB_BUF1_ID_V3(x)		(((x) >> 20) & 0x1)

enum {
	VTS1,
	VTS2,
	VTS3,
	VTS4,
	VTS5,
	VTSABB,
	MAX_NUM_VTS,
};

enum mtk_thermal_version {
	MTK_THERMAL_V1 = 1,
	MTK_THERMAL_V2,
	MTK_THERMAL_V3,
	MTK_THERMAL_V4,
};

/* MT2701 thermal sensors */
#define MT2701_TS1	0
#define MT2701_TS2	1
#define MT2701_TSABB	2

/* AUXADC channel 11 is used for the temperature sensors */
#define MT2701_TEMP_AUXADC_CHANNEL	11

/* The total number of temperature sensors in the MT2701 */
#define MT2701_NUM_SENSORS	3

/* The number of sensing points per bank */
#define MT2701_NUM_SENSORS_PER_ZONE	3

/* The number of controller in the MT2701 */
#define MT2701_NUM_CONTROLLER		1

/* The calibration coefficient of sensor  */
#define MT2701_CALIBRATION	165

/* MT2712 thermal sensors */
#define MT2712_TS1	0
#define MT2712_TS2	1
#define MT2712_TS3	2
#define MT2712_TS4	3

/* AUXADC channel 11 is used for the temperature sensors */
#define MT2712_TEMP_AUXADC_CHANNEL	11

/* The total number of temperature sensors in the MT2712 */
#define MT2712_NUM_SENSORS	4

/* The number of sensing points per bank */
#define MT2712_NUM_SENSORS_PER_ZONE	4

/* The number of controller in the MT2712 */
#define MT2712_NUM_CONTROLLER		1

/* The calibration coefficient of sensor  */
#define MT2712_CALIBRATION	165

#define MT7622_TEMP_AUXADC_CHANNEL	11
#define MT7622_NUM_SENSORS		1
#define MT7622_NUM_ZONES		1
#define MT7622_NUM_SENSORS_PER_ZONE	1
#define MT7622_TS1	0
#define MT7622_NUM_CONTROLLER		1

/* The maximum number of banks */
#define MAX_NUM_ZONES		8

/* The calibration coefficient of sensor  */
#define MT7622_CALIBRATION	165

/* MT8183 thermal sensors */
#define MT8183_TS1	0
#define MT8183_TS2	1
#define MT8183_TS3	2
#define MT8183_TS4	3
#define MT8183_TS5	4
#define MT8183_TSABB	5

/* AUXADC channel  is used for the temperature sensors */
#define MT8183_TEMP_AUXADC_CHANNEL	11

/* The total number of temperature sensors in the MT8183 */
#define MT8183_NUM_SENSORS	6

/* The number of banks in the MT8183 */
#define MT8183_NUM_ZONES               1

/* The number of sensing points per bank */
#define MT8183_NUM_SENSORS_PER_ZONE	 6

/* The number of controller in the MT8183 */
#define MT8183_NUM_CONTROLLER		2

/* The calibration coefficient of sensor  */
#define MT8183_CALIBRATION	153

/* AUXADC channel 11 is used for the temperature sensors */
#define MT7986_TEMP_AUXADC_CHANNEL	11

/* The total number of temperature sensors in the MT7986 */
#define MT7986_NUM_SENSORS		1

/* The number of banks in the MT7986 */
#define MT7986_NUM_ZONES		1

/* The number of sensing points per bank */
#define MT7986_NUM_SENSORS_PER_ZONE	1

/* MT7986 thermal sensors */
#define MT7986_TS1			0

/* The number of controller in the MT7986 */
#define MT7986_NUM_CONTROLLER		1

/* The calibration coefficient of sensor  */
#define MT7986_CALIBRATION		165

/* MT8365 */
#define MT8365_TEMP_AUXADC_CHANNEL 11
#define MT8365_CALIBRATION 164
#define MT8365_NUM_CONTROLLER 1
#define MT8365_NUM_BANKS 1
#define MT8365_NUM_SENSORS 3
#define MT8365_NUM_SENSORS_PER_ZONE 3
#define MT8365_TS1 0
#define MT8365_TS2 1
#define MT8365_TS3 2

/* MT6797 thermal sensors */
#define MT6797_TS_MCU1		0
#define MT6797_TS_MCU2		1
#define MT6797_TS_MCU3		2
#define MT6797_TS_MCU4		3
#define MT6797_TS_ABB		4
#define MT6797_TEMP_AUXADC_CHANNEL	11
#define MT6797_NUM_SENSORS	5
#define MT6797_NUM_ZONES		6
#define MT6797_NUM_SENSORS_PER_ZONE	2
#define MT6797_NUM_CONTROLLER	1

struct mtk_thermal;

struct thermal_bank_cfg {
	unsigned int num_sensors;
	const int *sensors;
};

struct mtk_thermal_bank {
	struct mtk_thermal *mt;
	int id;
};

struct mtk_thermal_data {
	s32 num_banks;
	s32 num_sensors;
	s32 auxadc_channel;
	const int *vts_index;
	const int *sensor_mux_values;
	const int *msr;
	const int *adcpnp;
	const int cali_val;
	const int num_controller;
	const int *controller_offset;
	bool need_switch_bank;
	bool require_valid_calibration;
	struct thermal_bank_cfg bank_data[MAX_NUM_ZONES];
	enum mtk_thermal_version version;
	u32 apmixed_buffer_ctl_reg;
	u32 apmixed_buffer_ctl_mask;
	u32 apmixed_buffer_ctl_set;
	u32 temp_ahbpoll;
	u32 temp_msrctl0;
	u32 temp_adcvalidmask;
};

struct mtk_thermal {
	struct device *dev;
	void __iomem *thermal_base;

	struct clk *clk_peri_therm;
	struct clk *clk_auxadc;
	struct reset_control *rst;
	void __iomem *auxadc_base;
	void __iomem *apmixed_base;
	u64 auxadc_phys_base;
	u64 apmixed_phys_base;
	u32 apmixed_buffer_saved;
	struct mtk_thermal_transaction_state transaction;
	/* lock: for getting and putting banks */
	struct mutex lock;

	/* Calibration values */
	s32 adc_ge;
	s32 adc_oe;
	s32 degc_cali;
	s32 o_slope;
	s32 o_slope_sign;
	s32 vts[MAX_NUM_VTS];

	const struct mtk_thermal_data *conf;
	struct mtk_thermal_bank banks[MAX_NUM_ZONES];

	int (*raw_to_mcelsius)(struct mtk_thermal *mt, int sensno, s32 raw);
};

/* MT8183 thermal sensor data */
static const int mt8183_bank_data[MT8183_NUM_SENSORS] = {
	MT8183_TS1, MT8183_TS2, MT8183_TS3, MT8183_TS4, MT8183_TS5, MT8183_TSABB
};

static const int mt8183_msr[MT8183_NUM_SENSORS_PER_ZONE] = {
	TEMP_MSR0_1, TEMP_MSR1_1, TEMP_MSR2_1, TEMP_MSR1, TEMP_MSR0, TEMP_MSR3_1
};

static const int mt8183_adcpnp[MT8183_NUM_SENSORS_PER_ZONE] = {
	TEMP_ADCPNP0_1, TEMP_ADCPNP1_1, TEMP_ADCPNP2_1,
	TEMP_ADCPNP1, TEMP_ADCPNP0, TEMP_ADCPNP3_1
};

static const int mt8183_mux_values[MT8183_NUM_SENSORS] = { 0, 1, 2, 3, 4, 0 };
static const int mt8183_tc_offset[MT8183_NUM_CONTROLLER] = {0x0, 0x100};

static const int mt8183_vts_index[MT8183_NUM_SENSORS] = {
	VTS1, VTS2, VTS3, VTS4, VTS5, VTSABB
};

/* MT8173 thermal sensor data */
static const int mt8173_bank_data[MT8173_NUM_ZONES][3] = {
	{ MT8173_TS2, MT8173_TS3 },
	{ MT8173_TS2, MT8173_TS4 },
	{ MT8173_TS1, MT8173_TS2, MT8173_TSABB },
	{ MT8173_TS2 },
};

static const int mt8173_msr[MT8173_NUM_SENSORS_PER_ZONE] = {
	TEMP_MSR0, TEMP_MSR1, TEMP_MSR2, TEMP_MSR3
};

static const int mt8173_adcpnp[MT8173_NUM_SENSORS_PER_ZONE] = {
	TEMP_ADCPNP0, TEMP_ADCPNP1, TEMP_ADCPNP2, TEMP_ADCPNP3
};

static const int mt8173_mux_values[MT8173_NUM_SENSORS] = { 0, 1, 2, 3, 16 };
static const int mt8173_tc_offset[MT8173_NUM_CONTROLLER] = { 0x0, };

static const int mt8173_vts_index[MT8173_NUM_SENSORS] = {
	VTS1, VTS2, VTS3, VTS4, VTSABB
};

/* MT2701 thermal sensor data */
static const int mt2701_bank_data[MT2701_NUM_SENSORS] = {
	MT2701_TS1, MT2701_TS2, MT2701_TSABB
};

static const int mt2701_msr[MT2701_NUM_SENSORS_PER_ZONE] = {
	TEMP_MSR0, TEMP_MSR1, TEMP_MSR2
};

static const int mt2701_adcpnp[MT2701_NUM_SENSORS_PER_ZONE] = {
	TEMP_ADCPNP0, TEMP_ADCPNP1, TEMP_ADCPNP2
};

static const int mt2701_mux_values[MT2701_NUM_SENSORS] = { 0, 1, 16 };
static const int mt2701_tc_offset[MT2701_NUM_CONTROLLER] = { 0x0, };

static const int mt2701_vts_index[MT2701_NUM_SENSORS] = {
	VTS1, VTS2, VTS3
};

/* MT2712 thermal sensor data */
static const int mt2712_bank_data[MT2712_NUM_SENSORS] = {
	MT2712_TS1, MT2712_TS2, MT2712_TS3, MT2712_TS4
};

static const int mt2712_msr[MT2712_NUM_SENSORS_PER_ZONE] = {
	TEMP_MSR0, TEMP_MSR1, TEMP_MSR2, TEMP_MSR3
};

static const int mt2712_adcpnp[MT2712_NUM_SENSORS_PER_ZONE] = {
	TEMP_ADCPNP0, TEMP_ADCPNP1, TEMP_ADCPNP2, TEMP_ADCPNP3
};

static const int mt2712_mux_values[MT2712_NUM_SENSORS] = { 0, 1, 2, 3 };
static const int mt2712_tc_offset[MT2712_NUM_CONTROLLER] = { 0x0, };

static const int mt2712_vts_index[MT2712_NUM_SENSORS] = {
	VTS1, VTS2, VTS3, VTS4
};

/* MT7622 thermal sensor data */
static const int mt7622_bank_data[MT7622_NUM_SENSORS] = { MT7622_TS1, };
static const int mt7622_msr[MT7622_NUM_SENSORS_PER_ZONE] = { TEMP_MSR0, };
static const int mt7622_adcpnp[MT7622_NUM_SENSORS_PER_ZONE] = { TEMP_ADCPNP0, };
static const int mt7622_mux_values[MT7622_NUM_SENSORS] = { 0, };
static const int mt7622_vts_index[MT7622_NUM_SENSORS] = { VTS1 };
static const int mt7622_tc_offset[MT7622_NUM_CONTROLLER] = { 0x0, };

/* MT7986 thermal sensor data */
static const int mt7986_bank_data[MT7986_NUM_SENSORS] = { MT7986_TS1, };
static const int mt7986_msr[MT7986_NUM_SENSORS_PER_ZONE] = { TEMP_MSR0, };
static const int mt7986_adcpnp[MT7986_NUM_SENSORS_PER_ZONE] = { TEMP_ADCPNP0, };
static const int mt7986_mux_values[MT7986_NUM_SENSORS] = { 0, };
static const int mt7986_vts_index[MT7986_NUM_SENSORS] = { VTS1 };
static const int mt7986_tc_offset[MT7986_NUM_CONTROLLER] = { 0x0, };

/* MT8365 thermal sensor data */
static const int mt8365_bank_data[MT8365_NUM_SENSORS] = {
	MT8365_TS1, MT8365_TS2, MT8365_TS3
};

static const int mt8365_msr[MT8365_NUM_SENSORS_PER_ZONE] = {
	TEMP_MSR0, TEMP_MSR1, TEMP_MSR2
};

static const int mt8365_adcpnp[MT8365_NUM_SENSORS_PER_ZONE] = {
	TEMP_ADCPNP0, TEMP_ADCPNP1, TEMP_ADCPNP2
};

static const int mt8365_mux_values[MT8365_NUM_SENSORS] = { 0, 1, 2 };
static const int mt8365_tc_offset[MT8365_NUM_CONTROLLER] = { 0 };

static const int mt8365_vts_index[MT8365_NUM_SENSORS] = { VTS1, VTS2, VTS3 };

/*
 * The MT8173 thermal controller has four banks. Each bank can read up to
 * four temperature sensors simultaneously. The MT8173 has a total of 5
 * temperature sensors. We use each bank to measure a certain area of the
 * SoC. Since TS2 is located centrally in the SoC it is influenced by multiple
 * areas, hence is used in different banks.
 *
 * The thermal core only gets the maximum temperature of all banks, so
 * the bank concept wouldn't be necessary here. However, the SVS (Smart
 * Voltage Scaling) unit makes its decisions based on the same bank
 * data, and this indeed needs the temperatures of the individual banks
 * for making better decisions.
 */
static const struct mtk_thermal_data mt8173_thermal_data = {
	.auxadc_channel = MT8173_TEMP_AUXADC_CHANNEL,
	.num_banks = MT8173_NUM_ZONES,
	.num_sensors = MT8173_NUM_SENSORS,
	.vts_index = mt8173_vts_index,
	.cali_val = MT8173_CALIBRATION,
	.num_controller = MT8173_NUM_CONTROLLER,
	.controller_offset = mt8173_tc_offset,
	.need_switch_bank = true,
	.bank_data = {
		{
			.num_sensors = 2,
			.sensors = mt8173_bank_data[0],
		}, {
			.num_sensors = 2,
			.sensors = mt8173_bank_data[1],
		}, {
			.num_sensors = 3,
			.sensors = mt8173_bank_data[2],
		}, {
			.num_sensors = 1,
			.sensors = mt8173_bank_data[3],
		},
	},
	.msr = mt8173_msr,
	.adcpnp = mt8173_adcpnp,
	.sensor_mux_values = mt8173_mux_values,
	.version = MTK_THERMAL_V1,
};

/*
 * The MT2701 thermal controller has one bank, which can read up to
 * three temperature sensors simultaneously. The MT2701 has a total of 3
 * temperature sensors.
 *
 * The thermal core only gets the maximum temperature of this one bank,
 * so the bank concept wouldn't be necessary here. However, the SVS (Smart
 * Voltage Scaling) unit makes its decisions based on the same bank
 * data.
 */
static const struct mtk_thermal_data mt2701_thermal_data = {
	.auxadc_channel = MT2701_TEMP_AUXADC_CHANNEL,
	.num_banks = 1,
	.num_sensors = MT2701_NUM_SENSORS,
	.vts_index = mt2701_vts_index,
	.cali_val = MT2701_CALIBRATION,
	.num_controller = MT2701_NUM_CONTROLLER,
	.controller_offset = mt2701_tc_offset,
	.need_switch_bank = true,
	.bank_data = {
		{
			.num_sensors = 3,
			.sensors = mt2701_bank_data,
		},
	},
	.msr = mt2701_msr,
	.adcpnp = mt2701_adcpnp,
	.sensor_mux_values = mt2701_mux_values,
	.version = MTK_THERMAL_V1,
};

/*
 * The MT8365 thermal controller has one bank, which can read up to
 * four temperature sensors simultaneously. The MT8365 has a total of 3
 * temperature sensors.
 *
 * The thermal core only gets the maximum temperature of this one bank,
 * so the bank concept wouldn't be necessary here. However, the SVS (Smart
 * Voltage Scaling) unit makes its decisions based on the same bank
 * data.
 */
static const struct mtk_thermal_data mt8365_thermal_data = {
	.auxadc_channel = MT8365_TEMP_AUXADC_CHANNEL,
	.num_banks = MT8365_NUM_BANKS,
	.num_sensors = MT8365_NUM_SENSORS,
	.vts_index = mt8365_vts_index,
	.cali_val = MT8365_CALIBRATION,
	.num_controller = MT8365_NUM_CONTROLLER,
	.controller_offset = mt8365_tc_offset,
	.need_switch_bank = false,
	.bank_data = {
		{
			.num_sensors = MT8365_NUM_SENSORS,
			.sensors = mt8365_bank_data
		},
	},
	.msr = mt8365_msr,
	.adcpnp = mt8365_adcpnp,
	.sensor_mux_values = mt8365_mux_values,
	.version = MTK_THERMAL_V1,
	.apmixed_buffer_ctl_reg = APMIXED_SYS_TS_CON0,
	.apmixed_buffer_ctl_mask = (u32) ~GENMASK(29, 28),
	.apmixed_buffer_ctl_set = 0,
};

static const int mt6797_bank_data[MT6797_NUM_ZONES][MT6797_NUM_SENSORS_PER_ZONE] = {
	{ MT6797_TS_MCU1 },
	{ MT6797_TS_MCU4 },
	{ MT6797_TS_MCU2, MT6797_TS_MCU3 },
	{ MT6797_TS_MCU2 },
	{ MT6797_TS_MCU2 },
	{ MT6797_TS_MCU2 },
};

static const int mt6797_msr[MT6797_NUM_SENSORS_PER_ZONE] = {
	TEMP_MSR0, TEMP_MSR1,
};

static const int mt6797_adcpnp[MT6797_NUM_SENSORS_PER_ZONE] = {
	TEMP_ADCPNP0, TEMP_ADCPNP1,
};

static const int mt6797_mux_values[MT6797_NUM_SENSORS] = {
	0, 1, 2, 3, 0,
};

static const int mt6797_vts_index[MT6797_NUM_SENSORS] = {
	VTS1, VTS2, VTS3, VTS4, VTSABB,
};

static const int mt6797_tc_offset[MT6797_NUM_CONTROLLER] = { 0x0 };

static const struct mtk_thermal_data mt6797_thermal_data = {
	.num_banks = MT6797_NUM_ZONES,
	.num_sensors = MT6797_NUM_SENSORS,
	.auxadc_channel = MT6797_TEMP_AUXADC_CHANNEL,
	.vts_index = mt6797_vts_index,
	.num_controller = MT6797_NUM_CONTROLLER,
	.controller_offset = mt6797_tc_offset,
	.need_switch_bank = true,
	.require_valid_calibration = true,
	.bank_data = {
		{ .num_sensors = 1, .sensors = mt6797_bank_data[0] },
		{ .num_sensors = 1, .sensors = mt6797_bank_data[1] },
		{ .num_sensors = 2, .sensors = mt6797_bank_data[2] },
		{ .num_sensors = 1, .sensors = mt6797_bank_data[3] },
		{ .num_sensors = 1, .sensors = mt6797_bank_data[4] },
		{ .num_sensors = 1, .sensors = mt6797_bank_data[5] },
	},
	.msr = mt6797_msr,
	.adcpnp = mt6797_adcpnp,
	.sensor_mux_values = mt6797_mux_values,
	.version = MTK_THERMAL_V4,
	.apmixed_buffer_ctl_reg = APMIXED_SYS_TS_CON1,
	.apmixed_buffer_ctl_mask = (u32)~GENMASK(5, 4),
	.apmixed_buffer_ctl_set = 0,
	.temp_ahbpoll = 0x30d,
	.temp_msrctl0 = 0x492,
	.temp_adcvalidmask = 0x2c,
};

/*
 * The MT2712 thermal controller has one bank, which can read up to
 * four temperature sensors simultaneously. The MT2712 has a total of 4
 * temperature sensors.
 *
 * The thermal core only gets the maximum temperature of this one bank,
 * so the bank concept wouldn't be necessary here. However, the SVS (Smart
 * Voltage Scaling) unit makes its decisions based on the same bank
 * data.
 */
static const struct mtk_thermal_data mt2712_thermal_data = {
	.auxadc_channel = MT2712_TEMP_AUXADC_CHANNEL,
	.num_banks = 1,
	.num_sensors = MT2712_NUM_SENSORS,
	.vts_index = mt2712_vts_index,
	.cali_val = MT2712_CALIBRATION,
	.num_controller = MT2712_NUM_CONTROLLER,
	.controller_offset = mt2712_tc_offset,
	.need_switch_bank = true,
	.bank_data = {
		{
			.num_sensors = 4,
			.sensors = mt2712_bank_data,
		},
	},
	.msr = mt2712_msr,
	.adcpnp = mt2712_adcpnp,
	.sensor_mux_values = mt2712_mux_values,
	.version = MTK_THERMAL_V1,
};

/*
 * MT7622 have only one sensing point which uses AUXADC Channel 11 for raw data
 * access.
 */
static const struct mtk_thermal_data mt7622_thermal_data = {
	.auxadc_channel = MT7622_TEMP_AUXADC_CHANNEL,
	.num_banks = MT7622_NUM_ZONES,
	.num_sensors = MT7622_NUM_SENSORS,
	.vts_index = mt7622_vts_index,
	.cali_val = MT7622_CALIBRATION,
	.num_controller = MT7622_NUM_CONTROLLER,
	.controller_offset = mt7622_tc_offset,
	.need_switch_bank = true,
	.bank_data = {
		{
			.num_sensors = 1,
			.sensors = mt7622_bank_data,
		},
	},
	.msr = mt7622_msr,
	.adcpnp = mt7622_adcpnp,
	.sensor_mux_values = mt7622_mux_values,
	.version = MTK_THERMAL_V2,
	.apmixed_buffer_ctl_reg = APMIXED_SYS_TS_CON1,
	.apmixed_buffer_ctl_mask = GENMASK(31, 6) | BIT(3),
	.apmixed_buffer_ctl_set = BIT(0),
};

/*
 * The MT8183 thermal controller has one bank for the current SW framework.
 * The MT8183 has a total of 6 temperature sensors.
 * There are two thermal controller to control the six sensor.
 * The first one bind 2 sensor, and the other bind 4 sensors.
 * The thermal core only gets the maximum temperature of all sensor, so
 * the bank concept wouldn't be necessary here. However, the SVS (Smart
 * Voltage Scaling) unit makes its decisions based on the same bank
 * data, and this indeed needs the temperatures of the individual banks
 * for making better decisions.
 */
static const struct mtk_thermal_data mt8183_thermal_data = {
	.auxadc_channel = MT8183_TEMP_AUXADC_CHANNEL,
	.num_banks = MT8183_NUM_ZONES,
	.num_sensors = MT8183_NUM_SENSORS,
	.vts_index = mt8183_vts_index,
	.cali_val = MT8183_CALIBRATION,
	.num_controller = MT8183_NUM_CONTROLLER,
	.controller_offset = mt8183_tc_offset,
	.need_switch_bank = false,
	.bank_data = {
		{
			.num_sensors = 6,
			.sensors = mt8183_bank_data,
		},
	},

	.msr = mt8183_msr,
	.adcpnp = mt8183_adcpnp,
	.sensor_mux_values = mt8183_mux_values,
	.version = MTK_THERMAL_V1,
};

/*
 * MT7986 uses AUXADC Channel 11 for raw data access.
 */
static const struct mtk_thermal_data mt7986_thermal_data = {
	.auxadc_channel = MT7986_TEMP_AUXADC_CHANNEL,
	.num_banks = MT7986_NUM_ZONES,
	.num_sensors = MT7986_NUM_SENSORS,
	.vts_index = mt7986_vts_index,
	.cali_val = MT7986_CALIBRATION,
	.num_controller = MT7986_NUM_CONTROLLER,
	.controller_offset = mt7986_tc_offset,
	.need_switch_bank = true,
	.bank_data = {
		{
			.num_sensors = 1,
			.sensors = mt7986_bank_data,
		},
	},
	.msr = mt7986_msr,
	.adcpnp = mt7986_adcpnp,
	.sensor_mux_values = mt7986_mux_values,
	.version = MTK_THERMAL_V3,
	.apmixed_buffer_ctl_reg = APMIXED_SYS_TS_CON1,
	.apmixed_buffer_ctl_mask = GENMASK(31, 6) | BIT(3),
	.apmixed_buffer_ctl_set = BIT(0),
};

static bool mtk_thermal_temp_is_valid(int temp)
{
	return (temp >= MT8173_TEMP_MIN) && (temp <= MT8173_TEMP_MAX);
}

/**
 * raw_to_mcelsius_v1 - convert a raw ADC value to mcelsius
 * @mt:	The thermal controller
 * @sensno:	sensor number
 * @raw:	raw ADC value
 *
 * This converts the raw ADC value to mcelsius using the SoC specific
 * calibration constants
 */
static int raw_to_mcelsius_v1(struct mtk_thermal *mt, int sensno, s32 raw)
{
	s32 tmp;

	raw &= 0xfff;

	tmp = 203450520 << 3;
	tmp /= mt->conf->cali_val + mt->o_slope;
	tmp /= 10000 + mt->adc_ge;
	tmp *= raw - mt->vts[sensno] - 3350;
	tmp >>= 3;

	return mt->degc_cali * 500 - tmp;
}

static int raw_to_mcelsius_v2(struct mtk_thermal *mt, int sensno, s32 raw)
{
	s32 format_1;
	s32 format_2;
	s32 g_oe;
	s32 g_gain;
	s32 g_x_roomt;
	s32 tmp;

	if (raw == 0)
		return 0;

	raw &= 0xfff;
	g_gain = 10000 + (((mt->adc_ge - 512) * 10000) >> 12);
	g_oe = mt->adc_oe - 512;
	format_1 = mt->vts[VTS2] + 3105 - g_oe;
	format_2 = (mt->degc_cali * 10) >> 1;
	g_x_roomt = (((format_1 * 10000) >> 12) * 10000) / g_gain;

	tmp = (((((raw - g_oe) * 10000) >> 12) * 10000) / g_gain) - g_x_roomt;
	tmp = tmp * 10 * 100 / 11;

	if (mt->o_slope_sign == 0)
		tmp = tmp / (165 - mt->o_slope);
	else
		tmp = tmp / (165 + mt->o_slope);

	return (format_2 - tmp) * 100;
}

static int raw_to_mcelsius_v3(struct mtk_thermal *mt, int sensno, s32 raw)
{
	s32 tmp;

	if (raw == 0)
		return 0;

	raw &= 0xfff;
	tmp = 100000 * 15 / 16 * 10000;
	tmp /= 4096 - 512 + mt->adc_ge;
	tmp /= 1490;
	tmp *= raw - mt->vts[sensno] - 2900;

	return mt->degc_cali * 500 - tmp;
}

static int raw_to_mcelsius_v4(struct mtk_thermal *mt, int sensno, s32 raw)
{
	s64 gain, x_roomt, measured, delta, slope_denominator;

	raw &= 0xfff;
	if (!raw)
		return THERMAL_TEMP_INVALID;

	gain = 10000 + ((s64)(mt->adc_ge - 512) * 10000) / 4096;
	if (gain <= 0)
		return THERMAL_TEMP_INVALID;

	x_roomt = (((((s64)mt->vts[sensno] + 3350 - mt->adc_oe) *
		      10000) >> 12) * 10000) / gain;
	measured = (((((s64)raw - mt->adc_oe) * 10000) >> 12) *
		    10000) / gain;
	delta = (measured - x_roomt) * 15 / 18;
	slope_denominator = 1663 +
		(mt->o_slope_sign ? -1 : 1) * (s64)mt->o_slope * 10;
	if (slope_denominator <= 0)
		return THERMAL_TEMP_INVALID;

	delta = (delta * 1000) / slope_denominator;

	/* Vendor result is in 0.1 degrees C; return milli-degrees C. */
	return (int)(((s64)(mt->degc_cali * 10 / 2) - delta) * 100);
}

/**
 * mtk_thermal_get_bank - get bank
 * @bank:	The bank
 *
 * The bank registers are banked, we have to select a bank in the
 * PTPCORESEL register to access it.
 */
static void mtk_thermal_get_bank(struct mtk_thermal_bank *bank)
{
	struct mtk_thermal *mt = bank->mt;
	u32 val;

	if (mt->conf->need_switch_bank) {
		mutex_lock(&mt->lock);

		val = readl(mt->thermal_base + PTPCORESEL);
		val &= ~0xf;
		val |= bank->id;
		writel(val, mt->thermal_base + PTPCORESEL);
	}
}

/**
 * mtk_thermal_put_bank - release bank
 * @bank:	The bank
 *
 * release a bank previously taken with mtk_thermal_get_bank,
 */
static void mtk_thermal_put_bank(struct mtk_thermal_bank *bank)
{
	struct mtk_thermal *mt = bank->mt;

	if (mt->conf->need_switch_bank)
		mutex_unlock(&mt->lock);
}

/**
 * mtk_thermal_bank_temperature - get the temperature of a bank
 * @bank:	The bank
 *
 * The temperature of a bank is considered the maximum temperature of
 * the sensors associated to the bank.
 */
static int mtk_thermal_bank_temperature(struct mtk_thermal_bank *bank)
{
	struct mtk_thermal *mt = bank->mt;
	const struct mtk_thermal_data *conf = mt->conf;
	int i, temp = INT_MIN, max = INT_MIN;
	u32 raw;

	for (i = 0; i < conf->bank_data[bank->id].num_sensors; i++) {
		raw = readl(mt->thermal_base + conf->msr[i]);

		temp = mt->raw_to_mcelsius(
			mt, conf->bank_data[bank->id].sensors[i], raw);

		/*
		 * Depending on the filt/sen intervals and ADC polling time,
		 * we may need up to 60 milliseconds after initialization: this
		 * will result in the first reading containing an out of range
		 * temperature value.
		 * Validate the reading to both address the aforementioned issue
		 * and to eventually avoid bogus readings during runtime in the
		 * event that the AUXADC gets unstable due to high EMI, etc.
		 */
		if (!mtk_thermal_temp_is_valid(temp))
			temp = THERMAL_TEMP_INVALID;

		if (temp > max)
			max = temp;
	}

	return max;
}

static void mt6797_eem_unpack_anchors(u32 packed, u8 *anchors)
{
	unsigned int i;

	for (i = 0; i < MT6797_EEM_READBACK_ANCHORS / 2; i++)
		anchors[i] = (packed >> (i * 8)) & 0xff;
}

static int mt6797_thermal_temperature_locked(struct mtk_thermal *mt,
						      u32 selector)
{
	int i;
	int maximum = INT_MIN;

	for (i = 0; i < mt->conf->num_banks; i++) {
		struct mtk_thermal_bank *bank = &mt->banks[i];
		u32 value;
		int temperature;

		value = (selector & ~GENMASK(3, 0)) |
			(bank->id & GENMASK(3, 0));
		writel(value, mt->thermal_base + PTPCORESEL);
		readl(mt->thermal_base + PTPCORESEL);

		temperature = mtk_thermal_bank_temperature(bank);
		maximum = max(maximum, temperature);
	}

	return maximum;
}

int mt6797_thermal_eem_readback(struct device *dev,
				 struct mt6797_eem_readback *readback)
{
	static const u8 bank_ids[MT6797_EEM_READBACK_BANK_COUNT] = {
		MT6797_EEM_READBACK_BANK_BIG,
		MT6797_EEM_READBACK_BANK_L,
		MT6797_EEM_READBACK_BANK_2L,
		MT6797_EEM_READBACK_BANK_CCI,
	};
	struct mtk_thermal *mt;
	u32 selector;
	u32 restored_selector;
	unsigned int i;

	if (!dev || !readback)
		return -EINVAL;

	mt = dev_get_drvdata(dev);
	if (!mt)
		return -EPROBE_DEFER;
	if (!mt->conf || mt->conf->version != MTK_THERMAL_V4)
		return -EOPNOTSUPP;

	memset(readback, 0, sizeof(*readback));
	readback->abi = MT6797_EEM_READBACK_ABI;

	/* The thermal lock also serializes PTPCORESEL bank ownership. */
	mutex_lock(&mt->lock);
	selector = readl(mt->thermal_base + PTPCORESEL);
	readback->selector_before = selector;
	readback->thermal_interrupt_status =
		readl(mt->thermal_base + MT6797_EEM_THERM_INTSTS);
	readback->modulation_interrupt_status =
		readl(mt->thermal_base + MT6797_EEM_MOD_INTSTS);

	for (i = 0; i < ARRAY_SIZE(bank_ids); i++) {
		struct mt6797_eem_readback_bank *bank = &readback->bank[i];
		u32 value;

		value = (selector & ~MT6797_EEMCORESEL_MASK) | bank_ids[i];
		writel(value, mt->thermal_base + PTPCORESEL);
		readl(mt->thermal_base + PTPCORESEL);

		bank->bank = bank_ids[i];
		bank->eem_enable = readl(mt->thermal_base + MT6797_EEM_EEMEN);
		bank->interrupt_status = readl(mt->thermal_base +
						MT6797_EEM_INTSTS);
		bank->interrupt_raw = readl(mt->thermal_base +
						MT6797_EEM_INTSTS_RAW);
		bank->interrupt_enable = readl(mt->thermal_base +
						MT6797_EEM_INTEN);
		bank->dc_values = readl(mt->thermal_base +
						MT6797_EEM_DCVALUES);
		bank->age_values = readl(mt->thermal_base +
						MT6797_EEM_AGEVALUES);
		bank->temperature = readl(mt->thermal_base + MT6797_EEM_TEMP);

		value = readl(mt->thermal_base + MT6797_EEM_FREQPCT30);
		mt6797_eem_unpack_anchors(value, bank->frequency_pct);
		value = readl(mt->thermal_base + MT6797_EEM_VOP30);
		mt6797_eem_unpack_anchors(value, bank->vop);
		value = readl(mt->thermal_base + MT6797_EEM_FREQPCT74);
		mt6797_eem_unpack_anchors(value,
					 &bank->frequency_pct[4]);
		value = readl(mt->thermal_base + MT6797_EEM_VOP74);
		mt6797_eem_unpack_anchors(value, &bank->vop[4]);
		readback->bank_mask |= BIT(bank_ids[i]);
	}

	readback->temperature_millic =
		mt6797_thermal_temperature_locked(mt, selector);

	/* Restore the exact selector word, including unrelated control bits. */
	writel(selector, mt->thermal_base + PTPCORESEL);
	readl(mt->thermal_base + PTPCORESEL);
	restored_selector = readl(mt->thermal_base + PTPCORESEL);
	readback->selector_after = restored_selector;
	mutex_unlock(&mt->lock);

	if (restored_selector != selector ||
	    readback->bank_mask != (BIT(MT6797_EEM_READBACK_BANK_BIG) |
					BIT(MT6797_EEM_READBACK_BANK_L) |
					BIT(MT6797_EEM_READBACK_BANK_2L) |
					BIT(MT6797_EEM_READBACK_BANK_CCI)))
		return -EIO;
	if (readback->temperature_millic == INT_MIN)
		return -EAGAIN;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_thermal_eem_readback);

#define MT6797_EEM_CALIBRATION_EEM_BASE_UV	700000
#define MT6797_EEM_CALIBRATION_EEM_STEP_UV	6250
#define MT6797_EEM_CALIBRATION_BIG_BASE_UV	300000
#define MT6797_EEM_CALIBRATION_BIG_STEP_UV	10000

static int mt6797_eem_calibration_provenance_check(
	const struct mt6797_dvfsp_state_provenance *provenance)
{
	if (!provenance ||
	    provenance->abi != MT6797_DVFSP_STATE_PROVENANCE_ABI ||
	    provenance->source_mask != MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL ||
	    !provenance->variant_id || !provenance->table_epoch ||
	    !provenance->calibration_handle)
		return -EPROTO;

	return 0;
}

static int mt6797_eem_calibration_ptp_state_check(
	const struct mt6797_dvfsp_ptp_state *state)
{
	static const u32 expected_banks[MT6797_EEM_CALIBRATION_BANKS] = {
		MT6797_DVFSP_PTP_BANK_BIG,
		MT6797_DVFSP_PTP_BANK_L,
		MT6797_DVFSP_PTP_BANK_2L,
		MT6797_DVFSP_PTP_BANK_CCI,
	};
	unsigned int i;

	if (!state || state->abi != MT6797_DVFSP_PTP_STATE_ABI ||
	    state->bank_mask != MT6797_DVFSP_PTP_BANK_MASK_ALL)
		return -EPROTO;

	for (i = 0; i < MT6797_EEM_CALIBRATION_BANKS; i++) {
		const struct mt6797_dvfsp_ptp_bank_state *bank = &state->bank[i];

		if (bank->bank != expected_banks[i] ||
		    bank->init_enable != 1 || bank->mon_enable != 1 ||
		    bank->dvfs_level > 3 || bank->bin_spec > 7)
			return -EPROTO;
	}

	return 0;
}

static u32 mt6797_eem_calibration_code_to_uv(unsigned int bank, u8 code)
{
	if (bank == MT6797_EEM_READBACK_BANK_BIG)
		return MT6797_EEM_CALIBRATION_BIG_BASE_UV +
			(unsigned int)code * MT6797_EEM_CALIBRATION_BIG_STEP_UV;

	return MT6797_EEM_CALIBRATION_EEM_BASE_UV +
		(unsigned int)code * MT6797_EEM_CALIBRATION_EEM_STEP_UV;
}

static int mt6797_eem_calibration_interpolate(u8 target, u8 x0, u8 x1,
						 u32 y0, u32 y1, u32 *result)
{
	s64 numerator;
	s64 denominator;
	s64 value;

	if (!result)
		return -EINVAL;
	if (x0 == x1) {
		if (y0 != y1)
			return -EPROTO;
		*result = y0;
		return 0;
	}

	numerator = ((s64)y1 - (s64)y0) * ((s64)target - (s64)x0);
	denominator = (s64)x1 - (s64)x0;
	value = (s64)y0 + div_s64(numerator, denominator);
	if (value < 0 || value > U32_MAX)
		return -ERANGE;
	*result = (u32)value;

	return 0;
}

static int mt6797_eem_calibration_ppm_check(
	const struct mt6797_eem_calibration_input *input)
{
	static const u32 ppm_clusters[MT6797_DVFSP_PPM_CLUSTER_COUNT] = {
		MT6797_DVFSP_PPM_CLUSTER_B,
		MT6797_DVFSP_PPM_CLUSTER_L,
		MT6797_DVFSP_PPM_CLUSTER_LL,
	};
	unsigned int bank_index;
	int ret;

	if (!input || !input->ppm_snapshot || !input->ppm_policy)
		return -EINVAL;
	ret = mt6797_dvfsp_ppm_snapshot_validate(input->ppm_snapshot);
	if (ret)
		return ret;
	if (input->provenance.table_epoch !=
	    input->ppm_snapshot->table_epoch)
		return -EAGAIN;
	ret = mt6797_dvfsp_ppm_policy_validate(input->ppm_policy);
	if (ret)
		return ret;
	if (input->provenance.table_epoch != input->ppm_policy->table_epoch)
		return -EAGAIN;
	for (bank_index = 0;
	     bank_index < MT6797_DVFSP_PPM_POLICY_BANK_COUNT; bank_index++) {
		unsigned int row;

		for (row = 0; row < MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES; row++)
			if (input->frequency_khz[bank_index][row] !=
				    input->ppm_policy->frequency_khz[bank_index][row] ||
			    input->ppm_limit_khz[bank_index][row] !=
				    input->ppm_policy->limit_khz[bank_index][row])
				return -EAGAIN;
	}

	for (bank_index = 0;
	     bank_index < MT6797_DVFSP_PPM_CLUSTER_COUNT; bank_index++) {
		const struct mt6797_dvfsp_ppm_cluster_snapshot *cluster =
			&input->ppm_snapshot->cluster[ppm_clusters[bank_index]];
		unsigned int row;

		for (row = 0; row < MT6797_DVFSP_PPM_TABLE_ENTRIES; row++) {
			if (input->frequency_khz[bank_index][row] !=
			    cluster->frequency_khz[row])
				return -EAGAIN;
		}
	}

	return 0;
}

static int mt6797_eem_calibration_row_voltage(
	const struct mt6797_eem_calibration_input *input,
	const struct mt6797_eem_readback_bank *bank,
	unsigned int bank_index, unsigned int row, u32 *vproc_uv)
{
	u32 raw_uv;
	int ret;

	if (row % 2 == 0) {
		raw_uv = mt6797_eem_calibration_code_to_uv(
			bank->bank, bank->vop[row / 2]);
	} else if (row + 1 < MT6797_EEM_CALIBRATION_ROWS) {
		ret = mt6797_eem_calibration_interpolate(
			input->frequency_pct[bank_index][row],
			input->frequency_pct[bank_index][row - 1],
			input->frequency_pct[bank_index][row + 1],
			mt6797_eem_calibration_code_to_uv(
				bank->bank, bank->vop[(row - 1) / 2]),
			mt6797_eem_calibration_code_to_uv(
				bank->bank, bank->vop[(row + 1) / 2]),
			&raw_uv);
		if (ret)
			return ret;
	} else {
		ret = mt6797_eem_calibration_interpolate(
			input->frequency_pct[bank_index][row],
			input->frequency_pct[bank_index][row - 3],
			input->frequency_pct[bank_index][row - 1],
			mt6797_eem_calibration_code_to_uv(
				bank->bank, bank->vop[(row - 3) / 2]),
			mt6797_eem_calibration_code_to_uv(
				bank->bank, bank->vop[(row - 1) / 2]),
			&raw_uv);
		if (ret)
			return ret;
	}

	if (input->temperature_millic <= 33000)
		raw_uv += MT6797_EEM_CALIBRATION_TEMP_OFFSET_UV;
	*vproc_uv = clamp_t(u32, raw_uv, input->vmin_uv[bank_index],
				     input->vmax_uv[bank_index]);
	if (*vproc_uv > input->record_vproc_uv[bank_index][row])
		*vproc_uv = input->record_vproc_uv[bank_index][row];
	if (*vproc_uv < input->vmin_uv[bank_index])
		return -EPROTO;

	return 0;
}

int mt6797_eem_calibration_build(
	const struct mt6797_eem_calibration_input *input,
	struct mt6797_dvfsp_calibration_state *state)
{
	static const u32 bank_ids[MT6797_EEM_CALIBRATION_BANKS] = {
		MT6797_EEM_READBACK_BANK_BIG,
		MT6797_EEM_READBACK_BANK_L,
		MT6797_EEM_READBACK_BANK_2L,
		MT6797_EEM_READBACK_BANK_CCI,
	};
	static const u32 cluster_ids[MT6797_EEM_CALIBRATION_BANKS] = {
		MT6797_DVFSP_STATE_CLUSTER_B,
		MT6797_DVFSP_STATE_CLUSTER_L,
		MT6797_DVFSP_STATE_CLUSTER_LL,
		MT6797_DVFSP_STATE_CLUSTER_CCI,
	};
	unsigned int bank_index;
	int ret;

	if (!input || !state || !input->readback ||
	    !input->ptp_state ||
	    !input->ppm_snapshot ||
	    mt6797_eem_calibration_ptp_state_check(input->ptp_state) ||
	    input->temperature_millic < -40000 ||
	    input->temperature_millic > 150000 ||
	    !input->thermal_generation || !input->clock_owner_generation ||
	    !input->rail_owner_generation ||
	    !input->source_generation ||
	    mt6797_eem_calibration_provenance_check(&input->provenance))
		return -EPROTO;
	ret = mt6797_eem_calibration_ppm_check(input);
	if (ret)
		return ret;
	if (input->readback->abi != MT6797_EEM_READBACK_ABI ||
	    input->readback->bank_mask !=
		(BIT(MT6797_EEM_READBACK_BANK_BIG) |
		 BIT(MT6797_EEM_READBACK_BANK_L) |
		 BIT(MT6797_EEM_READBACK_BANK_2L) |
		 BIT(MT6797_EEM_READBACK_BANK_CCI)) ||
	    input->readback->selector_before != input->readback->selector_after ||
	    input->temperature_millic != input->readback->temperature_millic)
		return -EAGAIN;

	memset(state, 0, sizeof(*state));
	state->abi = MT6797_DVFSP_CALIBRATION_STATE_ABI;
	state->phase = MT6797_DVFSP_CALIBRATION_STATE_PHASE_MON;
	state->bank_mask = MT6797_DVFSP_CALIBRATION_STATE_BANK_ALL;
	state->temperature_millic = input->temperature_millic;
	state->thermal_generation = input->thermal_generation;
	state->clock_owner_generation = input->clock_owner_generation;
	state->rail_owner_generation = input->rail_owner_generation;
	state->source_generation = input->source_generation;
	state->provenance = input->provenance;

	for (bank_index = 0; bank_index < MT6797_EEM_CALIBRATION_BANKS;
	     bank_index++) {
		const struct mt6797_eem_readback_bank *bank =
			&input->readback->bank[bank_index];
		struct mt6797_dvfsp_calibration_cluster *cluster;
		unsigned int row;

		if (bank->bank != bank_ids[bank_index] ||
		    (bank->eem_enable & MT6797_EEM_CALIBRATION_ENABLE_MASK) !=
			MT6797_EEM_CALIBRATION_MON_ENABLE ||
		    bank->interrupt_status &
			~MT6797_EEM_CALIBRATION_MON_STATUS_MASK)
			return -EAGAIN;
		if (input->vmin_uv[bank_index] >= input->vmax_uv[bank_index])
			return -EPROTO;

		for (row = 0; row < MT6797_EEM_CALIBRATION_ROWS; row++) {
			u32 vsram_uv = input->vsram_uv[bank_index][row];

			if (!input->frequency_pct[bank_index][row] ||
			    !input->frequency_khz[bank_index][row] ||
			    !input->record_vproc_uv[bank_index][row] ||
			    !input->ppm_limit_khz[bank_index][row] ||
			    !vsram_uv ||
			    (row && input->frequency_khz[bank_index][row] >=
				input->frequency_khz[bank_index][row - 1]) ||
			    (row && input->frequency_pct[bank_index][row] >
				input->frequency_pct[bank_index][row - 1]) ||
			    input->record_vproc_uv[bank_index][row] <
				input->vmin_uv[bank_index] ||
			    vsram_uv < MT6797_EEM_CALIBRATION_VSRAM_MIN_UV ||
			    vsram_uv > MT6797_EEM_CALIBRATION_VSRAM_MAX_UV)
				return -EPROTO;
			if (row % 2 == 0 &&
			    input->frequency_pct[bank_index][row] !=
				bank->frequency_pct[row / 2])
				return -EAGAIN;
		}

		cluster = &state->cluster[cluster_ids[bank_index]];
		cluster->table_count = MT6797_EEM_CALIBRATION_ROWS;
		for (row = 0; row < MT6797_EEM_CALIBRATION_ROWS; row++) {
			struct mt6797_dvfsp_calibration_table_entry *entry =
				&cluster->table[MT6797_EEM_CALIBRATION_ROWS - 1 - row];
			u32 vsram_uv = input->vsram_uv[bank_index][row];
			int ret;

			ret = mt6797_eem_calibration_row_voltage(
				input, bank, bank_index, row, &entry->vproc_uv);
			if (ret)
				return ret;
			if (vsram_uv < entry->vproc_uv +
				MT6797_EEM_CALIBRATION_VSRAM_DELTA_MIN_UV ||
			    vsram_uv > entry->vproc_uv +
				MT6797_EEM_CALIBRATION_VSRAM_DELTA_MAX_UV)
				return -EPROTO;
			entry->frequency_khz = input->frequency_khz[bank_index][row];
			entry->vsram_uv = vsram_uv;
			entry->ppm_limit_khz =
				input->ppm_limit_khz[bank_index][row];
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_eem_calibration_build);

static int mtk_read_temp(struct thermal_zone_device *tz, int *temperature)
{
	struct mtk_thermal *mt = thermal_zone_device_priv(tz);
	int i;
	int tempmax = INT_MIN;

	for (i = 0; i < mt->conf->num_banks; i++) {
		struct mtk_thermal_bank *bank = &mt->banks[i];

		mtk_thermal_get_bank(bank);

		tempmax = max(tempmax, mtk_thermal_bank_temperature(bank));

		mtk_thermal_put_bank(bank);
	}

	if (mt->conf == &mt6797_thermal_data &&
	    !mtk_thermal_temp_is_valid(tempmax))
		return -EAGAIN;

	*temperature = tempmax;

	return 0;
}

static const struct thermal_zone_device_ops mtk_thermal_ops = {
	.get_temp = mtk_read_temp,
};

#if IS_REACHABLE(CONFIG_HWMON)
/*
 * Six MT6797 banks sample four physical MCU sensors. Banks 3/4/5 all
 * reuse MCU2; expose each physical sensor once, not six thermometers.
 * ABB has calibration fuses but is not sampled by the active bank table.
 */
static const struct {
	u8 bank;
	u8 slot;
	u8 sensor;
	const char *label;
} mt6797_hwmon_sensors[] = {
	{ 0, 0, MT6797_TS_MCU1, "MCU1 (A72)" },
	{ 2, 0, MT6797_TS_MCU2, "MCU2 (A53/SoC shared)" },
	{ 2, 1, MT6797_TS_MCU3, "MCU3 (SoC)" },
	{ 1, 0, MT6797_TS_MCU4, "MCU4 (GPU)" },
};

static umode_t mt6797_hwmon_is_visible(const void *data,
				       enum hwmon_sensor_types type,
				       u32 attr, int channel)
{
	if (type != hwmon_temp || channel < 0 ||
	    channel >= ARRAY_SIZE(mt6797_hwmon_sensors))
		return 0;

	return attr == hwmon_temp_input || attr == hwmon_temp_label ? 0444 : 0;
}

static int mt6797_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *value)
{
	struct mtk_thermal *mt = dev_get_drvdata(dev);
	u32 selector, selected, raw;
	int temp;

	if (type != hwmon_temp || attr != hwmon_temp_input || channel < 0 ||
	    channel >= ARRAY_SIZE(mt6797_hwmon_sensors))
		return -EOPNOTSUPP;

	/* Share ownership with aggregate reads and EEM; restore every bit. */
	mutex_lock(&mt->lock);
	selector = readl(mt->thermal_base + PTPCORESEL);
	selected = (selector & ~0xf) | mt6797_hwmon_sensors[channel].bank;
	writel(selected, mt->thermal_base + PTPCORESEL);
	readl(mt->thermal_base + PTPCORESEL);
	raw = readl(mt->thermal_base +
		    mt->conf->msr[mt6797_hwmon_sensors[channel].slot]);
	writel(selector, mt->thermal_base + PTPCORESEL);
	readl(mt->thermal_base + PTPCORESEL);
	mutex_unlock(&mt->lock);

	/* An unpopulated measurement must not masquerade as a real 0 C. */
	if (!(raw & 0xfff))
		return -EAGAIN;

	temp = mt->raw_to_mcelsius(mt, mt6797_hwmon_sensors[channel].sensor, raw);
	if (!mtk_thermal_temp_is_valid(temp))
		return -EAGAIN;

	*value = temp;
	return 0;
}

static int mt6797_hwmon_read_string(struct device *dev,
				    enum hwmon_sensor_types type,
				    u32 attr, int channel, const char **str)
{
	if (type != hwmon_temp || attr != hwmon_temp_label || channel < 0 ||
	    channel >= ARRAY_SIZE(mt6797_hwmon_sensors))
		return -EOPNOTSUPP;

	*str = mt6797_hwmon_sensors[channel].label;
	return 0;
}

static const struct hwmon_ops mt6797_hwmon_ops = {
	.is_visible = mt6797_hwmon_is_visible,
	.read = mt6797_hwmon_read,
	.read_string = mt6797_hwmon_read_string,
};

static const struct hwmon_channel_info * const mt6797_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL,
};

static const struct hwmon_chip_info mt6797_hwmon_chip = {
	.ops = &mt6797_hwmon_ops,
	.info = mt6797_hwmon_info,
};

static int mt6797_hwmon_register(struct mtk_thermal *mt)
{
	struct device *hwmon;

	if (mt->conf != &mt6797_thermal_data)
		return 0;

	hwmon = devm_hwmon_device_register_with_info(mt->dev, "mt6797_sensors",
						     mt, &mt6797_hwmon_chip, NULL);
	return PTR_ERR_OR_ZERO(hwmon);
}
#else
static int mt6797_hwmon_register(struct mtk_thermal *mt)
{
	return 0;
}
#endif

static void mtk_thermal_init_bank(struct mtk_thermal *mt, int num,
				  u32 apmixed_phys_base, u32 auxadc_phys_base,
				  int ctrl_id)
{
	struct mtk_thermal_bank *bank = &mt->banks[num];
	const struct mtk_thermal_data *conf = mt->conf;
	int i;

	int offset = mt->conf->controller_offset[ctrl_id];
	void __iomem *controller_base = mt->thermal_base + offset;

	bank->id = num;
	bank->mt = mt;

	mtk_thermal_get_bank(bank);

	/* bus clock 66M counting unit is 12 * 15.15ns * 256 = 46.540us */
	writel(TEMP_MONCTL1_PERIOD_UNIT(12), controller_base + TEMP_MONCTL1);

	/*
	 * filt interval is 1 * 46.540us = 46.54us,
	 * sen interval is 429 * 46.540us = 19.96ms
	 */
	writel(TEMP_MONCTL2_FILTER_INTERVAL(1) |
			TEMP_MONCTL2_SENSOR_INTERVAL(429),
			controller_base + TEMP_MONCTL2);

	/* poll is set to 10u */
	writel(TEMP_AHBPOLL_ADC_POLL_INTERVAL(mt->conf->temp_ahbpoll ?
					     mt->conf->temp_ahbpoll : 768),
	       controller_base + TEMP_AHBPOLL);

	/* temperature sampling control, 1 sample */
	writel(mt->conf->temp_msrctl0, controller_base + TEMP_MSRCTL0);

	/* exceed this polling time, IRQ would be inserted */
	writel(0xffffffff, controller_base + TEMP_AHBTO);

	/* number of interrupts per event, 1 is enough */
	writel(0x0, controller_base + TEMP_MONIDET0);
	writel(0x0, controller_base + TEMP_MONIDET1);

	/*
	 * The MT8173 thermal controller does not have its own ADC. Instead it
	 * uses AHB bus accesses to control the AUXADC. To do this the thermal
	 * controller has to be programmed with the physical addresses of the
	 * AUXADC registers and with the various bit positions in the AUXADC.
	 * Also the thermal controller controls a mux in the APMIXEDSYS register
	 * space.
	 */

	/*
	 * this value will be stored to TEMP_PNPMUXADDR (TEMP_SPARE0)
	 * automatically by hw
	 */
	writel(BIT(conf->auxadc_channel), controller_base + TEMP_ADCMUX);

	/* AHB address for auxadc mux selection */
	writel(auxadc_phys_base + AUXADC_CON1_CLR_V,
	       controller_base + TEMP_ADCMUXADDR);

	if (mt->conf->version == MTK_THERMAL_V1) {
		/* AHB address for pnp sensor mux selection */
		writel(apmixed_phys_base + APMIXED_SYS_TS_CON1,
		       controller_base + TEMP_PNPMUXADDR);
	}

	/* AHB value for auxadc enable */
	writel(BIT(conf->auxadc_channel), controller_base + TEMP_ADCEN);

	/* AHB address for auxadc enable (channel 0 immediate mode selected) */
	writel(auxadc_phys_base + AUXADC_CON1_SET_V,
	       controller_base + TEMP_ADCENADDR);

	/* AHB address for auxadc valid bit */
	writel(auxadc_phys_base + AUXADC_DATA(conf->auxadc_channel),
	       controller_base + TEMP_ADCVALIDADDR);

	/* AHB address for auxadc voltage output */
	writel(auxadc_phys_base + AUXADC_DATA(conf->auxadc_channel),
	       controller_base + TEMP_ADCVOLTADDR);

	/* read valid & voltage are at the same register */
	writel(0x0, controller_base + TEMP_RDCTRL);

	/* indicate where the valid bit is */
	writel(mt->conf->temp_adcvalidmask ?
	       mt->conf->temp_adcvalidmask :
	       TEMP_ADCVALIDMASK_VALID_HIGH | TEMP_ADCVALIDMASK_VALID_POS(12),
	       controller_base + TEMP_ADCVALIDMASK);

	/* no shift */
	writel(0x0, controller_base + TEMP_ADCVOLTAGESHIFT);

	/* enable auxadc mux write transaction */
	writel(TEMP_ADCWRITECTRL_ADC_MUX_WRITE,
		controller_base + TEMP_ADCWRITECTRL);

	for (i = 0; i < conf->bank_data[num].num_sensors; i++)
		writel(conf->sensor_mux_values[conf->bank_data[num].sensors[i]],
		       mt->thermal_base + conf->adcpnp[i]);

	writel((1 << conf->bank_data[num].num_sensors) - 1,
	       controller_base + TEMP_MONCTL0);

	writel(TEMP_ADCWRITECTRL_ADC_PNP_WRITE |
	       TEMP_ADCWRITECTRL_ADC_MUX_WRITE,
	       controller_base + TEMP_ADCWRITECTRL);

	mtk_thermal_put_bank(bank);
}

static u64 of_get_phys_base(struct device_node *np)
{
	struct resource res;

	if (of_address_to_resource(np, 0, &res))
		return OF_BAD_ADDR;

	return res.start;
}

static int mt6797_thermal_enable_auxadc_clock(void *context)
{
	struct mtk_thermal *mt = context;

	return clk_prepare_enable(mt->clk_auxadc);
}

static void mt6797_thermal_disable_auxadc_clock(void *context)
{
	struct mtk_thermal *mt = context;

	clk_disable_unprepare(mt->clk_auxadc);
}

static int mt6797_thermal_enable_thermal_clock(void *context)
{
	struct mtk_thermal *mt = context;

	return clk_prepare_enable(mt->clk_peri_therm);
}

static void mt6797_thermal_disable_thermal_clock(void *context)
{
	struct mtk_thermal *mt = context;

	clk_disable_unprepare(mt->clk_peri_therm);
}

static int mt6797_thermal_reset(void *context)
{
	struct mtk_thermal *mt = context;

	return reset_control_reset(mt->rst);
}

static void mt6797_thermal_assert_reset(void *context)
{
	struct mtk_thermal *mt = context;

	reset_control_assert(mt->rst);
}

static int mt6797_thermal_configure_apmixed(void *context)
{
	struct mtk_thermal *mt = context;
	u32 expected, value;

	mt->apmixed_buffer_saved = readl(mt->apmixed_base +
					     APMIXED_SYS_TS_CON1);
	expected = mtk_thermal_mt6797_apmixed_value(mt->apmixed_buffer_saved);
	writel(expected, mt->apmixed_base + APMIXED_SYS_TS_CON1);
	usleep_range(200, 250);
	value = readl(mt->apmixed_base + APMIXED_SYS_TS_CON1);

	return value == expected ? 0 : -EIO;
}

static void mt6797_thermal_restore_apmixed(void *context)
{
	struct mtk_thermal *mt = context;

	writel(mt->apmixed_buffer_saved,
	       mt->apmixed_base + APMIXED_SYS_TS_CON1);
}

static int mt6797_thermal_wait_for_idle(void *context)
{
	struct mtk_thermal *mt = context;
	u32 value;
	int ret;

	ret = readl_poll_timeout(mt->thermal_base + MT6797_THERMAL_AHB_STATUS,
				 value, mtk_thermal_mt6797_ahb_idle(value),
				 2, 100);
	if (ret)
		return ret;

	return readl_poll_timeout(mt->auxadc_base + AUXADC_CON2_V, value,
				  mtk_thermal_mt6797_auxadc_idle(value),
				  1000, 30000);
}

static void mt6797_thermal_pause_disable_banks(void *context)
{
	struct mtk_thermal *mt = context;
	unsigned int bank;

	for (bank = 0; bank < mt->conf->num_banks; bank++) {
		struct mtk_thermal_bank *selected = &mt->banks[bank];
		u32 value;

		mtk_thermal_get_bank(selected);
		value = readl(mt->thermal_base + TEMP_MSRCTL1);
		writel(value | MT6797_PERIODIC_PAUSE_MASK,
		       mt->thermal_base + TEMP_MSRCTL1);
		writel(0, mt->thermal_base + TEMP_MONCTL0);
		mtk_thermal_put_bank(selected);
	}
}

static void mt6797_thermal_disable_auxadc_channel(void *context)
{
	struct mtk_thermal *mt = context;
	u32 value;

	value = readl(mt->auxadc_base + AUXADC_CON0_V);
	writel(value & ~BIT(mt->conf->auxadc_channel),
	       mt->auxadc_base + AUXADC_CON0_V);
	writel(BIT(mt->conf->auxadc_channel),
	       mt->auxadc_base + AUXADC_CON1_CLR_V);
}

static int mt6797_thermal_clear_auxadc_channel(void *context)
{
	mt6797_thermal_disable_auxadc_channel(context);

	return 0;
}

static int mt6797_thermal_prepare_bank(void *context, unsigned int num)
{
	struct mtk_thermal *mt = context;
	struct mtk_thermal_bank *bank = &mt->banks[num];
	const struct mtk_thermal_data *conf = mt->conf;
	void __iomem *controller_base = mt->thermal_base +
		conf->controller_offset[0];
	unsigned int i;

	mtk_thermal_get_bank(bank);
	writel(TEMP_MONCTL1_PERIOD_UNIT(12),
	       controller_base + TEMP_MONCTL1);
	writel(TEMP_MONCTL2_FILTER_INTERVAL(1) |
	       TEMP_MONCTL2_SENSOR_INTERVAL(429),
	       controller_base + TEMP_MONCTL2);
	writel(TEMP_AHBPOLL_ADC_POLL_INTERVAL(conf->temp_ahbpoll),
	       controller_base + TEMP_AHBPOLL);
	writel(conf->temp_msrctl0, controller_base + TEMP_MSRCTL0);
	writel(0, controller_base + TEMP_MONIDET0);
	writel(0, controller_base + TEMP_MONIDET1);
	writel(BIT(conf->auxadc_channel), controller_base + TEMP_ADCMUX);
	writel(mt->auxadc_phys_base + AUXADC_CON1_CLR_V,
	       controller_base + TEMP_ADCMUXADDR);
	writel(mt->apmixed_phys_base + APMIXED_SYS_TS_CON1,
	       controller_base + TEMP_PNPMUXADDR);
	writel(BIT(conf->auxadc_channel), controller_base + TEMP_ADCEN);
	writel(mt->auxadc_phys_base + AUXADC_CON1_SET_V,
	       controller_base + TEMP_ADCENADDR);
	writel(mt->auxadc_phys_base + AUXADC_DATA(conf->auxadc_channel),
	       controller_base + TEMP_ADCVALIDADDR);
	writel(mt->auxadc_phys_base + AUXADC_DATA(conf->auxadc_channel),
	       controller_base + TEMP_ADCVOLTADDR);
	writel(0, controller_base + TEMP_RDCTRL);
	writel(conf->temp_adcvalidmask,
	       controller_base + TEMP_ADCVALIDMASK);
	writel(0, controller_base + TEMP_ADCVOLTAGESHIFT);
	writel(TEMP_ADCWRITECTRL_ADC_MUX_WRITE,
	       controller_base + TEMP_ADCWRITECTRL);

	for (i = 0; i < conf->bank_data[num].num_sensors; i++) {
		unsigned int sensor_id = conf->bank_data[num].sensors[i];

		writel(conf->sensor_mux_values[sensor_id],
		       controller_base + conf->adcpnp[i]);
	}

	writel(TEMP_ADCWRITECTRL_ADC_PNP_WRITE |
	       TEMP_ADCWRITECTRL_ADC_MUX_WRITE,
	       controller_base + TEMP_ADCWRITECTRL);
	mtk_thermal_put_bank(bank);

	return 0;
}

static int mt6797_thermal_commit_auxadc_channel(void *context)
{
	struct mtk_thermal *mt = context;

	writel(BIT(mt->conf->auxadc_channel),
	       mt->auxadc_base + AUXADC_CON1_SET_V);

	return 0;
}

static int mt6797_thermal_enable_bank(void *context, unsigned int num)
{
	struct mtk_thermal *mt = context;
	struct mtk_thermal_bank *bank = &mt->banks[num];

	mtk_thermal_get_bank(bank);
	writel(GENMASK(mt->conf->bank_data[num].num_sensors - 1, 0),
	       mt->thermal_base + TEMP_MONCTL0);
	mtk_thermal_put_bank(bank);

	return 0;
}

static int mt6797_thermal_release_bank(void *context, unsigned int num)
{
	struct mtk_thermal *mt = context;
	struct mtk_thermal_bank *bank = &mt->banks[num];
	u32 value;

	mtk_thermal_get_bank(bank);
	value = readl(mt->thermal_base + TEMP_MSRCTL1);
	writel(value & ~MT6797_PERIODIC_PAUSE_MASK,
	       mt->thermal_base + TEMP_MSRCTL1);
	mtk_thermal_put_bank(bank);

	return 0;
}

static int mt6797_thermal_first_sample(void *context, unsigned int num)
{
	struct mtk_thermal *mt = context;
	const struct thermal_bank_cfg *bank_conf =
		&mt->conf->bank_data[num];
	struct mtk_thermal_bank *bank = &mt->banks[num];
	unsigned int attempt, sensor;

	for (attempt = 0; attempt < MT6797_FIRST_SAMPLE_ATTEMPTS;
	     attempt++) {
		bool valid = true;

		mtk_thermal_get_bank(bank);
		for (sensor = 0; sensor < bank_conf->num_sensors; sensor++) {
			u32 raw = readl(mt->thermal_base + mt->conf->msr[sensor]);
			int temperature;

			temperature = mt->raw_to_mcelsius(mt,
					     bank_conf->sensors[sensor], raw);

			valid = mtk_thermal_mt6797_first_sample_valid(raw, temperature);
			if (!valid) {
				valid = false;
				break;
			}
		}
		mtk_thermal_put_bank(bank);
		if (valid)
			return 0;
		usleep_range(1000, 1500);
	}

	dev_err(mt->dev, "bank %u has no valid first thermal sample\n", num);

	return -ETIMEDOUT;
}

static const struct mtk_thermal_transaction_ops
mt6797_thermal_transaction_ops = {
	.enable_auxadc_clock = mt6797_thermal_enable_auxadc_clock,
	.disable_auxadc_clock = mt6797_thermal_disable_auxadc_clock,
	.enable_thermal_clock = mt6797_thermal_enable_thermal_clock,
	.disable_thermal_clock = mt6797_thermal_disable_thermal_clock,
	.reset_thermal = mt6797_thermal_reset,
	.assert_thermal_reset = mt6797_thermal_assert_reset,
	.configure_apmixed = mt6797_thermal_configure_apmixed,
	.restore_apmixed = mt6797_thermal_restore_apmixed,
	.wait_for_idle = mt6797_thermal_wait_for_idle,
	.pause_disable_banks = mt6797_thermal_pause_disable_banks,
	.clear_auxadc_channel = mt6797_thermal_clear_auxadc_channel,
	.disable_auxadc_channel = mt6797_thermal_disable_auxadc_channel,
	.prepare_bank = mt6797_thermal_prepare_bank,
	.commit_auxadc_channel = mt6797_thermal_commit_auxadc_channel,
	.enable_bank = mt6797_thermal_enable_bank,
	.release_bank = mt6797_thermal_release_bank,
	.first_sample = mt6797_thermal_first_sample,
};

static void mtk_thermal_iounmap(void *base)
{
	iounmap(base);
}

static int
mtk_thermal_map_phandle(struct device *dev, struct device_node *owner,
			const char *property, void __iomem **base, u64 *phys)
{
	struct device_node *node;
	int ret;

	node = of_parse_phandle(owner, property, 0);
	if (!node)
		return -ENODEV;

	*phys = of_get_phys_base(node);
	if (*phys == OF_BAD_ADDR) {
		of_node_put(node);
		return -EINVAL;
	}

	*base = of_iomap(node, 0);
	of_node_put(node);
	if (!*base)
		return -ENOMEM;

	ret = devm_add_action_or_reset(dev, mtk_thermal_iounmap, *base);
	if (ret)
		return ret;

	return 0;
}

static int mtk_thermal_extract_efuse_v1(struct mtk_thermal *mt, u32 *buf)
{
	int i;

	if (!(buf[0] & CALIB_BUF0_VALID_V1))
		return -EINVAL;

	mt->adc_ge = CALIB_BUF1_ADC_GE_V1(buf[1]);

	for (i = 0; i < mt->conf->num_sensors; i++) {
		switch (mt->conf->vts_index[i]) {
		case VTS1:
			mt->vts[VTS1] = CALIB_BUF0_VTS_TS1_V1(buf[0]);
			break;
		case VTS2:
			mt->vts[VTS2] = CALIB_BUF0_VTS_TS2_V1(buf[0]);
			break;
		case VTS3:
			mt->vts[VTS3] = CALIB_BUF1_VTS_TS3_V1(buf[1]);
			break;
		case VTS4:
			mt->vts[VTS4] = CALIB_BUF2_VTS_TS4_V1(buf[2]);
			break;
		case VTS5:
			mt->vts[VTS5] = CALIB_BUF2_VTS_TS5_V1(buf[2]);
			break;
		case VTSABB:
			mt->vts[VTSABB] =
				CALIB_BUF2_VTS_TSABB_V1(buf[2]);
			break;
		default:
			break;
		}
	}

	mt->degc_cali = CALIB_BUF0_DEGC_CALI_V1(buf[0]);
	if (CALIB_BUF1_ID_V1(buf[1]) &
	    CALIB_BUF0_O_SLOPE_SIGN_V1(buf[0]))
		mt->o_slope = -CALIB_BUF0_O_SLOPE_V1(buf[0]);
	else
		mt->o_slope = CALIB_BUF0_O_SLOPE_V1(buf[0]);

	return 0;
}

static int mtk_thermal_extract_efuse_v2(struct mtk_thermal *mt, u32 *buf)
{
	if (!CALIB_BUF1_VALID_V2(buf[1]))
		return -EINVAL;

	mt->adc_oe = CALIB_BUF0_ADC_OE_V2(buf[0]);
	mt->adc_ge = CALIB_BUF0_ADC_GE_V2(buf[0]);
	mt->degc_cali = CALIB_BUF0_DEGC_CALI_V2(buf[0]);
	mt->o_slope = CALIB_BUF0_O_SLOPE_V2(buf[0]);
	mt->vts[VTS1] = CALIB_BUF1_VTS_TS1_V2(buf[1]);
	mt->vts[VTS2] = CALIB_BUF1_VTS_TS2_V2(buf[1]);
	mt->vts[VTSABB] = CALIB_BUF1_VTS_TSABB_V2(buf[1]);
	mt->o_slope_sign = CALIB_BUF1_O_SLOPE_SIGN_V2(buf[1]);

	return 0;
}

static int mtk_thermal_extract_efuse_v3(struct mtk_thermal *mt, u32 *buf)
{
	if (!CALIB_BUF1_VALID_V3(buf[1]))
		return -EINVAL;

	mt->adc_ge = CALIB_BUF0_ADC_GE_V3(buf[0]);
	mt->degc_cali = CALIB_BUF0_DEGC_CALI_V3(buf[0]);
	mt->o_slope = CALIB_BUF0_O_SLOPE_V3(buf[0]);
	mt->vts[VTS1] = CALIB_BUF1_VTS_TS1_V3(buf[1]);
	mt->vts[VTS2] = CALIB_BUF1_VTS_TS2_V3(buf[1]);
	mt->vts[VTSABB] = CALIB_BUF1_VTS_TSABB_V3(buf[1]);
	mt->o_slope_sign = CALIB_BUF1_O_SLOPE_SIGN_V3(buf[1]);

	if (CALIB_BUF1_ID_V3(buf[1]) == 0)
		mt->o_slope = 0;

	return 0;
}

static int mtk_thermal_extract_efuse_v4(struct mtk_thermal *mt, u32 *buf)
{
	int i;

	/* MT6797 calibration-enable is bit 0 of efuse word 0x10206180. */
	if (!(buf[1] & BIT(0)))
		return -EINVAL;

	mt->adc_ge = (buf[0] >> 22) & 0x3ff;
	mt->adc_oe = (buf[0] >> 12) & 0x3ff;
	mt->degc_cali = (buf[1] >> 1) & 0x3f;
	mt->o_slope = (buf[1] >> 26) & 0x3f;
	mt->o_slope_sign = (buf[1] >> 7) & 0x1;
	mt->vts[VTS1] = (buf[1] >> 17) & 0x1ff;
	mt->vts[VTS2] = (buf[1] >> 8) & 0x1ff;
	mt->vts[VTS3] = buf[0] & 0x1ff;
	mt->vts[VTS4] = (buf[2] >> 23) & 0x1ff;
	mt->vts[VTSABB] = (buf[2] >> 14) & 0x1ff;

	/* ID=0 samples ignore the slope fuse in the vendor contract. */
	if (!((buf[0] >> 9) & 0x1))
		mt->o_slope = 0;

	if (mt->adc_ge < 265 || mt->adc_ge > 758 ||
	    mt->adc_oe < 265 || mt->adc_oe > 758 ||
	    mt->degc_cali < 1 || mt->degc_cali > 63)
		return -EINVAL;

	for (i = 0; i < mt->conf->num_sensors; i++)
		if (mt->vts[mt->conf->vts_index[i]] > 484)
			return -EINVAL;

	return 0;
}

static int mtk_thermal_get_calibration_data(struct device *dev,
					    struct mtk_thermal *mt)
{
	struct nvmem_cell *cell;
	u32 *buf;
	size_t len;
	int i, ret = 0;

	/* Start with default values */
	mt->adc_ge = 512;
	mt->adc_oe = 512;
	for (i = 0; i < MAX_NUM_VTS; i++)
		mt->vts[i] = 260;
	mt->degc_cali = 40;
	mt->o_slope = 0;

	cell = nvmem_cell_get(dev, "calibration-data");
	if (IS_ERR(cell)) {
		if (PTR_ERR(cell) == -EPROBE_DEFER)
			return PTR_ERR(cell);
		if (mt->conf->require_valid_calibration)
			return dev_err_probe(dev, PTR_ERR(cell),
					     "required calibration data unavailable\n");
		return 0;
	}

	buf = (u32 *)nvmem_cell_read(cell, &len);

	nvmem_cell_put(cell);

	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (len < 3 * sizeof(u32)) {
		dev_warn(dev, "invalid calibration data\n");
		ret = -EINVAL;
		goto out;
	}

	switch (mt->conf->version) {
	case MTK_THERMAL_V1:
		ret = mtk_thermal_extract_efuse_v1(mt, buf);
		break;
	case MTK_THERMAL_V2:
		ret = mtk_thermal_extract_efuse_v2(mt, buf);
		break;
	case MTK_THERMAL_V3:
		ret = mtk_thermal_extract_efuse_v3(mt, buf);
		break;
	case MTK_THERMAL_V4:
		ret = mtk_thermal_extract_efuse_v4(mt, buf);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret) {
		if (mt->conf->require_valid_calibration) {
			dev_err(dev, "required calibration data is invalid\n");
			goto out;
		}
		dev_info(dev, "Device not calibrated, using default calibration values\n");
		ret = 0;
	}

out:
	kfree(buf);

	return ret;
}

static const struct of_device_id mtk_thermal_of_match[] = {
	{
		.compatible = "mediatek,mt8173-thermal",
		.data = (void *)&mt8173_thermal_data,
	},
	{
		.compatible = "mediatek,mt2701-thermal",
		.data = (void *)&mt2701_thermal_data,
	},
	{
		.compatible = "mediatek,mt2712-thermal",
		.data = (void *)&mt2712_thermal_data,
	},
	{
		.compatible = "mediatek,mt7622-thermal",
		.data = (void *)&mt7622_thermal_data,
	},
	{
		.compatible = "mediatek,mt7986-thermal",
		.data = (void *)&mt7986_thermal_data,
	},
	{
		.compatible = "mediatek,mt8183-thermal",
		.data = (void *)&mt8183_thermal_data,
	},
	{
		.compatible = "mediatek,mt8365-thermal",
		.data = (void *)&mt8365_thermal_data,
	}, {
		.compatible = "mediatek,mt6797-thermal",
		.data = (void *)&mt6797_thermal_data,
	}, {
		.compatible = "mediatek,mt6797-therm_ctrl",
		.data = (void *)&mt6797_thermal_data,
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_thermal_of_match);

static void mtk_thermal_turn_on_buffer(struct mtk_thermal *mt,
				       void __iomem *apmixed_base)
{
	u32 tmp;

	if (!mt->conf->apmixed_buffer_ctl_reg)
		return;

	tmp = readl(apmixed_base + mt->conf->apmixed_buffer_ctl_reg);
	tmp &= mt->conf->apmixed_buffer_ctl_mask;
	tmp |= mt->conf->apmixed_buffer_ctl_set;
	writel(tmp, apmixed_base + mt->conf->apmixed_buffer_ctl_reg);
	udelay(200);
}

static void mtk_thermal_release_periodic_ts(struct mtk_thermal *mt,
					    void __iomem *auxadc_base)
{
	int tmp;

	writel(0x800, auxadc_base + AUXADC_CON1_SET_V);
	writel(0x1, mt->thermal_base + TEMP_MONCTL0);
	tmp = readl(mt->thermal_base + TEMP_MSRCTL1);
	writel((tmp & (~0x10e)), mt->thermal_base + TEMP_MSRCTL1);
}

static int mtk_thermal_probe(struct platform_device *pdev)
{
	const struct mtk_thermal_transaction_ops *ops;
	struct device_node *np = pdev->dev.of_node;
	struct thermal_zone_device *tzdev;
	struct mtk_thermal *mt;
	bool readers_group = false;
	int ctrl_id, i, ret;

	mt = devm_kzalloc(&pdev->dev, sizeof(*mt), GFP_KERNEL);
	if (!mt)
		return -ENOMEM;

	mt->conf = of_device_get_match_data(&pdev->dev);
	if (!mt->conf)
		return -EINVAL;
	ops = &mt6797_thermal_transaction_ops;

	ret = mtk_thermal_get_calibration_data(&pdev->dev, mt);
	if (ret)
		return ret;

	mt->thermal_base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(mt->thermal_base))
		return PTR_ERR(mt->thermal_base);

	mutex_init(&mt->lock);
	mt->dev = &pdev->dev;
	platform_set_drvdata(pdev, mt);
	for (i = 0; i < mt->conf->num_banks; i++) {
		mt->banks[i].id = i;
		mt->banks[i].mt = mt;
	}

	ret = mtk_thermal_map_phandle(&pdev->dev, np, "mediatek,auxadc",
				      &mt->auxadc_base,
				      &mt->auxadc_phys_base);
	if (ret) {
		dev_err(&pdev->dev, "cannot map AUXADC: %d\n", ret);
		return ret;
	}

	ret = mtk_thermal_map_phandle(&pdev->dev, np, "mediatek,apmixedsys",
				      &mt->apmixed_base,
				      &mt->apmixed_phys_base);
	if (ret) {
		dev_err(&pdev->dev, "cannot map APMIXEDSYS: %d\n", ret);
		return ret;
	}

	if (mt->conf->version == MTK_THERMAL_V4) {
		mt->rst = devm_reset_control_get_exclusive(&pdev->dev, NULL);
		if (IS_ERR(mt->rst))
			return dev_err_probe(&pdev->dev, PTR_ERR(mt->rst),
					     "cannot acquire thermal reset\n");

		mt->clk_auxadc = devm_clk_get(&pdev->dev, "auxadc");
		if (IS_ERR(mt->clk_auxadc))
			return dev_err_probe(&pdev->dev,
					     PTR_ERR(mt->clk_auxadc),
					     "cannot acquire AUXADC clock\n");

		mt->clk_peri_therm = devm_clk_get(&pdev->dev, "therm");
		if (IS_ERR(mt->clk_peri_therm))
			return dev_err_probe(&pdev->dev,
					     PTR_ERR(mt->clk_peri_therm),
					     "cannot acquire thermal clock\n");

		mt->raw_to_mcelsius = raw_to_mcelsius_v4;
		ret = mtk_thermal_transaction_execute(mt, ops, &mt->transaction,
						      mt->conf->num_banks);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "MT6797 transaction failed\n");
	} else {
		ret = device_reset_optional(&pdev->dev);
		if (ret)
			return ret;

		mt->clk_auxadc = devm_clk_get_enabled(&pdev->dev, "auxadc");
		if (IS_ERR(mt->clk_auxadc))
			return dev_err_probe(&pdev->dev,
					     PTR_ERR(mt->clk_auxadc),
					     "cannot enable AUXADC clock\n");

		mt->clk_peri_therm = devm_clk_get_enabled(&pdev->dev, "therm");
		if (IS_ERR(mt->clk_peri_therm))
			return dev_err_probe(&pdev->dev,
					     PTR_ERR(mt->clk_peri_therm),
					     "cannot enable thermal clock\n");

		mtk_thermal_turn_on_buffer(mt, mt->apmixed_base);
		if (mt->conf->version != MTK_THERMAL_V1)
			mtk_thermal_release_periodic_ts(mt, mt->auxadc_base);

		if (mt->conf->version == MTK_THERMAL_V1)
			mt->raw_to_mcelsius = raw_to_mcelsius_v1;
		else if (mt->conf->version == MTK_THERMAL_V2)
			mt->raw_to_mcelsius = raw_to_mcelsius_v2;
		else
			mt->raw_to_mcelsius = raw_to_mcelsius_v3;

		for (ctrl_id = 0; ctrl_id < mt->conf->num_controller;
		     ctrl_id++)
			for (i = 0; i < mt->conf->num_banks; i++)
				mtk_thermal_init_bank(mt, i,
						      mt->apmixed_phys_base,
						      mt->auxadc_phys_base,
						      ctrl_id);
	}

	/* ixoo 0539: drain all public readers before closing the transaction. */
	if (mt->conf->version == MTK_THERMAL_V4) {
		if (!devres_open_group(&pdev->dev, mt, GFP_KERNEL)) {
			ret = -ENOMEM;
			goto fail_zone;
		}
		readers_group = true;
	}

	tzdev = devm_thermal_of_zone_register(&pdev->dev, 0, mt,
					      &mtk_thermal_ops);
	if (IS_ERR(tzdev)) {
		ret = PTR_ERR(tzdev);
		goto fail_zone;
	}

	ret = devm_thermal_add_hwmon_sysfs(&pdev->dev, tzdev);
	if (ret)
		dev_warn(&pdev->dev, "error in thermal_add_hwmon_sysfs\n");

	ret = mt6797_hwmon_register(mt);
	if (ret)
		dev_warn(&pdev->dev, "physical sensor hwmon registration failed: %d\n", ret);

	if (readers_group)
		devres_close_group(&pdev->dev, mt);
	return 0;

fail_zone:
	if (readers_group)
		devres_release_group(&pdev->dev, mt);
	if (mt->conf->version == MTK_THERMAL_V4)
		mtk_thermal_transaction_close(mt, ops, &mt->transaction);
	return ret;
}

static void mtk_thermal_remove(struct platform_device *pdev)
{
	struct mtk_thermal *mt = platform_get_drvdata(pdev);

	if (mt && mt->conf->version == MTK_THERMAL_V4) {
		devres_release_group(&pdev->dev, mt);
		mtk_thermal_transaction_close(mt, &mt6797_thermal_transaction_ops,
					      &mt->transaction);
	}
}

static struct platform_driver mtk_thermal_driver = {
	.probe = mtk_thermal_probe,
	.remove = mtk_thermal_remove,
	.driver = {
		.name = "mtk-thermal",
		.of_match_table = mtk_thermal_of_match,
	},
};

module_platform_driver(mtk_thermal_driver);

MODULE_AUTHOR("Michael Kao <michael.kao@mediatek.com>");
MODULE_AUTHOR("Louis Yu <louis.yu@mediatek.com>");
MODULE_AUTHOR("Dawei Chien <dawei.chien@mediatek.com>");
MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_AUTHOR("Hanyi Wu <hanyi.wu@mediatek.com>");
MODULE_DESCRIPTION("Mediatek thermal driver");
MODULE_LICENSE("GPL v2");
