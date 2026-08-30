// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dormant MT6797 BTIF PIO byte-transport owner.
 *
 * The retained vendor PIO implementation is the evidence boundary here:
 *
 * - BTIF_RBR/THR, IER, IIR, FIFOCTRL, FAKELCR, LSR, DMA_EN, TRI_LVL and
 *   HANDSHAKE offsets and bit definitions are in btif_priv.h:32-101;
 * - hal_btif_hw_init() selects normal/new-handshake mode, clears RX then TX
 *   FIFO, selects PIO, sets automatic RX-timeout reset, disables TX IER and
 *   enables RX IER (btif_plat.c:476-521);
 * - the enabled vendor TX path owns a 4096-byte software FIFO and starts TX
 *   only by enabling TX IER (btif_plat.c:129-176,1014-1036); TX-empty drains
 *   16 bytes for TEMT and 8 bytes for THRE (btif_plat.c:874-917);
 * - PIO IRQ processing drains RBR while RX/RX-timeout is pending, then handles
 *   TX-empty (btif_plat.c:737-793,816-924).
 *
 * There is deliberately no platform driver or Device Tree match here.  This
 * object cannot map MMIO, turn on clocks, request/enable an IRQ, wake CONSYS,
 * allocate DMA, load firmware, or initiate a WMT/radio transaction.
 * Its first hardware-facing attachment also pins the caller's already-live
 * runtime lease; close/fence releases that pin before the long-lived owner
 * may unwind its identity transaction.
 */

#include <linux/errno.h>
#include <linux/kfifo.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_PIO_KUNIT_TEST)
#include <kunit/test.h>
#endif

#include <linux/soc/mediatek/mt6797-btif-pio.h>
#include <linux/soc/mediatek/mt6797-consys.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>
#include <linux/soc/mediatek/mtk-stp.h>

#define MT6797_BTIF_RBR			0x00
#define MT6797_BTIF_THR			0x00
#define MT6797_BTIF_IER			0x04
#define MT6797_BTIF_IIR			0x08
#define MT6797_BTIF_FIFOCTRL		0x08
#define MT6797_BTIF_FAKELCR		0x0c
#define MT6797_BTIF_LSR			0x14
#define MT6797_BTIF_DMA_EN		0x4c
#define MT6797_BTIF_TRI_LVL		0x60
#define MT6797_BTIF_HANDSHAKE		0x6c

#define MT6797_BTIF_IER_RX		BIT(0)
#define MT6797_BTIF_IER_TX		BIT(1)
#define MT6797_BTIF_IIR_NINT		BIT(0)
#define MT6797_BTIF_IIR_TX_EMPTY	BIT(1)
#define MT6797_BTIF_IIR_RX		BIT(2)
#define MT6797_BTIF_IIR_RX_TIMEOUT	(0x11 << 2)
#define MT6797_BTIF_LSR_THRE		BIT(5)
#define MT6797_BTIF_LSR_TEMT		BIT(6)
#define MT6797_BTIF_FIFOCLR_RX		BIT(1)
#define MT6797_BTIF_FIFOCLR_TX		BIT(2)
#define MT6797_BTIF_DMA_RX		BIT(0)
#define MT6797_BTIF_DMA_TX		BIT(1)
#define MT6797_BTIF_DMA_AUTORST	BIT(2)
#define MT6797_BTIF_TRI_TX(value)	(((value) & 0xf) << 0)
#define MT6797_BTIF_TRI_RX(value)	(((value) & 0x7) << 4)
#define MT6797_BTIF_HANDSHAKE_EN	BIT(0)
#define MT6797_BTIF_TRI_LOOP_EN		BIT(7)
#define MT6797_BTIF_TX_FIFO_SIZE	16
#define MT6797_BTIF_TX_THRESHOLD	8

enum mt6797_btif_pio_state {
	MT6797_BTIF_PIO_NEW,
	MT6797_BTIF_PIO_LEASED,
	MT6797_BTIF_PIO_QUALIFYING,
	MT6797_BTIF_PIO_LOOPBACK_QUALIFYING,
	MT6797_BTIF_PIO_MANDATORY,
	MT6797_BTIF_PIO_FULL_BOOTSTRAP,
	MT6797_BTIF_PIO_ACTIVE,
	MT6797_BTIF_PIO_FENCED,
};

struct mt6797_btif_pio {
	struct mutex state_lock;
	spinlock_t tx_lock;
	struct mt6797_btif_pio_io_ops io;
	struct kfifo tx_fifo;
	struct mt6797_btif_pio_lease *lease;
	const struct mt6797_consys_runtime_lease *runtime_lease;
	bool runtime_pinned;
	const struct mt6797_consys_stp_session_lease *stp_lease;
	mt6797_btif_pio_rx_cb_t receive;
	void *receive_context;
	/* Protected by state_lock, except TX fields protected by tx_lock. */
	u64 tx_enqueue_count;
	u64 tx_enqueue_bytes;
	u64 tx_thr_writes;
	u64 irq_count;
	u64 iir_reads;
	u64 iir_rx;
	u64 iir_tx_empty;
	u64 iir_spurious;
	u64 rx_bytes;
	u64 rx_callbacks;
	enum mt6797_btif_pio_state state;
};

struct mt6797_btif_pio_lease {
	struct mt6797_btif_pio *pio;
};

static bool mt6797_btif_pio_lease_valid(
		const struct mt6797_btif_pio_lease *lease)
{
	return lease && lease->pio && lease == lease->pio->lease &&
		(READ_ONCE(lease->pio->state) == MT6797_BTIF_PIO_MANDATORY ||
		 READ_ONCE(lease->pio->state) == MT6797_BTIF_PIO_FULL_BOOTSTRAP ||
		 READ_ONCE(lease->pio->state) == MT6797_BTIF_PIO_ACTIVE);
}

static void mt6797_btif_pio_update_bits(struct mt6797_btif_pio *pio,
					u32 offset, u32 mask, u32 value)
{
	u32 reg;

	reg = pio->io.read32(pio->io.context, offset);
	reg &= ~mask;
	reg |= value & mask;
	pio->io.write32(pio->io.context, offset, reg);
}

static void mt6797_btif_pio_set_ier(struct mt6797_btif_pio *pio,
				     u32 mask, u32 value)
{
	mt6797_btif_pio_update_bits(pio, MT6797_BTIF_IER, mask, value);
}

static void mt6797_btif_pio_record_iir(struct mt6797_btif_pio *pio, u32 iir)
{
	pio->iir_reads++;
	if (iir & MT6797_BTIF_IIR_NINT)
		pio->iir_spurious++;
	if (iir & (MT6797_BTIF_IIR_RX | MT6797_BTIF_IIR_RX_TIMEOUT))
		pio->iir_rx++;
	if (iir & MT6797_BTIF_IIR_TX_EMPTY)
		pio->iir_tx_empty++;
}

static void mt6797_btif_pio_hw_init(struct mt6797_btif_pio *pio)
{
	/* Exact PIO subset and ordering of vendor hal_btif_hw_init(). */
	pio->io.write32(pio->io.context, MT6797_BTIF_FAKELCR, 0);
	mt6797_btif_pio_update_bits(pio, MT6797_BTIF_HANDSHAKE,
					MT6797_BTIF_HANDSHAKE_EN,
					MT6797_BTIF_HANDSHAKE_EN);
	/* FIFOCTRL is write-only in the vendor register definition. */
	pio->io.write32(pio->io.context, MT6797_BTIF_FIFOCTRL,
				MT6797_BTIF_FIFOCLR_RX);
	pio->io.write32(pio->io.context, MT6797_BTIF_FIFOCTRL, 0);
	pio->io.write32(pio->io.context, MT6797_BTIF_FIFOCTRL,
				MT6797_BTIF_FIFOCLR_TX);
	pio->io.write32(pio->io.context, MT6797_BTIF_FIFOCTRL, 0);
	pio->io.write32(pio->io.context, MT6797_BTIF_TRI_LVL,
				MT6797_BTIF_TRI_TX(MT6797_BTIF_TX_THRESHOLD) |
				MT6797_BTIF_TRI_RX(1));
	mt6797_btif_pio_update_bits(pio, MT6797_BTIF_DMA_EN,
					MT6797_BTIF_DMA_RX | MT6797_BTIF_DMA_TX, 0);
	mt6797_btif_pio_update_bits(pio, MT6797_BTIF_DMA_EN,
					MT6797_BTIF_DMA_AUTORST,
					MT6797_BTIF_DMA_AUTORST);
	mt6797_btif_pio_set_ier(pio, MT6797_BTIF_IER_RX | MT6797_BTIF_IER_TX,
				 MT6797_BTIF_IER_RX);
}

static void mt6797_btif_pio_tx_drain_locked(struct mt6797_btif_pio *pio)
{
	unsigned int limit;
	unsigned int count;
	u32 lsr;
	u8 byte;

	lsr = pio->io.read32(pio->io.context, MT6797_BTIF_LSR);
	if (lsr & MT6797_BTIF_LSR_TEMT)
		limit = MT6797_BTIF_TX_FIFO_SIZE;
	else if (lsr & MT6797_BTIF_LSR_THRE)
		limit = MT6797_BTIF_TX_FIFO_SIZE - MT6797_BTIF_TX_THRESHOLD;
	else
		limit = 0;

	count = min_t(unsigned int, kfifo_len(&pio->tx_fifo), limit);
	while (count--) {
		if (WARN_ON_ONCE(kfifo_out(&pio->tx_fifo, &byte, sizeof(byte)) !=
				 sizeof(byte)))
			break;
		pio->io.write8(pio->io.context, MT6797_BTIF_THR, byte);
		pio->tx_thr_writes++;
	}
	if (kfifo_is_empty(&pio->tx_fifo))
		mt6797_btif_pio_set_ier(pio, MT6797_BTIF_IER_TX, 0);
	else
		mt6797_btif_pio_set_ier(pio, MT6797_BTIF_IER_TX,
					 MT6797_BTIF_IER_TX);
}

static void mt6797_btif_pio_fence_locked(struct mt6797_btif_pio *pio)
{
	unsigned long flags;

	pio->state = MT6797_BTIF_PIO_FENCED;
	pio->stp_lease = NULL;
	pio->receive = NULL;
	pio->receive_context = NULL;
	spin_lock_irqsave(&pio->tx_lock, flags);
	/*
	 * Transmit enables TX IER while holding tx_lock.  Clear both interrupt
	 * enables under the same lock so an in-flight submit cannot re-enable TX
	 * after this permanent fence.
	 */
	mt6797_btif_pio_set_ier(pio, MT6797_BTIF_IER_RX | MT6797_BTIF_IER_TX,
				 0);
	kfifo_reset(&pio->tx_fifo);
	spin_unlock_irqrestore(&pio->tx_lock, flags);
	if (pio->runtime_pinned) {
		mt6797_consys_runtime_lease_unpin(pio->runtime_lease);
		pio->runtime_pinned = false;
		pio->runtime_lease = NULL;
	}
}

static int mt6797_btif_pio_pin_runtime_locked(struct mt6797_btif_pio *pio,
		const struct mt6797_consys_runtime_lease *runtime_lease)
{
	int ret;

	if (!runtime_lease)
		return -EINVAL;
	if (pio->runtime_pinned)
		return pio->runtime_lease == runtime_lease ? 0 : -EBUSY;
	ret = mt6797_consys_runtime_lease_pin(runtime_lease);
	if (ret)
		return ret;
	pio->runtime_lease = runtime_lease;
	pio->runtime_pinned = true;
	return 0;
}

struct mt6797_btif_pio *
mt6797_btif_pio_create(const struct mt6797_btif_pio_io_ops *ops)
{
	struct mt6797_btif_pio *pio;
	struct mt6797_btif_pio_lease *lease;
	int ret;

	if (!ops || !ops->read32 || !ops->write32 || !ops->read8 ||
	    !ops->write8)
		return ERR_PTR(-EINVAL);

	pio = kzalloc_obj(*pio, GFP_KERNEL);
	if (!pio)
		return ERR_PTR(-ENOMEM);
	lease = kzalloc_obj(*lease, GFP_KERNEL);
	if (!lease) {
		kfree(pio);
		return ERR_PTR(-ENOMEM);
	}
	ret = kfifo_alloc(&pio->tx_fifo, MT6797_BTIF_PIO_TX_BUFFER_SIZE,
			  GFP_KERNEL);
	if (ret) {
		kfree(lease);
		kfree(pio);
		return ERR_PTR(ret);
	}

	mutex_init(&pio->state_lock);
	spin_lock_init(&pio->tx_lock);
	pio->io = *ops;
	pio->lease = lease;
	lease->pio = pio;
	pio->state = MT6797_BTIF_PIO_NEW;
	return pio;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_create);

const struct mt6797_btif_pio_lease *
mt6797_btif_pio_acquire(struct mt6797_btif_pio *pio)
{
	const struct mt6797_btif_pio_lease *lease;

	if (!pio)
		return ERR_PTR(-EINVAL);

	mutex_lock(&pio->state_lock);
	if (pio->state == MT6797_BTIF_PIO_NEW) {
		pio->state = MT6797_BTIF_PIO_LEASED;
		lease = pio->lease;
	} else if (pio->state == MT6797_BTIF_PIO_FENCED) {
		lease = ERR_PTR(-EPIPE);
	} else {
		lease = ERR_PTR(-EBUSY);
	}
	mutex_unlock(&pio->state_lock);
	return lease;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_acquire);

int mt6797_btif_pio_qualify(const struct mt6797_btif_pio_lease *lease,
			    u32 *tri_level)
{
	struct mt6797_btif_pio *pio;
	u32 tri;
	int ret = 0;

	if (!lease || !lease->pio || !tri_level)
		return -EINVAL;
	pio = lease->pio;

	mutex_lock(&pio->state_lock);
	if (lease != pio->lease || pio->state == MT6797_BTIF_PIO_FENCED) {
		ret = -ESHUTDOWN;
	} else if (pio->state != MT6797_BTIF_PIO_LEASED) {
		ret = -EBUSY;
	} else {
		/*
		 * The vendor enables btifc before hal_btif_hw_init(), then permits
		 * loopback only after normal mode has been selected (btif_plat.c:
		 * 476-521,699-721).  The platform owner supplies the former; this
		 * bounded path validates the latter and sends no byte to THR.
		 */
		pio->state = MT6797_BTIF_PIO_QUALIFYING;
		mt6797_btif_pio_hw_init(pio);
		mt6797_btif_pio_update_bits(pio, MT6797_BTIF_TRI_LVL,
					    MT6797_BTIF_TRI_LOOP_EN,
					    MT6797_BTIF_TRI_LOOP_EN);
		tri = pio->io.read32(pio->io.context, MT6797_BTIF_TRI_LVL);
		if (!(tri & MT6797_BTIF_TRI_LOOP_EN))
			ret = -EIO;

		/* Never leave the internal AP-side test route selected. */
		mt6797_btif_pio_update_bits(pio, MT6797_BTIF_TRI_LVL,
					    MT6797_BTIF_TRI_LOOP_EN, 0);
		tri = pio->io.read32(pio->io.context, MT6797_BTIF_TRI_LVL);
		if (!ret && (tri & MT6797_BTIF_TRI_LOOP_EN))
			ret = -EIO;
		if (!ret)
			*tri_level = tri;
		else
			mt6797_btif_pio_fence_locked(pio);
	}
	mutex_unlock(&pio->state_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_qualify);

int
mt6797_btif_pio_loopback_prepare(const struct mt6797_btif_pio_lease *lease,
				 u8 byte)
{
	struct mt6797_btif_pio *pio;
	u32 tri;
	int ret = 0;

	if (!lease || !lease->pio)
		return -EINVAL;
	pio = lease->pio;

	mutex_lock(&pio->state_lock);
	if (lease != pio->lease || pio->state == MT6797_BTIF_PIO_FENCED) {
		ret = -ESHUTDOWN;
	} else if (pio->state != MT6797_BTIF_PIO_LEASED) {
		ret = -EBUSY;
	} else {
		/*
		 * The vendor marks this AP-internal route debug-only and says bytes
		 * sent to BTIF return to BTIF (mtk_btif_exp.c:382-387).  Its own
		 * IRQ path then observes IIR RX and reads RBR (btif_plat.c:765-789,
		 * 829-849).  The platform line remains masked until this one byte is
		 * queued, so neither external CONSYS nor a protocol consumer can run.
		 */
		pio->state = MT6797_BTIF_PIO_LOOPBACK_QUALIFYING;
		mt6797_btif_pio_hw_init(pio);
		mt6797_btif_pio_update_bits(pio, MT6797_BTIF_TRI_LVL,
					    MT6797_BTIF_TRI_LOOP_EN,
					    MT6797_BTIF_TRI_LOOP_EN);
		tri = pio->io.read32(pio->io.context, MT6797_BTIF_TRI_LVL);
		if (!(tri & MT6797_BTIF_TRI_LOOP_EN))
			ret = -EIO;
		else
			pio->io.write8(pio->io.context, MT6797_BTIF_THR, byte);
		if (ret)
			mt6797_btif_pio_fence_locked(pio);
	}
	mutex_unlock(&pio->state_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_loopback_prepare);

int
mt6797_btif_pio_loopback_irq(const struct mt6797_btif_pio_lease *lease, u8 *byte)
{
	struct mt6797_btif_pio *pio;
	u32 iir;
	u32 tri;
	int ret = 0;

	if (!lease || !lease->pio || !byte)
		return -EINVAL;
	pio = lease->pio;

	mutex_lock(&pio->state_lock);
	if (lease != pio->lease || pio->state == MT6797_BTIF_PIO_FENCED) {
		ret = -ESHUTDOWN;
	} else if (pio->state != MT6797_BTIF_PIO_LOOPBACK_QUALIFYING) {
		ret = -EBUSY;
	} else {
		iir = pio->io.read32(pio->io.context, MT6797_BTIF_IIR);
		if (iir & MT6797_BTIF_IIR_NINT) {
			ret = -EAGAIN;
		} else if (!(iir & (MT6797_BTIF_IIR_RX |
				   MT6797_BTIF_IIR_RX_TIMEOUT))) {
			ret = -EPROTO;
		} else {
			*byte = pio->io.read8(pio->io.context, MT6797_BTIF_RBR);
			iir = pio->io.read32(pio->io.context, MT6797_BTIF_IIR);
			if (iir & (MT6797_BTIF_IIR_RX |
				   MT6797_BTIF_IIR_RX_TIMEOUT))
				ret = -EOVERFLOW;
		}

		if (ret != -EAGAIN) {
			mt6797_btif_pio_update_bits(pio, MT6797_BTIF_TRI_LVL,
						    MT6797_BTIF_TRI_LOOP_EN, 0);
			tri = pio->io.read32(pio->io.context, MT6797_BTIF_TRI_LVL);
			if (!ret && (tri & MT6797_BTIF_TRI_LOOP_EN))
				ret = -EIO;
			mt6797_btif_pio_fence_locked(pio);
		}
	}
	mutex_unlock(&pio->state_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_loopback_irq);

static int mt6797_btif_pio_full_receive(void *context, const u8 *data,
					 size_t length)
{
	return mt6797_consys_stp_session_receive_lease(context, data, length);
}

int mt6797_btif_pio_attach_full_stp(
		const struct mt6797_btif_pio_lease *lease,
		const struct mt6797_consys_runtime_lease *runtime_lease,
		const struct mt6797_consys_stp_session_lease *stp_lease)
{
	struct mt6797_btif_pio *pio;
	int ret = 0;

	if (!lease || !lease->pio || !runtime_lease || !stp_lease)
		return -EINVAL;
	if (!mt6797_consys_stp_session_lease_active(stp_lease))
		return -ESHUTDOWN;
	pio = lease->pio;

	mutex_lock(&pio->state_lock);
	if (lease != pio->lease || pio->state == MT6797_BTIF_PIO_FENCED)
		ret = -ESHUTDOWN;
	else if (pio->state != MT6797_BTIF_PIO_LEASED || pio->stp_lease)
		ret = -EBUSY;
	else {
		ret = mt6797_btif_pio_pin_runtime_locked(pio, runtime_lease);
		if (!ret) {
			pio->stp_lease = stp_lease;
			pio->receive = mt6797_btif_pio_full_receive;
			pio->receive_context = (void *)stp_lease;
			mt6797_btif_pio_hw_init(pio);
			pio->state = MT6797_BTIF_PIO_ACTIVE;
		}
	}
	mutex_unlock(&pio->state_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_attach_full_stp);

int mt6797_btif_pio_begin_mandatory(
		const struct mt6797_btif_pio_lease *lease,
		const struct mt6797_consys_runtime_lease *runtime_lease,
		mt6797_btif_pio_rx_cb_t receive, void *receive_context)
{
	struct mt6797_btif_pio *pio;
	int ret = 0;

	if (!lease || !lease->pio || !runtime_lease || !receive)
		return -EINVAL;
	pio = lease->pio;

	mutex_lock(&pio->state_lock);
	if (lease != pio->lease || pio->state == MT6797_BTIF_PIO_FENCED)
		ret = -ESHUTDOWN;
	else if (pio->state != MT6797_BTIF_PIO_LEASED || pio->receive)
		ret = -EBUSY;
	else {
		ret = mt6797_btif_pio_pin_runtime_locked(pio, runtime_lease);
		if (!ret) {
			pio->receive = receive;
			pio->receive_context = receive_context;
			mt6797_btif_pio_hw_init(pio);
			pio->state = MT6797_BTIF_PIO_MANDATORY;
		}
	}
	mutex_unlock(&pio->state_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_begin_mandatory);

int mt6797_btif_pio_handoff_full_bootstrap(
		const struct mt6797_btif_pio_lease *lease,
		mt6797_btif_pio_rx_cb_t receive, void *receive_context)
{
	struct mt6797_btif_pio *pio;
	int ret = 0;

	if (!lease || !lease->pio || !receive)
		return -EINVAL;
	pio = lease->pio;

	mutex_lock(&pio->state_lock);
	if (lease != pio->lease || pio->state == MT6797_BTIF_PIO_FENCED)
		ret = -ESHUTDOWN;
	else if (pio->state != MT6797_BTIF_PIO_MANDATORY)
		ret = -EBUSY;
	else {
		pio->receive = receive;
		pio->receive_context = receive_context;
		pio->state = MT6797_BTIF_PIO_FULL_BOOTSTRAP;
	}
	mutex_unlock(&pio->state_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_handoff_full_bootstrap);

int mt6797_btif_pio_promote_full(const struct mt6797_btif_pio_lease *lease,
				 mt6797_btif_pio_rx_cb_t receive, void *receive_context)
{
	struct mt6797_btif_pio *pio;
	int ret = 0;

	if (!lease || !lease->pio || !receive)
		return -EINVAL;
	pio = lease->pio;

	mutex_lock(&pio->state_lock);
	if (lease != pio->lease || pio->state == MT6797_BTIF_PIO_FENCED)
		ret = -ESHUTDOWN;
	else if (pio->state != MT6797_BTIF_PIO_FULL_BOOTSTRAP)
		ret = -EBUSY;
	else {
		pio->receive = receive;
		pio->receive_context = receive_context;
		pio->state = MT6797_BTIF_PIO_ACTIVE;
	}
	mutex_unlock(&pio->state_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_promote_full);

int mt6797_btif_pio_transmit(void *context, const u8 *frame,
			     size_t frame_length)
{
	const struct mt6797_btif_pio_lease *lease = context;
	struct mt6797_btif_pio *pio;
	unsigned long flags;
	int ret = 0;

	if (!lease || (!frame && frame_length) || !frame_length)
		return -EINVAL;
	if (frame_length > MT6797_BTIF_PIO_TX_BUFFER_SIZE)
		return -EMSGSIZE;
	if (!mt6797_btif_pio_lease_valid(lease))
		return -ESHUTDOWN;
	pio = lease->pio;

	spin_lock_irqsave(&pio->tx_lock, flags);
	if (!mt6797_btif_pio_lease_valid(lease))
		ret = -ESHUTDOWN;
	else if (kfifo_avail(&pio->tx_fifo) < frame_length)
		ret = -EAGAIN;
	else if (kfifo_in(&pio->tx_fifo, frame, frame_length) != frame_length)
		ret = -EIO;
	else {
		pio->tx_enqueue_count++;
		pio->tx_enqueue_bytes += frame_length;
		mt6797_btif_pio_set_ier(pio, MT6797_BTIF_IER_TX,
					 MT6797_BTIF_IER_TX);
	}
	spin_unlock_irqrestore(&pio->tx_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_transmit);

int mt6797_btif_pio_irq(const struct mt6797_btif_pio_lease *lease)
{
	struct mt6797_btif_pio *pio;
	mt6797_btif_pio_rx_cb_t receive;
	void *receive_context;
	u8 rx[MT6797_BTIF_PIO_RX_BUDGET];
	unsigned long flags;
	unsigned int count = 0;
	u32 iir;
	int ret = 0;

	if (!lease || !lease->pio)
		return -EINVAL;
	pio = lease->pio;

	mutex_lock(&pio->state_lock);
	if (!mt6797_btif_pio_lease_valid(lease)) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}

	pio->irq_count++;
	iir = pio->io.read32(pio->io.context, MT6797_BTIF_IIR);
	mt6797_btif_pio_record_iir(pio, iir);
	if (iir & MT6797_BTIF_IIR_NINT) {
		ret = -EAGAIN;
		goto out_unlock;
	}
	while ((iir & (MT6797_BTIF_IIR_RX | MT6797_BTIF_IIR_RX_TIMEOUT)) &&
	       count < ARRAY_SIZE(rx)) {
		rx[count++] = pio->io.read8(pio->io.context, MT6797_BTIF_RBR);
		iir = pio->io.read32(pio->io.context, MT6797_BTIF_IIR);
		mt6797_btif_pio_record_iir(pio, iir);
	}
	if (iir & (MT6797_BTIF_IIR_RX | MT6797_BTIF_IIR_RX_TIMEOUT)) {
		mt6797_btif_pio_fence_locked(pio);
		ret = -EOVERFLOW;
		goto out_unlock;
	}

	receive = pio->receive;
	receive_context = pio->receive_context;
	if (count && !receive) {
		mt6797_btif_pio_fence_locked(pio);
		ret = -EPROTO;
		goto out_unlock;
	}
	if (count) {
		pio->rx_bytes += count;
		pio->rx_callbacks++;
		ret = receive(receive_context, rx, count);
		if (ret == -ESHUTDOWN)
			mt6797_btif_pio_fence_locked(pio);
		if (ret)
			goto out_unlock;
	}

	if (iir & MT6797_BTIF_IIR_TX_EMPTY) {
		spin_lock_irqsave(&pio->tx_lock, flags);
		mt6797_btif_pio_tx_drain_locked(pio);
		spin_unlock_irqrestore(&pio->tx_lock, flags);
	}

out_unlock:
	mutex_unlock(&pio->state_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_irq);

int mt6797_btif_pio_diagnostics_snapshot(
		const struct mt6797_btif_pio_lease *lease,
		struct mt6797_btif_pio_diagnostics *diagnostics)
{
	struct mt6797_btif_pio *pio;
	unsigned long flags;

	if (!lease || !lease->pio || !diagnostics)
		return -EINVAL;
	pio = lease->pio;

	mutex_lock(&pio->state_lock);
	if (!mt6797_btif_pio_lease_valid(lease)) {
		mutex_unlock(&pio->state_lock);
		return -ESHUTDOWN;
	}

	memset(diagnostics, 0, sizeof(*diagnostics));
	spin_lock_irqsave(&pio->tx_lock, flags);
	diagnostics->tx_enqueue_count = pio->tx_enqueue_count;
	diagnostics->tx_enqueue_bytes = pio->tx_enqueue_bytes;
	diagnostics->tx_thr_writes = pio->tx_thr_writes;
	diagnostics->tx_queued = kfifo_len(&pio->tx_fifo);
	spin_unlock_irqrestore(&pio->tx_lock, flags);
	diagnostics->irq_count = pio->irq_count;
	diagnostics->iir_reads = pio->iir_reads;
	diagnostics->iir_rx = pio->iir_rx;
	diagnostics->iir_tx_empty = pio->iir_tx_empty;
	diagnostics->iir_spurious = pio->iir_spurious;
	diagnostics->rx_bytes = pio->rx_bytes;
	diagnostics->rx_callbacks = pio->rx_callbacks;
	/* Do not read write-only THR/RBR, FIFOCTRL or WAK in a timeout report. */
	diagnostics->ier = pio->io.read32(pio->io.context, MT6797_BTIF_IER);
	diagnostics->iir = pio->io.read32(pio->io.context, MT6797_BTIF_IIR);
	diagnostics->fakelcr = pio->io.read32(pio->io.context, MT6797_BTIF_FAKELCR);
	diagnostics->lsr = pio->io.read32(pio->io.context, MT6797_BTIF_LSR);
	diagnostics->dma_en = pio->io.read32(pio->io.context, MT6797_BTIF_DMA_EN);
	diagnostics->tri_lvl = pio->io.read32(pio->io.context, MT6797_BTIF_TRI_LVL);
	diagnostics->handshake =
		pio->io.read32(pio->io.context, MT6797_BTIF_HANDSHAKE);
	mutex_unlock(&pio->state_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_diagnostics_snapshot);

int mt6797_btif_pio_tx_timeout(const struct mt6797_btif_pio_lease *lease)
{
	struct mt6797_btif_pio *pio;
	unsigned long flags;
	bool pending;
	int ret;

	if (!lease || !lease->pio)
		return -EINVAL;
	pio = lease->pio;
	mutex_lock(&pio->state_lock);
	if (!mt6797_btif_pio_lease_valid(lease)) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}
	spin_lock_irqsave(&pio->tx_lock, flags);
	pending = !kfifo_is_empty(&pio->tx_fifo);
	spin_unlock_irqrestore(&pio->tx_lock, flags);
	if (!pending)
		ret = 0;
	else {
		mt6797_btif_pio_fence_locked(pio);
		ret = -ETIMEDOUT;
	}

out_unlock:
	mutex_unlock(&pio->state_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_tx_timeout);

void mt6797_btif_pio_close(struct mt6797_btif_pio *pio)
{
	if (!pio)
		return;

	mutex_lock(&pio->state_lock);
	if (pio->state != MT6797_BTIF_PIO_FENCED) {
		if (pio->state == MT6797_BTIF_PIO_LOOPBACK_QUALIFYING) {
			/* A timed-out loopback qualification must not leave its route set. */
			mt6797_btif_pio_update_bits(pio, MT6797_BTIF_TRI_LVL,
						    MT6797_BTIF_TRI_LOOP_EN, 0);
			mt6797_btif_pio_fence_locked(pio);
		} else if (pio->state == MT6797_BTIF_PIO_QUALIFYING ||
		    pio->state == MT6797_BTIF_PIO_MANDATORY ||
		    pio->state == MT6797_BTIF_PIO_FULL_BOOTSTRAP ||
		    pio->state == MT6797_BTIF_PIO_ACTIVE)
			mt6797_btif_pio_fence_locked(pio);
		else {
			pio->state = MT6797_BTIF_PIO_FENCED;
			pio->stp_lease = NULL;
			pio->receive = NULL;
			pio->receive_context = NULL;
		}
	}
	mutex_unlock(&pio->state_lock);
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_close);

void mt6797_btif_pio_destroy(struct mt6797_btif_pio *pio)
{
	if (!pio || IS_ERR(pio))
		return;

	mt6797_btif_pio_close(pio);
	kfifo_free(&pio->tx_fifo);
	kfree(pio->lease);
	kfree(pio);
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_destroy);

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_PIO_KUNIT_TEST)

#define MT6797_BTIF_PIO_TEST_REGS	(0x70 / sizeof(u32))
#define MT6797_BTIF_PIO_TEST_RX_SIZE	(MT6797_BTIF_PIO_RX_BUDGET + 1)

struct mt6797_btif_pio_test_bus {
	u32 regs[MT6797_BTIF_PIO_TEST_REGS];
	u32 iir;
	u8 rx[MT6797_BTIF_PIO_TEST_RX_SIZE];
	size_t rx_length;
	size_t rx_position;
	u8 tx[MT6797_BTIF_PIO_TX_BUFFER_SIZE];
	size_t tx_length;
};

struct mt6797_btif_pio_test_endpoint {
	unsigned int receive_count;
	u8 payload[MTK_STP_MAX_PAYLOAD];
	size_t payload_length;
};

struct mt6797_btif_pio_test_raw_endpoint {
	unsigned int receive_count;
	u8 bytes[MT6797_BTIF_PIO_RX_BUDGET];
	size_t length;
};

struct mt6797_btif_pio_test_context {
	struct mt6797_btif_pio *pio;
	const struct mt6797_btif_pio_lease *pio_lease;
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease runtime_lease;
	struct mt6797_consys_stp_session *session;
	const struct mt6797_consys_stp_session_lease *stp_lease;
	struct mt6797_btif_pio_test_bus *bus;
	struct mt6797_btif_pio_test_endpoint *endpoint;
};

static int mt6797_btif_pio_test_lifecycle_op(void *context)
{
	return 0;
}

static int mt6797_btif_pio_test_lifecycle_poll(
		void *context, enum mt6797_consys_poll poll)
{
	return poll == MT6797_CONSYS_POLL_CHIP_READY ? 0 : -EINVAL;
}

static const struct mt6797_consys_ops mt6797_btif_pio_test_lifecycle_ops = {
	.abi = MT6797_CONSYS_LIFECYCLE_ABI,
	.enable_vcn18 = mt6797_btif_pio_test_lifecycle_op,
	.disable_vcn18 = mt6797_btif_pio_test_lifecycle_op,
	.enable_vcn28 = mt6797_btif_pio_test_lifecycle_op,
	.disable_vcn28 = mt6797_btif_pio_test_lifecycle_op,
	.configure_sleep_mask = mt6797_btif_pio_test_lifecycle_op,
	.assert_reset = mt6797_btif_pio_test_lifecycle_op,
	.configure_pwron = mt6797_btif_pio_test_lifecycle_op,
	.enable_scp_sys_conn = mt6797_btif_pio_test_lifecycle_op,
	.disable_scp_sys_conn = mt6797_btif_pio_test_lifecycle_op,
	.poll = mt6797_btif_pio_test_lifecycle_poll,
};

static const struct mt6797_consys_timeouts mt6797_btif_pio_test_timeouts = {
	.poll_interval_us = 1,
	.chip_ready_timeout_us = 1,
};

static void mt6797_btif_pio_test_runtime_acquire(
		struct kunit *test, struct mt6797_consys_lifecycle *lifecycle,
		struct mt6797_consys_runtime_lease *runtime_lease)
{
	KUNIT_ASSERT_EQ(test, mt6797_consys_lifecycle_init(lifecycle,
							    &mt6797_btif_pio_test_lifecycle_ops,
							    NULL,
							    &mt6797_btif_pio_test_timeouts), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_acquire(lifecycle,
									  runtime_lease), 0);
}

static u32 mt6797_btif_pio_test_read32(void *context, u32 offset)
{
	struct mt6797_btif_pio_test_bus *bus = context;

	if (offset == MT6797_BTIF_IIR) {
		if (bus->rx_position < bus->rx_length)
			return MT6797_BTIF_IIR_RX;
		return bus->iir;
	}
	return bus->regs[offset / sizeof(u32)];
}

static void mt6797_btif_pio_test_write32(void *context, u32 offset, u32 value)
{
	struct mt6797_btif_pio_test_bus *bus = context;

	bus->regs[offset / sizeof(u32)] = value;
}

static u8 mt6797_btif_pio_test_read8(void *context, u32 offset)
{
	struct mt6797_btif_pio_test_bus *bus = context;

	if (offset != MT6797_BTIF_RBR || bus->rx_position == bus->rx_length)
		return 0;
	return bus->rx[bus->rx_position++];
}

static void mt6797_btif_pio_test_write8(void *context, u32 offset, u8 value)
{
	struct mt6797_btif_pio_test_bus *bus = context;

	if (offset != MT6797_BTIF_THR)
		return;
	if (bus->tx_length < ARRAY_SIZE(bus->tx))
		bus->tx[bus->tx_length++] = value;
	if (bus->regs[MT6797_BTIF_TRI_LVL / sizeof(u32)] &
	    MT6797_BTIF_TRI_LOOP_EN && bus->rx_length < ARRAY_SIZE(bus->rx))
		bus->rx[bus->rx_length++] = value;
}

static int mt6797_btif_pio_test_receive(void *context,
					 enum mtk_stp_channel channel,
					 const u8 *payload, size_t payload_length)
{
	struct mt6797_btif_pio_test_endpoint *endpoint = context;

	if (channel != MTK_STP_WMT)
		return -EOPNOTSUPP;
	endpoint->receive_count++;
	endpoint->payload_length = payload_length;
	memcpy(endpoint->payload, payload, payload_length);
	return 0;
}

static int mt6797_btif_pio_test_receive_raw(void *context, const u8 *data,
						     size_t length)
{
	struct mt6797_btif_pio_test_raw_endpoint *endpoint = context;

	if (length > sizeof(endpoint->bytes))
		return -EMSGSIZE;
	endpoint->receive_count++;
	endpoint->length = length;
	memcpy(endpoint->bytes, data, length);
	return 0;
}

static struct mt6797_btif_pio_test_context *
mt6797_btif_pio_test_start(struct kunit *test)
{
	struct mt6797_btif_pio_test_context *ctx;
	struct mt6797_consys_stp_session_ops stp_ops;
	struct mt6797_btif_pio_io_ops io_ops;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->bus = kunit_kzalloc(test, sizeof(*ctx->bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->bus);
	ctx->endpoint = kunit_kzalloc(test, sizeof(*ctx->endpoint), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->endpoint);
	ctx->bus->iir = MT6797_BTIF_IIR_NINT;
	mt6797_btif_pio_test_runtime_acquire(test, &ctx->lifecycle,
						    &ctx->runtime_lease);
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_pio_test_read32,
		.write32 = mt6797_btif_pio_test_write32,
		.read8 = mt6797_btif_pio_test_read8,
		.write8 = mt6797_btif_pio_test_write8,
		.context = ctx->bus,
	};
	ctx->pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ctx->pio));
	ctx->pio_lease = mt6797_btif_pio_acquire(ctx->pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ctx->pio_lease));
	stp_ops = (struct mt6797_consys_stp_session_ops) {
		.transmit = mt6797_btif_pio_transmit,
		.receive = mt6797_btif_pio_test_receive,
		.transmit_context = (void *)ctx->pio_lease,
		.receive_context = ctx->endpoint,
	};
	ctx->session = mt6797_consys_stp_session_create(&stp_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ctx->session));
	ctx->stp_lease = mt6797_consys_stp_session_begin_full(ctx->session);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ctx->stp_lease));
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_attach_full_stp(ctx->pio_lease,
									&ctx->runtime_lease,
									ctx->stp_lease), 0);
	KUNIT_ASSERT_EQ(test, ctx->bus->regs[MT6797_BTIF_DMA_EN / sizeof(u32)] &
				 (MT6797_BTIF_DMA_RX | MT6797_BTIF_DMA_TX), 0U);
	KUNIT_ASSERT_NE(test, ctx->bus->regs[MT6797_BTIF_DMA_EN / sizeof(u32)] &
				 MT6797_BTIF_DMA_AUTORST, 0U);
	KUNIT_ASSERT_EQ(test, ctx->bus->regs[MT6797_BTIF_TRI_LVL / sizeof(u32)],
				 MT6797_BTIF_TRI_TX(MT6797_BTIF_TX_THRESHOLD) |
				 MT6797_BTIF_TRI_RX(1));
	KUNIT_ASSERT_EQ(test, ctx->bus->regs[MT6797_BTIF_IER / sizeof(u32)],
				 MT6797_BTIF_IER_RX);
	return ctx;
}

static void mt6797_btif_pio_test_stop(
			struct mt6797_btif_pio_test_context *ctx)
{
	mt6797_btif_pio_close(ctx->pio);
	WARN_ON_ONCE(mt6797_consys_runtime_lease_release(&ctx->runtime_lease));
	mt6797_consys_stp_session_quiesce(ctx->session);
	mt6797_consys_stp_session_destroy(ctx->session);
	mt6797_btif_pio_destroy(ctx->pio);
}

static void mt6797_btif_pio_test_partial_tx(struct kunit *test)
{
	struct mt6797_btif_pio_test_context *ctx;
	struct mt6797_btif_pio_diagnostics diagnostics;
	u8 frame[20];
	unsigned int index;

	ctx = mt6797_btif_pio_test_start(test);
	for (index = 0; index < ARRAY_SIZE(frame); index++)
		frame[index] = index;
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_transmit((void *)ctx->pio_lease,
								 frame, sizeof(frame)), 0);
	KUNIT_EXPECT_NE(test, ctx->bus->regs[MT6797_BTIF_IER / sizeof(u32)] &
				MT6797_BTIF_IER_TX, 0U);

	ctx->bus->iir = MT6797_BTIF_IIR_TX_EMPTY;
	ctx->bus->regs[MT6797_BTIF_LSR / sizeof(u32)] = MT6797_BTIF_LSR_TEMT;
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_irq(ctx->pio_lease), 0);
	KUNIT_EXPECT_EQ(test, ctx->bus->tx_length, 16UL);
	KUNIT_EXPECT_NE(test, ctx->bus->regs[MT6797_BTIF_IER / sizeof(u32)] &
				MT6797_BTIF_IER_TX, 0U);

	ctx->bus->regs[MT6797_BTIF_LSR / sizeof(u32)] = MT6797_BTIF_LSR_THRE;
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_irq(ctx->pio_lease), 0);
	KUNIT_EXPECT_EQ(test, ctx->bus->tx_length, sizeof(frame));
	KUNIT_EXPECT_MEMEQ(test, ctx->bus->tx, frame, sizeof(frame));
	KUNIT_EXPECT_EQ(test, ctx->bus->regs[MT6797_BTIF_IER / sizeof(u32)] &
				MT6797_BTIF_IER_TX, 0U);
	KUNIT_ASSERT_EQ(test,
			mt6797_btif_pio_diagnostics_snapshot(ctx->pio_lease,
							      &diagnostics), 0);
	KUNIT_EXPECT_EQ(test, diagnostics.tx_enqueue_count, 1ULL);
	KUNIT_EXPECT_EQ(test, diagnostics.tx_enqueue_bytes, (u64)sizeof(frame));
	KUNIT_EXPECT_EQ(test, diagnostics.tx_thr_writes, (u64)sizeof(frame));
	KUNIT_EXPECT_EQ(test, diagnostics.tx_queued, 0U);
	KUNIT_EXPECT_EQ(test, diagnostics.irq_count, 2ULL);
	KUNIT_EXPECT_EQ(test, diagnostics.iir_tx_empty, 2ULL);
	KUNIT_EXPECT_EQ(test, diagnostics.rx_bytes, 0ULL);
	KUNIT_EXPECT_EQ(test, diagnostics.ier, MT6797_BTIF_IER_RX);
	mt6797_btif_pio_test_stop(ctx);
}

static void mt6797_btif_pio_test_backpressure_and_timeout(struct kunit *test)
{
	struct mt6797_btif_pio_test_context *ctx;
	u8 *frame;
	u8 byte = 0;

	ctx = mt6797_btif_pio_test_start(test);
	frame = kunit_kzalloc(test, MT6797_BTIF_PIO_TX_BUFFER_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_transmit((void *)ctx->pio_lease,
								 frame,
								 MT6797_BTIF_PIO_TX_BUFFER_SIZE), 0);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_transmit((void *)ctx->pio_lease,
								  &byte, sizeof(byte)), -EAGAIN);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_tx_timeout(ctx->pio_lease),
				-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ctx->bus->regs[MT6797_BTIF_IER / sizeof(u32)], 0U);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_transmit((void *)ctx->pio_lease,
							  &byte, sizeof(byte)), -ESHUTDOWN);
	mt6797_btif_pio_test_stop(ctx);
}

static void mt6797_btif_pio_test_rx_fragmentation(struct kunit *test)
{
	struct mt6797_btif_pio_test_context *ctx;
	u8 frame[MTK_STP_HEADER_SIZE + 3 + MTK_STP_TRAILER_SIZE];
	u8 payload[] = { 0x12, 0x34, 0x56 };
	size_t frame_length;

	ctx = mt6797_btif_pio_test_start(test);
	KUNIT_ASSERT_EQ(test, mtk_stp_encode(MTK_STP_MODE_FULL, MTK_STP_WMT, 0, 7,
						 payload, sizeof(payload), frame, sizeof(frame),
						 &frame_length), 0);
	memcpy(ctx->bus->rx, frame, frame_length);
	ctx->bus->rx_length = 3;
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_irq(ctx->pio_lease), 0);
	KUNIT_EXPECT_EQ(test, ctx->endpoint->receive_count, 0U);

	ctx->bus->rx_length = frame_length;
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_irq(ctx->pio_lease), 0);
	KUNIT_EXPECT_EQ(test, ctx->endpoint->receive_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->endpoint->payload_length, sizeof(payload));
	KUNIT_EXPECT_MEMEQ(test, ctx->endpoint->payload, payload, sizeof(payload));
	mt6797_btif_pio_test_stop(ctx);
}

static void mt6797_btif_pio_test_rx_overflow_fences(struct kunit *test)
{
	struct mt6797_btif_pio_test_context *ctx;
	unsigned int index;
	u8 byte = 0;

	ctx = mt6797_btif_pio_test_start(test);
	for (index = 0; index < ARRAY_SIZE(ctx->bus->rx); index++)
		ctx->bus->rx[index] = index;
	ctx->bus->rx_length = ARRAY_SIZE(ctx->bus->rx);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_irq(ctx->pio_lease), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_transmit((void *)ctx->pio_lease,
								  &byte, sizeof(byte)), -ESHUTDOWN);
	mt6797_btif_pio_test_stop(ctx);
}

static void mt6797_btif_pio_test_quiesce_close_and_lease_exclusion(
									struct kunit *test)
{
	struct mt6797_btif_pio_test_context *ctx;
	u8 byte = 0;

	ctx = mt6797_btif_pio_test_start(test);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_attach_full_stp(ctx->pio_lease,
									&ctx->runtime_lease,
									ctx->stp_lease), -EBUSY);
	KUNIT_EXPECT_EQ(test,
			mt6797_consys_runtime_lease_release(&ctx->runtime_lease), -EBUSY);
	mt6797_consys_stp_session_quiesce(ctx->session);
	ctx->bus->rx[0] = byte;
	ctx->bus->rx_length = 1;
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_irq(ctx->pio_lease), -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_transmit((void *)ctx->pio_lease,
								  &byte, sizeof(byte)), -ESHUTDOWN);
	mt6797_btif_pio_close(ctx->pio);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_irq(ctx->pio_lease), -ESHUTDOWN);
	mt6797_btif_pio_test_stop(ctx);
}

static void mt6797_btif_pio_test_mandatory_handoff_is_single_owner(
									struct kunit *test)
{
	struct mt6797_btif_pio_test_bus *bus;
	struct mt6797_btif_pio_test_raw_endpoint *mandatory;
	struct mt6797_btif_pio_test_raw_endpoint *full;
	struct mt6797_btif_pio_io_ops io_ops;
	const struct mt6797_btif_pio_lease *lease;
	struct mt6797_btif_pio *pio;
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease runtime_lease = { };

	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	mandatory = kunit_kzalloc(test, sizeof(*mandatory), GFP_KERNEL);
	full = kunit_kzalloc(test, sizeof(*full), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	KUNIT_ASSERT_NOT_NULL(test, mandatory);
	KUNIT_ASSERT_NOT_NULL(test, full);
	bus->iir = MT6797_BTIF_IIR_NINT;
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_pio_test_read32,
		.write32 = mt6797_btif_pio_test_write32,
		.read8 = mt6797_btif_pio_test_read8,
		.write8 = mt6797_btif_pio_test_write8,
		.context = bus,
	};
	pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pio));
	mt6797_btif_pio_test_runtime_acquire(test, &lifecycle, &runtime_lease);
	lease = mt6797_btif_pio_acquire(pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_begin_mandatory(lease, &runtime_lease,
									mt6797_btif_pio_test_receive_raw,
									mandatory), 0);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_IER / sizeof(u32)],
				MT6797_BTIF_IER_RX);
	bus->rx[0] = 0x80;
	bus->rx[1] = 0x44;
	bus->rx_length = 2;
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_irq(lease), 0);
	KUNIT_EXPECT_EQ(test, mandatory->receive_count, 1U);
	KUNIT_EXPECT_MEMEQ(test, mandatory->bytes, bus->rx, bus->rx_length);

	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_handoff_full_bootstrap(lease,
										mt6797_btif_pio_test_receive_raw,
										full), 0);
	bus->rx[2] = 0x12;
	bus->rx[3] = 0x34;
	bus->rx_length = 4;
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_irq(lease), 0);
	KUNIT_EXPECT_EQ(test, mandatory->receive_count, 1U);
	KUNIT_EXPECT_EQ(test, full->receive_count, 1U);
	KUNIT_EXPECT_EQ(test, full->length, (size_t)2);
	KUNIT_EXPECT_MEMEQ(test, full->bytes, bus->rx + 2, full->length);
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_promote_full(lease,
									mt6797_btif_pio_test_receive_raw,
									full), 0);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_handoff_full_bootstrap(lease,
										mt6797_btif_pio_test_receive_raw,
										mandatory), -EBUSY);
	mt6797_btif_pio_close(pio);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_IER / sizeof(u32)], 0U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(&runtime_lease), 0);
	mt6797_btif_pio_destroy(pio);
}

static void mt6797_btif_pio_test_clocked_qualification_leaves_loopback_off(struct kunit *test)
{
	struct mt6797_btif_pio_test_bus *bus;
	const struct mt6797_btif_pio_lease *lease;
	struct mt6797_btif_pio *pio;
	struct mt6797_btif_pio_io_ops io_ops;
	u32 tri_level;

	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_pio_test_read32,
		.write32 = mt6797_btif_pio_test_write32,
		.read8 = mt6797_btif_pio_test_read8,
		.write8 = mt6797_btif_pio_test_write8,
		.context = bus,
	};
	pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pio));
	lease = mt6797_btif_pio_acquire(pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));

	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_qualify(lease, &tri_level), 0);
	KUNIT_EXPECT_EQ(test, tri_level,
			MT6797_BTIF_TRI_TX(MT6797_BTIF_TX_THRESHOLD) |
			MT6797_BTIF_TRI_RX(1));
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_FAKELCR / sizeof(u32)], 0U);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_HANDSHAKE / sizeof(u32)],
			MT6797_BTIF_HANDSHAKE_EN);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_DMA_EN / sizeof(u32)],
			MT6797_BTIF_DMA_AUTORST);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_IER / sizeof(u32)],
			MT6797_BTIF_IER_RX);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_TRI_LVL / sizeof(u32)] &
			MT6797_BTIF_TRI_LOOP_EN, 0U);

	mt6797_btif_pio_close(pio);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_IER / sizeof(u32)], 0U);
	mt6797_btif_pio_destroy(pio);
}

static void mt6797_btif_pio_test_loopback_byte_fences_after_rx(struct kunit *test)
{
	struct mt6797_btif_pio_test_bus *bus;
	const struct mt6797_btif_pio_lease *lease;
	struct mt6797_btif_pio *pio;
	struct mt6797_btif_pio_io_ops io_ops;
	u8 received;
	u8 pattern = 0xa5;

	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	bus->iir = MT6797_BTIF_IIR_NINT;
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_pio_test_read32,
		.write32 = mt6797_btif_pio_test_write32,
		.read8 = mt6797_btif_pio_test_read8,
		.write8 = mt6797_btif_pio_test_write8,
		.context = bus,
	};
	pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pio));
	lease = mt6797_btif_pio_acquire(pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));

	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_loopback_prepare(lease, pattern), 0);
	KUNIT_EXPECT_EQ(test, bus->tx_length, 1UL);
	KUNIT_EXPECT_EQ(test, bus->tx[0], pattern);
	KUNIT_EXPECT_NE(test, bus->regs[MT6797_BTIF_TRI_LVL / sizeof(u32)] &
			MT6797_BTIF_TRI_LOOP_EN, 0U);
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_loopback_irq(lease, &received), 0);
	KUNIT_EXPECT_EQ(test, received, pattern);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_TRI_LVL / sizeof(u32)] &
			MT6797_BTIF_TRI_LOOP_EN, 0U);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_IER / sizeof(u32)], 0U);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_loopback_prepare(lease, pattern),
			-ESHUTDOWN);
	mt6797_btif_pio_destroy(pio);
}

static void mt6797_btif_pio_test_loopback_close_clears_route(struct kunit *test)
{
	struct mt6797_btif_pio_test_bus *bus;
	const struct mt6797_btif_pio_lease *lease;
	struct mt6797_btif_pio *pio;
	struct mt6797_btif_pio_io_ops io_ops;

	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	bus->iir = MT6797_BTIF_IIR_NINT;
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_pio_test_read32,
		.write32 = mt6797_btif_pio_test_write32,
		.read8 = mt6797_btif_pio_test_read8,
		.write8 = mt6797_btif_pio_test_write8,
		.context = bus,
	};
	pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pio));
	lease = mt6797_btif_pio_acquire(pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_loopback_prepare(lease, 0xa5), 0);

	mt6797_btif_pio_close(pio);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_TRI_LVL / sizeof(u32)] &
			MT6797_BTIF_TRI_LOOP_EN, 0U);
	KUNIT_EXPECT_EQ(test, bus->regs[MT6797_BTIF_IER / sizeof(u32)], 0U);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_loopback_irq(lease, &bus->rx[0]),
			-ESHUTDOWN);
	mt6797_btif_pio_destroy(pio);
}

static struct kunit_case mt6797_btif_pio_test_cases[] = {
	KUNIT_CASE(mt6797_btif_pio_test_partial_tx),
	KUNIT_CASE(mt6797_btif_pio_test_backpressure_and_timeout),
	KUNIT_CASE(mt6797_btif_pio_test_rx_fragmentation),
	KUNIT_CASE(mt6797_btif_pio_test_rx_overflow_fences),
	KUNIT_CASE(mt6797_btif_pio_test_quiesce_close_and_lease_exclusion),
	KUNIT_CASE(mt6797_btif_pio_test_mandatory_handoff_is_single_owner),
	KUNIT_CASE(mt6797_btif_pio_test_clocked_qualification_leaves_loopback_off),
	KUNIT_CASE(mt6797_btif_pio_test_loopback_byte_fences_after_rx),
	KUNIT_CASE(mt6797_btif_pio_test_loopback_close_clears_route),
	{ }
};

static struct kunit_suite mt6797_btif_pio_test_suite = {
	.name = "mt6797-btif-pio",
	.test_cases = mt6797_btif_pio_test_cases,
};

kunit_test_suite(mt6797_btif_pio_test_suite);

#endif

MODULE_DESCRIPTION("Dormant MT6797 BTIF PIO byte-transport owner");
MODULE_LICENSE("GPL");
