// SPDX-License-Identifier: GPL-2.0-only
/* Caller-driven transactional ownership for the MT6797 GEN3 AIS primitives. */

#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "ais_runtime.h"

#define MT6797_WIFI_AIS_RUNTIME_SEQUENCE_COUNT \
	MT6797_WIFI_AIS_RUNTIME_COMMAND_COUNT

static const size_t mt6797_wifi_ais_runtime_packet_lengths[] = {
	[MT6797_WIFI_AIS_RUNTIME_BSS_ACTIVATE] =
		MT6797_WIFI_AIS_BSS_COMMAND_LENGTH,
	[MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST] =
		MT6797_WIFI_AIS_CHANNEL_COMMAND_LENGTH,
	[MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE] =
		MT6797_WIFI_AIS_STA_UPDATE_LENGTH,
	[MT6797_WIFI_AIS_RUNTIME_STA_REMOVE] =
		MT6797_WIFI_AIS_STA_REMOVE_LENGTH,
	[MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT] =
		MT6797_WIFI_AIS_CHANNEL_COMMAND_LENGTH,
	[MT6797_WIFI_AIS_RUNTIME_BSS_DEACTIVATE] =
		MT6797_WIFI_AIS_BSS_COMMAND_LENGTH,
};

static bool mt6797_wifi_ais_runtime_ops_valid(
	const struct mt6797_wifi_ais_runtime_ops *ops)
{
	return ops && ops->precheck && ops->send && ops->wait_channel &&
		ops->fence;
}

static void mt6797_wifi_ais_runtime_claim_terminal(
	struct mt6797_wifi_ais_runtime *runtime, int reason)
{
	int ret = reason < 0 ? reason : -EIO;

	if (runtime->state != MT6797_WIFI_AIS_RUNTIME_TERMINAL) {
		runtime->state = MT6797_WIFI_AIS_RUNTIME_TERMINAL;
		runtime->terminal_reason = ret;
		if (runtime->resources.state == MT6797_WIFI_AIS_RESOURCES_OPEN)
			mt6797_wifi_ais_resources_mark_terminal(&runtime->resources);
	}
}

static int mt6797_wifi_ais_runtime_terminal(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_ops *ops, int reason)
{
	mt6797_wifi_ais_runtime_claim_terminal(runtime, reason);
	if (!runtime->fenced) {
		runtime->fenced = true;
		ops->fence(ops->context, runtime->terminal_reason);
	}
	return runtime->terminal_reason;
}

static int mt6797_wifi_ais_runtime_send(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_ops *ops,
	enum mt6797_wifi_ais_runtime_command command)
{
	int ret;

	/* Publish ambiguity before the transport can observe the packet. */
	runtime->dirty[command] = true;
	ret = ops->send(ops->context, runtime, command,
			runtime->packet[command].data,
			runtime->packet[command].length);
	if (ret)
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
	runtime->sent[command] = true;
	return 0;
}

static int mt6797_wifi_ais_runtime_unwind_clean(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_ops *ops, int reason)
{
	int ret;

	if (runtime->sta.generation) {
		ret = mt6797_wifi_ais_sta_release(&runtime->resources,
						 &runtime->sta);
		if (ret)
			return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
		memset(&runtime->sta, 0, sizeof(runtime->sta));
	}
	if (runtime->bss.generation) {
		ret = mt6797_wifi_ais_bss_release(&runtime->resources,
						 &runtime->bss);
		if (ret)
			return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
		memset(&runtime->bss, 0, sizeof(runtime->bss));
	}
	ret = mt6797_wifi_ais_resources_close(&runtime->resources);
	if (ret)
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
	runtime->owner_context = NULL;
	return reason;
}

static int mt6797_wifi_ais_runtime_build(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_params *params)
{
	struct mt6797_wifi_ais_runtime_packet *packet = runtime->packet;
	u8 sequence = runtime->next_command_sequence;
	u8 token = runtime->next_channel_token;
	int ret;

	packet[MT6797_WIFI_AIS_RUNTIME_BSS_ACTIVATE].length =
		mt6797_wifi_ais_runtime_packet_lengths[
			MT6797_WIFI_AIS_RUNTIME_BSS_ACTIVATE];
	ret = mt6797_wifi_ais_bss_activate_build(&runtime->bss, sequence++,
		packet[MT6797_WIFI_AIS_RUNTIME_BSS_ACTIVATE].data,
		packet[MT6797_WIFI_AIS_RUNTIME_BSS_ACTIVATE].length);
	if (ret)
		return ret;
	packet[MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST].length =
		mt6797_wifi_ais_runtime_packet_lengths[
			MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST];
	ret = mt6797_wifi_ais_channel_request_build(&runtime->bss,
		&params->channel, token, sequence++,
		packet[MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST].data,
		packet[MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST].length,
		&runtime->channel_pending);
	if (ret)
		return ret;
	packet[MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE].length =
		mt6797_wifi_ais_runtime_packet_lengths[
			MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE];
	ret = mt6797_wifi_ais_sta_state1_build(&runtime->bss, &runtime->sta,
		sequence++,
		packet[MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE].data,
		packet[MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE].length);
	if (ret)
		return ret;
	packet[MT6797_WIFI_AIS_RUNTIME_STA_REMOVE].length =
		mt6797_wifi_ais_runtime_packet_lengths[
			MT6797_WIFI_AIS_RUNTIME_STA_REMOVE];
	ret = mt6797_wifi_ais_sta_remove_build(&runtime->bss, &runtime->sta,
		sequence++, packet[MT6797_WIFI_AIS_RUNTIME_STA_REMOVE].data,
		packet[MT6797_WIFI_AIS_RUNTIME_STA_REMOVE].length);
	if (ret)
		return ret;
	packet[MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT].length =
		mt6797_wifi_ais_runtime_packet_lengths[
			MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT];
	ret = mt6797_wifi_ais_channel_abort_build(&runtime->bss, token,
		sequence++, packet[MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT].data,
		packet[MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT].length);
	if (ret)
		return ret;
	packet[MT6797_WIFI_AIS_RUNTIME_BSS_DEACTIVATE].length =
		mt6797_wifi_ais_runtime_packet_lengths[
			MT6797_WIFI_AIS_RUNTIME_BSS_DEACTIVATE];
	ret = mt6797_wifi_ais_bss_deactivate_build(&runtime->bss, sequence++,
		packet[MT6797_WIFI_AIS_RUNTIME_BSS_DEACTIVATE].data,
		packet[MT6797_WIFI_AIS_RUNTIME_BSS_DEACTIVATE].length);
	if (ret)
		return ret;

	runtime->next_command_sequence +=
		MT6797_WIFI_AIS_RUNTIME_SEQUENCE_COUNT;
	runtime->next_channel_token++;
	return 0;
}

void mt6797_wifi_ais_runtime_init(struct mt6797_wifi_ais_runtime *runtime,
				  unsigned int first_command_sequence,
				  unsigned int first_channel_token)
{
	if (!runtime)
		return;
	memset(runtime, 0, sizeof(*runtime));
	mt6797_wifi_ais_resources_init(&runtime->resources);
	runtime->next_command_sequence = first_command_sequence;
	runtime->next_channel_token = first_channel_token;
}

int mt6797_wifi_ais_runtime_event(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_ops *ops,
	const struct mt6797_wifi_event_view *event,
	enum mt6797_wifi_ais_event_disposition *disposition)
{
	int ret;

	if (!runtime || !mt6797_wifi_ais_runtime_ops_valid(ops) || !event ||
	    !disposition)
		return -EINVAL;
	*disposition = MT6797_WIFI_AIS_EVENT_IGNORED;
	if (runtime->state != MT6797_WIFI_AIS_RUNTIME_WAIT_CHANNEL)
		return 0;
	/* A lookalike event cannot be ours before the write attempt is visible. */
	if (!runtime->dirty[MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST])
		return 0;
	if (runtime->owner_context != ops->context) {
		*disposition = MT6797_WIFI_AIS_EVENT_TERMINAL;
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, -EPROTO);
	}

	ret = mt6797_wifi_ais_channel_event_match(&runtime->channel_pending,
						  event, disposition);
	if (*disposition == MT6797_WIFI_AIS_EVENT_TERMINAL)
		return mt6797_wifi_ais_runtime_terminal(runtime, ops,
						       ret ?: -EPROTO);
	if (ret) {
		*disposition = MT6797_WIFI_AIS_EVENT_TERMINAL;
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
	}
	if (*disposition == MT6797_WIFI_AIS_EVENT_COMPLETE)
		runtime->state = MT6797_WIFI_AIS_RUNTIME_CHANNEL_GRANTED;
	return 0;
}

int mt6797_wifi_ais_runtime_start(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_ops *ops,
	const struct mt6797_wifi_ais_runtime_params *params)
{
	unsigned int max_first_sequence =
		U8_MAX + 1U - MT6797_WIFI_AIS_RUNTIME_SEQUENCE_COUNT;
	int ret;

	if (!runtime || !mt6797_wifi_ais_runtime_ops_valid(ops) || !params ||
	    !params->response_timeout_ms ||
	    !is_valid_ether_addr(params->own_addr) ||
	    !is_valid_ether_addr(params->peer_addr))
		return -EINVAL;
	if (runtime->state != MT6797_WIFI_AIS_RUNTIME_IDLE)
		return runtime->state == MT6797_WIFI_AIS_RUNTIME_TERMINAL ?
			runtime->terminal_reason : -EALREADY;
	if (runtime->next_command_sequence > max_first_sequence ||
	    runtime->next_channel_token > U8_MAX)
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, -EOVERFLOW);

	ret = ops->precheck(ops->context);
	if (ret)
		return ret;
	runtime->owner_context = ops->context;
	ret = mt6797_wifi_ais_resources_open(&runtime->resources);
	if (ret)
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
	ret = mt6797_wifi_ais_bss_reserve(&runtime->resources,
		params->bss_index, params->own_mac_index, params->own_addr,
		&runtime->bss);
	if (ret)
		return mt6797_wifi_ais_runtime_unwind_clean(runtime, ops, ret);
	ret = mt6797_wifi_ais_sta_reserve(&runtime->resources, &runtime->bss,
		params->peer_addr, &runtime->sta);
	if (ret)
		return mt6797_wifi_ais_runtime_unwind_clean(runtime, ops, ret);
	ret = mt6797_wifi_ais_runtime_build(runtime, params);
	if (ret)
		return mt6797_wifi_ais_runtime_unwind_clean(runtime, ops, ret);

	ret = mt6797_wifi_ais_runtime_send(runtime, ops,
					   MT6797_WIFI_AIS_RUNTIME_BSS_ACTIVATE);
	if (ret)
		return ret;
	/* The immutable pending snapshot is now published before CMD53. */
	runtime->state = MT6797_WIFI_AIS_RUNTIME_WAIT_CHANNEL;
	ret = mt6797_wifi_ais_runtime_send(runtime, ops,
					   MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST);
	if (ret)
		return ret;

	ret = ops->wait_channel(ops->context, params->response_timeout_ms);
	/* A completed event wins an observation of timeout at the same boundary. */
	if (runtime->state == MT6797_WIFI_AIS_RUNTIME_CHANNEL_GRANTED) {
		ret = 0;
	} else if (runtime->state == MT6797_WIFI_AIS_RUNTIME_TERMINAL) {
		return runtime->terminal_reason;
	} else if (ret == -ETIMEDOUT) {
		/* One best-effort abort narrows a late-grant race; never reuse it. */
		mt6797_wifi_ais_runtime_claim_terminal(runtime, -ETIMEDOUT);
		mt6797_wifi_ais_runtime_send(runtime, ops,
					     MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT);
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, -ETIMEDOUT);
	} else if (ret) {
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
	} else {
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, -EPROTO);
	}

	ret = mt6797_wifi_ais_runtime_send(runtime, ops,
					   MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE);
	if (ret)
		return ret;
	runtime->state = MT6797_WIFI_AIS_RUNTIME_STA_STATE1;
	return 0;
}

int mt6797_wifi_ais_runtime_release_channel(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_ops *ops)
{
	int ret;

	if (!runtime || !mt6797_wifi_ais_runtime_ops_valid(ops))
		return -EINVAL;
	if (runtime->state == MT6797_WIFI_AIS_RUNTIME_TERMINAL)
		return runtime->terminal_reason;
	if (runtime->state != MT6797_WIFI_AIS_RUNTIME_STA_STATE3 ||
	    runtime->owner_context != ops->context)
		return -EALREADY;
	if (runtime->sent[MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT])
		return 0;
	ret = ops->precheck(ops->context);
	if (ret)
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
	return mt6797_wifi_ais_runtime_send(runtime, ops,
		MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT);
}

int mt6797_wifi_ais_runtime_stop(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_ops *ops)
{
	int ret;

	if (!runtime || !mt6797_wifi_ais_runtime_ops_valid(ops))
		return -EINVAL;
	if (runtime->state == MT6797_WIFI_AIS_RUNTIME_TERMINAL)
		return runtime->terminal_reason;
	if ((runtime->state != MT6797_WIFI_AIS_RUNTIME_STA_STATE1 &&
	     runtime->state != MT6797_WIFI_AIS_RUNTIME_STA_STATE3) ||
	    runtime->owner_context != ops->context)
		return -EALREADY;

	ret = mt6797_wifi_ais_runtime_send(runtime, ops,
					   MT6797_WIFI_AIS_RUNTIME_STA_REMOVE);
	if (ret)
		return ret;
	if (!runtime->sent[MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT]) {
		ret = mt6797_wifi_ais_runtime_send(runtime, ops,
			MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT);
		if (ret)
			return ret;
	}
	ret = mt6797_wifi_ais_runtime_send(runtime, ops,
					   MT6797_WIFI_AIS_RUNTIME_BSS_DEACTIVATE);
	if (ret)
		return ret;

	ret = mt6797_wifi_ais_sta_release(&runtime->resources, &runtime->sta);
	if (ret)
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
	ret = mt6797_wifi_ais_bss_release(&runtime->resources, &runtime->bss);
	if (ret)
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
	ret = mt6797_wifi_ais_resources_close(&runtime->resources);
	if (ret)
		return mt6797_wifi_ais_runtime_terminal(runtime, ops, ret);
	runtime->state = MT6797_WIFI_AIS_RUNTIME_STOPPED;
	runtime->owner_context = NULL;
	return 0;
}
