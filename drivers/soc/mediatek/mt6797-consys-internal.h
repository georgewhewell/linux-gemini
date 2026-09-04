/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_CONSYS_INTERNAL_H
#define __MT6797_CONSYS_INTERNAL_H

#include <linux/types.h>

struct device;
struct mt6797_consys_fw_bundle;
struct mt6797_consys_stp_session;
struct mt6797_consys_runtime_lease;
struct mt6797_consys_stp_session_lease;
struct mt6797_btif_stp_bootstrap_full_wmt_lease;
struct mtk_wmt_match;

typedef int (*mt6797_consys_btif_stage_t)(void *context,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease);

struct mt6797_consys_btif_ops {
	int (*transmit)(void *context, const u8 *frame, size_t frame_length);
	int (*bind_session)(void *context,
			    struct mt6797_consys_stp_session *session);
	void (*unbind_session)(void *context,
			       struct mt6797_consys_stp_session *session);
	int (*start)(void *context,
		     const struct mt6797_consys_runtime_lease *runtime_lease,
		     mt6797_consys_btif_stage_t stage, void *stage_context,
		     const struct mt6797_consys_stp_session_lease **session_lease);
	int (*wmt_transaction)(void *context,
		     const struct mt6797_consys_stp_session_lease *session_lease,
		     u8 opcode, const u8 *data, size_t data_len,
		     const struct mtk_wmt_match *expected,
		     unsigned long timeout);
	int (*arm_session_timeout)(void *context,
		     const struct mt6797_consys_stp_session_lease *session_lease);
	void (*stop)(void *context);
};

int mt6797_consys_btif_attach(struct device *master, struct device *supplier,
			      const struct mt6797_consys_btif_ops *ops,
			      void *context);
void mt6797_consys_btif_detach(struct device *master, struct device *supplier);

#endif /* __MT6797_CONSYS_INTERNAL_H */
