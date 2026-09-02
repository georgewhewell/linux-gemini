// SPDX-License-Identifier: GPL-2.0-only
/* Evidence-bound register plan for the MT6797 CAMSV0 RAW10 path. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/videodev2.h>

#include "mtk_camsv_core.h"

int mt6797_camsv_format_validate(const struct mt6797_camsv_format *format)
{
	if (!format || format->width != MT6797_CAMSV_WIDTH ||
	    format->height != MT6797_CAMSV_HEIGHT ||
	    format->bytesperline != MT6797_CAMSV_BYTESPERLINE ||
	    format->sizeimage != MT6797_CAMSV_SIZEIMAGE ||
	    format->pixelformat != V4L2_PIX_FMT_SGRBG10P)
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_camsv_format_validate);

unsigned int mt6797_camsv_build_config_plan(struct mt6797_camsv_reg *plan,
					    unsigned int capacity)
{
	static const struct mt6797_camsv_reg config[] = {
		/* RAW10 packer, TG, IMGO DMA, and non-subsampled FBC. */
		{ MT6797_CAMSV_REG_MODULE_EN, BIT(30) | BIT(4) | BIT(2) | BIT(0) },
		{ MT6797_CAMSV_REG_FMT_SEL, 1 },
		{ MT6797_CAMSV_REG_INT_EN, MT6797_CAMSV_INT_MASK },
		{ MT6797_CAMSV_REG_PAK, 1 },
		{ MT6797_CAMSV_REG_DMA_FH_EN, 0 },
		{ MT6797_CAMSV_REG_DMA_RSV1, 0 },
		{ MT6797_CAMSV_REG_DMA_RSV6, U32_MAX },
		{ MT6797_CAMSV_REG_SPECIAL_FUN_EN, 0x61000000 },
		{ MT6797_CAMSV_REG_IMGO_OFST_ADDR, 0 },
		{ MT6797_CAMSV_REG_IMGO_XSIZE, MT6797_CAMSV_BYTESPERLINE - 1 },
		{ MT6797_CAMSV_REG_IMGO_YSIZE, MT6797_CAMSV_HEIGHT - 1 },
		{ MT6797_CAMSV_REG_IMGO_STRIDE,
		  MT6797_CAMSV_BYTESPERLINE | 0x01010000 },
		{ MT6797_CAMSV_REG_IMGO_CROP, 0 },
		{ MT6797_CAMSV_REG_FBC_CTL1, BIT(22) | BIT(16) | BIT(15) },
		{ MT6797_CAMSV_REG_TG_SUB_PERIOD, 0 },
		{ MT6797_CAMSV_REG_TG_SEN_MODE, BIT(16) | BIT(0) },
		{ MT6797_CAMSV_REG_TG_VF_CON, 0 },
		{ MT6797_CAMSV_REG_TG_GRAB_PXL, MT6797_CAMSV_WIDTH << 16 },
		{ MT6797_CAMSV_REG_TG_GRAB_LIN, MT6797_CAMSV_HEIGHT << 16 },
		{ MT6797_CAMSV_REG_CLK_EN, BIT(15) | BIT(2) | BIT(0) },
	};

	if (capacity < ARRAY_SIZE(config))
		return ARRAY_SIZE(config);
	memcpy(plan, config, sizeof(config));

	return ARRAY_SIZE(config);
}
EXPORT_SYMBOL_GPL(mt6797_camsv_build_config_plan);
