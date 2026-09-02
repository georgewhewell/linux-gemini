// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 CCCI boot-handshake state machine. */

#include <linux/errno.h>
#include <linux/string.h>

#include "mtk_mt6797_hs.h"

#define MTK_MT6797_HS1_V2_SIZE \
	(MTK_CCCI_HEADER_SIZE + MTK_CCCI_FEATURE_QUERY_SIZE)

int mtk_mt6797_hs_init(struct mtk_mt6797_hs *hs)
{
	if (!hs)
		return -EINVAL;

	memset(hs, 0, sizeof(*hs));
	spin_lock_init(&hs->lock);
	hs->state = MTK_MT6797_HS_OFF;

	return 0;
}

int mtk_mt6797_hs_start(struct mtk_mt6797_hs *hs)
{
	unsigned long flags;
	int ret = 0;

	if (!hs)
		return -EINVAL;

	spin_lock_irqsave(&hs->lock, flags);
	if (hs->state != MTK_MT6797_HS_OFF)
		ret = -EALREADY;
	else
		hs->state = MTK_MT6797_HS_WAIT_HS1;
	spin_unlock_irqrestore(&hs->lock, flags);

	return ret;
}

static int mtk_mt6797_hs_hs1(struct mtk_mt6797_hs *hs, size_t wire_size,
			     enum mtk_mt6797_hs_action *action)
{
	if (wire_size == MTK_CCCI_HEADER_SIZE)
		*action = MTK_MT6797_HS_ACTION_HS1_LEGACY;
	else if (wire_size == MTK_MT6797_HS1_V2_SIZE)
		*action = MTK_MT6797_HS_ACTION_HS1_V2;
	else
		return -EMSGSIZE;

	hs->state = MTK_MT6797_HS_WAIT_HS2;

	return 0;
}

int mtk_mt6797_hs_rx(struct mtk_mt6797_hs *hs,
		     const struct mtk_ccci_frame *frame, size_t wire_size,
		     enum mtk_mt6797_hs_action *action)
{
	unsigned long flags;
	int ret = 0;

	if (!hs || !frame || !action)
		return -EINVAL;

	*action = MTK_MT6797_HS_ACTION_NONE;
	spin_lock_irqsave(&hs->lock, flags);
	if (wire_size < MTK_CCCI_HEADER_SIZE) {
		ret = -EMSGSIZE;
		goto unlock;
	}
	if (frame->channel != MTK_CCCI_CONTROL_RX) {
		ret = -ENOMSG;
		goto unlock;
	}

	switch (hs->state) {
	case MTK_MT6797_HS_OFF:
		ret = -EPERM;
		break;
	case MTK_MT6797_HS_WAIT_HS1:
		if (frame->data1 || frame->reserved != MTK_CCCI_MD_INIT_CHK_ID)
			ret = -ENOMSG;
		else
			ret = mtk_mt6797_hs_hs1(hs, wire_size, action);
		break;
	case MTK_MT6797_HS_WAIT_HS2:
		if (frame->data1 || frame->reserved == MTK_CCCI_MD_INIT_CHK_ID) {
			ret = -ENOMSG;
		} else {
			hs->state = MTK_MT6797_HS_READY;
			*action = MTK_MT6797_HS_ACTION_HS2_READY;
		}
		break;
	case MTK_MT6797_HS_READY:
		ret = -EALREADY;
		break;
	case MTK_MT6797_HS_CRASHED:
		ret = -ESHUTDOWN;
		break;
	}

unlock:
	spin_unlock_irqrestore(&hs->lock, flags);

	return ret;
}

int mtk_mt6797_hs_crash(struct mtk_mt6797_hs *hs)
{
	unsigned long flags;
	int ret = 0;

	if (!hs)
		return -EINVAL;

	spin_lock_irqsave(&hs->lock, flags);
	if (hs->state == MTK_MT6797_HS_OFF)
		ret = -EPERM;
	else
		hs->state = MTK_MT6797_HS_CRASHED;
	spin_unlock_irqrestore(&hs->lock, flags);

	return ret;
}

int mtk_mt6797_hs_reset(struct mtk_mt6797_hs *hs)
{
	unsigned long flags;

	if (!hs)
		return -EINVAL;

	spin_lock_irqsave(&hs->lock, flags);
	hs->state = MTK_MT6797_HS_OFF;
	spin_unlock_irqrestore(&hs->lock, flags);

	return 0;
}

enum mtk_mt6797_hs_state mtk_mt6797_hs_state(struct mtk_mt6797_hs *hs)
{
	enum mtk_mt6797_hs_state state;
	unsigned long flags;

	if (!hs)
		return MTK_MT6797_HS_OFF;

	spin_lock_irqsave(&hs->lock, flags);
	state = hs->state;
	spin_unlock_irqrestore(&hs->lock, flags);

	return state;
}
