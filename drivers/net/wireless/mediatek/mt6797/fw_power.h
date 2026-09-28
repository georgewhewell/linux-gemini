/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_FW_POWER_H
#define __MT6797_WIFI_FW_POWER_H

#include <linux/types.h>

/* GEN3 include/config.h:368-370 and include/nic_cmd_event.h:77. */
#define MT6797_WIFI_POWER_OFF_POLLS		512U
#define MT6797_WIFI_POWER_OFF_COMMAND_POLLS	100U
#define MT6797_WIFI_POWER_OFF_INT_POLLS		20U
#define MT6797_WIFI_POWER_OFF_DRAIN_PASSES	32U
#define MT6797_WIFI_CMD_NIC_POWER_CTRL		0x04U
#define MT6797_WIFI_NIC_POWER_OFF		1U
#define MT6797_WIFI_NIC_POWER_CTRL_LENGTH	12U

/*
 * Hardware-free steps of GEN3 wlanAdapterStop(). Every callback performs one
 * bounded access. send_command returns -EAGAIN when no TX page was available
 * and nothing was written; drain reports whether an interrupt was pending.
 */
struct mt6797_wifi_power_off_ops {
	int (*send_command)(void *context);
	int (*drain)(void *context, bool *pending);
	int (*read_ready)(void *context, bool *ready);
	int (*request_interrupt)(void *context);
	int (*clear_mailbox)(void *context);
	void (*sleep_ms)(void *context, unsigned int ms);
	void *context;
};

struct mt6797_wifi_power_off_result {
	unsigned int polls;
	unsigned int interrupts;
	bool commanded;
};

int mt6797_wifi_power_off_command_build(u8 *command, size_t capacity,
					u8 sequence);
/*
 * Return 0 only after WCIR.READY was observed clear, which proves that the
 * RAM firmware stopped and a fresh download is valid. -ETIMEDOUT means the
 * firmware never released READY; the caller must then treat the chip as
 * needing a whole-CONSYS reset.
 */
int mt6797_wifi_power_off_run(const struct mt6797_wifi_power_off_ops *ops,
			      struct mt6797_wifi_power_off_result *result);

#endif /* __MT6797_WIFI_FW_POWER_H */
