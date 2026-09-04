// SPDX-License-Identifier: GPL-2.0-only
/* Pure, bounded passive-scan transaction for the MT6797 GEN3 WLAN ABI. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "scan.h"

#define MT6797_WIFI_COMMAND_SCAN_V2		0x03U
#define MT6797_WIFI_COMMAND_SCAN_CANCEL		0x1bU
#define MT6797_WIFI_EVENT_SCAN_DONE		0x0dU

#define MT6797_WIFI_SCAN_BODY_LENGTH		226U
#define MT6797_WIFI_SCAN_SEQUENCE_OFFSET		8U
#define MT6797_WIFI_SCAN_TYPE_OFFSET		10U
#define MT6797_WIFI_SCAN_SSID_TYPE_OFFSET	11U
#define MT6797_WIFI_SCAN_CHANNEL_TYPE_OFFSET	166U
#define MT6797_WIFI_SCAN_CHANNEL_COUNT_OFFSET	167U
#define MT6797_WIFI_SCAN_CHANNELS_OFFSET		168U
#define MT6797_WIFI_SCAN_IE_LENGTH_OFFSET	232U

#define MT6797_WIFI_SCAN_TYPE_PASSIVE		0U
#define MT6797_WIFI_SCAN_SSID_WILDCARD		BIT(0)
#define MT6797_WIFI_SCAN_CHANNEL_SPECIFIED	4U
#define MT6797_WIFI_SCAN_DONE_LEGACY_LENGTH	4U
#define MT6797_WIFI_SCAN_DONE_V3_LENGTH		16U

static bool mt6797_wifi_scan_channel_valid(
	const struct mt6797_wifi_scan_channel *channel)
{
	if (channel->band == MT6797_WIFI_SCAN_BAND_2GHZ)
		return channel->number >= 1 && channel->number <= 14;
	if (channel->band == MT6797_WIFI_SCAN_BAND_5GHZ)
		return channel->number != 0;
	return false;
}

static int mt6797_wifi_scan_channels_validate(
	const struct mt6797_wifi_scan_channel *channels, size_t channel_count)
{
	size_t first;
	size_t second;

	if (!channels || !channel_count ||
	    channel_count > MT6797_WIFI_SCAN_MAX_CHANNELS)
		return -EINVAL;

	for (first = 0; first < channel_count; first++) {
		if (!mt6797_wifi_scan_channel_valid(&channels[first]))
			return -EINVAL;
		for (second = first + 1; second < channel_count; second++)
			if (channels[first].band == channels[second].band &&
			    channels[first].number == channels[second].number)
				return -EINVAL;
	}
	return 0;
}

int mt6797_wifi_scan_start(struct mt6797_wifi_scan_transaction *transaction,
			   const struct mt6797_wifi_scan_channel *channels,
			   size_t channel_count, u8 command_sequence,
			   u8 scan_sequence, u8 *command, size_t capacity)
{
	size_t index;
	int ret;

	if (!transaction || !command)
		return -EINVAL;
	if (transaction->state != MT6797_WIFI_SCAN_IDLE)
		return transaction->state == MT6797_WIFI_SCAN_TERMINAL ?
			-ESHUTDOWN : -EBUSY;
	if (capacity < MT6797_WIFI_SCAN_COMMAND_LENGTH)
		return -EMSGSIZE;
	ret = mt6797_wifi_scan_channels_validate(channels, channel_count);
	if (ret)
		return ret;

	memset(command, 0, MT6797_WIFI_SCAN_COMMAND_LENGTH);
	ret = mt6797_wifi_command_init(command, capacity,
			MT6797_WIFI_SCAN_BODY_LENGTH,
			MT6797_WIFI_COMMAND_SCAN_V2, true, command_sequence);
	if (ret)
		return ret;
	/* nicTxCmd() includes DWORD padding in u2TxByteCount. */
	put_unaligned_le16(MT6797_WIFI_SCAN_COMMAND_LENGTH, command);
	command[MT6797_WIFI_SCAN_SEQUENCE_OFFSET] = scan_sequence;
	command[MT6797_WIFI_SCAN_TYPE_OFFSET] = MT6797_WIFI_SCAN_TYPE_PASSIVE;
	command[MT6797_WIFI_SCAN_SSID_TYPE_OFFSET] =
		MT6797_WIFI_SCAN_SSID_WILDCARD;
	command[MT6797_WIFI_SCAN_CHANNEL_TYPE_OFFSET] =
		MT6797_WIFI_SCAN_CHANNEL_SPECIFIED;
	command[MT6797_WIFI_SCAN_CHANNEL_COUNT_OFFSET] = channel_count;
	for (index = 0; index < channel_count; index++) {
		command[MT6797_WIFI_SCAN_CHANNELS_OFFSET + index * 2] =
			channels[index].band;
		command[MT6797_WIFI_SCAN_CHANNELS_OFFSET + index * 2 + 1] =
			channels[index].number;
	}
	put_unaligned_le16(0, command + MT6797_WIFI_SCAN_IE_LENGTH_OFFSET);

	transaction->scan_sequence = scan_sequence;
	transaction->state = MT6797_WIFI_SCAN_WAIT_DONE;
	return 0;
}

int mt6797_wifi_scan_event(struct mt6797_wifi_scan_transaction *transaction,
			   const struct mt6797_wifi_event_view *event,
			   enum mt6797_wifi_scan_event_result *result)
{
	if (!transaction || !event || !result)
		return -EINVAL;
	*result = MT6797_WIFI_SCAN_EVENT_IGNORED;
	if (event->id != MT6797_WIFI_EVENT_SCAN_DONE)
		return 0;
	if (transaction->state != MT6797_WIFI_SCAN_WAIT_DONE)
		return 0;
	if (!event->payload || !event->payload_length) {
		transaction->state = MT6797_WIFI_SCAN_TERMINAL;
		return -EPROTO;
	}
	/* Scan ownership is carried in the event body, not its HIF header. */
	if (event->payload[0] != transaction->scan_sequence)
		return 0;
	if (event->payload_length != MT6797_WIFI_SCAN_DONE_LEGACY_LENGTH &&
	    event->payload_length != MT6797_WIFI_SCAN_DONE_V3_LENGTH) {
		transaction->state = MT6797_WIFI_SCAN_TERMINAL;
		return -EPROTO;
	}

	transaction->state = MT6797_WIFI_SCAN_DONE;
	*result = MT6797_WIFI_SCAN_EVENT_COMPLETE;
	return 0;
}

int mt6797_wifi_scan_timeout(struct mt6797_wifi_scan_transaction *transaction,
			     u8 command_sequence, u8 *command, size_t capacity)
{
	int ret;

	if (!transaction || !command)
		return -EINVAL;
	if (transaction->state != MT6797_WIFI_SCAN_WAIT_DONE)
		return transaction->state == MT6797_WIFI_SCAN_TERMINAL ?
			-ESHUTDOWN : -EALREADY;

	ret = mt6797_wifi_command_init(command, capacity,
			MT6797_WIFI_SCAN_CANCEL_LENGTH -
			MT6797_WIFI_COMMAND_HEADER_LENGTH,
			MT6797_WIFI_COMMAND_SCAN_CANCEL, true, command_sequence);
	transaction->state = MT6797_WIFI_SCAN_TERMINAL;
	if (ret)
		return ret;
	command[MT6797_WIFI_COMMAND_HEADER_LENGTH] = transaction->scan_sequence;
	return 0;
}

int mt6797_wifi_scan_reset(struct mt6797_wifi_scan_transaction *transaction)
{
	if (!transaction)
		return -EINVAL;
	if (transaction->state != MT6797_WIFI_SCAN_DONE)
		return transaction->state == MT6797_WIFI_SCAN_TERMINAL ?
			-ESHUTDOWN : -EBUSY;
	*transaction = (struct mt6797_wifi_scan_transaction) {};
	return 0;
}
