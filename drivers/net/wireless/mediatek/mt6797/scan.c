// SPDX-License-Identifier: GPL-2.0-only
/* Pure, bounded passive/active scan transaction for the MT6797 GEN3 WLAN ABI. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "scan.h"

#define MT6797_WIFI_COMMAND_SCAN_V2		0x03U
#define MT6797_WIFI_COMMAND_SCAN_CANCEL		0x1bU
#define MT6797_WIFI_SCAN_BODY_LENGTH		226U
#define MT6797_WIFI_SCAN_SEQUENCE_OFFSET		8U
#define MT6797_WIFI_SCAN_TYPE_OFFSET		10U
#define MT6797_WIFI_SCAN_SSID_TYPE_OFFSET	11U
#define MT6797_WIFI_SCAN_SSIDS_OFFSET		16U
#define MT6797_WIFI_SCAN_SSID_STRIDE		36U
#define MT6797_WIFI_SCAN_CHANNEL_TYPE_OFFSET	166U
#define MT6797_WIFI_SCAN_CHANNEL_COUNT_OFFSET	167U
#define MT6797_WIFI_SCAN_CHANNELS_OFFSET		168U
#define MT6797_WIFI_SCAN_IE_LENGTH_OFFSET	232U
#define MT6797_WIFI_SCAN_IES_OFFSET		234U

#define MT6797_WIFI_SCAN_TYPE_PASSIVE		0U
#define MT6797_WIFI_SCAN_TYPE_ACTIVE		1U
#define MT6797_WIFI_SCAN_SSID_WILDCARD		BIT(0)
#define MT6797_WIFI_SCAN_SSID_SPECIFIED		BIT(2)
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

int mt6797_wifi_scan_probe_validate(const struct mt6797_wifi_scan_probe *probe)
{
	size_t index, offset = 0;

	if (!probe)
		return 0;
	if (probe->ssid_count > MT6797_WIFI_SCAN_MAX_SSIDS ||
	    (probe->ssid_count && !probe->ssids) ||
	    probe->ie_length > MT6797_WIFI_SCAN_MAX_IE_LENGTH ||
	    (probe->ie_length && !probe->ies))
		return -EINVAL;
	for (index = 0; index < probe->ssid_count; index++)
		if (probe->ssids[index].length > MT6797_WIFI_SCAN_MAX_SSID_LENGTH)
			return -EINVAL;
	while (offset < probe->ie_length) {
		if (probe->ie_length - offset < 2 ||
		    probe->ies[offset + 1] > probe->ie_length - offset - 2)
			return -EINVAL;
		offset += 2 + probe->ies[offset + 1];
	}
	return 0;
}

int mt6797_wifi_scan_start_probe(struct mt6797_wifi_scan_transaction *transaction,
			   const struct mt6797_wifi_scan_channel *channels,
			   size_t channel_count,
			   const struct mt6797_wifi_scan_probe *probe,
			   u8 command_sequence, u8 scan_sequence,
			   u8 *command, size_t capacity, size_t *command_length)
{
	size_t index, slot = 0, ie_length, length;
	bool active = probe && probe->active;
	int ret;

	if (!transaction || !command || !command_length)
		return -EINVAL;
	if (transaction->state != MT6797_WIFI_SCAN_IDLE)
		return transaction->state == MT6797_WIFI_SCAN_TERMINAL ?
			-ESHUTDOWN : -EBUSY;
	ret = mt6797_wifi_scan_probe_validate(probe);
	if (ret)
		return ret;
	ie_length = active ? probe->ie_length : 0;
	length = ALIGN(MT6797_WIFI_SCAN_IES_OFFSET + ie_length, 4);
	if (capacity < length)
		return -EMSGSIZE;
	ret = mt6797_wifi_scan_channels_validate(channels, channel_count);
	if (ret)
		return ret;

	memset(command, 0, length);
	ret = mt6797_wifi_command_init(command, capacity,
			MT6797_WIFI_SCAN_BODY_LENGTH + ie_length,
			MT6797_WIFI_COMMAND_SCAN_V2, true, command_sequence);
	if (ret)
		return ret;
	/* nicTxCmd() includes DWORD padding in u2TxByteCount. */
	put_unaligned_le16(length, command);
	command[MT6797_WIFI_SCAN_SEQUENCE_OFFSET] = scan_sequence;
	command[MT6797_WIFI_SCAN_TYPE_OFFSET] = active ?
		MT6797_WIFI_SCAN_TYPE_ACTIVE : MT6797_WIFI_SCAN_TYPE_PASSIVE;
	command[MT6797_WIFI_SCAN_SSID_TYPE_OFFSET] =
		MT6797_WIFI_SCAN_SSID_WILDCARD;
	if (active) {
		for (index = 0; index < probe->ssid_count; index++) {
			const struct mt6797_wifi_scan_ssid *ssid = &probe->ssids[index];
			u8 *entry;

			if (!ssid->length)
				continue;
			entry = command + MT6797_WIFI_SCAN_SSIDS_OFFSET +
				slot++ * MT6797_WIFI_SCAN_SSID_STRIDE;
			put_unaligned_le32(ssid->length, entry);
			memcpy(entry + sizeof(u32), ssid->bytes, ssid->length);
			command[MT6797_WIFI_SCAN_SSID_TYPE_OFFSET] =
				MT6797_WIFI_SCAN_SSID_SPECIFIED;
		}
		/*
		 * Public GEN3 scnSendScanReqV2() leaves ucSSIDNum and probe count
		 * zero; only these four length-prefixed PARAM_SSID slots are filled.
		 */
		if (ie_length)
			memcpy(command + MT6797_WIFI_SCAN_IES_OFFSET, probe->ies,
			       ie_length);
	}
	command[MT6797_WIFI_SCAN_CHANNEL_TYPE_OFFSET] =
		MT6797_WIFI_SCAN_CHANNEL_SPECIFIED;
	command[MT6797_WIFI_SCAN_CHANNEL_COUNT_OFFSET] = channel_count;
	for (index = 0; index < channel_count; index++) {
		command[MT6797_WIFI_SCAN_CHANNELS_OFFSET + index * 2] =
			channels[index].band;
		command[MT6797_WIFI_SCAN_CHANNELS_OFFSET + index * 2 + 1] =
			channels[index].number;
	}
	put_unaligned_le16(ie_length, command + MT6797_WIFI_SCAN_IE_LENGTH_OFFSET);

	transaction->scan_sequence = scan_sequence;
	transaction->state = MT6797_WIFI_SCAN_WAIT_DONE;
	*command_length = length;
	return 0;
}

int mt6797_wifi_scan_start(struct mt6797_wifi_scan_transaction *transaction,
			   const struct mt6797_wifi_scan_channel *channels,
			   size_t channel_count, u8 command_sequence,
			   u8 scan_sequence, u8 *command, size_t capacity)
{
	size_t length;

	return mt6797_wifi_scan_start_probe(transaction, channels, channel_count,
		NULL, command_sequence, scan_sequence, command, capacity, &length);
}

int mt6797_wifi_scan_event(struct mt6797_wifi_scan_transaction *transaction,
			   const struct mt6797_wifi_event_view *event,
			   enum mt6797_wifi_scan_event_result *result)
{
	if (!transaction || !event || !result)
		return -EINVAL;
	*result = MT6797_WIFI_SCAN_EVENT_IGNORED;
	if (event->id != MT6797_WIFI_SCAN_DONE_EVENT_ID)
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
