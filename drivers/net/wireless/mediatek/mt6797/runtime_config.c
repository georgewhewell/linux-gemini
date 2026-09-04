// SPDX-License-Identifier: GPL-2.0-only
/* Transactional post-capability configuration for MT6797 GEN3 WLAN. */

#include <linux/device.h>
#include <linux/errno.h>

#include <linux/soc/mediatek/mt6797-consys-wifi.h>

#include "boot.h"
#include "hif.h"
#include "protocol.h"
#include "runtime_config.h"

#define MT6797_WIFI_MCR_WTDR1		0x0034
#define MT6797_WIFI_BASIC_CONFIG_SEQUENCE	2U

static bool
mt6797_wifi_config_ops_valid(const struct mt6797_wifi_config_ops *ops)
{
	return ops && ops->precheck && ops->send && ops->teardown &&
		ops->fence;
}

int
mt6797_wifi_config_transaction_run(const struct mt6797_wifi_config_ops *ops,
				   struct mt6797_wifi_config_transaction *transaction)
{
	int ret;

	if (!mt6797_wifi_config_ops_valid(ops) || !transaction)
		return -EINVAL;
	if (transaction->dirty || transaction->configured)
		return -EALREADY;

	ret = ops->precheck(ops->context);
	if (ret) {
		if (ret == -EUCLEAN)
			ops->fence(ops->context);
		goto teardown;
	}

	/* A portal-write attempt may change firmware state without an ACK. */
	transaction->dirty = true;
	ret = ops->send(ops->context);
	if (ret) {
		ops->fence(ops->context);
		goto teardown;
	}
	transaction->configured = true;

	return 0;

teardown:
	/* boot_stop() owns rollback and any teardown-time fencing. */
	ops->teardown(ops->context);
	return ret;
}

static int mt6797_wifi_runtime_config_precheck(void *context)
{
	struct mt6797_wifi_runtime_config *config = context;
	struct mt6797_wifi_boot *boot = config->boot;

	if (!boot->online || !boot->transaction.ready || !boot->lease ||
	    boot->hif != config->hif)
		return -EPIPE;

	return 0;
}

static int mt6797_wifi_runtime_config_send(void *context)
{
	struct mt6797_wifi_runtime_config *config = context;
	u8 command[MT6797_WIFI_BASIC_CONFIG_LENGTH];
	int ret;

	ret = mt6797_wifi_basic_config_build(command, sizeof(command),
					     MT6797_WIFI_BASIC_CONFIG_SEQUENCE);
	if (ret)
		return ret;

	return mt6797_wifi_ahb_cmd53_write(config->hif,
					 MT6797_WIFI_MCR_WTDR1, command,
					 sizeof(command));
}

static int mt6797_wifi_runtime_config_teardown(void *context)
{
	struct mt6797_wifi_runtime_config *config = context;
	int ret;

	ret = mt6797_wifi_boot_stop(config->boot);
	if (ret)
		dev_err(config->boot->dev,
			"WLAN basic-config rollback failed: %d\n", ret);
	return ret;
}

static void mt6797_wifi_runtime_config_fence(void *context)
{
	struct mt6797_wifi_runtime_config *config = context;

	mt6797_consys_wifi_fence(config->boot->lease);
}

int mt6797_wifi_runtime_configure(struct mt6797_wifi_runtime_config *config,
				  struct mt6797_wifi_boot *boot)
{
	struct mt6797_wifi_config_ops ops;

	if (!config || !boot || !boot->hif || !boot->lease || !boot->online ||
	    !boot->transaction.ready)
		return -EINVAL;
	if (config->transaction.dirty || config->transaction.configured)
		return -EALREADY;
	config->boot = boot;
	config->hif = boot->hif;
	ops = (struct mt6797_wifi_config_ops) {
		.precheck = mt6797_wifi_runtime_config_precheck,
		.send = mt6797_wifi_runtime_config_send,
		.teardown = mt6797_wifi_runtime_config_teardown,
		.fence = mt6797_wifi_runtime_config_fence,
		.context = config,
	};

	return mt6797_wifi_config_transaction_run(&ops, &config->transaction);
}
