/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_FS_DIAG_H
#define _MTK_MT6797_CCCI_FS_DIAG_H

/* Numeric metadata only. No paths, payload bytes, handles or modem pointers.
 * The ordered FS worker writes; fatal IRQ/worker paths freeze under irqsave.
 * No other driver lock is acquired while holding this lock.
 */
#define MT6797_FS_DIAG_COUNT 16U
struct mt6797_fs_diag_entry {
	u64 sequence;
	u32 op, flags, requested;
	s32 parse_error, status, result;
	size_t sent;
	bool compact, done;
};
struct mt6797_fs_diag {
	spinlock_t lock;
	bool frozen;
	u64 total;
	unsigned int head, count;
	struct mt6797_fs_diag_entry entry[MT6797_FS_DIAG_COUNT];
};

static inline u64 mt6797_fs_diag_begin(struct mt6797_fs_diag *d,
				     const u8 *frame, size_t len)
{
	struct mt6797_fs_request request;
	struct mt6797_fs_diag_entry e = { 0 };
	unsigned long irqflags;
	u64 sequence = 0;

	e.parse_error = mt6797_fs_request_parse(frame, len, &request);
	if (!e.parse_error) {
		e.op = request.op;
		if (request.op == MT6797_FS_COMPACT_READ && request.argc == 2 &&
		    request.len[1] == 40) {
			e.compact = true;
			e.flags = mt6797_fs_read32(request.arg[1]);
			e.requested = mt6797_fs_read32(request.arg[1] + 32);
		}
	}
	spin_lock_irqsave(&d->lock, irqflags);
	if (!d->frozen) {
		e.sequence = ++d->total;
		sequence = e.sequence;
		d->entry[d->head] = e;
		d->head = (d->head + 1) % MT6797_FS_DIAG_COUNT;
		if (d->count < MT6797_FS_DIAG_COUNT)
			d->count++;
	}
	spin_unlock_irqrestore(&d->lock, irqflags);
	return sequence;
}

static inline void mt6797_fs_diag_end(struct mt6797_fs_diag *d, u64 sequence,
				     s32 status, int result, size_t sent)
{
	unsigned long irqflags;
	struct mt6797_fs_diag_entry *e;

	spin_lock_irqsave(&d->lock, irqflags);
	e = &d->entry[(d->head + MT6797_FS_DIAG_COUNT - 1) % MT6797_FS_DIAG_COUNT];
	/* One ordered worker; freeze can interrupt any service or TX operation. */
	if (!d->frozen && sequence && e->sequence == sequence) {
		e->status = status;
		e->result = result;
		e->sent = sent;
		e->done = true;
	}
	spin_unlock_irqrestore(&d->lock, irqflags);
}

static inline void mt6797_fs_diag_freeze(struct mt6797_fs_diag *d)
{
	unsigned long irqflags;

	spin_lock_irqsave(&d->lock, irqflags);
	d->frozen = true;
	spin_unlock_irqrestore(&d->lock, irqflags);
}
#endif
