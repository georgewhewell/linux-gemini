// SPDX-License-Identifier: GPL-2.0-only
/* Side-effect-free validation for the MediaTek MT6797 SENINF receiver. */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/overflow.h>
#include <linux/sizes.h>

#include "mtk_seninf_core.h"

static bool mt6797_seninf_range_end(const struct mt6797_seninf_range *range,
				    u64 *end)
{
	if (!range->size)
		return false;

	return !check_add_overflow(range->start, range->size - 1, end);
}

static bool
mt6797_seninf_ranges_overlap(const struct mt6797_seninf_range *left,
			     const struct mt6797_seninf_range *right)
{
	u64 left_end;
	u64 right_end;

	if (!mt6797_seninf_range_end(left, &left_end) ||
	    !mt6797_seninf_range_end(right, &right_end))
		return true;

	return left->start <= right_end && right->start <= left_end;
}

int mt6797_seninf_resources_validate(const struct mt6797_seninf_res *res)
{
	u64 end;

	if (!res ||
	    res->core.size < MT6797_SENINF_CORE_MIN_SIZE ||
	    res->csi0_ana.size < MT6797_SENINF_ANA_MIN_SIZE ||
	    res->csi1_ana.size < MT6797_SENINF_ANA_MIN_SIZE ||
	    !mt6797_seninf_range_end(&res->core, &end) ||
	    !mt6797_seninf_range_end(&res->csi0_ana, &end) ||
	    !mt6797_seninf_range_end(&res->csi1_ana, &end))
		return -EINVAL;

	if (mt6797_seninf_ranges_overlap(&res->core, &res->csi0_ana) ||
	    mt6797_seninf_ranges_overlap(&res->core, &res->csi1_ana) ||
	    mt6797_seninf_ranges_overlap(&res->csi0_ana, &res->csi1_ana))
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_seninf_resources_validate);

int mt6797_seninf_endpoint_validate(enum v4l2_mbus_type bus_type,
				    unsigned int num_data_lanes)
{
	if (bus_type != V4L2_MBUS_CSI2_DPHY)
		return -EINVAL;

	if (!num_data_lanes || num_data_lanes > 4)
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_seninf_endpoint_validate);

void mt6797_seninf_format_adjust(struct v4l2_mbus_framefmt *format)
{
	if (!format)
		return;

	format->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	if (!format->width)
		format->width = MT6797_SENINF_DEFAULT_WIDTH;
	if (!format->height)
		format->height = MT6797_SENINF_DEFAULT_HEIGHT;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->xfer_func = V4L2_XFER_FUNC_NONE;
	format->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
}
EXPORT_SYMBOL_GPL(mt6797_seninf_format_adjust);
