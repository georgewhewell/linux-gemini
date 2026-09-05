/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_DIAG_H
#define _MTK_MT6797_CCCI_DIAG_H

#define MT6797_CCCI_DIAG_RX_COUNT 4
#define MT6797_CCCI_DIAG_RX_BYTES 1024
#define MT6797_CCCI_DIAG_RX_WIRE_MAX 3584

enum mt6797_ccci_diag_drop {
	MT6797_CCCI_DIAG_BAD_HEADER = 1,
	MT6797_CCCI_DIAG_BAD_CHANNEL,
	MT6797_CCCI_DIAG_BEFORE_READY,
	MT6797_CCCI_DIAG_BAD_SEQUENCE,
};

struct mt6797_ccci_diag_rx {
	u32 queue, cause, channel, payload_word0;
	size_t length, captured;
	int decode_error;
	bool header_valid, payload_word0_valid, truncated;
	u8 bytes[MT6797_CCCI_DIAG_RX_BYTES];
};

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
	u32 rx_count;
	struct mt6797_ccci_diag_rx rx[MT6797_CCCI_DIAG_RX_COUNT];
};

static void mt6797_ccci_diag_rx_save(struct mt6797_ccci_diag *d,
		const void *data, size_t length, u32 queue, u32 cause,
		int decode_error, u32 channel)
{
	const u8 *bytes = data;
	struct mt6797_ccci_diag_rx *r;
	unsigned long flags;

	if (!bytes || length > MT6797_CCCI_DIAG_RX_WIRE_MAX || queue >= 7 ||
	    cause < MT6797_CCCI_DIAG_BAD_HEADER ||
	    cause > MT6797_CCCI_DIAG_BAD_SEQUENCE)
		return;
	spin_lock_irqsave(&d->lock, flags);
	if (d->frozen || d->rx_count == MT6797_CCCI_DIAG_RX_COUNT)
		goto out;
	r = &d->rx[d->rx_count++];
	r->queue = queue;
	r->cause = cause;
	r->length = length;
	r->captured = length < sizeof(r->bytes) ? length : sizeof(r->bytes);
	r->truncated = length > r->captured;
	r->decode_error = decode_error;
	r->header_valid = !decode_error && length >= MTK_CCCI_HEADER_SIZE;
	if (r->header_valid) {
		r->channel = channel;
		/* Raw first payload word, not an assumed RPC/FS operation. */
		if (length >= MTK_CCCI_HEADER_SIZE + 4) {
			const u8 *p = bytes + MTK_CCCI_HEADER_SIZE;

			r->payload_word0 = p[0] | ((u32)p[1] << 8) |
				((u32)p[2] << 16) | ((u32)p[3] << 24);
			r->payload_word0_valid = true;
		}
	}
	memcpy(r->bytes, data, r->captured);
out:
	spin_unlock_irqrestore(&d->lock, flags);
}

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
