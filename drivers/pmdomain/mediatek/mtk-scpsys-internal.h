/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_SCPSYS_INTERNAL_H
#define __MTK_SCPSYS_INTERNAL_H

#include <linux/types.h>

static inline bool mtk_scpsys_bus_prot_group_is_on(u32 status, u32 status2, u32 mask)
{
	return (status & mask) == mask && (status2 & mask) == mask;
}

#endif /* __MTK_SCPSYS_INTERNAL_H */
