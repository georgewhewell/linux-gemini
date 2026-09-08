/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_RADIO_H
#define __MT6797_WIFI_RADIO_H
struct device;
struct mt6797_wifi_hif_irq;
struct mt6797_wifi_capability;
struct mt6797_wifi_radio;
struct mt6797_wifi_radio *mt6797_wifi_radio_register(struct device *dev,
	struct mt6797_wifi_hif_irq *irq,
	const struct mt6797_wifi_capability *capability);
void mt6797_wifi_radio_unregister(struct mt6797_wifi_radio *radio);
#endif
