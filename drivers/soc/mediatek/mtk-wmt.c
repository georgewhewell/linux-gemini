// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-independent MediaTek WMT packet and transaction layer.
 *
 * The WMT header is type, opcode and little-endian SDU length. The SDU is raw
 * operation-specific data and may be empty. Controller
 * operation values and bring-up ordering intentionally remain with a future
 * firmware/transport owner.
 */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mtk-wmt.h>

static size_t mtk_wmt_max_data(enum mtk_stp_mode mode)
{
	if (mode == MTK_STP_MODE_MANDATORY)
		return MTK_STP_MANDATORY_MAX_PAYLOAD - MTK_WMT_HEADER_SIZE;

	return MTK_WMT_MAX_DATA;
}

static void mtk_wmt_trace_bytes(u8 *prefix, u8 *prefix_length, u8 *tail,
				u8 *tail_length, const u8 *data, size_t length)
{
	size_t first = min_t(size_t, length, MTK_WMT_TRACE_BYTES);
	size_t last = min_t(size_t, length, MTK_WMT_TRACE_BYTES);

	*prefix_length = first;
	*tail_length = last;
	if (first)
		memcpy(prefix, data, first);
	if (last)
		memcpy(tail, data + length - last, last);
}

void mtk_wmt_trace_begin(struct mtk_wmt *wmt, u8 command_opcode)
{
	unsigned long flags;

	if (!wmt)
		return;
	spin_lock_irqsave(&wmt->state_lock, flags);
	memset(&wmt->trace, 0, sizeof(wmt->trace));
	wmt->trace.command_opcode = command_opcode;
	wmt->trace_started = jiffies;
	wmt->trace_active = true;
	spin_unlock_irqrestore(&wmt->state_lock, flags);
}
EXPORT_SYMBOL_GPL(mtk_wmt_trace_begin);

void mtk_wmt_trace_end(struct mtk_wmt *wmt, struct mtk_wmt_trace *trace)
{
	unsigned long flags;

	if (!wmt || !trace)
		return;
	spin_lock_irqsave(&wmt->state_lock, flags);
	*trace = wmt->trace;
	wmt->trace_active = false;
	spin_unlock_irqrestore(&wmt->state_lock, flags);
}
EXPORT_SYMBOL_GPL(mtk_wmt_trace_end);

void mtk_wmt_init(struct mtk_wmt *wmt, enum mtk_stp_mode stp_mode,
		  mtk_wmt_tx_cb_t transmit, void *transport_context)
{
	if (!wmt)
		return;

	memset(wmt, 0, sizeof(*wmt));
	mutex_init(&wmt->transaction_lock);
	mutex_init(&wmt->issue_lock);
	spin_lock_init(&wmt->state_lock);
	atomic_set(&wmt->reset_requests, 0);
	init_completion(&wmt->response);
	wmt->stp_mode = stp_mode;
	wmt->transmit = transmit;
	wmt->transport_context = transport_context;
	wmt->generation = 1;
}
EXPORT_SYMBOL_GPL(mtk_wmt_init);

void mtk_wmt_init_payload_transport(struct mtk_wmt *wmt,
				    enum mtk_stp_mode stp_mode,
				    mtk_wmt_payload_tx_cb_t transmit,
				    void *transport_context)
{
	if (!wmt)
		return;

	memset(wmt, 0, sizeof(*wmt));
	mutex_init(&wmt->transaction_lock);
	mutex_init(&wmt->issue_lock);
	spin_lock_init(&wmt->state_lock);
	atomic_set(&wmt->reset_requests, 0);
	init_completion(&wmt->response);
	wmt->stp_mode = stp_mode;
	wmt->transmit_payload = transmit;
	wmt->transport_context = transport_context;
	wmt->generation = 1;
}
EXPORT_SYMBOL_GPL(mtk_wmt_init_payload_transport);

void mtk_wmt_set_unsolicited(struct mtk_wmt *wmt,
			     mtk_wmt_unsolicited_cb_t receive, void *context)
{
	unsigned long flags;

	if (!wmt)
		return;

	spin_lock_irqsave(&wmt->state_lock, flags);
	wmt->unsolicited = receive;
	wmt->unsolicited_context = context;
	spin_unlock_irqrestore(&wmt->state_lock, flags);
}
EXPORT_SYMBOL_GPL(mtk_wmt_set_unsolicited);

int mtk_wmt_encode_command(u8 opcode, const u8 *data, size_t data_len,
			   u8 *packet, size_t packet_size,
			   size_t *packet_len)
{
	size_t length;

	if ((!data && data_len) || !packet || !packet_len)
		return -EINVAL;
	if (data_len > MTK_WMT_MAX_DATA)
		return -EMSGSIZE;

	length = MTK_WMT_HEADER_SIZE + data_len;
	if (packet_size < length)
		return -ENOSPC;

	packet[0] = MTK_WMT_DIR_COMMAND;
	packet[1] = opcode;
	put_unaligned_le16(data_len, packet + 2);
	if (data_len)
		memcpy(packet + MTK_WMT_HEADER_SIZE, data, data_len);
	*packet_len = length;

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wmt_encode_command);

int mtk_wmt_decode_event(const u8 *packet, size_t packet_len,
			 struct mtk_wmt_packet *event)
{
	u16 declared_len;

	if (!packet || !event)
		return -EINVAL;
	if (packet_len < MTK_WMT_HEADER_SIZE)
		return -EMSGSIZE;
	if (packet[0] != MTK_WMT_DIR_EVENT)
		return -EPROTO;

	declared_len = get_unaligned_le16(packet + 2);
	if (packet_len != MTK_WMT_HEADER_SIZE + declared_len)
		return -EMSGSIZE;

	event->opcode = packet[1];
	event->data = packet + MTK_WMT_HEADER_SIZE;
	event->data_len = declared_len;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wmt_decode_event);

static bool mtk_wmt_matches(const struct mtk_wmt_match *expected,
			    const struct mtk_wmt_packet *event)
{
	return expected->opcode == event->opcode &&
		expected->data_len == event->data_len &&
		(!event->data_len ||
		 !memcmp(expected->data, event->data, event->data_len));
}

int mtk_wmt_receive(void *context, const u8 *payload, size_t payload_len)
{
	struct mtk_wmt *wmt = context;
	struct mtk_wmt_packet event;
	mtk_wmt_unsolicited_cb_t unsolicited;
	void *unsolicited_context;
	unsigned long flags;
	bool matched = false;
	int ret;

	if (!wmt)
		return -EINVAL;

	ret = mtk_wmt_decode_event(payload, payload_len, &event);
	if (ret) {
		spin_lock_irqsave(&wmt->state_lock, flags);
		if (wmt->trace_active) {
			wmt->trace.decode_failures++;
			wmt->trace.malformed_length = payload_len;
			mtk_wmt_trace_bytes(wmt->trace.malformed_prefix,
					    &wmt->trace.malformed_prefix_length,
					    wmt->trace.malformed_tail,
					    &wmt->trace.malformed_tail_length,
					    payload, payload_len);
		}
		spin_unlock_irqrestore(&wmt->state_lock, flags);
		return ret;
	}

	spin_lock_irqsave(&wmt->state_lock, flags);
	if (wmt->trace_active) {
		struct mtk_wmt_trace_event *trace_event;

		if (wmt->trace.event_count < MTK_WMT_TRACE_EVENTS) {
			trace_event = &wmt->trace.events[wmt->trace.event_count++];
			trace_event->opcode = event.opcode;
			trace_event->data_len = event.data_len;
			trace_event->elapsed_ms = jiffies_to_msecs(jiffies -
							wmt->trace_started);
			mtk_wmt_trace_bytes(trace_event->prefix,
					    &trace_event->prefix_length,
					    trace_event->tail,
					    &trace_event->tail_length,
					    event.data, event.data_len);
		}
		wmt->trace.decoded_events++;
	}
	if (wmt->pending && ((wmt->capture_pending &&
		    wmt->capture_opcode == event.opcode &&
		    wmt->capture_data_len == event.data_len) ||
		   (!wmt->capture_pending && mtk_wmt_matches(&wmt->expected, &event)))) {
		if (wmt->capture_pending && event.data_len)
			memcpy(wmt->capture_data, event.data, event.data_len);
		wmt->pending = false;
		wmt->result = 0;
		matched = true;
		unsolicited = NULL;
		unsolicited_context = NULL;
	} else {
		unsolicited = wmt->unsolicited;
		unsolicited_context = wmt->unsolicited_context;
	}
	spin_unlock_irqrestore(&wmt->state_lock, flags);

	if (matched)
		complete(&wmt->response);
	else if (unsolicited)
		unsolicited(unsolicited_context, &event);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wmt_receive);

void mtk_wmt_reset(struct mtk_wmt *wmt)
{
	unsigned long flags;
	bool wake = false;

	if (!wmt)
		return;

	/* This is the reset-before-issue linearization point. */
	atomic_inc(&wmt->reset_requests);
	mutex_lock(&wmt->issue_lock);
	spin_lock_irqsave(&wmt->state_lock, flags);
	wmt->generation++;
	wmt->reset_required = false;
	if (wmt->pending) {
		wmt->pending = false;
		wmt->result = -ECANCELED;
		wake = true;
	}
	spin_unlock_irqrestore(&wmt->state_lock, flags);
	atomic_dec(&wmt->reset_requests);
	mutex_unlock(&wmt->issue_lock);

	if (wake)
		complete(&wmt->response);
}
EXPORT_SYMBOL_GPL(mtk_wmt_reset);

static int mtk_wmt_finish(struct mtk_wmt *wmt, u32 generation, int error,
			  bool fence)
{
	unsigned long flags;
	int result;

	spin_lock_irqsave(&wmt->state_lock, flags);
	if (wmt->generation != generation) {
		result = -ECANCELED;
	} else if (error && fence) {
		/* A transport error wins even if its callback delivered a reply. */
		wmt->pending = false;
		wmt->result = error;
		wmt->generation++;
		wmt->reset_required = true;
		result = error;
	} else if (wmt->pending) {
		wmt->pending = false;
		wmt->result = error;
		if (fence) {
			wmt->generation++;
			wmt->reset_required = true;
		}
		result = error;
	} else {
		result = wmt->result;
	}
	spin_unlock_irqrestore(&wmt->state_lock, flags);

	return result;
}

static int mtk_wmt_transaction_common(struct mtk_wmt *wmt, u8 opcode,
			const u8 *data, size_t data_len,
			const struct mtk_wmt_match *expected,
			bool capture, u8 capture_opcode, size_t capture_len,
			u8 *capture_data, unsigned long timeout)
{
	u8 *packet;
	u8 *frame;
	size_t packet_len;
	size_t frame_len;
	size_t frame_size;
	unsigned long flags;
	long wait;
	u32 generation;
	bool can_issue;
	bool issued = false;
	int ret;

	if (!wmt || (!data && data_len) || !timeout ||
	    (!capture && (!expected ||
	     (!expected->data && expected->data_len))) ||
	    (capture && !capture_data))
		return -EINVAL;
	if (wmt->stp_mode != MTK_STP_MODE_MANDATORY &&
	    wmt->stp_mode != MTK_STP_MODE_FULL)
		return -EINVAL;
	if (!wmt->transmit && !wmt->transmit_payload)
		return -ENODEV;
	if (data_len > mtk_wmt_max_data(wmt->stp_mode) ||
	    (capture ? capture_len : expected->data_len) >
		mtk_wmt_max_data(wmt->stp_mode))
		return -EMSGSIZE;

	mutex_lock(&wmt->transaction_lock);
	spin_lock_irqsave(&wmt->state_lock, flags);
	if (wmt->reset_required) {
		spin_unlock_irqrestore(&wmt->state_lock, flags);
		mutex_unlock(&wmt->transaction_lock);
		return -EPIPE;
	}
	generation = wmt->generation;
	wmt->capture_pending = capture;
	if (capture) {
		wmt->capture_opcode = capture_opcode;
		wmt->capture_data_len = capture_len;
		wmt->capture_data = capture_data;
	} else {
		wmt->expected = *expected;
	}
	wmt->result = -EINPROGRESS;
	wmt->pending = true;
	reinit_completion(&wmt->response);
	spin_unlock_irqrestore(&wmt->state_lock, flags);

	packet = kmalloc(MTK_WMT_HEADER_SIZE + data_len, GFP_KERNEL);
	if (!packet) {
		ret = -ENOMEM;
		goto out_packet;
	}
	ret = mtk_wmt_encode_command(opcode, data, data_len, packet,
				     MTK_WMT_HEADER_SIZE + data_len,
				     &packet_len);
	if (ret)
		goto out_packet;

	if (wmt->transmit_payload) {
		mutex_lock(&wmt->issue_lock);
		/* A reset queued first may already have completed at this point. */
		spin_lock_irqsave(&wmt->state_lock, flags);
		can_issue = wmt->generation == generation && wmt->pending &&
			!wmt->reset_required && !atomic_read(&wmt->reset_requests);
		spin_unlock_irqrestore(&wmt->state_lock, flags);
		if (!can_issue) {
			ret = -ECANCELED;
		} else {
			issued = true;
			ret = wmt->transmit_payload(wmt->transport_context, packet,
						    packet_len);
		}
		mutex_unlock(&wmt->issue_lock);
	} else {
		frame_size = MTK_STP_HEADER_SIZE + packet_len + MTK_STP_TRAILER_SIZE;
		frame = kmalloc(frame_size, GFP_KERNEL);
		if (!frame) {
			ret = -ENOMEM;
			goto out_packet;
		}
		ret = mtk_stp_encode(wmt->stp_mode, MTK_STP_WMT, 0, 0, packet,
				    packet_len, frame, frame_size, &frame_len);
		if (!ret) {
			mutex_lock(&wmt->issue_lock);
			/* A reset queued first may already have completed at this point. */
			spin_lock_irqsave(&wmt->state_lock, flags);
			can_issue = wmt->generation == generation && wmt->pending &&
				!wmt->reset_required && !atomic_read(&wmt->reset_requests);
			spin_unlock_irqrestore(&wmt->state_lock, flags);
			if (!can_issue) {
				ret = -ECANCELED;
			} else {
				issued = true;
				ret = wmt->transmit(wmt->transport_context, frame,
							   frame_len);
			}
			mutex_unlock(&wmt->issue_lock);
		}
		kfree(frame);
	}
	if (ret)
		goto out_packet;

	wait = wait_for_completion_interruptible_timeout(&wmt->response, timeout);
	if (wait < 0)
		ret = wait;
	else if (!wait)
		ret = -ETIMEDOUT;
	else
		ret = 0;

out_packet:
	kfree(packet);
	ret = mtk_wmt_finish(wmt, generation, ret, issued && ret);
	spin_lock_irqsave(&wmt->state_lock, flags);
	wmt->capture_pending = false;
	wmt->capture_data = NULL;
	spin_unlock_irqrestore(&wmt->state_lock, flags);
	mutex_unlock(&wmt->transaction_lock);
	return ret;
}

int mtk_wmt_transaction(struct mtk_wmt *wmt, u8 opcode,
			const u8 *data, size_t data_len,
			const struct mtk_wmt_match *expected,
			unsigned long timeout)
{
	return mtk_wmt_transaction_common(wmt, opcode, data, data_len, expected,
					  false, 0, 0, NULL, timeout);
}
EXPORT_SYMBOL_GPL(mtk_wmt_transaction);

int mtk_wmt_transaction_capture(struct mtk_wmt *wmt, u8 opcode,
				const u8 *data, size_t data_len,
				u8 expected_opcode, size_t expected_data_len,
				u8 *reply, size_t reply_len,
				unsigned long timeout)
{
	if (!wmt || !reply || reply_len != expected_data_len)
		return -EINVAL;
	return mtk_wmt_transaction_common(wmt, opcode, data, data_len, NULL,
					  true, expected_opcode, expected_data_len,
					  reply, timeout);
}
EXPORT_SYMBOL_GPL(mtk_wmt_transaction_capture);

MODULE_DESCRIPTION("MediaTek WMT packet codec and single-flight transactions");
MODULE_LICENSE("GPL");
