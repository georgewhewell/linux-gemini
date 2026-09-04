/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_BT_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_BT_H

#include <linux/types.h>

struct device;
struct mt6797_consys_bt_lease;

typedef int (*mt6797_consys_bt_rx_cb_t)(void *context, const u8 *frame,
					 size_t frame_length);

/* Acquire the aggregate's exclusive, patch-ready Bluetooth function. */
const struct mt6797_consys_bt_lease *
mt6797_consys_bt_acquire(struct device *consumer,
			 mt6797_consys_bt_rx_cb_t receive,
			 void *receive_context);
int mt6797_consys_bt_transmit(const struct mt6797_consys_bt_lease *lease,
			      const u8 *frame, size_t frame_length);
int mt6797_consys_bt_release(const struct mt6797_consys_bt_lease *lease);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_BT_H */
