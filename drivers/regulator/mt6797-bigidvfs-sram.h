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

/* First-start backend: full words, not the ordinary voltage-only interface. */
struct mt6797_sram_sample {
	u32 control;
	u32 calibration;
};

struct mt6797_sram_first_io {
	int (*set_voltage)(void *context, unsigned int firmware_uv_div10);
	void (*settle)(void *context, unsigned int usec);
	int (*read_sample)(void *context, struct mt6797_sram_sample *sample);
};

struct mt6797_sram_first_state {
	bool attempted;
	bool ready;
	bool faulted;
	int error;
	struct mt6797_sram_sample samples[2];
};

/*
 * Provider-internal operation. Caller must hold the provider lock and establish
 * exclusive first-CPU8 ownership, both A72s offline, and isolation crossed.
 * No public caller may substitute this for ordinary regulator_set_voltage().
 */
int mt6797_sram_first_start(const struct mt6797_sram_first_io *io,
			  void *context, struct mt6797_sram_first_state *state);

#endif /* __MT6797_BIGIDVFS_SRAM_H */
