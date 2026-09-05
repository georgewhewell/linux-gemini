/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __DRV_CLK_MTK_CLK_MT6797_RESET_H
#define __DRV_CLK_MTK_CLK_MT6797_RESET_H

#include <dt-bindings/reset/mt6797-resets.h>

#include "reset.h"

/* Only reset paths confirmed by exact MT6797 sources are exposed. */
static u16 infra_rst_ofs[] = {
	INFRA_RST0_SET_OFFSET,
	INFRA_RST2_SET_OFFSET,
};

static u16 infra_rst_idx_map[] = {
	[MT6797_INFRA_THERM_CTRL_RST] = 0 * RST_NR_PER_BANK,
	[MT6797_INFRA_PMIC_WRAP_RST] = 1 * RST_NR_PER_BANK,
	[MT6797_INFRA_CLDMA_RST] = 0 * RST_NR_PER_BANK + 13,
	[MT6797_INFRA_CLDMA_AO_RST] = 1 * RST_NR_PER_BANK + 4,
};

static const struct mtk_clk_rst_desc infra_rst_desc = {
	.version = MTK_RST_SET_CLR,
	.rst_bank_ofs = infra_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(infra_rst_ofs),
	.rst_idx_map = infra_rst_idx_map,
	.rst_idx_map_nr = ARRAY_SIZE(infra_rst_idx_map),
};

#endif /* __DRV_CLK_MTK_CLK_MT6797_RESET_H */
