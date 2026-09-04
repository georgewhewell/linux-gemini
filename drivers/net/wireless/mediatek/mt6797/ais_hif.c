// SPDX-License-Identifier: GPL-2.0-only
/* Serialized bridge from the persistent HIF event pump to pure AIS state. */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include "ais_hif.h"
#include "boot.h"
#include "capability.h"
#include "hif.h"
#include "hif_regs.h"
#include "hif_runtime.h"
#include "runtime_config.h"
#include "scan_target.h"

#define MT6797_WIFI_AIS_CHANNEL_EVENT_ID	0x10U

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

	return mt6797_wifi_ahb_cmd53_write(ais->irq->hif, address, packet,
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
	int ret;

	if (copy->error)
		return copy->error;
	mt6797_wifi_ais_hif_init_runtime_ops(ais, &ops);
	ret = mt6797_wifi_ais_runtime_event(&ais->runtime, &ops, &event,
					    &disposition);
	if (ret)
		return ret;
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
