// SPDX-License-Identifier: GPL-2.0-only
/* Pure management TX ownership and wire codecs for the MT6797 GEN3 AIS path. */

#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_mgmt_tx.h"

#define MT6797_WIFI_AIS_MGMT_EVENT_TX_DONE	0x0fU
#define MT6797_WIFI_AIS_MGMT_HEADER_LENGTH	24U
#define MT6797_WIFI_AIS_MGMT_SEQUENCE_MAX	GENMASK(11, 0)
#define MT6797_WIFI_AIS_MGMT_OWN_MAC_MAX		GENMASK(5, 0)

#define MT6797_WIFI_AIS_MGMT_TC4_MCU_Q1		(BIT(7) | BIT(3))
#define MT6797_WIFI_AIS_MGMT_LONG_80211		(BIT(7) | (2U << 5))
#define MT6797_WIFI_AIS_MGMT_FIXED_RATE		BIT(7)
#define MT6797_WIFI_AIS_MGMT_FORCE_TX		BIT(0)
#define MT6797_WIFI_AIS_MGMT_RETRY_LIMIT		(30U << 11)
#define MT6797_WIFI_AIS_MGMT_SN_VALID		BIT(15)
#define MT6797_WIFI_AIS_MGMT_TXS_TO_MCU		BIT(1)
#define MT6797_WIFI_AIS_MGMT_RATE_FROM_DESC	0U /* Bit 0 selects CR when set. */

#define MT6797_WIFI_AIS_MGMT_DEST_OFFSET		4U
#define MT6797_WIFI_AIS_MGMT_SOURCE_OFFSET	10U
#define MT6797_WIFI_AIS_MGMT_BSSID_OFFSET	16U

static int mt6797_wifi_ais_mgmt_require_open(
	const struct mt6797_wifi_ais_mgmt_pid_owner *owner)
{
	if (!owner)
		return -EINVAL;
	if (owner->state == MT6797_WIFI_AIS_MGMT_PID_TERMINAL)
		return -ESHUTDOWN;
	return owner->state == MT6797_WIFI_AIS_MGMT_PID_OPEN ? 0 : -ENOTCONN;
}

static bool mt6797_wifi_ais_mgmt_lease_matches(
	const struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	const struct mt6797_wifi_ais_mgmt_pid_lease *lease)
{
	return owner && lease && owner->held && lease->owner_generation &&
		lease->cookie && lease->owner_generation == owner->generation &&
		lease->cookie == owner->active.cookie &&
		lease->sta_generation == owner->sta_generation &&
		lease->bss_cookie == owner->bss_cookie &&
		lease->sta_cookie == owner->sta_cookie &&
		lease->wlan_index == owner->wlan_index &&
		lease->pid == owner->active.pid &&
		lease->owner_generation == owner->active.owner_generation &&
		lease->sta_generation == owner->active.sta_generation &&
		lease->bss_cookie == owner->active.bss_cookie &&
		lease->sta_cookie == owner->active.sta_cookie &&
		lease->wlan_index == owner->active.wlan_index &&
		lease->pid >= MT6797_WIFI_AIS_MGMT_PID_MIN &&
		lease->pid <= MT6797_WIFI_AIS_MGMT_PID_MAX;
}

void mt6797_wifi_ais_mgmt_pid_init(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner)
{
	if (owner)
		memset(owner, 0, sizeof(*owner));
}

int mt6797_wifi_ais_mgmt_pid_open(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta)
{
	struct mt6797_wifi_ais_sta_pending pending;
	int ret;

	if (!owner)
		return -EINVAL;
	if (owner->state == MT6797_WIFI_AIS_MGMT_PID_TERMINAL)
		return -ESHUTDOWN;
	if (owner->state != MT6797_WIFI_AIS_MGMT_PID_CLOSED)
		return -EBUSY;
	ret = mt6797_wifi_ais_sta_pending_init(bss, sta, 0, &pending);
	if (ret)
		return ret;
	if (owner->generation == U64_MAX)
		goto overflow;

	owner->generation++;
	owner->next_cookie = 1;
	owner->sta_generation = pending.generation;
	owner->bss_cookie = pending.bss_cookie;
	owner->sta_cookie = pending.sta_cookie;
	owner->wlan_index = sta->pairwise.index;
	if (owner->next_pid < MT6797_WIFI_AIS_MGMT_PID_MIN ||
	    owner->next_pid > MT6797_WIFI_AIS_MGMT_PID_MAX)
		owner->next_pid = MT6797_WIFI_AIS_MGMT_PID_MIN;
	owner->state = MT6797_WIFI_AIS_MGMT_PID_OPEN;
	return 0;

overflow:
	owner->state = MT6797_WIFI_AIS_MGMT_PID_TERMINAL;
	return -EOVERFLOW;
}

int mt6797_wifi_ais_mgmt_pid_acquire(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	struct mt6797_wifi_ais_mgmt_pid_lease *lease)
{
	struct mt6797_wifi_ais_mgmt_pid_lease candidate;
	int ret;

	if (lease)
		memset(lease, 0, sizeof(*lease));
	if (!lease)
		return -EINVAL;
	ret = mt6797_wifi_ais_mgmt_require_open(owner);
	if (ret)
		return ret;
	if (owner->held)
		return -EBUSY;
	if (owner->next_cookie == U64_MAX) {
		owner->state = MT6797_WIFI_AIS_MGMT_PID_TERMINAL;
		return -EOVERFLOW;
	}

	candidate = (struct mt6797_wifi_ais_mgmt_pid_lease) {
		.owner_generation = owner->generation,
		.cookie = owner->next_cookie++,
		.sta_generation = owner->sta_generation,
		.bss_cookie = owner->bss_cookie,
		.sta_cookie = owner->sta_cookie,
		.wlan_index = owner->wlan_index,
		.pid = owner->next_pid,
	};
	owner->next_pid = candidate.pid == MT6797_WIFI_AIS_MGMT_PID_MAX ?
		MT6797_WIFI_AIS_MGMT_PID_MIN : candidate.pid + 1;
	owner->active = candidate;
	owner->held = true;
	*lease = candidate;
	return 0;
}

int mt6797_wifi_ais_mgmt_pid_release(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	const struct mt6797_wifi_ais_mgmt_pid_lease *lease)
{
	int ret = mt6797_wifi_ais_mgmt_require_open(owner);

	if (ret)
		return ret;
	if (!mt6797_wifi_ais_mgmt_lease_matches(owner, lease))
		return -ESTALE;
	memset(&owner->active, 0, sizeof(owner->active));
	owner->held = false;
	return 0;
}

int mt6797_wifi_ais_mgmt_pid_close(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner)
{
	int ret = mt6797_wifi_ais_mgmt_require_open(owner);

	if (ret)
		return ret;
	if (owner->held)
		return -EBUSY;
	owner->state = MT6797_WIFI_AIS_MGMT_PID_CLOSED;
	owner->sta_generation = 0;
	owner->bss_cookie = 0;
	owner->sta_cookie = 0;
	owner->wlan_index = 0;
	return 0;
}

int mt6797_wifi_ais_mgmt_pid_mark_terminal(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner)
{
	if (!owner)
		return -EINVAL;
	if (owner->state == MT6797_WIFI_AIS_MGMT_PID_TERMINAL)
		return 0;
	if (owner->state != MT6797_WIFI_AIS_MGMT_PID_OPEN)
		return -ESHUTDOWN;
	owner->state = MT6797_WIFI_AIS_MGMT_PID_TERMINAL;
	return 0;
}

static bool mt6797_wifi_ais_mgmt_frame_valid(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta, const u8 *frame,
	size_t length, u16 *frame_control)
{
	u16 control;

	if (!frame || length < MT6797_WIFI_AIS_MGMT_HEADER_LENGTH)
		return false;
	control = get_unaligned_le16(frame);
	if ((control & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE)) !=
	    IEEE80211_FTYPE_MGMT ||
	    control & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS |
		       IEEE80211_FCTL_ORDER) ||
	    !ether_addr_equal(frame + MT6797_WIFI_AIS_MGMT_DEST_OFFSET,
			      sta->peer_addr) ||
	    !ether_addr_equal(frame + MT6797_WIFI_AIS_MGMT_SOURCE_OFFSET,
			      bss->own_addr) ||
	    !ether_addr_equal(frame + MT6797_WIFI_AIS_MGMT_BSSID_OFFSET,
			      sta->peer_addr))
		return false;
	*frame_control = control;
	return true;
}

int mt6797_wifi_ais_mgmt_tx_build(
	const struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	const struct mt6797_wifi_ais_mgmt_pid_lease *lease,
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_mgmt_tx_params *params,
	const u8 *frame, size_t frame_length, u8 *packet, size_t capacity,
	size_t *transfer_length)
{
	struct mt6797_wifi_ais_sta_pending pending;
	size_t packet_length;
	size_t aligned_length;
	u16 frame_control;
	u16 subtype;
	int ret;

	if (transfer_length)
		*transfer_length = 0;
	if (!params || !packet || !transfer_length)
		return -EINVAL;
	ret = mt6797_wifi_ais_mgmt_require_open(owner);
	if (ret)
		return ret;
	if (!mt6797_wifi_ais_mgmt_lease_matches(owner, lease))
		return -ESTALE;
	ret = mt6797_wifi_ais_sta_pending_init(bss, sta, 0, &pending);
	if (ret || pending.generation != lease->sta_generation ||
	    pending.bss_cookie != lease->bss_cookie ||
	    pending.sta_cookie != lease->sta_cookie ||
	    sta->pairwise.index != lease->wlan_index ||
	    bss->own_mac_index > MT6797_WIFI_AIS_MGMT_OWN_MAC_MAX ||
	    params->fixed_rate > MT6797_WIFI_AIS_MGMT_RATE_MAX ||
	    params->sequence_number > MT6797_WIFI_AIS_MGMT_SEQUENCE_MAX)
		return -EINVAL;
	if (!mt6797_wifi_ais_mgmt_frame_valid(bss, sta, frame, frame_length,
					       &frame_control))
		return -EINVAL;
	subtype = frame_control & IEEE80211_FCTL_STYPE;
	if (subtype != IEEE80211_STYPE_AUTH &&
	    subtype != IEEE80211_STYPE_ASSOC_REQ)
		return -EOPNOTSUPP;
	if (frame_control & IEEE80211_FCTL_PROTECTED)
		return -EOPNOTSUPP;
	if (check_add_overflow(frame_length,
			       (size_t)MT6797_WIFI_TX_DESCRIPTOR_LENGTH,
			       &packet_length))
		return -EOVERFLOW;
	if (packet_length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH)
		return -EMSGSIZE;
	aligned_length = ALIGN(packet_length, sizeof(u32));
	if (aligned_length > capacity)
		return -EMSGSIZE;

	memset(packet, 0, aligned_length);
	put_unaligned_le16(packet_length, packet);
	packet[2] = (MT6797_WIFI_TX_DESCRIPTOR_LENGTH +
		     MT6797_WIFI_AIS_MGMT_HEADER_LENGTH) >> 1;
	packet[3] = MT6797_WIFI_AIS_MGMT_TC4_MCU_Q1;
	packet[4] = lease->wlan_index;
	packet[5] = MT6797_WIFI_AIS_MGMT_LONG_80211 |
		(MT6797_WIFI_AIS_MGMT_HEADER_LENGTH >> 1);
	packet[7] = bss->own_mac_index << 2;
	packet[8] = ((frame_control & IEEE80211_FCTL_STYPE) >> 4) |
		((frame_control & IEEE80211_FCTL_FTYPE) << 2);
	packet[11] = MT6797_WIFI_AIS_MGMT_FIXED_RATE;
	put_unaligned_le16(MT6797_WIFI_AIS_MGMT_RETRY_LIMIT |
			   MT6797_WIFI_AIS_MGMT_FORCE_TX, packet + 12);
	put_unaligned_le16(params->sequence_number |
			   MT6797_WIFI_AIS_MGMT_SN_VALID, packet + 14);
	packet[20] = lease->pid;
	packet[21] = MT6797_WIFI_AIS_MGMT_TXS_TO_MCU;
	packet[24] = MT6797_WIFI_AIS_MGMT_RATE_FROM_DESC;
	put_unaligned_le16(params->fixed_rate << 2, packet + 26);
	memcpy(packet + MT6797_WIFI_TX_DESCRIPTOR_LENGTH, frame, frame_length);
	*transfer_length = aligned_length;
	return 0;
}

int mt6797_wifi_ais_mgmt_tx_event_match(
	const struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	const struct mt6797_wifi_ais_mgmt_pid_lease *lease,
	const struct mt6797_wifi_event_view *event,
	enum mt6797_wifi_ais_event_disposition *disposition,
	struct mt6797_wifi_ais_mgmt_tx_status *status)
{
	const u8 *payload;
	int ret;

	if (!event || !disposition || !status)
		return -EINVAL;
	ret = mt6797_wifi_ais_mgmt_require_open(owner);
	if (ret)
		return ret;
	if (!mt6797_wifi_ais_mgmt_lease_matches(owner, lease))
		return -ESTALE;
	*disposition = MT6797_WIFI_AIS_EVENT_IGNORED;
	memset(status, 0, sizeof(*status));
	if (event->id != MT6797_WIFI_AIS_MGMT_EVENT_TX_DONE)
		return 0;
	if (!event->payload ||
	    event->payload_length != MT6797_WIFI_AIS_MGMT_TX_EVENT_LENGTH -
		MT6797_WIFI_EVENT_HEADER_LENGTH ||
	    event->packet_length != MT6797_WIFI_AIS_MGMT_TX_EVENT_LENGTH) {
		*disposition = MT6797_WIFI_AIS_EVENT_TERMINAL;
		return -EPROTO;
	}
	payload = event->payload;
	if (payload[0] != lease->pid || payload[4] != lease->wlan_index)
		return 0;

	status->status = payload[1];
	status->sequence_number = get_unaligned_le16(payload + 2);
	status->flags = payload[8];
	status->advanced = payload[8] & BIT(0);
	if (status->advanced) {
		status->tx_count = payload[5];
		status->rate = get_unaligned_le16(payload + 6);
	}
	*disposition = MT6797_WIFI_AIS_EVENT_COMPLETE;
	return 0;
}
