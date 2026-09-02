// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 WLAN GEN3 persistent HIF-session core.
 *
 * Vendor wlan_lib.c:512-537 orders post-READY acknowledgement/TX reset,
 * NIC capability, basic configuration, and MAC application.  Lines 671-677
 * then select interrupt read-clear before nicEnableInterrupt().  nic.c:348-385
 * writes WHLPCR.INT_SET before enabling the host IRQ.  This core is only that
 * lifecycle contract; it owns no platform device, regulator, CONSYS lease,
 * firmware, HIF mapping, IRQ, netdev, cfg80211 object, or packet dispatcher.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "hif.h"

static int mt6797_wifi_hif_drain_rx(struct mt6797_wifi_hif_session *session)
{
	unsigned int i;
	int ret;

	for (i = 0; i < MT6797_WIFI_HIF_RX_DRAIN_MAX_PACKETS; i++) {
		u16 rx0_length;
		u16 rx1_length;

		ret = session->ops->rx_lengths(session->context, &rx0_length,
			&rx1_length);
		if (ret)
			return ret;
		if (!rx0_length && !rx1_length)
			return 0;
		if (rx0_length) {
			if (rx0_length > MT6797_WIFI_HIF_RX_PACKET_MAX)
				return -EMSGSIZE;
			ret = session->ops->rx_drain(session->context, 0, rx0_length);
			if (ret)
				return ret;
			session->rx0_drained++;
		}
		if (rx1_length) {
			if (rx1_length > MT6797_WIFI_HIF_RX_PACKET_MAX)
				return -EMSGSIZE;
			ret = session->ops->rx_drain(session->context, 1, rx1_length);
			if (ret)
				return ret;
			session->rx1_drained++;
		}
	}
	return -EOVERFLOW;
}

static int mt6797_wifi_hif_ack_and_reset_tx(struct mt6797_wifi_hif_session *session)
{
	int ret;

	ret = session->ops->mcr_read(session->context, MT6797_WIFI_HIF_MCR_WHISR,
		&session->last_whisr);
	if (ret)
		return ret;
	if (session->last_whisr & MT6797_WIFI_HIF_WHISR_TX_DONE) {
		ret = session->ops->tx_released(session->context, session->tx_released);
		if (ret)
			return ret;
	}
	/* wlanQueryNicResourceInformation() resets software accounting. */
	session->tx_resource_resets++;
	return 0;
}

static int mt6797_wifi_hif_select_read_clear(struct mt6797_wifi_hif_session *session)
{
	u32 whcr;
	int ret;

	ret = session->ops->mcr_read(session->context, MT6797_WIFI_HIF_MCR_WHCR,
		&whcr);
	if (ret)
		return ret;
	return session->ops->mcr_write(session->context, MT6797_WIFI_HIF_MCR_WHCR,
		whcr & ~MT6797_WIFI_HIF_WHCR_INT_READ_CLEAR);
}

static int mt6797_wifi_hif_stop_owned(struct mt6797_wifi_hif_session *session)
{
	int causal = 0;
	int ret;

	if (session->host_irq_enabled) {
		ret = session->ops->host_irq_disable(session->context);
		if (ret && !causal)
			causal = ret;
		session->host_irq_enabled = false;
	}
	if (session->device_irq_enabled) {
		ret = session->ops->mcr_write(session->context,
			MT6797_WIFI_HIF_MCR_WHLPCR,
			MT6797_WIFI_HIF_WHLPCR_INT_CLR);
		if (ret && !causal)
			causal = ret;
		session->device_irq_enabled = false;
	}
	ret = mt6797_wifi_hif_ack_and_reset_tx(session);
	if (ret && !causal)
		causal = ret;
	ret = mt6797_wifi_hif_drain_rx(session);
	if (ret && !causal)
		causal = ret;
	if (session->host_irq_requested) {
		session->ops->host_irq_free(session->context);
		session->host_irq_requested = false;
	}
	if (session->hif_owned) {
		session->ops->hif_put(session->context);
		session->hif_owned = false;
	}
	session->active = false;
	return causal;
}

int mt6797_wifi_hif_session_start(struct mt6797_wifi_hif_session *session,
		const struct mt6797_wifi_hif_ops *ops, void *context)
{
	int ret;

	if (!session || !ops || !ops->hif_get || !ops->hif_put ||
	    !ops->host_irq_request || !ops->host_irq_free || !ops->host_irq_enable ||
	    !ops->host_irq_disable || !ops->mcr_read || !ops->mcr_write ||
	    !ops->tx_released || !ops->rx_lengths || !ops->rx_drain ||
	    !ops->capability_query || !ops->configure)
		return -EINVAL;
	if (session->hif_owned || session->host_irq_requested || session->active)
		return -EBUSY;
	memset(session, 0, sizeof(*session));
	session->ops = ops;
	session->context = context;
	ret = ops->hif_get(context);
	if (ret)
		goto out;
	session->hif_owned = true;
	ret = ops->host_irq_request(context);
	if (ret)
		goto out_stop;
	session->host_irq_requested = true;
	ret = mt6797_wifi_hif_ack_and_reset_tx(session);
	if (ret)
		goto out_stop;
	ret = mt6797_wifi_hif_drain_rx(session);
	if (ret)
		goto out_stop;
	ret = ops->capability_query(context);
	if (ret)
		goto out_stop;
	ret = mt6797_wifi_hif_drain_rx(session);
	if (ret)
		goto out_stop;
	ret = ops->configure(context);
	if (ret)
		goto out_stop;
	ret = mt6797_wifi_hif_select_read_clear(session);
	if (ret)
		goto out_stop;
	/* Match nicEnableInterrupt(): ready software state before device INT_SET. */
	session->active = true;
	ret = ops->mcr_write(context, MT6797_WIFI_HIF_MCR_WHLPCR,
		MT6797_WIFI_HIF_WHLPCR_INT_SET);
	if (ret)
		goto out_stop;
	session->device_irq_enabled = true;
	ret = ops->host_irq_enable(context);
	if (ret)
		goto out_stop;
	session->host_irq_enabled = true;
	return 0;

out_stop:
	session->cleanup_error = mt6797_wifi_hif_stop_owned(session);
out:
	session->causal_error = ret;
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_hif_session_start);

int mt6797_wifi_hif_session_stop(struct mt6797_wifi_hif_session *session)
{
	int ret;

	if (!session || !session->ops)
		return -EINVAL;
	ret = mt6797_wifi_hif_stop_owned(session);
	session->cleanup_error = ret;
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_hif_session_stop);

MODULE_DESCRIPTION("MediaTek MT6797 WLAN persistent HIF session core");
MODULE_LICENSE("GPL");
