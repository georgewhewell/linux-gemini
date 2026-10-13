// SPDX-License-Identifier: GPL-2.0-only
/* Bounded ordinary-mode MT6797 WLAN interrupt and RX pump. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "hif_pump.h"
#include "hif_regs.h"

static bool mt6797_wifi_hif_pump_ops_valid(
	const struct mt6797_wifi_hif_pump_ops *ops)
{
	return ops && ops->read_status && ops->read_tx_released &&
		ops->read_lengths && ops->read_packet;
}

int mt6797_wifi_hif_pump_account_tx(
	struct mt6797_wifi_hif_pump_stats *stats,
	const u16 released[MT6797_WIFI_HIF_TX_QUEUES])
{
	u64 next[MT6797_WIFI_HIF_TX_QUEUES];
	unsigned int queue;

	if (!stats || !released)
		return -EINVAL;
	for (queue = 0; queue < ARRAY_SIZE(next); queue++)
		if (check_add_overflow(stats->tx_released[queue],
				       (u64)released[queue], &next[queue]))
			return -EOVERFLOW;
	memcpy(stats->tx_released, next, sizeof(next));
	return 0;
}

int mt6797_wifi_hif_pump_dispatch(
	const struct mt6797_wifi_hif_pump_ops *ops,
	struct mt6797_wifi_hif_pump_stats *stats,
	const struct mt6797_wifi_rx_view *frame)
{
	struct mt6797_wifi_event_view event;
	u64 packets;
	u64 typed;
	bool deliver = false;
	int ret = 0;

	if (!stats || !frame)
		return -EINVAL;
	if (check_add_overflow(stats->rx_packets, 1ULL, &packets))
		return -EOVERFLOW;

	switch (frame->kind) {
	case MT6797_WIFI_RX_EVENT:
		if (check_add_overflow(stats->events, 1ULL, &typed))
			return -EOVERFLOW;
		stats->events = typed;
		stats->last_event_id = frame->event_id;
		stats->last_event_sequence = frame->sequence;
		event = (struct mt6797_wifi_event_view) {
			.payload = frame->payload,
			.payload_length = frame->payload_length,
			.packet_length = frame->packet_length,
			.id = frame->event_id,
			.sequence = frame->sequence,
		};
		deliver = true;
		break;
	case MT6797_WIFI_RX_IEEE80211:
	case MT6797_WIFI_RX_NATIVE_DATA:
		if (check_add_overflow(stats->ieee80211_frames, 1ULL, &typed))
			return -EOVERFLOW;
		stats->ieee80211_frames = typed;
		if (ops && ops->deliver_frame)
			ret = ops->deliver_frame(ops->context, frame);
		break;
	case MT6797_WIFI_RX_ETHERNET:
		if (check_add_overflow(stats->ethernet_frames, 1ULL, &typed))
			return -EOVERFLOW;
		stats->ethernet_frames = typed;
		if (ops && ops->deliver_frame)
			ret = ops->deliver_frame(ops->context, frame);
		break;
	default:
		return -EPROTO;
	}
	stats->rx_packets = packets;
	if (ret)
		return ret > 0 ? -EPROTO : ret;
	if (deliver && ops && ops->deliver_event) {
		ret = ops->deliver_event(ops->context, &event);
		return ret > 0 ? -EPROTO : ret;
	}
	return 0;
}

static int mt6797_wifi_hif_pump_more(struct mt6797_wifi_hif_pump_stats *stats)
{
	u64 yields;

	if (check_add_overflow(stats->rx_budget_yields, 1ULL, &yields))
		return -EOVERFLOW;
	/* WRPLR is a non-consuming head-length query; unread FIFO entries
	 * remain in their ports even when a pass stops between the two ports.
	 * WHISR is read-clear, so remember the unconsumed RX work explicitly.
	 */
	stats->rx_budget_yields = yields;
	stats->rx_pending = true;
	return MT6797_WIFI_HIF_PUMP_MORE;
}

/*
 * One pass is bounded by work, never by wall-clock time: one WHISR read, eight
 * WTQCR reads and at most MT6797_WIFI_HIF_RX_BUDGET FIFO packets. GEN3 bounds
 * its interrupt service the same way (nic/nic.c nicProcessIST(), a loop of
 * u4HifIstLoopCount passes). A threaded IRQ can be preempted by hard and soft
 * interrupts for an unbounded time, so elapsed time says nothing about the
 * health of the interface and must not be reported as a transport fault.
 */
int mt6797_wifi_hif_pump_run(const struct mt6797_wifi_hif_pump_ops *ops,
			     struct mt6797_wifi_hif_pump_stats *stats,
			     u32 *acknowledged_status)
{
	struct mt6797_wifi_rx_view frame;
	u16 released[MT6797_WIFI_HIF_TX_QUEUES];
	u16 lengths[2];
	unsigned int packets = 0;
	bool saw_length = false;
	u32 status;
	int ret;
	unsigned int port;

	if (!mt6797_wifi_hif_pump_ops_valid(ops) || !stats ||
	    !acknowledged_status)
		return -EINVAL;
	ret = ops->read_status(ops->context, &status);
	if (ret)
		return ret > 0 ? -EPROTO : ret;
	*acknowledged_status = status;
	if (!status && !stats->rx_pending)
		return ops->allow_empty_status ? 0 : -ENODATA;
	if (status & ~MT6797_WIFI_WHIER_DEFAULT)
		return -EPROTO;
	if (status & (MT6797_WIFI_WHISR_ABNORMAL |
		      MT6797_WIFI_WHISR_D2H_SW))
		return -EIO;

	if (status & MT6797_WIFI_WHISR_TX_DONE) {
		ret = ops->read_tx_released(ops->context, released);
		if (ret)
			return ret > 0 ? -EPROTO : ret;
		ret = mt6797_wifi_hif_pump_account_tx(stats, released);
		if (ret)
			return ret > 0 ? -EPROTO : ret;
	}
	if (!(status & MT6797_WIFI_WHISR_RX_DONE) && !stats->rx_pending)
		return 0;

	while (packets < MT6797_WIFI_HIF_RX_BUDGET) {
		ret = ops->read_lengths(ops->context, lengths);
		if (ret)
			return ret > 0 ? -EPROTO : ret;
		if (!lengths[0] && !lengths[1]) {
			bool continued = stats->rx_pending;

			stats->rx_pending = false;
			return saw_length || continued ? 0 : -ENODATA;
		}
		saw_length = true;

		for (port = 0; port < ARRAY_SIZE(lengths); port++) {
			const u8 *packet;

			if (!lengths[port])
				continue;
			if (lengths[port] > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH)
				return -EMSGSIZE;
			if (packets == MT6797_WIFI_HIF_RX_BUDGET)
				return mt6797_wifi_hif_pump_more(stats);
			ret = ops->read_packet(ops->context, port,
					       lengths[port], &packet);
			if (ret)
				return ret > 0 ? -EPROTO : ret;
			ret = mt6797_wifi_rx_parse(packet, lengths[port], &frame);
			if (ret) {
				if (ops->reject_packet)
					ops->reject_packet(ops->context, packet,
						lengths[port], ret);
				/* This complete FIFO item has already been drained.
				 * Bad air frames and unsupported hardware reports do
				 * not invalidate the command transport or live keys.
				 */
				if (ret != -EBADMSG && ret != -EOPNOTSUPP)
					return ret;
				if (check_add_overflow(stats->rx_dropped, 1ULL,
						       &stats->rx_dropped))
					return -EOVERFLOW;
				packets++;
				continue;
			}
			ret = mt6797_wifi_hif_pump_dispatch(ops, stats, &frame);
			if (ret)
				return ret > 0 ? -EPROTO : ret;
			packets++;
		}
	}

	ret = ops->read_lengths(ops->context, lengths);
	if (ret)
		return ret > 0 ? -EPROTO : ret;
	if (lengths[0] || lengths[1])
		return mt6797_wifi_hif_pump_more(stats);
	stats->rx_pending = false;
	return 0;
}
