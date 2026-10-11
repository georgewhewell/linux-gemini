/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_DIAG_H
#define _MTK_MT6797_CCCI_DIAG_H

#define MT6797_CCCI_DIAG_RX_COUNT 4
#define MT6797_CCCI_DIAG_RX_BYTES 1024
#define MT6797_CCCI_DIAG_RX_WIRE_MAX 3584

/* Public MT6797 eccci/ccci_config.h and mdee_dumper_v2.h UMOLY ABI.
 * Decode fixed PL exception fields and a validated assertion source basename.
 * Never print paths, stacks or the contents of the shared exception region.
 */
#define MT6797_CCCI_MDSS_OFFSET 2048U
#define MT6797_CCCI_MDSS_SIZE 8192U
#define MT6797_CCCI_EX_OVERVIEW_SIZE 144U
#define MT6797_CCCI_EX_PL_CONTENT_OFFSET 320U
#define MT6797_CCCI_EX_PL_ASSERT_PATH_SIZE 256U
#define MT6797_CCCI_EX_PL_ASSERT_FILE_SIZE 64U
#define MT6797_CCCI_EX_PL_ASSERT_LINE_OFFSET 640U

struct mt6797_ccci_pl_exception {
	bool header_valid, content_valid, offender;
	bool type_known;
	u32 core_id;
	u8 type;
	u32 codes[3], assert_line;
	bool assert_parameters_valid, assert_file_valid;
	u32 assert_parameters[3];
	char assert_file[MT6797_CCCI_EX_PL_ASSERT_FILE_SIZE];
};

struct mt6797_ccci_md_exception {
	bool overview_valid;
	bool pl_prefix_only;
	u32 core_count, ect_status, cs_status, md32_status;
	struct mt6797_ccci_pl_exception pl[2];
};

static u32 mt6797_ccci_diag_le32(const u8 *p)
{
	return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
		((u32)p[3] << 24);
}

/* Firmware's public ASSERT record has a path and a separate filename. Accept
 * only a terminated, bounded ASCII basename; no directory or log delimiters
 * leave the snapshot. Validate first so rejection cannot publish a prefix.
 */
static bool mt6797_ccci_assert_basename(const u8 *field, size_t size,
				       char *name)
{
	size_t end, start = 0, i;

	for (end = 0; end < size && field[end]; end++)
		if (field[end] == '/' || field[end] == '\\')
			start = end + 1;
	if (end == size || end == start ||
	    end - start >= MT6797_CCCI_EX_PL_ASSERT_FILE_SIZE)
		return false;
	for (i = start; i < end; i++) {
		u8 c = field[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
			return false;
	}
	if (field[start] == '.' || field[start] == '-')
		return false;
	memcpy(name, field + start, end - start);
	name[end - start] = '\0';
	return true;
}

static void mt6797_ccci_md_exception_decode(const u8 *bytes, size_t length,
					   struct mt6797_ccci_md_exception *out)
{
	unsigned int i;

	memset(out, 0, sizeof(*out));
	if (!bytes || length < MT6797_CCCI_EX_OVERVIEW_SIZE)
		return;
	out->core_count = mt6797_ccci_diag_le32(bytes);
	if (!out->core_count || out->core_count > 9)
		return;
	/* Public v2 consumers inspect a fixed eight-entry table regardless of
	 * core_num. The observed count 9 is not a qualified overview layout:
	 * inspect only independently bounded leading PL records, without guessing
	 * a ninth entry or shifted status offsets. core_id is numeric metadata;
	 * its wire-value mapping is not established by the public header.
	 */
	out->pl_prefix_only = out->core_count == 9;
	if (!out->pl_prefix_only) {
		out->overview_valid = true;
		out->ect_status = mt6797_ccci_diag_le32(bytes + 132);
		out->cs_status = mt6797_ccci_diag_le32(bytes + 136);
		out->md32_status = mt6797_ccci_diag_le32(bytes + 140);
	}
	for (i = 0; i < ARRAY_SIZE(out->pl) && i < out->core_count; i++) {
		struct mt6797_ccci_pl_exception *pl = &out->pl[i];
		u32 offset = mt6797_ccci_diag_le32(bytes + 4 + 16 * i);
		size_t needed;
		const u8 *record;

		if (out->pl_prefix_only && bytes[8 + 16 * i] > 1)
			continue;
		pl->offender = bytes[8 + 16 * i] != 0;
		if (offset < MT6797_CCCI_EX_OVERVIEW_SIZE || offset > length - 8)
			continue;
		record = bytes + offset;
		pl->core_id = mt6797_ccci_diag_le32(record);
		pl->type = record[4];
		pl->header_valid = true;
		pl->type_known = (pl->type >= 1 && pl->type <= 7) ||
			(pl->type >= 16 && pl->type <= 18) ||
			(pl->type >= 0x20 && pl->type <= 0x26);
		if (pl->type >= 16 && pl->type <= 18) {
			needed = MT6797_CCCI_EX_PL_ASSERT_LINE_OFFSET + 4;
			if (needed > length || offset > length - needed)
				continue;
			pl->assert_line = mt6797_ccci_diag_le32(record +
				MT6797_CCCI_EX_PL_ASSERT_LINE_OFFSET);
			pl->assert_file_valid = mt6797_ccci_assert_basename(record +
				MT6797_CCCI_EX_PL_CONTENT_OFFSET +
				MT6797_CCCI_EX_PL_ASSERT_PATH_SIZE,
				MT6797_CCCI_EX_PL_ASSERT_FILE_SIZE, pl->assert_file);
			if (!pl->assert_file_valid)
				pl->assert_file_valid = mt6797_ccci_assert_basename(
					record + MT6797_CCCI_EX_PL_CONTENT_OFFSET,
					MT6797_CCCI_EX_PL_ASSERT_PATH_SIZE,
					pl->assert_file);
			needed += sizeof(pl->assert_parameters);
			if (needed <= length && offset <= length - needed) {
				unsigned int j;

				for (j = 0; j < ARRAY_SIZE(pl->assert_parameters); j++)
					pl->assert_parameters[j] = mt6797_ccci_diag_le32(
						record + MT6797_CCCI_EX_PL_ASSERT_LINE_OFFSET +
						4 + 4 * j);
				pl->assert_parameters_valid = true;
			}
		} else if ((pl->type >= 1 && pl->type <= 7) ||
			   (pl->type >= 0x20 && pl->type <= 0x26)) {
			needed = MT6797_CCCI_EX_PL_CONTENT_OFFSET + 12;
			if (needed > length || offset > length - needed)
				continue;
			pl->codes[0] = mt6797_ccci_diag_le32(record +
				MT6797_CCCI_EX_PL_CONTENT_OFFSET);
			pl->codes[1] = mt6797_ccci_diag_le32(record +
				MT6797_CCCI_EX_PL_CONTENT_OFFSET + 4);
			pl->codes[2] = mt6797_ccci_diag_le32(record +
				MT6797_CCCI_EX_PL_CONTENT_OFFSET + 8);
		} else {
			continue;
		}
		pl->content_valid = true;
	}
}

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

enum mt6797_ccci_at_event {
	MT6797_CCCI_AT_OPEN,
	MT6797_CCCI_AT_CLOSE,
	MT6797_CCCI_AT_TX_SUBMITTED,
	MT6797_CCCI_AT_Q1_RECLAIMED,
	MT6797_CCCI_AT_RX_SEEN,
	MT6797_CCCI_AT_RX_CLOSED_DROP,
	MT6797_CCCI_AT_RX_DELIVERED,
	MT6797_CCCI_AT_EVENT_COUNT,
};

/* No allocation, MMIO, or descriptor writes. Caller owns this through teardown.
 * TX lock may nest this lock; diagnostic code never takes the TX lock.
 * Freezing makes all saved bytes immutable before any printk or STOP command.
 */
struct mt6797_ccci_diag {
	spinlock_t lock;
	bool frozen, payload_valid, reply_finalized;
	u32 at_events[MT6797_CCCI_AT_EVENT_COUNT];
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
	/* First-fatal observation only, retained in driver-owned memory. */
	u8 md_exception[MT6797_CCCI_MDSS_SIZE];
};

/* Payload-free counters, frozen with the existing first-fatal snapshot. */
static void mt6797_ccci_diag_at(struct mt6797_ccci_diag *d, unsigned int event)
{
	unsigned long flags;

	if (event >= MT6797_CCCI_AT_EVENT_COUNT)
		return;
	spin_lock_irqsave(&d->lock, flags);
	if (!d->frozen && d->at_events[event] != U32_MAX)
		d->at_events[event]++;
	spin_unlock_irqrestore(&d->lock, flags);
}

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
