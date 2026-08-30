/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 BTIF/APDMA virtual-FIFO helpers.
 *
 * These helpers model the part of the BTIF transport contract which is
 * independent of power, pad muxing, firmware and STP mode selection. The
 * caller serializes access to one FIFO. In particular, the TX helper must use
 * the WPT and free-space values sampled from the hardware immediately before
 * it publishes the returned WPT value.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_BTIF_H
#define __LINUX_SOC_MEDIATEK_MT6797_BTIF_H

#include <linux/dma-mapping.h>
#include <linux/types.h>

#define MT6797_BTIF_VFIFO_SIZE		8192
#define MT6797_BTIF_VFIFO_ALIGNMENT	8
#define MT6797_BTIF_VFIFO_PTR_MASK	0xffff
#define MT6797_BTIF_VFIFO_WRAP		BIT(16)

struct mt6797_btif_vfifo {
	u8 *cpu_addr;
	dma_addr_t dma_addr;
	u32 wpt;
	u32 rpt;
};

struct mt6797_btif_rx_segments {
	const u8 *first;
	size_t first_len;
	const u8 *second;
	size_t second_len;
	u32 next_rpt;
};

/* Advance one hardware virtual-FIFO pointer by at most one complete ring. */
int mt6797_btif_vfifo_advance(u32 pointer, size_t count, u32 *next);

/*
 * Copy a TX packet into the ring and return the WPT to publish. A packet which
 * does not fit is rejected with -EAGAIN; it is never partially copied.
 */
int mt6797_btif_vfifo_tx_copy(struct mt6797_btif_vfifo *fifo, u32 hw_wpt,
			      u32 hw_left, const u8 *data, size_t length,
			      u32 *next_wpt);

/*
 * Describe the up to two linear portions newly produced by RX DMA. The caller
 * invokes its receive function for both portions, then writes next_rpt to the
 * hardware only after consuming them.
 */
int mt6797_btif_vfifo_rx_segments(const struct mt6797_btif_vfifo *fifo,
				  u32 hw_wpt,
				  struct mt6797_btif_rx_segments *segments);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_BTIF_H */
