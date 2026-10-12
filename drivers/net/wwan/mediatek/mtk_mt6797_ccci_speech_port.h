/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_SPEECH_PORT_H
#define _MTK_MT6797_CCCI_SPEECH_PORT_H

struct device;
struct sk_buff;
struct mt6797_speech_port;

struct mt6797_speech_backend {
	/* Called under the port TX mutex; context lives until destroy drains it.
	 * Zero means submitted, every negative result means not published and
	 * permits restoring a reserved request credit. Never return a late error. */
	int (*send)(void *context, const u8 *frame, size_t bytes);
	bool (*room)(void *context);
};

struct mt6797_speech_port *mt6797_speech_port_create(struct device *parent,
		const struct mt6797_speech_backend *backend, void *context);
void mt6797_speech_port_ready(struct mt6797_speech_port *port, bool ready);
void mt6797_speech_port_stop(struct mt6797_speech_port *port, int error);
void mt6797_speech_port_tx_wake(struct mt6797_speech_port *port);
/* Success consumes skb; on failure the caller retains it. */
int mt6797_speech_port_rx(struct mt6797_speech_port *port, struct sk_buff *skb);
void mt6797_speech_port_destroy(struct mt6797_speech_port *port);

#endif
