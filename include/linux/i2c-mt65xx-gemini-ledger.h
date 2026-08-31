/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_I2C_MT65XX_GEMINI_LEDGER_H
#define __LINUX_I2C_MT65XX_GEMINI_LEDGER_H

#include <linux/types.h>

struct i2c_adapter;

struct mtk_i2c_gemini_read_expectation {
	u16 address;
	u8 register_pointer;
};

int mtk_i2c_gemini_verify_read_ledger(struct i2c_adapter *adapter,
				      const struct mtk_i2c_gemini_read_expectation *expected,
	unsigned int count);

#endif /* __LINUX_I2C_MT65XX_GEMINI_LEDGER_H */
