// SPDX-License-Identifier: GPL-2.0-only
/* Serialized bridge from the persistent HIF event pump to pure AIS state. */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/string.h>
#include <linux/slab.h>

#include "ais_hif.h"
#include "boot.h"
#include "capability.h"
#include "hif.h"
#include "hif_regs.h"
#include "hif_runtime.h"
#include "runtime_config.h"
#include "scan_target.h"

#define MT6797_WIFI_AIS_CHANNEL_EVENT_ID	0x10U
#define MT6797_WIFI_AIS_AUTH_PACKET_LENGTH	60U
#define MT6797_WIFI_AIS_AUTH_HEADER_LENGTH	24U

static bool mt6797_wifi_ais_hif_backend_valid(
	const struct mt6797_wifi_ais_hif_backend *backend)
{
	return backend && backend->send && backend->now && backend->wait &&
		backend->terminal;
}

static int mt6797_wifi_ais_hif_real_send(void *context, u32 address,
					 const u8 *packet, size_t length)
{
	struct mt6797_wifi_ais_hif *ais = context;

	return mt6797_wifi_hif_runtime_send(ais->irq, address, packet,
					   length);
}

static ktime_t mt6797_wifi_ais_hif_real_now(void *context)
{
	(void)context;
	return ktime_get();
}

static unsigned long mt6797_wifi_ais_hif_real_wait(
	void *context, struct completion *completion, unsigned long timeout)
{
	(void)context;
	return wait_for_completion_timeout(completion, timeout);
}

static void mt6797_wifi_ais_hif_real_terminal(void *context, int reason)
{
	struct mt6797_wifi_ais_hif *ais = context;

	mt6797_wifi_hif_runtime_fail(ais->irq, reason);
}

static bool mt6797_wifi_ais_hif_ready(const struct mt6797_wifi_ais_hif *ais)
{
	const struct mt6797_wifi_runtime_config *config = ais->config;
	const struct mt6797_wifi_hif_irq *irq = ais->irq;

	return irq->transaction.active && !irq->transaction.terminal &&
		mt6797_wifi_hif_irq_mode(irq) == MT6797_WIFI_HIF_IRQ_RUNTIME &&
		irq->boot && irq->boot->online && irq->boot->transaction.ready &&
		config->transaction.configured && config->boot == irq->boot &&
		config->hif == irq->hif;
}

static int mt6797_wifi_ais_hif_status(const struct mt6797_wifi_ais_hif *ais)
{
	int ret;

	ret = mt6797_wifi_hif_runtime_status(ais->irq);
	if (ret)
		return ret;
	return mt6797_wifi_ais_hif_ready(ais) ? 0 : -EPIPE;
}

static int mt6797_wifi_ais_hif_precheck(void *context)
{
	struct mt6797_wifi_ais_hif *ais = context;

	return mt6797_wifi_ais_hif_status(ais);
}

static int mt6797_wifi_ais_hif_send(
	void *context, const struct mt6797_wifi_ais_runtime *runtime,
	enum mt6797_wifi_ais_runtime_command command, const u8 *packet,
	size_t length)
{
	struct mt6797_wifi_ais_hif *ais = context;
	u8 match[2];
	int ret;

	if (!runtime || !packet || !length ||
	    command >= MT6797_WIFI_AIS_RUNTIME_COMMAND_COUNT)
		return -EINVAL;
	ret = mt6797_wifi_ais_hif_status(ais);
	if (ret)
		return ret;
	if (command == MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST) {
		match[0] = runtime->channel_pending.bss_index;
		match[1] = runtime->channel_pending.token;
		ret = mt6797_wifi_hif_wait_event_arm(&ais->irq->wait_event,
			MT6797_WIFI_AIS_CHANNEL_EVENT_ID, match, sizeof(match));
		if (ret)
			return ret;
		ais->channel_requested_at = ais->backend.now(ais->backend.context);
		ais->channel_deadline = 0;
	}

	ret = ais->backend.send(ais->backend.context, MT6797_WIFI_MCR_WTDR1,
				packet, length);
	if (ret && command == MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST)
		mt6797_wifi_hif_wait_event_disarm(&ais->irq->wait_event);
	return ret;
}

static unsigned long mt6797_wifi_ais_hif_remaining(
	struct mt6797_wifi_ais_hif *ais, ktime_t deadline)
{
	s64 remaining = ktime_to_ns(ktime_sub(deadline,
					       ais->backend.now(
						       ais->backend.context)));

	if (remaining <= 0)
		return 0;
	return max_t(unsigned long, 1, nsecs_to_jiffies(remaining));
}

static void mt6797_wifi_ais_hif_fence(void *context, int reason);
static int mt6797_wifi_ais_hif_wait_channel(void *context,
					    unsigned int timeout_ms);

static void mt6797_wifi_ais_hif_init_runtime_ops(
	struct mt6797_wifi_ais_hif *ais,
	struct mt6797_wifi_ais_runtime_ops *ops)
{
	*ops = (struct mt6797_wifi_ais_runtime_ops) {
		.precheck = mt6797_wifi_ais_hif_precheck,
		.send = mt6797_wifi_ais_hif_send,
		.wait_channel = mt6797_wifi_ais_hif_wait_channel,
		.fence = mt6797_wifi_ais_hif_fence,
		.context = ais,
	};
}

static int mt6797_wifi_ais_hif_deliver_copy(
	struct mt6797_wifi_ais_hif *ais,
	const struct mt6797_wifi_hif_event_copy *copy)
{
	struct mt6797_wifi_event_view event = {
		.payload = copy->payload,
		.payload_length = copy->payload_length,
		.packet_length = copy->packet_length,
		.id = copy->id,
		.sequence = copy->sequence,
	};
	enum mt6797_wifi_ais_event_disposition disposition;
	struct mt6797_wifi_ais_runtime_ops ops;
	struct mt6797_wifi_ais_channel_event grant;
	int ret;

	if (copy->error)
		return copy->error;
	mt6797_wifi_ais_hif_init_runtime_ops(ais, &ops);
	ret = mt6797_wifi_ais_runtime_event(&ais->runtime, &ops, &event,
					    &disposition);
	if (ret)
		return ret;
	if (disposition == MT6797_WIFI_AIS_EVENT_COMPLETE) {
		ret = mt6797_wifi_ais_channel_event_parse(&event, &grant);
		if (ret)
			return ret;
		/* Conservatively count from request submission, not delayed IRQ delivery. */
		ais->channel_deadline = ktime_add_ms(ais->channel_requested_at,
			grant.grant_interval_ms - 10); /* Firmware join guard. */
	}
	return disposition == MT6797_WIFI_AIS_EVENT_IGNORED ? -EAGAIN : 0;
}

static int mt6797_wifi_ais_hif_wait_channel(void *context,
					    unsigned int timeout_ms)
{
	struct mt6797_wifi_ais_hif *ais = context;
	struct mt6797_wifi_hif_event_copy copy;
	ktime_t deadline;
	unsigned long remaining;
	int ret;

	deadline = ktime_add_ms(ais->backend.now(ais->backend.context),
				timeout_ms);
	for (;;) {
		remaining = mt6797_wifi_ais_hif_remaining(ais, deadline);
		if (remaining)
			ais->backend.wait(ais->backend.context,
					  &ais->irq->wait_event.completion,
					  remaining);
		/* A copied owned event wins observation of the deadline. */
		ret = mt6797_wifi_hif_wait_event_take(&ais->irq->wait_event,
						       &copy);
		if (!ret) {
			ret = mt6797_wifi_ais_hif_deliver_copy(ais, &copy);
			if (ret != -EAGAIN)
				break;
		} else if (ret != -EAGAIN) {
			break;
		}
		ret = mt6797_wifi_ais_hif_status(ais);
		if (ret)
			break;
		if (!mt6797_wifi_ais_hif_remaining(ais, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
	}
	mt6797_wifi_hif_wait_event_disarm(&ais->irq->wait_event);
	return ret;
}

static void mt6797_wifi_ais_hif_fence(void *context, int reason)
{
	struct mt6797_wifi_ais_hif *ais = context;

	mt6797_wifi_hif_wait_event_disarm(&ais->irq->wait_event);
	ais->backend.terminal(ais->backend.context, reason);
}

static int mt6797_wifi_ais_hif_auth_claim_terminal(
	struct mt6797_wifi_ais_hif_auth *auth, int reason)
{
	reason = reason < 0 ? reason : -EIO;
	if (!auth->terminal_reason) {
		auth->terminal_reason = reason;
		mt6797_wifi_ais_mgmt_pid_mark_terminal(&auth->pid_owner);
	}
	complete(&auth->completion);
	return auth->terminal_reason;
}

static bool mt6797_wifi_ais_hif_auth_finished(
	const struct mt6797_wifi_ais_hif_auth *auth)
{
	return auth->tx_done && (auth->tx.status || auth->response_received);
}

static int mt6797_wifi_ais_hif_auth_event(
	void *context, const struct mt6797_wifi_event_view *event)
{
	struct mt6797_wifi_ais_hif *ais = context;
	struct mt6797_wifi_ais_hif_auth *auth = &ais->auth;
	struct mt6797_wifi_ais_mgmt_tx_status status;
	enum mt6797_wifi_ais_event_disposition disposition;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&auth->lock, flags);
	if (!auth->armed || !auth->dirty) {
		ret = 0;
		goto out;
	}
	ret = mt6797_wifi_ais_mgmt_tx_event_match(&auth->pid_owner,
		&auth->pid_lease, event, &disposition, &status);
	if (ret) {
		ret = mt6797_wifi_ais_hif_auth_claim_terminal(auth, ret);
		goto out;
	}
	if (disposition == MT6797_WIFI_AIS_EVENT_IGNORED)
		goto out;
	if (disposition == MT6797_WIFI_AIS_EVENT_TERMINAL) {
		ret = mt6797_wifi_ais_hif_auth_claim_terminal(auth, -EPROTO);
		goto out;
	}
	if (auth->tx_done) {
		ret = mt6797_wifi_ais_hif_auth_claim_terminal(auth, -EOVERFLOW);
		goto out;
	}
	auth->tx = status;
	auth->tx_done = true;
	if (mt6797_wifi_ais_hif_auth_finished(auth))
		complete(&auth->completion);
out:
	spin_unlock_irqrestore(&auth->lock, flags);
	return ret;
}

static int mt6797_wifi_ais_hif_auth_frame(
	void *context, const struct mt6797_wifi_rx_view *frame)
{
	struct mt6797_wifi_ais_hif *ais = context;
	struct mt6797_wifi_ais_hif_auth *auth = &ais->auth;
	struct mt6797_wifi_ais_auth_response response;
	enum mt6797_wifi_ais_event_disposition disposition;
	unsigned long flags;
	int ret;

	if (frame->kind != MT6797_WIFI_RX_IEEE80211)
		return 0;
	spin_lock_irqsave(&auth->lock, flags);
	if (!auth->armed || !auth->dirty) {
		ret = 0;
		goto out;
	}
	ret = mt6797_wifi_ais_open_auth_response_match(&auth->pending,
		frame->payload, frame->payload_length, &disposition, &response);
	if (ret) {
		ret = mt6797_wifi_ais_hif_auth_claim_terminal(auth, ret);
		goto out;
	}
	if (disposition == MT6797_WIFI_AIS_EVENT_IGNORED)
		goto out;
	if (frame->header_translated ||
	    frame->header_length != MT6797_WIFI_AIS_AUTH_HEADER_LENGTH ||
	    frame->wlan_index != auth->pid_lease.wlan_index) {
		ret = mt6797_wifi_ais_hif_auth_claim_terminal(auth, -EPROTO);
		goto out;
	}
	if (auth->response_received) {
		ret = mt6797_wifi_ais_hif_auth_claim_terminal(auth, -EOVERFLOW);
		goto out;
	}
	auth->response = response;
	auth->response_received = true;
	if (mt6797_wifi_ais_hif_auth_finished(auth))
		complete(&auth->completion);
out:
	spin_unlock_irqrestore(&auth->lock, flags);
	return ret;
}

static void mt6797_wifi_ais_hif_auth_abort(void *context, int reason)
{
	struct mt6797_wifi_ais_hif *ais = context;
	struct mt6797_wifi_ais_hif_auth *auth = &ais->auth;
	unsigned long flags;

	spin_lock_irqsave(&auth->lock, flags);
	if (auth->armed)
		mt6797_wifi_ais_hif_auth_claim_terminal(auth, reason);
	spin_unlock_irqrestore(&auth->lock, flags);
}

static int mt6797_wifi_ais_hif_auth_prepare(
	struct mt6797_wifi_ais_hif *ais,
	const struct mt6797_wifi_ais_hif_auth_request *request,
	u8 packet[MT6797_WIFI_AIS_AUTH_PACKET_LENGTH], size_t *packet_length)
{
	struct mt6797_wifi_ais_hif_auth *auth = &ais->auth;
	struct mt6797_wifi_ais_mgmt_tx_params tx = {
		.sequence_number = request->sequence_number,
		.fixed_rate = request->fixed_rate,
	};
	u8 frame[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];
	int ret;

	ret = mt6797_wifi_ais_mgmt_pid_open(&auth->pid_owner,
		&ais->runtime.bss, &ais->runtime.sta);
	if (ret)
		return ret;
	ret = mt6797_wifi_ais_open_auth_request_build(&ais->runtime.bss,
		&ais->runtime.sta, frame, sizeof(frame), &auth->pending);
	if (ret)
		goto close;
	ret = mt6797_wifi_ais_mgmt_pid_acquire(&auth->pid_owner,
		&auth->pid_lease);
	if (ret)
		goto close;
	ret = mt6797_wifi_ais_mgmt_tx_build(&auth->pid_owner,
		&auth->pid_lease, &ais->runtime.bss, &ais->runtime.sta, &tx,
		frame, sizeof(frame), packet, MT6797_WIFI_AIS_AUTH_PACKET_LENGTH,
		packet_length);
	if (!ret)
		return 0;
	mt6797_wifi_ais_mgmt_pid_release(&auth->pid_owner,
		&auth->pid_lease);
close:
	mt6797_wifi_ais_mgmt_pid_close(&auth->pid_owner);
	memset(&auth->pid_lease, 0, sizeof(auth->pid_lease));
	memset(&auth->pending, 0, sizeof(auth->pending));
	return ret;
}

static int mt6797_wifi_ais_hif_auth_wait(
	struct mt6797_wifi_ais_hif *ais, unsigned int timeout_ms)
{
	struct mt6797_wifi_ais_hif_auth *auth = &ais->auth;
	ktime_t deadline = ktime_add_ms(
		ais->backend.now(ais->backend.context), timeout_ms);
	unsigned long flags;
	unsigned long remaining;
	int status;

	for (;;) {
		remaining = mt6797_wifi_ais_hif_remaining(ais, deadline);
		if (remaining)
			ais->backend.wait(ais->backend.context,
				&auth->completion, remaining);
		spin_lock_irqsave(&auth->lock, flags);
		if (auth->terminal_reason) {
			status = auth->terminal_reason;
			spin_unlock_irqrestore(&auth->lock, flags);
			return status;
		}
		if (mt6797_wifi_ais_hif_auth_finished(auth)) {
			spin_unlock_irqrestore(&auth->lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&auth->lock, flags);
		status = mt6797_wifi_ais_hif_status(ais);
		if (status)
			return status;
		if (mt6797_wifi_ais_hif_remaining(ais, deadline))
			continue;

		/* Completion and timeout choose one winner under the same lock. */
		spin_lock_irqsave(&auth->lock, flags);
		if (auth->terminal_reason)
			status = auth->terminal_reason;
		else if (mt6797_wifi_ais_hif_auth_finished(auth))
			status = 0;
		else
			status = mt6797_wifi_ais_hif_auth_claim_terminal(auth, -ETIMEDOUT);
		spin_unlock_irqrestore(&auth->lock, flags);
		return status;
	}
}

int mt6797_wifi_ais_hif_init(
	struct mt6797_wifi_ais_hif *ais, struct mt6797_wifi_hif_irq *irq,
	struct mt6797_wifi_runtime_config *config,
	const struct mt6797_wifi_capability *capability,
	unsigned int first_command_sequence,
	unsigned int first_channel_token,
	const struct mt6797_wifi_ais_hif_backend *backend)
{
	if (!ais || !irq || !config || !capability)
		return -EINVAL;
	if (backend && !mt6797_wifi_ais_hif_backend_valid(backend))
		return -EINVAL;
	memset(ais, 0, sizeof(*ais));
	ais->irq = irq;
	ais->config = config;
	ais->capability = capability;
	if (backend) {
		ais->backend = *backend;
	} else {
		ais->backend = (struct mt6797_wifi_ais_hif_backend) {
			.send = mt6797_wifi_ais_hif_real_send,
			.now = mt6797_wifi_ais_hif_real_now,
			.wait = mt6797_wifi_ais_hif_real_wait,
			.terminal = mt6797_wifi_ais_hif_real_terminal,
			.context = ais,
		};
	}
	mt6797_wifi_ais_runtime_init(&ais->runtime, first_command_sequence,
				     first_channel_token);
	spin_lock_init(&ais->auth.lock);
	init_completion(&ais->auth.completion);
	mt6797_wifi_ais_mgmt_pid_init(&ais->auth.pid_owner);
	ais->initialized = true;
	return 0;
}

int mt6797_wifi_ais_hif_start(
	struct mt6797_wifi_ais_hif *ais,
	const struct mt6797_wifi_ais_hif_request *request)
{
	struct mt6797_wifi_ais_runtime_params params = {};
	struct mt6797_wifi_ais_runtime_ops ops;
	int ret;

	if (!ais || !ais->initialized || !request || !request->target_owner ||
	    !request->target || !request->response_timeout_ms ||
	    request->response_timeout_ms > MT6797_WIFI_AIS_JOIN_MAX_INTERVAL_MS)
		return -EINVAL;
	mutex_lock(&ais->irq->lifecycle_lock);
	ret = mt6797_wifi_ais_hif_status(ais);
	if (ret)
		goto out;
	ret = mt6797_wifi_scan_target_validate(request->target_owner,
					       request->now_ms,
					       request->target);
	if (ret)
		goto out;
	if (request->target->band != MT6797_WIFI_RX_BAND_2GHZ &&
	    request->target->band != MT6797_WIFI_RX_BAND_5GHZ) {
		ret = -EINVAL;
		goto out;
	}

	params.channel.band = request->target->band;
	params.channel.primary = request->target->channel;
	params.bss_index = request->bss_index;
	params.own_mac_index = request->own_mac_index;
	params.response_timeout_ms = request->response_timeout_ms;
	memcpy(params.own_addr, ais->capability->mac_address, ETH_ALEN);
	memcpy(params.peer_addr, request->target->bssid, ETH_ALEN);
	ais->target_generation = request->target->generation;
	ais->target_cookie = request->target->cookie;
	mt6797_wifi_ais_hif_init_runtime_ops(ais, &ops);
	ret = mt6797_wifi_ais_runtime_start(&ais->runtime, &ops, &params);
	mt6797_wifi_hif_wait_event_disarm(&ais->irq->wait_event);
out:
	mutex_unlock(&ais->irq->lifecycle_lock);
	return ret;
}

static bool mt6797_ais_join_finished(const struct mt6797_wifi_ais_hif_join *join)
{
	return join->error || (join->station_phase ? join->activated :
		(join->tx_done && (join->tx_status || join->response_length)));
}

static int mt6797_ais_join_event(void *context,
	const struct mt6797_wifi_event_view *event)
{
	struct mt6797_wifi_ais_hif *ais = context;
	struct mt6797_wifi_ais_hif_join *join = &ais->join;
	struct mt6797_wifi_ais_mgmt_tx_status status;
	enum mt6797_wifi_ais_event_disposition disposition;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&join->lock, flags);
	if (!join->armed)
		goto out;
	if (join->station_phase) {
		ret = mt6797_wifi_ais_sta_event_match(&join->station, event, &disposition);
		if (!ret && disposition == MT6797_WIFI_AIS_EVENT_COMPLETE)
			join->activated = true;
	} else {
		ret = mt6797_wifi_ais_mgmt_tx_event_match(&ais->auth.pid_owner,
			&join->lease, event, &disposition, &status);
		if (!ret && disposition == MT6797_WIFI_AIS_EVENT_COMPLETE && !join->tx_done) {
			join->tx_done = true;
			join->tx_status = status.status;
		}
	}
	if (ret && !join->error)
		join->error = ret;
	if (mt6797_ais_join_finished(join))
		complete(&join->completion);
out:
	spin_unlock_irqrestore(&join->lock, flags);
	return ret;
}

static int mt6797_ais_join_frame(void *context,
	const struct mt6797_wifi_rx_view *frame)
{
	struct mt6797_wifi_ais_hif *ais = context;
	struct mt6797_wifi_ais_hif_join *join = &ais->join;
	struct mt6797_wifi_ais_assoc_response response;
	enum mt6797_wifi_ais_event_disposition disposition;
	unsigned long flags;
	int ret = 0;

	if (frame->kind != MT6797_WIFI_RX_IEEE80211)
		return 0;
	spin_lock_irqsave(&join->lock, flags);
	if (!join->armed || join->station_phase || join->response_length)
		goto out;
	ret = mt6797_wifi_ais_assoc_response_match(&join->association,
		frame->payload, frame->payload_length, &disposition, &response);
	if (!ret && disposition == MT6797_WIFI_AIS_EVENT_COMPLETE) {
		if (frame->header_translated || frame->header_length != 24 ||
		    frame->wlan_index != join->lease.wlan_index) {
			ret = -EPROTO;
		} else {
			memcpy(join->response, frame->payload, frame->payload_length);
			join->response_length = frame->payload_length;
			join->status_code = response.status_code;
		}
	}
	if (ret && !join->error)
		join->error = ret;
	if (mt6797_ais_join_finished(join))
		complete(&join->completion);
out:
	spin_unlock_irqrestore(&join->lock, flags);
	return ret;
}

static void mt6797_ais_join_abort(void *context, int reason)
{
	struct mt6797_wifi_ais_hif *ais = context;
	unsigned long flags;

	spin_lock_irqsave(&ais->join.lock, flags);
	if (!ais->join.error)
		ais->join.error = reason < 0 ? reason : -EIO;
	complete(&ais->join.completion);
	spin_unlock_irqrestore(&ais->join.lock, flags);
}

static int mt6797_ais_join_wait(struct mt6797_wifi_ais_hif *ais, ktime_t deadline)
{
	struct mt6797_wifi_ais_hif_join *join = &ais->join;
	unsigned long flags, remaining;
	int ret;

	for (;;) {
		remaining = mt6797_wifi_ais_hif_remaining(ais, deadline);
		spin_lock_irqsave(&join->lock, flags);
		if (mt6797_ais_join_finished(join)) {
			ret = join->error;
			spin_unlock_irqrestore(&join->lock, flags);
			return ret;
		}
		if (!remaining) {
			join->error = -ETIMEDOUT;
			spin_unlock_irqrestore(&join->lock, flags);
			return -ETIMEDOUT;
		}
		spin_unlock_irqrestore(&join->lock, flags);
		ret = mt6797_wifi_ais_hif_status(ais);
		if (ret)
			return ret;
		ais->backend.wait(ais->backend.context, &join->completion, remaining);
	}
}

static int mt6797_ais_join_pre_send(struct mt6797_wifi_ais_hif *ais,
	ktime_t deadline)
{
	unsigned long flags;
	int ret;

	ret = mt6797_wifi_ais_hif_status(ais);
	if (ret)
		return ret;
	spin_lock_irqsave(&ais->join.lock, flags);
	ret = ais->join.error;
	spin_unlock_irqrestore(&ais->join.lock, flags);
	if (ret)
		return ret;
	return mt6797_wifi_ais_hif_remaining(ais, deadline) ? 0 : -ETIMEDOUT;
}

int mt6797_wifi_ais_hif_associate(
	struct mt6797_wifi_ais_hif *ais,
	const struct mt6797_wifi_ais_hif_join_request *request,
	struct mt6797_wifi_ais_join_params *negotiated)
{
	struct mt6797_wifi_hif_runtime_sink sink = {
		.event = mt6797_ais_join_event, .frame = mt6797_ais_join_frame,
		.abort = mt6797_ais_join_abort, .context = ais,
	};
	struct mt6797_wifi_ais_mgmt_tx_params tx;
	struct mt6797_wifi_ais_assoc_response response;
	struct mt6797_wifi_ais_join_params params;
	struct mt6797_wifi_ais_hif_join *join;
	enum mt6797_wifi_ais_event_disposition disposition;
	u8 *packet, *frame;
	unsigned long flags;
	unsigned int sequence;
	size_t frame_length, packet_length, offset;
	ktime_t deadline;
	bool attached = false, pid_open = false, held = false, uncertain = false;
	int ret, detach;

	if (!ais || !ais->initialized || !request || !negotiated ||
	    !request->timeout_ms || request->timeout_ms > MT6797_WIFI_AIS_JOIN_MAX_INTERVAL_MS ||
	    request->association.listen_interval != request->negotiated.listen_interval)
		return -EINVAL;
	packet = kmalloc(2 * MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;
	frame = packet + MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH;
	join = &ais->join;
	mutex_lock(&ais->irq->lifecycle_lock);
	ret = mt6797_wifi_ais_hif_status(ais);
	if (ret)
		goto out;
	if (ais->runtime.state != MT6797_WIFI_AIS_RUNTIME_STA_STATE1 ||
	    !ais->auth.tx_done || ais->auth.tx.status ||
	    !ais->auth.response_received || ais->auth.response.status_code) {
		ret = -ENOTCONN;
		goto out;
	}
	if (join->attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (request->negotiated.channel.band != ais->runtime.channel_pending.band ||
	    request->negotiated.channel.primary != ais->runtime.channel_pending.primary) {
		ret = -EINVAL;
		goto out;
	}
	deadline = ktime_add_ms(ais->backend.now(ais->backend.context), request->timeout_ms);
	if (ktime_before(ais->channel_deadline, deadline))
		deadline = ais->channel_deadline;
	if (!mt6797_wifi_ais_hif_remaining(ais, deadline)) {
		ret = -ETIMEDOUT;
		goto out;
	}
	sequence = ais->runtime.next_command_sequence;
	if (sequence > 254) {
		ret = -EOVERFLOW;
		goto out;
	}
	/* Validate all caller policy before emitting the association frame. */
	params = request->negotiated;
	params.association_id = 1; /* Validation only; replaced by the AP's response. */
	ret = mt6797_wifi_ais_join_bss_build(&ais->runtime.bss, &ais->runtime.sta,
		&params, sequence, packet, MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH);
	if (ret)
		goto out;
	memset(join, 0, sizeof(*join));
	spin_lock_init(&join->lock);
	init_completion(&join->completion);
	ret = mt6797_wifi_ais_assoc_request_build(&ais->runtime.bss, &ais->runtime.sta,
		&request->association, frame, MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH,
		&frame_length, &join->association);
	if (ret)
		goto out;
	/* The validated IE stream must describe the same SSID as the BSS command. */
	for (offset = MT6797_WIFI_AIS_ASSOC_REQUEST_FIXED_LENGTH; offset < frame_length;
	     offset += frame[offset + 1] + 2) {
		if (frame[offset] != 0)
			continue;
		if (frame[offset + 1] != params.ssid_length ||
		    memcmp(frame + offset + 2, params.ssid, params.ssid_length)) {
			ret = -EINVAL;
			goto out;
		}
		break;
	}
	ret = mt6797_wifi_ais_mgmt_pid_open(&ais->auth.pid_owner,
		&ais->runtime.bss, &ais->runtime.sta);
	if (ret)
		goto out;
	pid_open = true;
	ret = mt6797_wifi_ais_mgmt_pid_acquire(&ais->auth.pid_owner, &join->lease);
	if (ret)
		goto out;
	held = true;
	tx.sequence_number = request->sequence_number;
	tx.fixed_rate = params.default_rate;
	ret = mt6797_wifi_ais_mgmt_tx_build(&ais->auth.pid_owner, &join->lease,
		&ais->runtime.bss, &ais->runtime.sta, &tx, frame, frame_length,
		packet, MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH, &packet_length);
	if (ret)
		goto out;
	ret = mt6797_wifi_hif_runtime_sink_attach(ais->irq, &sink);
	if (ret)
		goto out;
	attached = true;
	join->attempted = true;
	spin_lock_irqsave(&join->lock, flags);
	join->armed = true;
	spin_unlock_irqrestore(&join->lock, flags);
	ret = mt6797_ais_join_pre_send(ais, deadline);
	if (ret)
		goto out;
	uncertain = true;
	ret = ais->backend.send(ais->backend.context, MT6797_WIFI_MCR_WTDR1, packet, packet_length);
	if (!ret)
		ret = mt6797_ais_join_wait(ais, deadline);
	if (ret)
		goto out;
	uncertain = false;
	if (join->tx_status || join->status_code) {
		ret = join->tx_status ? -EREMOTEIO : -EACCES;
		goto out;
	}
	ret = mt6797_wifi_ais_assoc_response_match(&join->association,
		join->response, join->response_length, &disposition, &response);
	if (ret)
		goto out;
	params.association_id = response.association_id;
	ret = mt6797_wifi_ais_join_rates_parse(response.ies, response.ies_length,
		response.capability, &params);
	if (ret)
		goto out;
	ret = mt6797_wifi_ais_join_bss_build(&ais->runtime.bss, &ais->runtime.sta,
		&params, sequence, packet, MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH);
	if (ret)
		goto out;
	/* Reserve both sequence numbers before firmware observes either command. */
	ais->runtime.next_command_sequence += 2;
	ret = mt6797_ais_join_pre_send(ais, deadline);
	if (ret)
		goto out;
	uncertain = true;
	ret = ais->backend.send(ais->backend.context, MT6797_WIFI_MCR_WTDR1,
		packet, MT6797_WIFI_AIS_BSS_INFO_LENGTH);
	if (ret)
		goto out;
	ret = mt6797_wifi_ais_join_sta_build(&ais->runtime.bss, &ais->runtime.sta,
		&params, sequence + 1, packet, MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH,
		&join->station);
	if (ret)
		goto out;
	spin_lock_irqsave(&join->lock, flags);
	join->station_phase = true;
	reinit_completion(&join->completion);
	spin_unlock_irqrestore(&join->lock, flags);
	ret = mt6797_ais_join_pre_send(ais, deadline);
	if (ret)
		goto out;
	ret = ais->backend.send(ais->backend.context, MT6797_WIFI_MCR_WTDR1,
		packet, MT6797_WIFI_AIS_STA_UPDATE_LENGTH);
	if (!ret)
		ret = mt6797_ais_join_wait(ais, deadline);
	if (!ret) {
		uncertain = false;
		ais->runtime.state = MT6797_WIFI_AIS_RUNTIME_STA_STATE3;
		*negotiated = params;
	}
out:
	if (attached) {
		detach = mt6797_wifi_hif_runtime_sink_detach(ais->irq, ais);
		if (detach) {
			ret = ret ?: detach;
			uncertain = true;
		}
		join->armed = false; /* Detach synchronizes the IRQ callbacks. */
		if (join->error) {
			ret = join->error;
			uncertain = true;
		}
	}
	if (uncertain) {
		mt6797_wifi_ais_mgmt_pid_mark_terminal(&ais->auth.pid_owner);
		ais->backend.terminal(ais->backend.context, ret ?: -EIO);
	} else {
		if (held) {
			detach = mt6797_wifi_ais_mgmt_pid_release(&ais->auth.pid_owner, &join->lease);
			ret = ret ?: detach;
			if (detach)
				ais->backend.terminal(ais->backend.context, detach);
		}
		if (pid_open) {
			detach = mt6797_wifi_ais_mgmt_pid_close(&ais->auth.pid_owner);
			ret = ret ?: detach;
			if (detach)
				ais->backend.terminal(ais->backend.context, detach);
		}
	}
	mutex_unlock(&ais->irq->lifecycle_lock);
	kfree(packet);
	return ret;
}

int mt6797_wifi_ais_hif_stop(struct mt6797_wifi_ais_hif *ais)
{
	struct mt6797_wifi_ais_runtime_ops ops;
	int ret;

	if (!ais || !ais->initialized)
		return -EINVAL;
	mutex_lock(&ais->irq->lifecycle_lock);
	mt6797_wifi_ais_hif_init_runtime_ops(ais, &ops);
	ret = mt6797_wifi_ais_runtime_stop(&ais->runtime, &ops);
	mt6797_wifi_hif_wait_event_disarm(&ais->irq->wait_event);
	mutex_unlock(&ais->irq->lifecycle_lock);
	return ret;
}

int mt6797_wifi_ais_hif_open_auth(
	struct mt6797_wifi_ais_hif *ais,
	const struct mt6797_wifi_ais_hif_auth_request *request,
	struct mt6797_wifi_ais_hif_auth_result *result)
{
	struct mt6797_wifi_hif_runtime_sink sink = {
		.event = mt6797_wifi_ais_hif_auth_event,
		.frame = mt6797_wifi_ais_hif_auth_frame,
		.abort = mt6797_wifi_ais_hif_auth_abort,
		.context = ais,
	};
	struct mt6797_wifi_ais_hif_auth *auth;
	u8 packet[MT6797_WIFI_AIS_AUTH_PACKET_LENGTH];
	unsigned long flags;
	size_t packet_length;
	int detach;
	int ret;

	if (result)
		memset(result, 0, sizeof(*result));
	if (!ais || !ais->initialized || !request || !result ||
	    !request->response_timeout_ms ||
	    request->response_timeout_ms > MT6797_WIFI_AIS_JOIN_MAX_INTERVAL_MS)
		return -EINVAL;
	auth = &ais->auth;
	mutex_lock(&ais->irq->lifecycle_lock);
	ret = mt6797_wifi_ais_hif_status(ais);
	if (ret)
		goto out;
	if (ais->runtime.state != MT6797_WIFI_AIS_RUNTIME_STA_STATE1) {
		ret = -ENOTCONN;
		goto out;
	}
	if (auth->attempted) {
		ret = -EALREADY;
		goto out;
	}
	ret = mt6797_wifi_ais_hif_auth_prepare(ais, request, packet,
		&packet_length);
	if (ret)
		goto out;

	reinit_completion(&auth->completion);
	auth->terminal_reason = 0;
	auth->attempted = true;
	auth->armed = true;
	auth->dirty = false;
	auth->sent = false;
	auth->tx_done = false;
	auth->response_received = false;
	memset(&auth->tx, 0, sizeof(auth->tx));
	memset(&auth->response, 0, sizeof(auth->response));
	ret = mt6797_wifi_hif_runtime_sink_attach(ais->irq, &sink);
	if (ret) {
		auth->attempted = false;
		auth->armed = false;
		mt6797_wifi_ais_mgmt_pid_release(&auth->pid_owner,
			&auth->pid_lease);
		mt6797_wifi_ais_mgmt_pid_close(&auth->pid_owner);
		memset(&auth->pid_lease, 0, sizeof(auth->pid_lease));
		memset(&auth->pending, 0, sizeof(auth->pending));
		goto out;
	}

	/* Publish ambiguity before CMD53 can expose the management frame. */
	spin_lock_irqsave(&auth->lock, flags);
	auth->dirty = true;
	spin_unlock_irqrestore(&auth->lock, flags);
	ret = ais->backend.send(ais->backend.context, MT6797_WIFI_MCR_WTDR1,
		packet, packet_length);
	spin_lock_irqsave(&auth->lock, flags);
	if (ret)
		ret = mt6797_wifi_ais_hif_auth_claim_terminal(auth, ret);
	else
		auth->sent = true;
	spin_unlock_irqrestore(&auth->lock, flags);
	if (!ret)
		ret = mt6797_wifi_ais_hif_auth_wait(ais,
			request->response_timeout_ms);
	if (ret) {
		spin_lock_irqsave(&auth->lock, flags);
		if (auth->terminal_reason)
			ret = auth->terminal_reason;
		else
			ret = mt6797_wifi_ais_hif_auth_claim_terminal(auth, ret);
		spin_unlock_irqrestore(&auth->lock, flags);
	}

	detach = mt6797_wifi_hif_runtime_sink_detach(ais->irq, ais);
	if (detach && !ret) {
		spin_lock_irqsave(&auth->lock, flags);
		ret = mt6797_wifi_ais_hif_auth_claim_terminal(auth, detach);
		spin_unlock_irqrestore(&auth->lock, flags);
	}
	/* Detach synchronizes any in-flight IRQ callback before disarm/release. */
	spin_lock_irqsave(&auth->lock, flags);
	auth->armed = false;
	result->tx = auth->tx;
	result->response = auth->response;
	result->tx_done = auth->tx_done;
	result->response_received = auth->response_received;
	spin_unlock_irqrestore(&auth->lock, flags);
	if (ret) {
		ais->backend.terminal(ais->backend.context, ret);
		goto out;
	}

	ret = mt6797_wifi_ais_mgmt_pid_release(&auth->pid_owner,
		&auth->pid_lease);
	if (!ret)
		ret = mt6797_wifi_ais_mgmt_pid_close(&auth->pid_owner);
	if (ret) {
		spin_lock_irqsave(&auth->lock, flags);
		ret = mt6797_wifi_ais_hif_auth_claim_terminal(auth, ret);
		spin_unlock_irqrestore(&auth->lock, flags);
		ais->backend.terminal(ais->backend.context, ret);
		goto out;
	}
	if (result->tx.status)
		ret = -EREMOTEIO;
	else if (result->response.status_code)
		ret = -EACCES;
out:
	mutex_unlock(&ais->irq->lifecycle_lock);
	return ret;
}
