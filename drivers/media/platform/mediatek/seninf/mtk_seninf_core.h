/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_MT6797_SENINF_CORE_H__
#define __MTK_MT6797_SENINF_CORE_H__

#include <linux/sizes.h>
#include <linux/types.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#define MT6797_SENINF_CORE_MIN_SIZE SZ_32K
#define MT6797_SENINF_ANA_MIN_SIZE SZ_4K

#define MT6797_SENINF_PAD_SINK 0
#define MT6797_SENINF_PAD_SOURCE 1
#define MT6797_SENINF_NUM_PADS 2

#define MT6797_SENINF_DEFAULT_WIDTH 2592
#define MT6797_SENINF_DEFAULT_HEIGHT 1944
#define MT6797_SENINF_CAL_WORDS 5
#define MT6797_SENINF_FULL_LINK_FREQ 440000000ULL
#define MT6797_SENINF_FULL_SETTLE_NS 14

enum mt6797_seninf_bank {
	MT6797_SENINF_BANK_CORE,
	MT6797_SENINF_BANK_CSI0_ANA,
};

struct mt6797_seninf_io {
	u32 (*read)(void *context, enum mt6797_seninf_bank bank, u32 offset);
	void (*write)(void *context, enum mt6797_seninf_bank bank, u32 offset,
		      u32 value);
	void (*delay_us)(void *context, unsigned int min, unsigned int max);
	void *context;
};

struct mt6797_seninf_config {
	u32 calibration[MT6797_SENINF_CAL_WORDS];
	unsigned int num_data_lanes;
	unsigned int settle_ns;
};

struct mt6797_seninf_range {
	u64 start;
	u64 size;
};

struct mt6797_seninf_res {
	struct mt6797_seninf_range core;
	struct mt6797_seninf_range csi0_ana;
	struct mt6797_seninf_range csi1_ana;
};

int mt6797_seninf_resources_validate(const struct mt6797_seninf_res *res);
int mt6797_seninf_endpoint_validate(enum v4l2_mbus_type bus_type,
				    unsigned int num_data_lanes);
void mt6797_seninf_format_adjust(struct v4l2_mbus_framefmt *format);
int mt6797_seninf_route_validate(const unsigned int *data_lanes,
				 unsigned int num_data_lanes,
				 const struct v4l2_mbus_framefmt *format,
				 s64 link_freq);
int mt6797_seninf_hw_start(const struct mt6797_seninf_io *io,
			   const struct mt6797_seninf_config *config);
void mt6797_seninf_hw_stop(const struct mt6797_seninf_io *io);

#endif /* __MTK_MT6797_SENINF_CORE_H__ */
