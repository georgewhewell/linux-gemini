/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD1_POWER_TRANSACTION_H
#define __MT6797_MD1_POWER_TRANSACTION_H

#include <linux/types.h>

enum mt6797_md1_power_rail {
	MT6797_MD1_RAIL_VSRAM_MD,
	MT6797_MD1_RAIL_VMD1,
	MT6797_MD1_RAIL_VMODEM,
	MT6797_MD1_POWER_RAIL_COUNT,
};

enum mt6797_md1_rail_hw_control {
	/* Semantic controls: a platform adapter must provide exact readback. */
	MT6797_MD1_RAIL_ENABLE_HW_CONTROL,
	MT6797_MD1_RAIL_VOLTAGE_HW_CONTROL,
	MT6797_MD1_RAIL_HW_CONTROL_COUNT,
};

enum mt6797_md1_power_control {
	MT6797_MD1_CTRL_EXT_BUCK_ISO,
	MT6797_MD1_CTRL_PWR_ON,
	MT6797_MD1_CTRL_PWR_ON_2ND,
	MT6797_MD1_CTRL_PWR_CLK_DIS,
	MT6797_MD1_CTRL_PWR_ISO,
	MT6797_MD1_CTRL_SRAM_PDN,
	MT6797_MD1_CTRL_BUS_PROTECT,
	MT6797_MD1_CTRL_RESET_ASSERT,
	MT6797_MD1_POWER_CONTROL_COUNT,
};

#define MT6797_MD1_POWER_MUTATION_COUNT \
	(MT6797_MD1_POWER_RAIL_COUNT * 3 + 2 + \
	 MT6797_MD1_POWER_CONTROL_COUNT)
#define MT6797_MD1_REQUIRED_UV		1000000
#define MT6797_MD1_RAIL_SETTLE_US	300

struct mt6797_md1_power_ops {
	int (*set_rail)(void *context, enum mt6797_md1_power_rail rail,
			bool enable);
	int (*set_rail_voltage)(void *context,
				enum mt6797_md1_power_rail rail, int voltage_uv);
	int (*set_rail_hw_control)(void *context,
				   enum mt6797_md1_power_rail rail,
				   enum mt6797_md1_rail_hw_control control,
				   bool enable);
	int (*set_control)(void *context,
			   enum mt6797_md1_power_control control,
			   bool assert);
	int (*read_rail)(void *context, enum mt6797_md1_power_rail rail,
			 bool *enabled);
	int (*read_rail_voltage)(void *context,
				 enum mt6797_md1_power_rail rail,
				 int *voltage_uv);
	int (*read_rail_hw_control)(void *context,
				    enum mt6797_md1_power_rail rail,
				    enum mt6797_md1_rail_hw_control control,
				    bool *enabled);
	int (*read_control)(void *context,
			    enum mt6797_md1_power_control control,
			    bool *asserted);
	int (*read_power_ack)(void *context, bool *primary, bool *secondary);
	int (*read_bus_protected)(void *context, bool *protected);
	int (*settle_rails)(void *context, unsigned int usec);
	void (*ack_delay)(void *context);
};

/*
 * Exercise only the evidenced power path, then restore its exact inherited
 * rail contract and verified-off domain state. Hardware register, regulator
 * and delay operations are deliberately supplied by a future adapter.
 */
int mt6797_md1_power_transaction(const struct mt6797_md1_power_ops *ops,
				 void *context, unsigned int ack_attempts);

#endif /* __MT6797_MD1_POWER_TRANSACTION_H */
