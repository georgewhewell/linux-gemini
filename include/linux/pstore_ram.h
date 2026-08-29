/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2010 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright (C) 2011 Kees Cook <keescook@chromium.org>
 * Copyright (C) 2011 Google, Inc.
 */

#ifndef __LINUX_PSTORE_RAM_H__
#define __LINUX_PSTORE_RAM_H__

#include <linux/pstore.h>

struct persistent_ram_ecc_info {
	int block_size;
	int ecc_size;
	int symsize;
	int poly;
	uint16_t *par;
};

/*
 * Ramoops platform data
 * @mem_size	memory size for ramoops
 * @mem_address	physical memory address to contain ramoops
 */

#define RAMOOPS_FLAG_FTRACE_PER_CPU	BIT(0)

struct ramoops_platform_data {
	unsigned long	mem_size;
	phys_addr_t	mem_address;
	unsigned int	mem_type;
	unsigned long	record_size;
	unsigned long	console_size;
	unsigned long	ftrace_size;
	unsigned long	pmsg_size;
	int		max_reason;
	u32		flags;
	struct persistent_ram_ecc_info ecc_info;
};

#ifdef CONFIG_PSTORE_GEMINI_PRE_RAMOOPS_LEDGER
void gemini_pre_ramoops_ledger_setup_checkpoint(void);
#else
static inline void gemini_pre_ramoops_ledger_setup_checkpoint(void) { }
#endif

#ifdef CONFIG_PSTORE_GEMINI_ARM64_ENTRY_LEDGER
void gemini_arm64_entry_ledger_post_mmu_checkpoint(void);
void gemini_arm64_entry_ledger_post_reserved_checkpoint(void);
#else
static inline void gemini_arm64_entry_ledger_post_mmu_checkpoint(void) { }
static inline void gemini_arm64_entry_ledger_post_reserved_checkpoint(void)
{
}
#endif

#endif
