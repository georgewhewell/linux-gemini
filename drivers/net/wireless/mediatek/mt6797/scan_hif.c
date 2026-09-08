// SPDX-License-Identifier: GPL-2.0-only
/* One owned passive-scan transaction over the persistent MT6797 HIF. */

#include <linux/errno.h>
#include <linux/ieee80211.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "hif.h"
#include "hif_regs.h"
#include "hif_runtime.h"
#include "scan_hif.h"

static bool mt6797_wifi_scan_hif_backend_valid(
	const struct mt6797_wifi_scan_hif_backend *backend)
{
	return backend && backend->send && backend->now && backend->wait &&
		backend->terminal;
}

static int mt6797_wifi_scan_hif_real_send(void *context, u32 address,
					   const u8 *packet, size_t length)
{
	struct mt6797_wifi_scan_hif *scan = context;

	return mt6797_wifi_hif_runtime_send(scan->irq, address, packet,
					  length);
}

static ktime_t mt6797_wifi_scan_hif_real_now(void *context)
{
	(void)context;
	return ktime_get();
}

static unsigned long mt6797_wifi_scan_hif_real_wait(
	void *context, struct completion *completion, unsigned long timeout)
{
	(void)context;
	return wait_for_completion_timeout(completion, timeout);
}

static void mt6797_wifi_scan_hif_real_terminal(void *context, int reason)
{
	struct mt6797_wifi_scan_hif *scan = context;

	mt6797_wifi_hif_runtime_fail(scan->irq, reason);
}

static int mt6797_wifi_scan_hif_claim_terminal(
	struct mt6797_wifi_scan_hif *scan, int reason)
{
	reason = reason < 0 ? reason : -EIO;
	if (!scan->terminal_reason)
		scan->terminal_reason = reason;
	if (scan->armed)
		complete(&scan->completion);
	return scan->terminal_reason;
}

static int mt6797_wifi_scan_hif_event(
	void *context, const struct mt6797_wifi_event_view *event)
{
	struct mt6797_wifi_scan_hif *scan = context;
	enum mt6797_wifi_scan_event_result disposition;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&scan->lock, flags);
	if (!scan->armed || !scan->dirty)
		goto out;
	ret = mt6797_wifi_scan_event(&scan->transaction, event,
				     &disposition);
	if (ret) {
		ret = mt6797_wifi_scan_hif_claim_terminal(scan, ret);
		goto out;
	}
	if (disposition == MT6797_WIFI_SCAN_EVENT_COMPLETE) {
		scan->scan_done = true;
		complete(&scan->completion);
	}
out:
	spin_unlock_irqrestore(&scan->lock, flags);
	return ret;
}

static bool mt6797_wifi_scan_hif_bss_frame(
	const struct mt6797_wifi_rx_view *frame)
{
	__le16 frame_control;

	if (frame->kind != MT6797_WIFI_RX_IEEE80211 || !frame->packet ||
	    !frame->payload || frame->payload_length < sizeof(frame_control))
		return false;
	frame_control = cpu_to_le16(get_unaligned_le16(frame->payload));
	return ieee80211_is_beacon(frame_control) ||
		ieee80211_is_probe_resp(frame_control);
}

static int mt6797_wifi_scan_hif_frame(
	void *context, const struct mt6797_wifi_rx_view *frame)
{
	struct mt6797_wifi_scan_hif *scan = context;
	enum mt6797_wifi_scan_target_update update;
	unsigned long flags;
	s64 now_ms;
	int ret = 0;

	if (!mt6797_wifi_scan_hif_bss_frame(frame))
		return 0;
	now_ms = ktime_to_ms(scan->backend.now(scan->backend.context));

	spin_lock_irqsave(&scan->lock, flags);
	if (!scan->armed || !scan->dirty || scan->scan_done)
		goto out;
	if (now_ms < 0) {
		ret = mt6797_wifi_scan_hif_claim_terminal(scan, -ERANGE);
		goto out;
	}
	ret = mt6797_wifi_scan_target_observe(&scan->target_owner,
		frame->packet, frame->packet_length, now_ms, &update);
	if (ret) {
		ret = mt6797_wifi_scan_hif_claim_terminal(scan, ret);
		goto out;
	}
	if (update == MT6797_WIFI_SCAN_TARGET_IGNORED)
		goto out;
	if (scan->target_owner.selected.band != scan->expected_band ||
	    scan->target_owner.selected.channel != scan->expected_channel) {
		ret = mt6797_wifi_scan_hif_claim_terminal(scan, -EPROTO);
		goto out;
	}
	scan->target_seen = true;
out:
	spin_unlock_irqrestore(&scan->lock, flags);
	return ret;
}

static void mt6797_wifi_scan_hif_abort(void *context, int reason)
{
	struct mt6797_wifi_scan_hif *scan = context;
	unsigned long flags;

	spin_lock_irqsave(&scan->lock, flags);
	if (scan->armed)
		mt6797_wifi_scan_hif_claim_terminal(scan, reason);
	spin_unlock_irqrestore(&scan->lock, flags);
}

static unsigned long mt6797_wifi_scan_hif_timeout(unsigned int timeout_ms)
{
	return max_t(unsigned long, 1, msecs_to_jiffies(timeout_ms));
}

static int mt6797_wifi_scan_hif_cancel(
	struct mt6797_wifi_scan_hif *scan,
	const struct mt6797_wifi_scan_hif_request *request)
{
	u8 command[MT6797_WIFI_SCAN_CANCEL_LENGTH];
	unsigned long flags;
	int send_ret;
	int ret;

	spin_lock_irqsave(&scan->lock, flags);
	/* This lock is the completion-versus-timeout arbitration point. */
	if (scan->scan_done) {
		ret = 0;
		goto out;
	}
	if (scan->terminal_reason) {
		ret = scan->terminal_reason;
		goto out;
	}
	ret = mt6797_wifi_scan_timeout(&scan->transaction,
		request->cancel_sequence, command, sizeof(command));
	if (ret) {
		ret = mt6797_wifi_scan_hif_claim_terminal(scan, ret);
		goto out;
	}
	scan->cancel_dirty = true;
	ret = mt6797_wifi_scan_hif_claim_terminal(scan, -ETIMEDOUT);
out:
	spin_unlock_irqrestore(&scan->lock, flags);
	if (ret != -ETIMEDOUT)
		return ret;

	send_ret = scan->backend.send(scan->backend.context,
		MT6797_WIFI_MCR_WTDR1, command, sizeof(command));
	spin_lock_irqsave(&scan->lock, flags);
	if (!send_ret)
		scan->cancel_sent = true;
	spin_unlock_irqrestore(&scan->lock, flags);
	return -ETIMEDOUT;
}

int mt6797_wifi_scan_hif_init(
	struct mt6797_wifi_scan_hif *scan, struct mt6797_wifi_hif_irq *irq,
	const struct mt6797_wifi_scan_hif_backend *backend)
{
	if (!scan || !irq || (backend &&
	    !mt6797_wifi_scan_hif_backend_valid(backend)))
		return -EINVAL;
	memset(scan, 0, sizeof(*scan));
	scan->irq = irq;
	if (backend) {
		scan->backend = *backend;
	} else {
		scan->backend = (struct mt6797_wifi_scan_hif_backend) {
			.send = mt6797_wifi_scan_hif_real_send,
			.now = mt6797_wifi_scan_hif_real_now,
			.wait = mt6797_wifi_scan_hif_real_wait,
			.terminal = mt6797_wifi_scan_hif_real_terminal,
			.context = scan,
		};
	}
	spin_lock_init(&scan->lock);
	init_completion(&scan->completion);
	mt6797_wifi_scan_target_init(&scan->target_owner);
	scan->initialized = true;
	return 0;
}

int mt6797_wifi_scan_hif_run(
	struct mt6797_wifi_scan_hif *scan,
	const struct mt6797_wifi_scan_hif_request *request,
	struct mt6797_wifi_scan_hif_result *result)
{
	struct mt6797_wifi_hif_runtime_sink sink = {
		.event = mt6797_wifi_scan_hif_event,
		.frame = mt6797_wifi_scan_hif_frame,
		.abort = mt6797_wifi_scan_hif_abort,
		.context = scan,
	};
	struct mt6797_wifi_scan_channel channel;
	u8 command[MT6797_WIFI_SCAN_COMMAND_LENGTH];
	unsigned long flags;
	bool attached = false;
	bool fence = false;
	s64 now_ms;
	int detach;
	int ret;

	if (result)
		memset(result, 0, sizeof(*result));
	if (!scan || !scan->initialized || !request || !result ||
	    !request->max_age_ms || !request->timeout_ms ||
	    request->timeout_ms > MT6797_WIFI_SCAN_TIMEOUT_MS ||
	    request->max_age_ms < request->timeout_ms ||
	    request->command_sequence == request->cancel_sequence ||
	    (request->band != MT6797_WIFI_SCAN_BAND_2GHZ &&
	     request->band != MT6797_WIFI_SCAN_BAND_5GHZ))
		return -EINVAL;

	channel = (struct mt6797_wifi_scan_channel) {
		.band = request->band,
		.number = request->channel,
	};
	mutex_lock(&scan->irq->lifecycle_lock);
	ret = mt6797_wifi_hif_runtime_status(scan->irq);
	if (ret)
		goto out;
	if (scan->attempted) {
		ret = -EALREADY;
		goto out;
	}
	ret = mt6797_wifi_scan_target_open(&scan->target_owner,
		&request->key, request->max_age_ms);
	if (ret)
		goto out;
	ret = mt6797_wifi_scan_start(&scan->transaction, &channel, 1,
		request->command_sequence, request->scan_sequence, command,
		sizeof(command));
	if (ret)
		goto close_target;

	reinit_completion(&scan->completion);
	spin_lock_irqsave(&scan->lock, flags);
	scan->attempted = true;
	scan->armed = true;
	scan->expected_band = request->band;
	scan->expected_channel = request->channel;
	spin_unlock_irqrestore(&scan->lock, flags);
	ret = mt6797_wifi_hif_runtime_sink_attach(scan->irq, &sink);
	if (ret)
		goto disarm;
	attached = true;

	/* Publish ambiguity before CMD53 can expose the scan command. */
	spin_lock_irqsave(&scan->lock, flags);
	scan->dirty = true;
	spin_unlock_irqrestore(&scan->lock, flags);
	ret = scan->backend.send(scan->backend.context,
		MT6797_WIFI_MCR_WTDR1, command, sizeof(command));
	spin_lock_irqsave(&scan->lock, flags);
	if (ret) {
		ret = mt6797_wifi_scan_hif_claim_terminal(scan, ret);
		fence = true;
	} else {
		scan->sent = true;
	}
	spin_unlock_irqrestore(&scan->lock, flags);
	if (ret)
		goto detach;

	if (!scan->backend.wait(scan->backend.context, &scan->completion,
		mt6797_wifi_scan_hif_timeout(request->timeout_ms))) {
		ret = mt6797_wifi_scan_hif_cancel(scan, request);
		if (ret)
			fence = true;
	} else {
		spin_lock_irqsave(&scan->lock, flags);
		if (scan->scan_done)
			ret = 0;
		else if (scan->terminal_reason)
			ret = scan->terminal_reason;
		else {
			ret = mt6797_wifi_scan_hif_claim_terminal(scan,
				-EPROTO);
			fence = true;
		}
		spin_unlock_irqrestore(&scan->lock, flags);
	}
	if (ret)
		fence = true;

detach:
	detach = mt6797_wifi_hif_runtime_sink_detach(scan->irq, scan);
	attached = false;
	if (detach) {
		spin_lock_irqsave(&scan->lock, flags);
		ret = mt6797_wifi_scan_hif_claim_terminal(scan, detach);
		spin_unlock_irqrestore(&scan->lock, flags);
		fence = true;
	}
disarm:
	spin_lock_irqsave(&scan->lock, flags);
	scan->armed = false;
	result->scan_done = scan->scan_done;
	result->target_seen = scan->target_seen;
	result->cancel_sent = scan->cancel_sent;
	spin_unlock_irqrestore(&scan->lock, flags);
	if (ret)
		goto failed;

	now_ms = ktime_to_ms(scan->backend.now(scan->backend.context));
	if (now_ms < 0) {
		ret = -ERANGE;
		goto failed;
	}
	ret = mt6797_wifi_scan_target_snapshot(&scan->target_owner, now_ms,
		&result->target);
	if (ret)
		goto failed;
	scan->target_valid = true;
	goto out;

failed:
	if (fence)
		scan->backend.terminal(scan->backend.context, ret);
close_target:
	if (scan->target_owner.state == MT6797_WIFI_SCAN_TARGET_OPEN)
		mt6797_wifi_scan_target_close(&scan->target_owner);
out:
	if (attached)
		mt6797_wifi_hif_runtime_sink_detach(scan->irq, scan);
	mutex_unlock(&scan->irq->lifecycle_lock);
	return ret;
}

int mt6797_wifi_scan_hif_close(struct mt6797_wifi_scan_hif *scan)
{
	unsigned long flags;
	int ret;

	if (!scan || !scan->initialized)
		return -EINVAL;
	mutex_lock(&scan->irq->lifecycle_lock);
	spin_lock_irqsave(&scan->lock, flags);
	if (scan->armed) {
		spin_unlock_irqrestore(&scan->lock, flags);
		ret = -EBUSY;
		goto out;
	}
	spin_unlock_irqrestore(&scan->lock, flags);
	if (!scan->target_valid) {
		ret = -ENODATA;
		goto out;
	}
	ret = mt6797_wifi_scan_target_close(&scan->target_owner);
	if (!ret)
		scan->target_valid = false;
out:
	mutex_unlock(&scan->irq->lifecycle_lock);
	return ret;
}
