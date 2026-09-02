/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_MT6797_SENINF_CORE_H__
#define __MTK_MT6797_SENINF_CORE_H__

#include <linux/sizes.h>
#include <linux/types.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#define MT6797_SENINF_CORE_MIN_SIZE	SZ_32K
#define MT6797_SENINF_ANA_MIN_SIZE	SZ_4K

#define MT6797_SENINF_PAD_SINK		0
#define MT6797_SENINF_PAD_SOURCE		1
#define MT6797_SENINF_NUM_PADS		2

#define MT6797_SENINF_DEFAULT_WIDTH	2592
#define MT6797_SENINF_DEFAULT_HEIGHT	1944

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

#endif /* __MTK_MT6797_SENINF_CORE_H__ */
