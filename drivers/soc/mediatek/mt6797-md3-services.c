// SPDX-License-Identifier: GPL-2.0-only
/* Optional real MD3 AP boot services. One ordered owner handles control and
 * FS; unsupported channels/mandatory runtime contracts terminate execution.
 * No claimed CSS completion, AT endpoint, SCP service, logging or network.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <linux/soc/mediatek/mt6797-md1-fs-provider.h>
#include <linux/soc/mediatek/mt6797-md3-ccif.h>
#include <linux/soc/mediatek/mt6797-md3-execution.h>
#include <linux/soc/mediatek/mt6797-md3-lk-handoff.h>
#include <linux/soc/mediatek/mt6797-md3-services.h>
#include "../../net/wwan/mediatek/mtk_mt6797_ccci_fs.h"
#include "mt6797-md3-ring.h"
#include "mt6797-md3-runtime.h"

struct mt6797_md3_services {
	struct device *dev;
	struct mt6797_md3_ccif *ccif;
	struct mt6797_md3_execution *execution;
	struct mutex lock;
	struct workqueue_struct *wq;
	struct delayed_work work;
	atomic_t stopping, fatal;
	bool attempted, stream, runtime_sent, hs2, stopped, fs_claimed;
	u16 rx_sequence[2], tx_fs_sequence, tx_control_sequence;
	unsigned int resume_attempts[8], next_queue;
	u64 flow_resumes;
	u32 c2k_mode, ccif_irqs, wdt_irqs, irq_mask;
	u32 last_queue, last_channel, last_control, last_fs_op;
	s32 last_fs_status;
	u64 fs_total, fs_replies;
	u32 fs_pending;
	s64 boot_deadline;
	int error;
	struct mt6797_fs_service fs;
	u8 *rx, *tx, *reply, *assembly[MT6797_FS_INDEX_COUNT];
	size_t used[MT6797_FS_INDEX_COUNT];
	u8 runtime[MD3_RUNTIME_SIZE], query[MD3_QUERY_FRAME_SIZE];
};

static void md3_services_irq(void *data, bool wdt, void __iomem *ap)
{
	struct mt6797_md3_services *s = data;
	u32 mask;

	if (wdt) {
		WRITE_ONCE(s->wdt_irqs, s->wdt_irqs + 1);
		atomic_cmpxchg(&s->fatal, 0, -EIO);
	} else {
		WRITE_ONCE(s->ccif_irqs, s->ccif_irqs + 1);
		mask = readl(ap + 0x10);
		WRITE_ONCE(s->irq_mask, s->irq_mask | mask);
		/* All eight queues are drained, so an ACK does not discard work.
		 * A second SRAM HS1 or exception/sequence indication is terminal.
		 */
		if (!mask || (mask & ~0xff00U))
			atomic_cmpxchg(&s->fatal, 0, -EPROTO);
		writel(mask & 0xffff, ap + 0x14);
	}
	if (!atomic_read(&s->stopping))
		mod_delayed_work(s->wq, &s->work, 0);
}

/* Caller holds lock and is the sole process owner. FS may have made durable
 * changes even if reply publication failed; never replay a request here.
 */
static void md3_services_terminal(struct mt6797_md3_services *s, int error)
{
	if (s->stopped)
		return;
	atomic_set(&s->stopping, 1);
	if (!s->error)
		s->error = error;
	if (s->stream) {
		mt6797_md3_ccif_stream_stop(s->ccif);
		s->stream = false;
	}
	/* There are no concurrent FS jobs: this same ordered worker owns them. */
	mt6797_fs_service_release(&s->fs);
	mt6797_md3_execution_shutdown(s->execution);
	s->stopped = true;
}

static int md3_services_send_fs(struct mt6797_md3_services *s,
				const struct mt6797_fs_reply *reply)
{
	unsigned int n;
	s64 deadline = ktime_to_ms(ktime_get()) + 1000;
	int ret, length;

	for (n = 0; n < mt6797_fs_reply_frames(reply); n++) {
		length = mt6797_fs_reply_frame(reply, n, s->tx,
					       MD3_RING_FRAME_MAX);
		if (length < 0)
			return length;
		/* Native sequence is per logical FS_TX15, then C2K maps to wire4.
		 * Like vendor, assert only during the single-task boot phase.
		 */
		put_unaligned_le32(4 | ((u32)s->tx_fs_sequence << 16) |
					   (s->hs2 ? 0 : BIT(31)),
				   s->tx + 8);
		for (;;) {
			ret = atomic_read(&s->fatal);
			if (ret || atomic_read(&s->stopping))
				return ret ?: -ESHUTDOWN;
			if (ktime_to_ms(ktime_get()) >= deadline ||
			    (!s->hs2 &&
			     ktime_to_ms(ktime_get()) >= s->boot_deadline))
				return -ETIMEDOUT;
			ret = mt6797_md3_ccif_transmit(s->ccif, 4, s->tx,
						       length);
			if (ret != -EAGAIN)
				break;
			usleep_range(500, 1000);
		}
		if (ret != length)
			return ret < 0 ? ret : -EIO;
		/* Publication is irrevocable even if deadline or WDT now fires. */
		s->tx_fs_sequence = (s->tx_fs_sequence + 1) & 0x7fff;
		if (n + 1 == mt6797_fs_reply_frames(reply))
			s->fs_replies++; /* Complete wire publication, even on late failure. */
		if (ktime_to_ms(ktime_get()) >= deadline ||
		    (!s->hs2 && ktime_to_ms(ktime_get()) >= s->boot_deadline))
			return -ETIMEDOUT;
	}
	return 0;
}

/* A real MD-advertised busy RX queue needs the standard control resume.
 * This is not an ACK to a fabricated request: the MD-owned FLOW record and
 * actual remaining RX occupancy decide it. No AP write to MD busy/tail.
 */
static int md3_services_resume_rx(struct mt6797_md3_services *s,
				  unsigned int queue)
{
	s64 deadline = ktime_to_ms(ktime_get()) + 1000;
	int ret;

	if (!s->hs2)
		return 0; /* Vendor sends resume only after genuine MD_NORMAL_BOOT. */
	ret = mt6797_md3_ccif_rx_resume_needed(s->ccif, queue,
					       &s->resume_attempts[queue]);
	if (ret <= 0)
		return ret;
	put_unaligned_le32(0xffffffff, s->tx);
	put_unaligned_le32(0x205, s->tx + 4);
	put_unaligned_le32(BIT(31) | ((u32)s->tx_control_sequence << 16),
			   s->tx + 8);
	put_unaligned_le32(queue, s->tx + 12);
	for (;;) {
		ret = atomic_read(&s->fatal);
		if (ret || atomic_read(&s->stopping))
			return ret ?: -ESHUTDOWN;
		if (ktime_to_ms(ktime_get()) >= deadline)
			return -ETIMEDOUT;
		ret = mt6797_md3_ccif_transmit(s->ccif, 0, s->tx, 16);
		if (ret != -EAGAIN)
			break;
		usleep_range(500, 1000);
	}
	if (ret != 16)
		return ret < 0 ? ret : -EIO;
	/* Both sequence and resume attempt are consumed on real publication. */
	s->tx_control_sequence = (s->tx_control_sequence + 1) & 0x7fff;
	if (s->resume_attempts[queue] < 15)
		s->resume_attempts[queue]++;
	s->flow_resumes++;
	return ktime_to_ms(ktime_get()) >= deadline ? -ETIMEDOUT : 0;
}

static int md3_services_frame(struct mt6797_md3_services *s, unsigned int queue,
			      size_t length)
{
	struct mt6797_fs_reply reply = { .payload = s->reply,
					 .capacity = MT6797_FS_REPLY_MAX };
	u32 channel = get_unaligned_le32(s->rx + 8);
	u32 index, control = get_unaligned_le32(s->rx + 4);
	u16 seq = (channel >> 16) & 0x7fff;
	unsigned int port;
	int ret;

	s->last_queue = queue;
	s->last_channel = channel & 0xffff;
	/* Active vendor mapping aliases CTRL0 and STATUS11 to logical
	 * CCCI_CONTROL_RX before its sequence check. Keep that single domain.
	 */
	if (!queue && (!(channel & 0xffff) || (channel & 0xffff) == 11))
		port = 0;
	else if (queue == 4 && (channel & 0xffff) == 4)
		port = 1;
	else
		return -EOPNOTSUPP;
	/* Exact active vendor exception: previous seq0 disables the next gap
	 * check. Every accepted record, including unasserted, updates previous.
	 */
	if ((channel & BIT(31)) && s->rx_sequence[port] != 0 &&
	    ((seq - s->rx_sequence[port]) & 0x7fff) != 1)
		return -EILSEQ;
	s->rx_sequence[port] = seq;
	if (!port) {
		s->last_control = control;
		if (length != 16)
			return -EPROTO;
		if (!control && get_unaligned_le32(s->rx + 12) != 0x5555ffff) {
			if (!s->runtime_sent || s->hs2)
				return -EPROTO;
			s->hs2 =
				true; /* Actual MD_NORMAL_BOOT, never CSS readiness. */
			return 0;
		}
		/* Exact vendor control indications have no AP reply. No DTR client
		 * is owned; retain scalar status without inventing a connected port.
		 */
		/* 0x207 is a heartbeat completion, not an MD-to-AP request.
		 * We own no poller and advertise no polling feature: consume the
		 * bounded header passively, without reply or health/HS2 credit.
		 */
		if (control == 0x201 || control == 0x202 || control == 0x205 ||
		    control == 0x207)
			return 0;
		return -EOPNOTSUPP;
	}
	if (length < 24 || length > MT6797_FS_PREFIX + MT6797_FS_FRAGMENT)
		return -EPROTO;
	index = get_unaligned_le32(s->rx + 12);
	if (index >= MT6797_FS_INDEX_COUNT)
		return -EPROTO;
	put_unaligned_le32((channel & 0xffff0000) | MT6797_FS_RX, s->rx + 8);
	ret = mt6797_fs_request_append_md3(s->assembly[index], &s->used[index],
					   s->rx, length);
	if (ret <= 0)
		return ret;
	s->fs_pending = 1;
	s->fs_total++;
	s->last_fs_op = get_unaligned_le32(s->assembly[index] + 16);
	ret = mt6797_fs_service_request(&s->fs, s->assembly[index],
					s->used[index], &reply,
					&s->last_fs_status);
	if (!ret)
		ret = md3_services_send_fs(s, &reply);
	if (!ret) {
		s->used[index] = 0;
	}
	s->fs_pending = 0;
	return ret;
}

static void md3_services_work(struct work_struct *work)
{
	struct mt6797_md3_services *s = container_of(
		to_delayed_work(work), struct mt6797_md3_services, work);
	unsigned int queue, n, count = 0;
	int ret = 0;

	mutex_lock(&s->lock);
	if (s->stopped || !s->runtime_sent)
		goto out;
	if (atomic_read(&s->stopping)) {
		ret = -ESHUTDOWN;
		goto failed;
	}
	for (n = 0; n < 8; n++) {
		queue = (s->next_queue + n) & 7;
		for (;;) {
			ret = atomic_read(&s->fatal);
			if (ret || atomic_read(&s->stopping)) {
				ret = ret ?: -ESHUTDOWN;
				goto failed;
			}
			if (!s->hs2 &&
			    ktime_to_ms(ktime_get()) >= s->boot_deadline) {
				ret = -ETIMEDOUT;
				goto failed;
			}
			ret = mt6797_md3_ccif_receive(s->ccif, queue, s->rx,
						      MD3_RING_FRAME_MAX);
			if (ret < 0)
				goto failed;
			if (!ret)
				break;
			/* A WDT/STOP may race the final copied RX byte. Do not begin
			 * a new durable FS operation after the transport was fenced.
			 */
			if (atomic_read(&s->fatal) ||
			    atomic_read(&s->stopping)) {
				ret = atomic_read(&s->fatal) ?: -ESHUTDOWN;
				goto failed;
			}
			if (!s->hs2 &&
			    ktime_to_ms(ktime_get()) >= s->boot_deadline) {
				ret = -ETIMEDOUT;
				goto failed;
			}
			ret = md3_services_frame(s, queue, ret);
			if (!ret)
				ret = md3_services_resume_rx(s, queue);
			if (ret)
				goto failed;
			/* Bound one dispatch turn; preserve all unread ring records. */
			if (++count == 64) {
				s->next_queue = (queue + 1) & 7;
				mod_delayed_work(s->wq, &s->work, 0);
				goto out;
			}
		}
	}
	if (!s->hs2) {
		if (ktime_to_ms(ktime_get()) >= s->boot_deadline) {
			ret = -ETIMEDOUT;
			goto failed;
		}
		mod_delayed_work(s->wq, &s->work, msecs_to_jiffies(20));
	}
	goto out;
failed:
	md3_services_terminal(s, ret);
out:
	mutex_unlock(&s->lock);
}

void mt6797_md3_services_shutdown(struct mt6797_md3_services *s)
{
	if (!s)
		return;
	atomic_set(&s->stopping, 1);
	/* Drain callback scheduling before cancelling; worker may itself STOP. */
	mt6797_md3_ccif_stream_stop(s->ccif);
	cancel_delayed_work_sync(&s->work);
	mutex_lock(&s->lock);
	md3_services_terminal(s, 0);
	mutex_unlock(&s->lock);
}

static void md3_services_destroy(void *data)
{
	struct mt6797_md3_services *s = data;

	mt6797_md3_services_shutdown(s);
	destroy_workqueue(s->wq);
}

int mt6797_md3_services_acquire(struct platform_device *pdev,
				struct mt6797_md3_ccif *ccif,
				struct mt6797_md3_execution *execution,
				struct mt6797_md3_services **out)
{
	struct mt6797_md3_lk_snapshot snapshot;
	struct mt6797_md3_services *s;
	unsigned int i;
	int ret;

	if (!pdev || !ccif || !execution || !out)
		return -EINVAL;
	ret = mt6797_md3_lk_handoff_get(&snapshot);
	if (ret)
		return ret;
	if (!snapshot.c2k_lte_mode_present || snapshot.c2k_lte_mode != 2 ||
	    snapshot.image_type != 2)
		return -EOPNOTSUPP;
	s = devm_kzalloc(&pdev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = &pdev->dev;
	s->ccif = ccif;
	s->execution = execution;
	s->c2k_mode = snapshot.c2k_lte_mode;
	mutex_init(&s->lock);
	atomic_set(&s->stopping, 0);
	atomic_set(&s->fatal, 0);
	INIT_DELAYED_WORK(&s->work, md3_services_work);
	s->rx = devm_kmalloc(s->dev, MD3_RING_FRAME_MAX, GFP_KERNEL);
	s->tx = devm_kmalloc(s->dev, MD3_RING_FRAME_MAX, GFP_KERNEL);
	s->reply = devm_kmalloc(s->dev, MT6797_FS_REPLY_MAX, GFP_KERNEL);
	if (!s->rx || !s->tx || !s->reply)
		return -ENOMEM;
	for (i = 0; i < MT6797_FS_INDEX_COUNT; i++) {
		s->assembly[i] = devm_kmalloc(s->dev, MT6797_FS_MD3_REQUEST_MAX,
					      GFP_KERNEL);
		if (!s->assembly[i])
			return -ENOMEM;
	}
	s->wq = alloc_ordered_workqueue("mt6797-md3-services", WQ_MEM_RECLAIM);
	if (!s->wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(s->dev, md3_services_destroy, s);
	if (ret)
		return ret;
	*out = s;
	return 0;
}

/* Called only after all resources and MD1 prerequisites have been acquired,
 * but before the cold owner can mutate reset/power. Missing roots are explicit.
 */
int mt6797_md3_services_claim(struct mt6797_md3_services *s)
{
	int ret;

	if (!s)
		return -EINVAL;
	mutex_lock(&s->lock);
	ret = s->fs_claimed || s->stopped ? -EALREADY :
					    mt6797_fs_service_init_md3(&s->fs);
	if (!ret)
		s->fs_claimed = true;
	mutex_unlock(&s->lock);
	return ret;
}

static int md3_services_pending_error(struct mt6797_md3_services *s)
{
	int ret = atomic_read(&s->fatal);

	if (ret)
		return ret;
	return atomic_read(&s->stopping) ? -ESHUTDOWN : 0;
}

int mt6797_md3_services_start(struct mt6797_md3_services *s)
{
	u8 query[64];
	size_t length;
	int ret;

	if (!s)
		return -EINVAL;
	mutex_lock(&s->lock);
	if (!s->fs_claimed) {
		ret = -ENODEV;
		goto out;
	}
	if (s->attempted || s->stopped) {
		ret = -EALREADY;
		goto out;
	}
	s->attempted = true;
	ret = mt6797_md3_execution_start(s->execution);
	if (ret)
		goto failed;
	ret = mt6797_md3_execution_query(s->execution, query,
					 &s->rx_sequence[0]);
	if (ret)
		goto failed;
	s->rx_sequence[1] = 0xffff;
	/* Native AP default SBP is0, as MD1; do not invent a carrier override. */
	ret = md3_runtime_build(query, s->runtime, sizeof(s->runtime), &length,
				s->query, 0, s->c2k_mode, true);
	if (ret)
		goto failed;
	ret = mt6797_md3_ccif_stream_start(s->ccif, md3_services_irq, s);
	if (ret)
		goto failed;
	s->stream = true;
	/* Enabling IRQs can synchronously expose a pending fatal event. */
	ret = md3_services_pending_error(s);
	if (ret)
		goto failed;
	ret = mt6797_md3_ccif_publish_runtime(s->ccif, s->runtime, length,
					      s->query, sizeof(s->query));
	if (ret != 1) {
		ret = ret < 0 ? ret : -EIO;
		goto failed;
	}
	s->runtime_sent = true;
	/* Publication cannot be undone or retried, even when STOP raced it. */
	ret = md3_services_pending_error(s);
	if (ret)
		goto failed;
	s->boot_deadline = ktime_to_ms(ktime_get()) + 30000;
	mod_delayed_work(s->wq, &s->work, 0);
	ret = 0;
	goto out;
failed:
	md3_services_terminal(s, ret);
out:
	mutex_unlock(&s->lock);
	return ret;
}

ssize_t mt6797_md3_services_show(struct mt6797_md3_services *s, char *buf)
{
	ssize_t length;

	if (!s)
		return sysfs_emit(
			buf, "requested=0 runtime_sent=0 hs2=0 css_ready=0\n");
	mutex_lock(&s->lock);
	length = sysfs_emit(
		buf,
		"requested=1 attempted=%u stopped=%u error=%d runtime_sent=%u hs2=%u css_ready=0 irqs=%u wdt=%u mask=%#x last_queue=%u last_channel=%u last_control=%#x fs_total=%llu fs_pending=%u fs_replies=%llu flow_resumes=%llu fs_op=%#x fs_status=%d\n",
		s->attempted, s->stopped, s->error, s->runtime_sent, s->hs2,
		READ_ONCE(s->ccif_irqs), READ_ONCE(s->wdt_irqs), READ_ONCE(s->irq_mask), s->last_queue,
		s->last_channel, s->last_control, s->fs_total, s->fs_pending,
		s->fs_replies, s->flow_resumes, s->last_fs_op,
		s->last_fs_status);
	mutex_unlock(&s->lock);
	return length;
}
