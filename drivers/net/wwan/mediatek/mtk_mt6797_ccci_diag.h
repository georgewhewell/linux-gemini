/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_DIAG_H
#define _MTK_MT6797_CCCI_DIAG_H

/* No allocation, MMIO, or descriptor writes. Caller owns this through teardown.
 * TX lock may nest this lock; diagnostic code never takes the TX lock.
 * Freezing makes all saved bytes immutable before any printk or STOP command.
 */
struct mt6797_ccci_diag {
	spinlock_t lock;
	bool frozen, payload_valid, reply_finalized;
	u32 irq_count, first_irq[4], last_irq[4];
	u32 submitted, reclaimed, head, tail, used;
	u8 descriptor[16];
	bool descriptor_valid;
	u8 hs1[MTK_CCCI_HEADER_SIZE + MTK_CCCI_FEATURE_QUERY_SIZE];
	u8 reply[MTK_CCCI_RUNTIME_V2_REPLY_SIZE];
	u8 runtime[MTK_CCCI_RUNTIME_AP_SIZE];
	size_t runtime_len;
};

static void mt6797_ccci_diag_irq(struct mt6797_ccci_diag *d,
			       u32 tx, u32 txmask, u32 rx, u32 rxmask)
{
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	if (!d->frozen) {
		if (!d->irq_count) {
			d->first_irq[0] = tx;
			d->first_irq[1] = txmask;
			d->first_irq[2] = rx;
			d->first_irq[3] = rxmask;
		}
		if (d->irq_count != U32_MAX)
			d->irq_count++;
		d->last_irq[0] = tx;
		d->last_irq[1] = txmask;
		d->last_irq[2] = rx;
		d->last_irq[3] = rxmask;
	}
	spin_unlock_irqrestore(&d->lock, flags);
}

static void mt6797_ccci_diag_tx(struct mt6797_ccci_diag *d, bool reclaim,
			      u32 head, u32 tail, u32 used)
{
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	if (!d->frozen) {
		if (reclaim) {
			if (d->reclaimed != U32_MAX)
				d->reclaimed++;
		} else if (d->submitted != U32_MAX) {
			d->submitted++;
		}
		d->head = head;
		d->tail = tail;
		d->used = used;
	}
	spin_unlock_irqrestore(&d->lock, flags);
}

static void mt6797_ccci_diag_payload(struct mt6797_ccci_diag *d,
		const void *hs1, size_t hs1_len, const void *reply, size_t reply_len,
		const void *runtime, size_t runtime_len)
{
	unsigned long flags;

	if (!hs1 || !reply || !runtime || hs1_len != sizeof(d->hs1) ||
	    reply_len != sizeof(d->reply) || runtime_len > sizeof(d->runtime))
		return;
	spin_lock_irqsave(&d->lock, flags);
	if (!d->frozen && !d->payload_valid) {
		memcpy(d->hs1, hs1, hs1_len);
		memcpy(d->reply, reply, reply_len);
		memcpy(d->runtime, runtime, runtime_len);
		d->runtime_len = runtime_len;
		d->payload_valid = true;
	}
	spin_unlock_irqrestore(&d->lock, flags);
}

static void mt6797_ccci_diag_final_reply(struct mt6797_ccci_diag *d,
				       const void *reply, size_t len)
{
	unsigned long flags;

	if (!reply || len != sizeof(d->reply))
		return;
	spin_lock_irqsave(&d->lock, flags);
	if (!d->frozen && d->payload_valid && !d->reply_finalized) {
		memcpy(d->reply, reply, len);
		d->reply_finalized = true;
	}
	spin_unlock_irqrestore(&d->lock, flags);
}

static bool mt6797_ccci_diag_freeze(struct mt6797_ccci_diag *d,
				  const void *first_q0_gpd)
{
	const u8 *bytes = first_q0_gpd;
	unsigned long flags;
	unsigned int i;
	bool first;

	spin_lock_irqsave(&d->lock, flags);
	first = !d->frozen;
	if (first) {
		d->frozen = true;
		if (bytes) {
			/* Live coherent descriptor observation, not an atomic DMA snapshot.
			 * The first Q0 slot is retained even after software reclamation.
			 */
			for (i = 0; i < sizeof(d->descriptor); i++)
				d->descriptor[i] = READ_ONCE(bytes[i]);
			d->descriptor_valid = true;
		}
	}
	spin_unlock_irqrestore(&d->lock, flags);
	return first;
}
#endif
