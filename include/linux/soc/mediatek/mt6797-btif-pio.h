/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Dormant MT6797 BTIF PIO transport owner.
 *
 * This owns the byte-level PIO contract only.  It does not map a resource,
 * obtain a clock, request an IRQ, change CONSYS power/reset state, request
 * firmware, or send WMT/radio traffic by itself.  A future platform owner
 * supplies individual register callbacks only after it has independently
 * established those hardware ownership prerequisites.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_BTIF_PIO_H
#define __LINUX_SOC_MEDIATEK_MT6797_BTIF_PIO_H

#include <linux/types.h>

#define MT6797_BTIF_PIO_TX_BUFFER_SIZE	4096
#define MT6797_BTIF_PIO_RX_BUDGET		256

struct mt6797_consys_stp_session_lease;
struct mt6797_btif_pio;
struct mt6797_btif_pio_lease;

/*
 * These callbacks perform one already-authorized BTIF register access.  They
 * must not sleep and may be called from a future threaded IRQ.  The transport
 * deliberately has no MMIO mapping helper so it cannot discover or enable
 * Gemini hardware itself.
 */
struct mt6797_btif_pio_io_ops {
	u32 (*read32)(void *context, u32 offset);
	void (*write32)(void *context, u32 offset, u32 value);
	u8 (*read8)(void *context, u32 offset);
	void (*write8)(void *context, u32 offset, u8 value);
	void *context;
};

/* Allocate an inert owner. No register access occurs here. */
struct mt6797_btif_pio *
mt6797_btif_pio_create(const struct mt6797_btif_pio_io_ops *ops);

/*
 * Reserve this single-use owner for one full-STP session.  This still performs
 * no register access.  It returns the opaque context used by the full-STP
 * session's transmit callback.
 */
const struct mt6797_btif_pio_lease *
mt6797_btif_pio_acquire(struct mt6797_btif_pio *pio);

/*
 * Bind exactly one active full-STP lease and then perform the vendor PIO core
 * setup. The future platform owner must have installed-but-disabled IRQ
 * handling before this call; this library neither owns nor enables Linux IRQs.
 */
int mt6797_btif_pio_attach_full_stp(
		const struct mt6797_btif_pio_lease *pio_lease,
		const struct mt6797_consys_stp_session_lease *stp_lease);

/*
 * Callback suitable for mt6797_consys_stp_session_ops.transmit. context must
 * be the PIO lease. It accepts the entire frame or returns -EAGAIN without
 * accepting any bytes; it never silently performs a short frame write.
 */
int mt6797_btif_pio_transmit(void *context, const u8 *frame,
			     size_t frame_length);

/*
 * Handle one already-delivered BTIF core interrupt in threaded-IRQ context.
 * RX bytes are passed as arbitrary fragments to the attached full-STP lease.
 */
int mt6797_btif_pio_irq(const struct mt6797_btif_pio_lease *lease);

/*
 * A future owner calls this only after its separately evidenced TX deadline.
 * Vendor source establishes no PIO timeout duration, so this library invents
 * none. A pending queue becomes permanently fenced rather than retried.
 */
int mt6797_btif_pio_tx_timeout(const struct mt6797_btif_pio_lease *lease);

/*
 * Fence TX/RX and invalidate the lease. The future IRQ owner must first mask
 * and synchronize its Linux IRQ, and retain the attached full-STP session,
 * before calling close() or destroy().
 */
void mt6797_btif_pio_close(struct mt6797_btif_pio *pio);
void mt6797_btif_pio_destroy(struct mt6797_btif_pio *pio);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_BTIF_PIO_H */
