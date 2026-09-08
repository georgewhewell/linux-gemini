/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_RX_REJECTED_H
#define _MTK_MT6797_CCCI_RX_REJECTED_H

/* Numeric post-decode rejection metadata, including requests refused before
 * a worker can record them. Never retain payload bytes or modem pointers.
 */
#define MT6797_RX_REJECTED_COUNT 16U
struct mt6797_rx_rejected_entry {
	u64 sequence;
	u32 channel, queue, op;
	size_t bytes;
	int error;
	bool op_valid;
};
struct mt6797_rx_rejected {
	spinlock_t lock;
	u64 total;
	unsigned int head, count;
	struct mt6797_rx_rejected_entry entry[MT6797_RX_REJECTED_COUNT];
};
struct mt6797_rx_rejected_snapshot {
	u64 total;
	unsigned int count;
	struct mt6797_rx_rejected_entry entry[MT6797_RX_REJECTED_COUNT];
};

static inline void mt6797_rx_rejected_record(struct mt6797_rx_rejected *d,
		u32 channel, u32 queue, const u8 *frame, size_t bytes, int error)
{
	struct mt6797_rx_rejected_entry e = {
		.channel = channel, .queue = queue, .bytes = bytes, .error = error,
	};
	unsigned long flags;

	/* Only RPC/FS use the operation word at this offset. */
	if (frame && bytes >= 20 && (channel == 14 || channel == 32)) {
		e.op_valid = true;
		e.op = frame[16] | ((u32)frame[17] << 8) |
			((u32)frame[18] << 16) | ((u32)frame[19] << 24);
	}
	spin_lock_irqsave(&d->lock, flags);
	e.sequence = ++d->total;
	d->entry[d->head] = e;
	d->head = (d->head + 1) % MT6797_RX_REJECTED_COUNT;
	if (d->count < MT6797_RX_REJECTED_COUNT)
		d->count++;
	spin_unlock_irqrestore(&d->lock, flags);
}

static inline void mt6797_rx_rejected_snapshot(struct mt6797_rx_rejected *d,
		struct mt6797_rx_rejected_snapshot *out)
{
	unsigned long flags;
	unsigned int i;

	memset(out, 0, sizeof(*out));
	spin_lock_irqsave(&d->lock, flags);
	out->total = d->total;
	out->count = d->count;
	for (i = 0; i < d->count; i++)
		out->entry[i] = d->entry[(d->head + MT6797_RX_REJECTED_COUNT -
					 d->count + i) % MT6797_RX_REJECTED_COUNT];
	spin_unlock_irqrestore(&d->lock, flags);
}
#endif
