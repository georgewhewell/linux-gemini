// SPDX-License-Identifier: GPL-2.0-only
/* Side-effect-free validation for the MediaTek MT6797 SENINF receiver. */

#include <linux/bits.h>
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

	if (!res || res->core.size < MT6797_SENINF_CORE_MIN_SIZE ||
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

/* Gemini camera1/sub is wired to MIPI port 1, i.e. SENINF instance 1. */
#define SENINF_TOP_MUX_CTRL 0x0008
#define SENINF_MUX1_CTRL 0x0d00
#define SENINF_MUX1_CTRL_EXT 0x0d3c
#define SENINF_MUX1_SPARE 0x0d2c
#define SENINF1_CTRL 0x1200
#define SENINF1_CTRL_EXT 0x1204
#define SENINF1_NCSI2_CTL 0x1700
#define SENINF1_NCSI2_LNRD_TIMING 0x1708
#define SENINF1_NCSI2_DPCM 0x170c
#define SENINF1_NCSI2_INT_EN 0x1710
#define SENINF1_NCSI2_CAL_CTRL 0x1738
#define SENINF1_NCSI2_DBG_CTRL 0x1838
#define SENINF1_NCSI2_DBG_SEL 0x183c
#define SENINF1_NCSI2_DBG_STA0 0x1844
#define SENINF1_NCSI2_DBG_STA1 0x1848
#define SENINF1_CSI2_CTL 0x1a00

#define MIPI_RX_ANA00_CSI1A 0x0400
#define MIPI_RX_ANA04_CSI1A 0x0404
#define MIPI_RX_ANA18_CSI1A 0x0418
#define MIPI_RX_ANA1C_CSI1A 0x041c
#define MIPI_RX_ANA20_CSI1A 0x0420
#define MIPI_RX_ANA80_CSI1A 0x0480
#define MIPI_RX_ANA00_CSI1B 0x0600
#define MIPI_RX_ANA04_CSI1B 0x0604
#define MIPI_RX_ANA18_CSI1B 0x0618
#define MIPI_RX_ANA1C_CSI1B 0x061c
#define MIPI_RX_ANA20_CSI1B 0x0620
#define MIPI_RX_ANA08_CSI1A 0x0408
#define MIPI_RX_ANA0C_CSI1A 0x040c
#define MIPI_RX_ANA08_CSI1B 0x0608
#define MIPI_RX_ANA0C_CSI1B 0x060c

static u32 mt6797_seninf_read(const struct mt6797_seninf_io *io,
			      enum mt6797_seninf_bank bank, u32 offset)
{
	return io->read(io->context, bank, offset);
}

static void mt6797_seninf_write(const struct mt6797_seninf_io *io,
				enum mt6797_seninf_bank bank, u32 offset,
				u32 value)
{
	io->write(io->context, bank, offset, value);
}

static void mt6797_seninf_update(const struct mt6797_seninf_io *io,
				 enum mt6797_seninf_bank bank, u32 offset,
				 u32 mask, u32 value)
{
	u32 reg = mt6797_seninf_read(io, bank, offset);

	mt6797_seninf_write(io, bank, offset, (reg & ~mask) | (value & mask));
}

static void mt6797_seninf_ana_lane_config(const struct mt6797_seninf_io *io,
					  u32 base, bool lane_a)
{
	u32 ana00 = BIT(6) | BIT(9) | BIT(12);

	if (lane_a)
		ana00 |= BIT(11);

	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA, base,
			     GENMASK(12, 4) | BIT(0), ana00);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA, base + 0x4,
			     GENMASK(14, 8), 0x1600);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA, base + 0x18,
			     GENMASK(7, 6) | GENMASK(23, 22), BIT(6) | BIT(22));
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA, base + 0x1c,
			     GENMASK(7, 6) | GENMASK(23, 22), BIT(6) | BIT(22));
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA, base + 0x20,
			     GENMASK(7, 6) | GENMASK(23, 22), BIT(6) | BIT(22));
}

static void
mt6797_seninf_apply_port1_calibration(const struct mt6797_seninf_io *io,
				      const u32 calibration[MT6797_SENINF_CAL_WORDS])
{
	u32 word27 = calibration[2];
	u32 word28 = calibration[3];

	/* The stock HAL deliberately leaves reset defaults when word 0x27 is 0. */
	if (!word27)
		return;

	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA08_CSI1A, GENMASK(4, 0),
			     (word27 >> 19) & GENMASK(4, 0));
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA08_CSI1A, GENMASK(12, 8),
			     ((word27 >> 24) & GENMASK(4, 0)) << 8);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA08_CSI1A, GENMASK(20, 16),
			     ((word27 >> 9) & GENMASK(4, 0)) << 16);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA08_CSI1A, GENMASK(28, 24),
			     ((word27 >> 14) & GENMASK(4, 0)) << 24);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA0C_CSI1A, GENMASK(4, 0),
			     ((word28 >> 31) | ((word27 & 0xf) << 1)) &
				     GENMASK(4, 0));
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA0C_CSI1A, GENMASK(12, 8),
			     ((word27 >> 4) & GENMASK(4, 0)) << 8);

	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA08_CSI1B, GENMASK(4, 0),
			     (word28 >> 21) & GENMASK(4, 0));
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA08_CSI1B, GENMASK(12, 8),
			     ((word28 >> 26) & GENMASK(4, 0)) << 8);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA08_CSI1B, GENMASK(20, 16),
			     ((word28 >> 11) & GENMASK(4, 0)) << 16);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA08_CSI1B, GENMASK(28, 24),
			     ((word28 >> 16) & GENMASK(4, 0)) << 24);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA0C_CSI1B, GENMASK(4, 0),
			     (word28 >> 1) & GENMASK(4, 0));
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA0C_CSI1B, GENMASK(12, 8),
			     ((word28 >> 6) & GENMASK(4, 0)) << 8);
}

int mt6797_seninf_route_validate(const unsigned int *data_lanes,
				 unsigned int num_data_lanes,
				 const struct v4l2_mbus_framefmt *format,
				 s64 link_freq)
{
	if (!data_lanes || !format || num_data_lanes != 2 ||
	    data_lanes[0] != 1 || data_lanes[1] != 2)
		return -EINVAL;

	if (format->code != MEDIA_BUS_FMT_SGRBG10_1X10 ||
	    format->width != MT6797_SENINF_DEFAULT_WIDTH ||
	    format->height != MT6797_SENINF_DEFAULT_HEIGHT ||
	    link_freq != MT6797_SENINF_FULL_LINK_FREQ)
		return -EOPNOTSUPP;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_seninf_route_validate);

void mt6797_seninf_hw_stop(const struct mt6797_seninf_io *io)
{
	if (!io || !io->read || !io->write)
		return;

	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF1_NCSI2_CTL,
			     GENMASK(4, 0), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF1_CSI2_CTL,
			     GENMASK(4, 0), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE,
			     SENINF1_NCSI2_DBG_CTRL, BIT(0) | BIT(2), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE,
			     SENINF1_NCSI2_CAL_CTRL, GENMASK(4, 0), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA00_CSI1A, BIT(3), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA00_CSI1B, BIT(3), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA00_CSI1A, BIT(2), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA00_CSI1B, BIT(2), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF_TOP_MUX_CTRL,
			     GENMASK(3, 0), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF_MUX1_CTRL,
			     BIT(31), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF1_CTRL, BIT(0),
			     0);
}
EXPORT_SYMBOL_GPL(mt6797_seninf_hw_stop);

int mt6797_seninf_hw_start(const struct mt6797_seninf_io *io,
			   const struct mt6797_seninf_config *config)
{
	u32 settle_count;
	u32 value;

	if (!io || !io->read || !io->write || !io->delay_us || !config ||
	    config->num_data_lanes != 2 ||
	    config->settle_ns != MT6797_SENINF_FULL_SETTLE_NS)
		return -EINVAL;

	mt6797_seninf_hw_stop(io);

	/* PAD2CAM=0, source=NCSI2(8), and the port-1 external-sync mode. */
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF1_CTRL,
			     BIT(0) | GENMASK(30, 28) | GENMASK(15, 12),
			     BIT(0) | (8 << 12));
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF1_CTRL_EXT,
			     BIT(6) | BIT(5), BIT(5));

	/* Mux 1: NCSI2 source 8, RAW10 format 1, one-pixel mode. */
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF_MUX1_CTRL,
			     BIT(31) | GENMASK(15, 12) | BIT(10) | BIT(9) |
				     BIT(8) | GENMASK(29, 28) |
				     GENMASK(27, 22) | GENMASK(21, 16),
			     BIT(31) | (8 << 12) | 0x06000000 | 0x001e0000);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF_MUX1_CTRL_EXT,
			     GENMASK(1, 0), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF_MUX1_SPARE,
			     BIT(13), 0);

	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA80_CSI1A, GENMASK(7, 4), 0);
	mt6797_seninf_ana_lane_config(io, MIPI_RX_ANA00_CSI1A, true);
	mt6797_seninf_ana_lane_config(io, MIPI_RX_ANA00_CSI1B, false);
	mt6797_seninf_apply_port1_calibration(io, config->calibration);

	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA00_CSI1A, BIT(3), BIT(3));
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA00_CSI1B, BIT(3), BIT(3));
	io->delay_us(io->context, 30, 30);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA00_CSI1A, BIT(2), BIT(2));
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CSI0_ANA,
			     MIPI_RX_ANA00_CSI1B, BIT(2), BIT(2));
	io->delay_us(io->context, 1, 1);

	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE,
			     SENINF1_NCSI2_CAL_CTRL, GENMASK(4, 0),
			     GENMASK(4, 0));
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE,
			     SENINF1_NCSI2_DBG_CTRL, BIT(0), BIT(0));
	mt6797_seninf_write(io, MT6797_SENINF_BANK_CORE, SENINF1_NCSI2_DBG_SEL,
			    0x1541);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE,
			     SENINF1_NCSI2_DBG_CTRL, BIT(2), BIT(2));
	io->delay_us(io->context, 500, 500);

	value = mt6797_seninf_read(io, MT6797_SENINF_BANK_CORE,
				   SENINF1_NCSI2_DBG_STA0);
	if (!(value & 0x10001))
		goto calibration_failed;
	value = mt6797_seninf_read(io, MT6797_SENINF_BANK_CORE,
				   SENINF1_NCSI2_DBG_STA1);
	if (!(value & 0x101))
		goto calibration_failed;

	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE,
			     SENINF1_NCSI2_DBG_CTRL, BIT(0), 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE,
			     SENINF1_NCSI2_CAL_CTRL, GENMASK(4, 0), 0);

	settle_count = config->settle_ns * 286 / 1000;
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF1_NCSI2_CTL,
			     BIT(7), BIT(7));
	mt6797_seninf_write(io, MT6797_SENINF_BANK_CORE,
			    SENINF1_NCSI2_LNRD_TIMING,
			    (settle_count & 0xff) << 8);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF1_NCSI2_CTL,
			     GENMASK(4, 0) | GENMASK(19, 16) | BIT(26) | BIT(4),
			     GENMASK(1, 0) | BIT(16) | BIT(26) | BIT(4));
	mt6797_seninf_write(io, MT6797_SENINF_BANK_CORE, SENINF1_NCSI2_INT_EN,
			    0x80007fff);
	mt6797_seninf_write(io, MT6797_SENINF_BANK_CORE, SENINF1_NCSI2_DPCM, 0);
	mt6797_seninf_update(io, MT6797_SENINF_BANK_CORE, SENINF_TOP_MUX_CTRL,
			     GENMASK(3, 0), 1);

	return 0;

calibration_failed:
	mt6797_seninf_hw_stop(io);
	return -EIO;
}
EXPORT_SYMBOL_GPL(mt6797_seninf_hw_start);
