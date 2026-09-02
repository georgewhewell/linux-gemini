/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6797 WLAN GEN3 persistent-HIF session contract. */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_WIFI_HIF_SESSION_H
#define __LINUX_SOC_MEDIATEK_MT6797_WIFI_HIF_SESSION_H

#include <linux/types.h>
#include <linux/bitops.h>

#define MT6797_WIFI_HIF_TX_QUEUE_COUNT	8U
#define MT6797_WIFI_HIF_RX_PACKET_MAX	2352U
#define MT6797_WIFI_HIF_RX_DRAIN_MAX_PACKETS	32U

#define MT6797_WIFI_HIF_MCR_WHLPCR	0x0004U
#define MT6797_WIFI_HIF_MCR_WHCR		0x000cU
#define MT6797_WIFI_HIF_MCR_WHISR		0x0010U
#define MT6797_WIFI_HIF_WHLPCR_INT_CLR	BIT(1)
#define MT6797_WIFI_HIF_WHLPCR_INT_SET	BIT(0)
#define MT6797_WIFI_HIF_WHCR_INT_READ_CLEAR	BIT(1)
#define MT6797_WIFI_HIF_WHISR_TX_DONE	BIT(0)

/*
 * The future platform owner supplies all stateful operations.  request_irq()
 * must leave the host line masked; irq_enable() is permitted only after the
 * core writes WHLPCR.INT_SET.  capability_query() is the exact capability vendor
 * polling exchange, and configure() represents the still-unimplemented basic
 * configuration and MAC-apply commands.  Neither callback may register a
 * netdev or cfg80211 device.
 */
struct mt6797_wifi_hif_ops {
	int (*hif_get)(void *context);
	void (*hif_put)(void *context);
	int (*host_irq_request)(void *context);
	void (*host_irq_free)(void *context);
	int (*host_irq_enable)(void *context);
	int (*host_irq_disable)(void *context);
	int (*mcr_read)(void *context, u32 offset, u32 *value);
	int (*mcr_write)(void *context, u32 offset, u32 value);
	int (*tx_released)(void *context,
			u32 released[MT6797_WIFI_HIF_TX_QUEUE_COUNT]);
	int (*rx_lengths)(void *context, u16 *rx0_length, u16 *rx1_length);
	int (*rx_drain)(void *context, unsigned int port, u16 packet_length);
	int (*capability_query)(void *context);
	int (*configure)(void *context);
};

struct mt6797_wifi_hif_session {
	const struct mt6797_wifi_hif_ops *ops;
	void *context;
	u32 tx_released[MT6797_WIFI_HIF_TX_QUEUE_COUNT];
	u32 last_whisr;
	unsigned int tx_resource_resets;
	unsigned int rx0_drained;
	unsigned int rx1_drained;
	int causal_error;
	int cleanup_error;
	bool hif_owned;
	bool host_irq_requested;
	bool host_irq_enabled;
	bool device_irq_enabled;
	bool active;
};

/* Start a HIF session after the caller has established HIF ownership. */
int mt6797_wifi_hif_session_start(struct mt6797_wifi_hif_session *session,
		const struct mt6797_wifi_hif_ops *ops, void *context);

/* Masks host then device IRQ, acknowledges/drains, and releases every owner. */
int mt6797_wifi_hif_session_stop(struct mt6797_wifi_hif_session *session);
#endif /* __LINUX_SOC_MEDIATEK_MT6797_WIFI_HIF_SESSION_H */
