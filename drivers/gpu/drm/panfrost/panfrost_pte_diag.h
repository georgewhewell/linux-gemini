/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PANFROST_PTE_DIAG_H
#define PANFROST_PTE_DIAG_H

#define PANFROST_PTE_BO_LIMIT 8U

static inline unsigned int panfrost_pte_bo_count(unsigned int count)
{
	return count < PANFROST_PTE_BO_LIMIT ? count : PANFROST_PTE_BO_LIMIT;
}

static inline bool panfrost_pte_iova_valid(unsigned long long va,
					unsigned int bits)
{
	/* Avoid end=iova+1 wrapping inside io-pgtable's walker. */
	return bits && bits < 64 && va < (1ULL << bits);
}

#endif
