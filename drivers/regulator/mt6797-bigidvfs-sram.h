/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_BIGIDVFS_SRAM_H
#define __MT6797_BIGIDVFS_SRAM_H

#include <linux/types.h>

#define MT6797_BIGIDVFS_SRAM_SELECTOR_COUNT	16

struct mt6797_sram_io {
	int (*read_selector)(void *context, unsigned int *selector);
	int (*set_voltage)(void *context, unsigned int firmware_uv_div10);
};

int mt6797_sram_set_transaction(const struct mt6797_sram_io *io,
				void *context, const unsigned int *voltages,
				unsigned int count, unsigned int selector);

#endif /* __MT6797_BIGIDVFS_SRAM_H */
