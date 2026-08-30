/* SPDX-License-Identifier: GPL-2.0-only */
/* Dormant hardware-neutral MT6797 CONSYS EMI-to-post-ID coordinator. */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_HANDOFF_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_HANDOFF_H

#include <linux/mutex.h>
#include <linux/soc/mediatek/mt6797-consys-emi.h>
#include <linux/soc/mediatek/mt6797-consys-postid.h>
#include <linux/soc/mediatek/mt6797-consys-runtime.h>

enum mt6797_consys_handoff_state {
	MT6797_CONSYS_HANDOFF_IDLE,
	MT6797_CONSYS_HANDOFF_STARTING,
	MT6797_CONSYS_HANDOFF_ACTIVE,
	MT6797_CONSYS_HANDOFF_QUIESCED,
	MT6797_CONSYS_HANDOFF_ERROR,
};

struct mt6797_consys_handoff_owner {
	struct mt6797_consys_emi_owner *emi;
	struct mt6797_consys_postid_owner *postid;
	struct device *runtime_dev;
	struct mt6797_consys_runtime_resource_lease resources;
	struct mt6797_consys_runtime_provider_lease identity;
	struct mutex lock;
	enum mt6797_consys_handoff_state state;
	bool resources_held;
	bool identity_held;
};

int mt6797_consys_handoff_init(struct mt6797_consys_handoff_owner *owner,
		struct mt6797_consys_emi_owner *emi,
		struct mt6797_consys_postid_owner *postid,
		struct device *runtime_dev);
int mt6797_consys_handoff_start(struct mt6797_consys_handoff_owner *owner,
		const struct mt6797_consys_emi_layout *layout, bool experiment_gate);
int mt6797_consys_handoff_stop(struct mt6797_consys_handoff_owner *owner);
enum mt6797_consys_handoff_state
mt6797_consys_handoff_state(struct mt6797_consys_handoff_owner *owner);

#endif
