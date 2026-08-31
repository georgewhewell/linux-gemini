// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-neutral MT6797 MD1 CCCI handshake contract.
 *
 * The active vendor sequence in eccci/port_ctlmsg.c and port_proxy.c routes a
 * CONTROL_RX frame with data[1] == 0 and reserved == 0x5555ffff to HS1. Exact
 * 16-byte and 88-byte HS1 frames request legacy and v2 AP runtime data. A
 * later data[1] == 0 frame is HS2 and enters READY. This core makes no CCCI
 * claim until a separate HIF owner has observed post-vector activity.
 */

#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-md1-hs.h>
#include <linux/string.h>

int mt6797_md1_hs_init(struct mt6797_md1_hs *hs)
{
	if (!hs)
		return -EINVAL;

	memset(hs, 0, sizeof(*hs));
	mutex_init(&hs->lock);
	hs->state = MT6797_MD1_HS_OFF;
	return 0;
}

int mt6797_md1_hs_arm(struct mt6797_md1_hs *hs, bool post_vector_activity)
{
	int ret = 0;

	if (!hs)
		return -EINVAL;

	mutex_lock(&hs->lock);
	if (!post_vector_activity)
		ret = -EPERM;
	else if (hs->state != MT6797_MD1_HS_OFF)
		ret = -EALREADY;
	else
		hs->state = MT6797_MD1_HS_WAIT_HS1;
	hs->last_error = ret;
	mutex_unlock(&hs->lock);
	return ret;
}

static int mt6797_md1_hs_hs1(struct mt6797_md1_hs *hs,
			     const struct mt6797_md1_hs_frame *frame,
			     enum mt6797_md1_hs_action *action)
{
	if (frame->wire_size == MT6797_MD1_CCCI_HEADER_SIZE)
		*action = MT6797_MD1_HS_ACTION_HS1_LEGACY_NEEDS_RUNTIME;
	else if (frame->wire_size == MT6797_MD1_HS1_V2_FRAME_SIZE)
		*action = MT6797_MD1_HS_ACTION_HS1_V2_NEEDS_RUNTIME;
	else
		return -EMSGSIZE;

	hs->state = MT6797_MD1_HS_WAIT_HS2;
	hs->last_action = *action;
	return 0;
}

int mt6797_md1_hs_rx(struct mt6797_md1_hs *hs,
		     const struct mt6797_md1_hs_frame *frame,
		     enum mt6797_md1_hs_action *action)
{
	int ret = 0;

	if (!hs || !frame || !action)
		return -EINVAL;

	*action = MT6797_MD1_HS_ACTION_NONE;
	mutex_lock(&hs->lock);
	if (frame->wire_size < MT6797_MD1_CCCI_HEADER_SIZE) {
		ret = -EMSGSIZE;
		goto out;
	}
	if (frame->channel != MT6797_MD1_CCCI_CONTROL_RX) {
		ret = -ENOMSG;
		goto out;
	}

	switch (hs->state) {
	case MT6797_MD1_HS_OFF:
		ret = -EPERM;
		break;
	case MT6797_MD1_HS_WAIT_HS1:
		if (frame->data1 != 0 ||
		    frame->reserved != MT6797_MD1_MD_INIT_CHK_ID)
			ret = -ENOMSG;
		else
			ret = mt6797_md1_hs_hs1(hs, frame, action);
		break;
	case MT6797_MD1_HS_WAIT_HS2:
		/* port_ctlmsg gives a matching reserved word priority to HS1. */
		if (frame->data1 != 0 ||
		    frame->reserved == MT6797_MD1_MD_INIT_CHK_ID)
			ret = -ENOMSG;
		else {
			hs->state = MT6797_MD1_HS_READY;
			*action = MT6797_MD1_HS_ACTION_HS2_READY;
			hs->last_action = *action;
		}
		break;
	case MT6797_MD1_HS_READY:
		ret = -EALREADY;
		break;
	}
out:
	hs->last_error = ret;
	mutex_unlock(&hs->lock);
	return ret;
}

enum mt6797_md1_hs_state mt6797_md1_hs_state(struct mt6797_md1_hs *hs)
{
	enum mt6797_md1_hs_state state;

	if (!hs)
		return MT6797_MD1_HS_OFF;
	mutex_lock(&hs->lock);
	state = hs->state;
	mutex_unlock(&hs->lock);
	return state;
}
