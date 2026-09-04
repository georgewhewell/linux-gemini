/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_POSTPATCH_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_POSTPATCH_H

#include <linux/types.h>

struct mtk_wmt_match;

struct mt6797_consys_postpatch_ops {
	int (*get_voltage)(void *context, bool wifi);
	int (*is_enabled)(void *context, bool wifi);
	int (*set_voltage)(void *context, bool wifi, int microvolts);
	int (*enable)(void *context, bool wifi);
	int (*disable)(void *context, bool wifi);
	int (*wmt)(void *context, u8 opcode, const u8 *data, size_t data_len,
		   const struct mtk_wmt_match *expected);
	int (*wmt_capture)(void *context, u8 opcode, const u8 *data,
			   size_t data_len, u8 expected_opcode,
			   size_t min_data_len, size_t max_data_len,
			   u8 *reply, size_t reply_len,
			   size_t *actual_data_len);
	void (*delay_ms)(void *context, unsigned int milliseconds);
	void *context;
};

/*
 * Complete the vendor-evidenced common post-patch sequence.  Both VCN33
 * consumers must be off on entry.  The helper leaves both off on success and
 * attempts bounded reverse cleanup on every failure.
 */
int mt6797_consys_postpatch_run(const struct mt6797_consys_postpatch_ops *ops);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_POSTPATCH_H */
