/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_MT6797_HS_H
#define __MTK_MT6797_HS_H

#include <linux/spinlock.h>

#include "mtk_ccci_common.h"

enum mtk_mt6797_hs_state {
	MTK_MT6797_HS_OFF,
	MTK_MT6797_HS_WAIT_HS1,
	MTK_MT6797_HS_WAIT_HS2,
	MTK_MT6797_HS_READY,
	MTK_MT6797_HS_CRASHED,
};

enum mtk_mt6797_hs_action {
	MTK_MT6797_HS_ACTION_NONE,
	MTK_MT6797_HS_ACTION_HS1_LEGACY,
	MTK_MT6797_HS_ACTION_HS1_V2,
	MTK_MT6797_HS_ACTION_HS2_READY,
};

struct mtk_mt6797_hs {
	/* Serializes start, receive, crash and reset transitions. */
	spinlock_t lock;
	enum mtk_mt6797_hs_state state;
};

int mtk_mt6797_hs_init(struct mtk_mt6797_hs *hs);
int mtk_mt6797_hs_start(struct mtk_mt6797_hs *hs);
int mtk_mt6797_hs_rx(struct mtk_mt6797_hs *hs,
		     const struct mtk_ccci_frame *frame, size_t wire_size,
		     enum mtk_mt6797_hs_action *action);
int mtk_mt6797_hs_crash(struct mtk_mt6797_hs *hs);
int mtk_mt6797_hs_reset(struct mtk_mt6797_hs *hs);
enum mtk_mt6797_hs_state mtk_mt6797_hs_state(struct mtk_mt6797_hs *hs);

#endif /* __MTK_MT6797_HS_H */
