/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WIFI_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WIFI_H

struct device;
struct resource;
struct mt6797_consys_wifi_lease;

/*
 * Acquire the aggregate's exclusive, patch-ready integrated WLAN function.
 *
 * Unlike a discrete combo chip, MT6797 does not send WMT FUNC_CTRL for WLAN.
 * The lease owns the Wi-Fi VCN33 vote while the endpoint owns firmware and
 * the AHB SDIO-like interface.  It is valid only for a child of the aggregate.
 */
const struct mt6797_consys_wifi_lease *
mt6797_consys_wifi_acquire(struct device *consumer);
int
mt6797_consys_wifi_emi_window_open(const struct mt6797_consys_wifi_lease *lease,
				   struct resource *window);
int
mt6797_consys_wifi_emi_window_close(const struct mt6797_consys_wifi_lease *lease);
void mt6797_consys_wifi_fence(const struct mt6797_consys_wifi_lease *lease);
int mt6797_consys_wifi_release(const struct mt6797_consys_wifi_lease *lease);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WIFI_H */
