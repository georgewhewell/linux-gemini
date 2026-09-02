/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 MD1 CCCI HS1/HS2 wire-contract classifier.
 *
 * This is deliberately only the control-plane state contract after a future
 * owner has observed post-vector MD1 activity. It owns no HIF, IRQ, CLDMA
 * descriptor, shared-memory, runtime-data, power, reset, SIM, or RF resource.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_HS_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_HS_H

#include <linux/mutex.h>
#include <linux/types.h>

#define MT6797_MD1_HS_ABI 1
#define MT6797_MD1_CCCI_HEADER_SIZE 16U
#define MT6797_MD1_CCCI_CONTROL_RX 0U
#define MT6797_MD1_CCCI_CONTROL_TX 1U
#define MT6797_MD1_MD_INIT_CHK_ID 0x5555ffffU
#define MT6797_MD1_MD_QUERY_FEATURE_COUNT 64U
/* u32 head + 64 one-byte feature masks + u32 tail from vendor ccci_core.h. */
#define MT6797_MD1_MD_QUERY_AP_FEATURE_SIZE 72U
#define MT6797_MD1_HS1_V2_FRAME_SIZE \
	(MT6797_MD1_CCCI_HEADER_SIZE + MT6797_MD1_MD_QUERY_AP_FEATURE_SIZE)

enum mt6797_md1_hs_state {
	MT6797_MD1_HS_OFF,
	MT6797_MD1_HS_WAIT_HS1,
	MT6797_MD1_HS_WAIT_HS2,
	MT6797_MD1_HS_READY,
	MT6797_MD1_HS_CRASHED,
};

enum mt6797_md1_hs_action {
	MT6797_MD1_HS_ACTION_NONE,
	/* A future transport must construct and send the matching AP runtime data. */
	MT6797_MD1_HS_ACTION_HS1_LEGACY_NEEDS_RUNTIME,
	MT6797_MD1_HS_ACTION_HS1_V2_NEEDS_RUNTIME,
	MT6797_MD1_HS_ACTION_HS2_READY,
};

/* Decoded CCCI header fields plus the complete received frame length. */
struct mt6797_md1_hs_frame {
	u32 data0;
	u32 data1;
	u16 channel;
	u32 reserved;
	size_t wire_size;
};

struct mt6797_md1_hs {
	struct mutex lock;
	enum mt6797_md1_hs_state state;
	enum mt6797_md1_hs_action last_action;
	int last_error;
};

int mt6797_md1_hs_init(struct mt6797_md1_hs *hs);
/* post_vector_activity must be independently observed by the future HIF owner. */
int mt6797_md1_hs_arm(struct mt6797_md1_hs *hs, bool post_vector_activity);
int mt6797_md1_hs_rx(struct mt6797_md1_hs *hs,
		     const struct mt6797_md1_hs_frame *frame,
		     enum mt6797_md1_hs_action *action);
int mt6797_md1_hs_crash(struct mt6797_md1_hs *hs, int reason);
int mt6797_md1_hs_reset(struct mt6797_md1_hs *hs);
enum mt6797_md1_hs_state mt6797_md1_hs_state(struct mt6797_md1_hs *hs);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_HS_H */
