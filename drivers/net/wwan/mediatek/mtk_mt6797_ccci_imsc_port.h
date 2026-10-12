/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_IMSC_PORT_H
#define _MTK_MT6797_CCCI_IMSC_PORT_H

#define MT6797_IMSC_TX 54U
#define MT6797_IMSC_RX 55U
#define MT6797_IMSC_QUEUE 6U
#define MT6797_IMSC_CCCI_HEADER 16U
#define MT6797_IMSC_ENVELOPE 64U
#define MT6797_IMSC_PAYLOAD 3456U

struct device;
struct sk_buff;
struct mt6797_imsc_port;

struct mt6797_imsc_backend {
	/*
	 * Called under the port TX mutex; context lives until destroy drains it.
	 * Zero means submitted, every negative result means not published and
	 * must never report a late error. The callback receives payload only.
	 */
	int (*send)(void *context, const u8 *payload, size_t bytes);
	bool (*room)(void *context);
};

struct mt6797_imsc_port *
mt6797_imsc_port_create(struct device *parent,
			const struct mt6797_imsc_backend *backend, void *context);
void mt6797_imsc_port_ready(struct mt6797_imsc_port *port, bool ready);
void mt6797_imsc_port_stop(struct mt6797_imsc_port *port, int error);
void mt6797_imsc_port_tx_wake(struct mt6797_imsc_port *port);
/* Success consumes skb; on failure the caller retains it. */
int mt6797_imsc_port_rx(struct mt6797_imsc_port *port, struct sk_buff *skb);
void mt6797_imsc_port_destroy(struct mt6797_imsc_port *port);

#endif
