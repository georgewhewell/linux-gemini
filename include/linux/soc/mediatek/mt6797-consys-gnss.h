/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_GNSS_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_GNSS_H

#include <linux/types.h>

struct device;
struct mt6797_consys_gnss_lease;
struct mt6797_consys_stp_session_lease;

/* Borrow the current aggregate session; this does not power GPS on. */
const struct mt6797_consys_gnss_lease *
mt6797_consys_gnss_acquire(struct device *consumer,
		const struct mt6797_consys_stp_session_lease **session);
int mt6797_consys_gnss_set_power(const struct mt6797_consys_gnss_lease *lease,
			       bool enable);
int mt6797_consys_gnss_transmit(const struct mt6797_consys_gnss_lease *lease,
			      const u8 *payload, size_t length);
int mt6797_consys_gnss_release(const struct mt6797_consys_gnss_lease *lease);

#endif
