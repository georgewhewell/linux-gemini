// SPDX-License-Identifier: GPL-2.0-only
/* Bounded GEN3 firmware power-off, the precondition for a WLAN restart. */

#include <linux/errno.h>
#include <linux/string.h>

#include "fw_power.h"
#include "protocol.h"

int mt6797_wifi_power_off_command_build(u8 *command, size_t capacity,
					u8 sequence)
{
	int ret;

	/* GEN3 common/wlan_lib.c:2267 wlanSendNicPowerCtrlCmd(prAdapter, 1). */
	ret = mt6797_wifi_command_init(command, capacity, 4,
				       MT6797_WIFI_CMD_NIC_POWER_CTRL, true,
				       sequence);
	if (ret)
		return ret;
	command[MT6797_WIFI_COMMAND_HEADER_LENGTH] = MT6797_WIFI_NIC_POWER_OFF;
	return 0;
}

static bool
mt6797_wifi_power_off_ops_valid(const struct mt6797_wifi_power_off_ops *ops)
{
	return ops && ops->send_command && ops->drain && ops->read_ready &&
		ops->request_interrupt && ops->clear_mailbox && ops->sleep_ms;
}

/* GEN3 common/wlan_lib.c:892 wlanPowerOffInt(). */
static int
mt6797_wifi_power_off_interrupt(const struct mt6797_wifi_power_off_ops *ops,
				struct mt6797_wifi_power_off_result *result)
{
	unsigned int poll;
	bool ready = true;
	int cleanup;
	int ret;

	result->interrupts++;
	ret = ops->request_interrupt(ops->context);
	for (poll = 0; !ret && poll < MT6797_WIFI_POWER_OFF_INT_POLLS; poll++) {
		ret = ops->read_ready(ops->context, &ready);
		if (ret || !ready)
			break;
		ops->sleep_ms(ops->context, 1);
	}
	/* The vendor always clears the mailbox, on success and on timeout. */
	cleanup = ops->clear_mailbox(ops->context);
	if (ret)
		return ret;
	if (cleanup)
		return cleanup;
	return ready ? -ETIMEDOUT : 0;
}

int mt6797_wifi_power_off_run(const struct mt6797_wifi_power_off_ops *ops,
			      struct mt6797_wifi_power_off_result *result)
{
	unsigned int pass;
	bool pending;
	bool ready;
	int ret;

	if (!mt6797_wifi_power_off_ops_valid(ops) || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));

	/* GEN3 common/wlan_lib.c:973: ask the firmware to power itself off. */
	ret = ops->send_command(ops->context);
	if (ret && ret != -EAGAIN)
		return ret;
	result->commanded = !ret;

	/* wlan_lib.c:975-979: service what the firmware raises while stopping.
	 * Draining is best effort; only WCIR.READY below proves the outcome.
	 */
	for (pass = 0; result->commanded &&
	     pass < MT6797_WIFI_POWER_OFF_DRAIN_PASSES; pass++) {
		pending = false;
		if (ops->drain(ops->context, &pending) || !pending)
			break;
	}

	/* wlan_lib.c:982-1004. Without a delivered command, go straight to the
	 * interrupt method the vendor uses once the command has not worked.
	 */
	for (; result->polls < MT6797_WIFI_POWER_OFF_POLLS; result->polls++) {
		ret = ops->read_ready(ops->context, &ready);
		if (ret)
			return ret;
		if (!ready)
			return 0;
		if (!result->commanded ||
		    result->polls >= MT6797_WIFI_POWER_OFF_COMMAND_POLLS) {
			ret = mt6797_wifi_power_off_interrupt(ops, result);
			if (ret != -ETIMEDOUT)
				return ret;
		}
		ops->sleep_ms(ops->context, 1);
	}
	return -ETIMEDOUT;
}
