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
	if (ret)
		return ret;

	spin_lock_irqsave(&wmt->state_lock, flags);
	if (wmt->pending && mtk_wmt_matches(&wmt->expected, &event)) {
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

int mtk_wmt_transaction(struct mtk_wmt *wmt, u8 opcode,
			const u8 *data, size_t data_len,
			const struct mtk_wmt_match *expected,
			unsigned long timeout)
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

	if (!wmt || !expected || (!data && data_len) ||
	    (!expected->data && expected->data_len) || !timeout)
		return -EINVAL;
	if (wmt->stp_mode != MTK_STP_MODE_MANDATORY &&
	    wmt->stp_mode != MTK_STP_MODE_FULL)
		return -EINVAL;
	if (!wmt->transmit)
		return -ENODEV;
	if (data_len > mtk_wmt_max_data(wmt->stp_mode) ||
	    expected->data_len > mtk_wmt_max_data(wmt->stp_mode))
		return -EMSGSIZE;

	mutex_lock(&wmt->transaction_lock);
	spin_lock_irqsave(&wmt->state_lock, flags);
	if (wmt->reset_required) {
		spin_unlock_irqrestore(&wmt->state_lock, flags);
		mutex_unlock(&wmt->transaction_lock);
		return -EPIPE;
	}
	generation = wmt->generation;
	wmt->expected = *expected;
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
			ret = wmt->transmit(wmt->transport_context, frame, frame_len);
		}
		mutex_unlock(&wmt->issue_lock);
	}
	kfree(frame);
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
	mutex_unlock(&wmt->transaction_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wmt_transaction);

MODULE_DESCRIPTION("MediaTek WMT packet codec and single-flight transactions");
MODULE_LICENSE("GPL");
