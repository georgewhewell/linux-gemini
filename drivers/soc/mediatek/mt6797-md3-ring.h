/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6797_MD3_RING_H
#define _MT6797_MD3_RING_H

/* Active GPL ccci_ringbuf.c: two guards, RX/TX read/write/length triples,
 * payload rings and two tail guards. Single AP process owner per queue.
 * The modem owns RX.write and TX.read; AP never resets either live pointer.
 */
#define MD3_RING_FIRST 0x19000U
#define MD3_RING_BYTES 0xb6950U /* eight rings plus the16-byte FLOW record */
#define MD3_RING_FRAME_MAX 4096U

struct md3_ring_layout {
	u32 offset, rx, tx;
};

static inline int md3_ring_layout(unsigned int queue,
				  struct md3_ring_layout *layout)
{
	static const u32 rx[] = { 10, 100, 100, 150, 50, 10, 10, 10 };
	static const u32 tx[] = { 10, 100, 50, 50, 50, 10, 10, 10 };
	u32 offset = MD3_RING_FIRST;
	unsigned int i;

	if (queue >= ARRAY_SIZE(rx))
		return -EINVAL;
	for (i = 0; i < queue; i++)
		offset += (rx[i] + tx[i]) * 1024 + 40;
	*layout = (struct md3_ring_layout){ offset, rx[queue] * 1024,
					    tx[queue] * 1024 };
	return 0;
}

static inline void md3_ring_copy_from(void *out, void __iomem *ring, u32 pos,
				      u32 count, u32 size)
{
	u32 first = min(count, size - pos);

	memcpy_fromio(out, ring + pos, first);
	if (count > first)
		memcpy_fromio((u8 *)out + first, ring, count - first);
}

static inline void md3_ring_copy_to(void __iomem *ring, u32 pos, const void *in,
				    u32 count, u32 size)
{
	u32 first = min(count, size - pos);

	memcpy_toio(ring + pos, in, first);
	if (count > first)
		memcpy_toio(ring, (const u8 *)in + first, count - first);
}

static inline int md3_ring_guards(void __iomem *smem,
				  const struct md3_ring_layout *l)
{
	void __iomem *p = smem + l->offset;
	u32 size = l->rx + l->tx + 40;

	if (readl(p) != 0xee0000ee || readl(p + 4) != 0xee0000ee ||
	    readl(p + size - 8) != 0xff0000ff ||
	    readl(p + size - 4) != 0xff0000ff || readl(p + 16) != l->rx ||
	    readl(p + 28) != l->tx)
		return -EPROTO;
	return 0;
}

/* Positive length consumes exactly one complete copied record. Empty returns
 * zero; partial/corrupt publication is an error, never an invented empty ring.
 * Caller validates CCCI framing before treating the operation as completed.
 */
static inline int md3_ring_receive(void __iomem *smem, unsigned int queue,
				   void *frame, size_t capacity)
{
	struct md3_ring_layout l;
	void __iomem *p, *ring;
	u32 read, write, available, header[2], footer[2], length, total, end;
	int ret = md3_ring_layout(queue, &l);

	if (ret || !frame)
		return ret ?: -EINVAL;
	ret = md3_ring_guards(smem, &l);
	if (ret)
		return ret;
	p = smem + l.offset;
	ring = p + 32;
	read = readl(p + 8);
	write = readl(p + 12);
	if (read >= l.rx || write >= l.rx || ((read | write) & 7))
		return -EPROTO;
	if (read == write)
		return 0;
	rmb(); /* Modem payload publication precedes its observed write pointer. */
	available = write >= read ? write - read : l.rx - read + write;
	if (available < 32)
		return -EPROTO;
	md3_ring_copy_from(header, ring, read, 8, l.rx);
	length = le32_to_cpu(header[1]);
	if (le32_to_cpu(header[0]) != 0xaabbaabb || length < 16 ||
	    length > MD3_RING_FRAME_MAX)
		return -EPROTO;
	if (length > capacity)
		return -EMSGSIZE;
	total = ALIGN(length, 8) + 16;
	if (total > available)
		return -EPROTO;
	end = (read + total) % l.rx;
	md3_ring_copy_from(footer, ring, (read + total - 8) % l.rx, 8, l.rx);
	if (le32_to_cpu(footer[0]) != 0xccddeeff ||
	    le32_to_cpu(footer[1]) != 0xccddeeff)
		return -EPROTO;
	md3_ring_copy_from(frame, ring, (read + 8) % l.rx, length, l.rx);
	/* Finish all payload reads before releasing these bytes for MD reuse. */
	mb();
	writel(end, p + 8);
	return length;
}

/* Positive length means the packet is published and must never be retried.
 * Caller serializes this with the queue's CCIF doorbell and STOP disposition.
 */
static inline int md3_ring_transmit(void __iomem *smem, unsigned int queue,
				    const void *frame, size_t length)
{
	struct md3_ring_layout l;
	void __iomem *p, *ring;
	u32 read, write, room, total, header[2], footer[2];
	int ret = md3_ring_layout(queue, &l);

	if (ret || !frame || length < 16 || length > MD3_RING_FRAME_MAX)
		return ret ?: -EINVAL;
	ret = md3_ring_guards(smem, &l);
	if (ret)
		return ret;
	p = smem + l.offset;
	ring = p + 32 + l.rx;
	read = readl(p + 20);
	write = readl(p + 24);
	if (read >= l.tx || write >= l.tx || ((read | write) & 7))
		return -EPROTO;
	total = ALIGN(length, 8) + 16;
	room = read > write ? read - write - 1 : l.tx - write + read - 1;
	/* Match vendor's strict room inequality, retaining its empty sentinel. */
	if (total >= room)
		return -EAGAIN;
	rmb();
	header[0] = cpu_to_le32(0xaabbaabb);
	header[1] = cpu_to_le32(length);
	footer[0] = footer[1] = cpu_to_le32(0xccddeeff);
	md3_ring_copy_to(ring, write, header, 8, l.tx);
	md3_ring_copy_to(ring, (write + 8) % l.tx, frame, length, l.tx);
	md3_ring_copy_to(ring, (write + total - 8) % l.tx, footer, 8, l.tx);
	wmb(); /* Entire packet precedes AP's producer index and doorbell. */
	writel((write + total) % l.tx, p + 24);
	return length;
}
/* Active vendor FLOW handshake: only the modem writes MD busy bits/tail.
 * Resume thresholds are half, eighth, ... of the RX ring. Saturating the
 * counter avoids the vendor's unbounded shift without widening a threshold.
 */
static inline int md3_ring_resume_needed(void __iomem *smem, unsigned int queue,
					 unsigned int *attempts)
{
	struct md3_ring_layout l;
	void __iomem *flow = smem + MD3_RING_FIRST + MD3_RING_BYTES - 16;
	u32 read, write, used, tail;
	int ret = md3_ring_layout(queue, &l);

	if (ret || !attempts)
		return ret ?: -EINVAL;
	if (readl(flow) != 0x464c4f57)
		return -EPROTO;
	tail = readl(flow + 12);
	if (!tail)
		return 0; /* MD has not advertised the matching flow protocol. */
	if (tail != 0x4354524c)
		return -EPROTO;
	if (!(readl(flow + 8) & BIT(queue))) {
		*attempts = 0;
		return 0;
	}
	read = readl(smem + l.offset + 8);
	write = readl(smem + l.offset + 12);
	if (read >= l.rx || write >= l.rx || ((read | write) & 7))
		return -EPROTO;
	used = write >= read ? write - read : l.rx - read + write;
	return used <= (l.rx >> (1 + 2 * min(*attempts, 15U)));
}
#endif
