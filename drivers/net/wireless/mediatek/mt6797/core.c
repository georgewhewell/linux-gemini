// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 integrated GEN3 WLAN endpoint. */

#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#include "boot.h"
#include "capability.h"
#include "firmware.h"
#include "hif.h"
#include "hif_runtime.h"
#include "runtime_config.h"
#include "radio.h"

#if IS_ENABLED(CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_QUALIFICATION)
#include "open_auth_qualification.h"
#endif

#define MT6797_WIFI_HIF_BASE	0x180f0000
#define MT6797_WIFI_HIF_SPI	283

/*
 * Firmware restarts after a runtime HIF fault, the counterpart of GEN3's
 * WMTRSTMSG_RESET_END handling (os/linux/gl_rst.c:147, then
 * wmt_chrdev_wifi.c:176 wifi_reset_end() re-running WLAN probe). At most
 * MAX attempts in any WINDOW; beyond that the device stays down.
 */
#define MT6797_WIFI_RECOVERY_MAX	3U
#define MT6797_WIFI_RECOVERY_WINDOW	(10 * 60 * HZ)
#define MT6797_WIFI_RECOVERY_RETRY	(2 * HZ)

struct mt6797_wifi {
	struct mt6797_wifi_ahb_hif hif;
	struct mt6797_wifi_firmware firmware;
	struct mt6797_wifi_boot boot;
	struct mt6797_wifi_hif_irq hif_irq;
	struct mt6797_wifi_capability_session capability_session;
	struct mt6797_wifi_capability capability;
	struct mt6797_wifi_runtime_config runtime_config;
#if IS_ENABLED(CONFIG_MTK_MT6797_WIFI_RADIO)
	struct mt6797_wifi_radio *radio;
	/* Serializes recovery with fault injection; remove cancels it first. */
	struct mutex lock;
	struct delayed_work recovery_work;
	spinlock_t recovery_lock;	/* Orders recovery queueing with remove. */
	unsigned long recovery_stamp[MT6797_WIFI_RECOVERY_MAX];
	unsigned int recoveries;
	unsigned int recovery_failures;
	bool removing;
	struct dentry *debugfs;
#endif
	struct device *dev;
#if IS_ENABLED(CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_QUALIFICATION)
	struct mt6797_wifi_open_auth_qualification qualification;
#endif
	int irq;
};

#if IS_ENABLED(CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_QUALIFICATION)
static ssize_t open_auth_qualification_store(
	struct device *dev, struct device_attribute *attribute,
	const char *buffer, size_t count)
{
	struct mt6797_wifi *wifi = dev_get_drvdata(dev);

	(void)attribute;
	return mt6797_wifi_open_auth_qualification_trigger(&wifi->qualification,
		buffer, count);
}
static DEVICE_ATTR_WO(open_auth_qualification);

static ssize_t open_auth_qualification_result_show(
	struct device *dev, struct device_attribute *attribute, char *buffer)
{
	struct mt6797_wifi *wifi = dev_get_drvdata(dev);

	(void)attribute;
	return mt6797_wifi_open_auth_qualification_result_show(
		&wifi->qualification, buffer);
}
static DEVICE_ATTR_RO(open_auth_qualification_result);

static struct attribute *mt6797_wifi_open_auth_qualification_attrs[] = {
	&dev_attr_open_auth_qualification.attr,
	&dev_attr_open_auth_qualification_result.attr,
	NULL,
};

static const struct attribute_group mt6797_wifi_open_auth_qualification_group = {
	.attrs = mt6797_wifi_open_auth_qualification_attrs,
};
#endif

static void mt6797_wifi_release_firmware(void *data)
{
	struct mt6797_wifi *wifi = data;

	mt6797_wifi_firmware_release(&wifi->firmware);
}

static int mt6797_wifi_validate_irq(struct platform_device *pdev)
{
	struct of_phandle_args spec;
	int ret;

	if (platform_irq_count(pdev) != 1)
		return -EINVAL;
	ret = of_irq_parse_one(pdev->dev.of_node, 0, &spec);
	if (ret)
		return ret;
	ret = spec.args_count == 3 && spec.args[0] == GIC_SPI &&
		spec.args[1] == MT6797_WIFI_HIF_SPI &&
		spec.args[2] == IRQ_TYPE_LEVEL_LOW ? 0 : -EINVAL;
	of_node_put(spec.np);
	return ret;
}

/*
 * Download and start the firmware, exchange capability and basic
 * configuration, then open the persistent HIF. On failure every step has
 * already rolled back (and fenced, when its state was ambiguous).
 */
static int mt6797_wifi_bring_up(struct mt6797_wifi *wifi, u32 *rxinf_before,
				u32 *rxinf_after)
{
	struct device *dev = wifi->dev;
	int ret;

	ret = mt6797_wifi_boot_start(&wifi->boot, dev, &wifi->hif,
				     &wifi->firmware);
	if (ret) {
		/* Probe reports deferral (CONSYS lease not yet available). */
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to start verified WLAN firmware: %d\n",
				ret);
		return ret;
	}
	ret = mt6797_wifi_capability_query(&wifi->capability_session,
					   &wifi->boot, &wifi->capability);
	if (ret) {
		int stop_ret = mt6797_wifi_boot_stop(&wifi->boot);

		if (stop_ret)
			dev_err(dev, "WLAN rollback failed: %d\n", stop_ret);
		dev_err(dev, "failed WLAN NIC capability exchange: %d\n", ret);
		return ret;
	}
	ret = mt6797_wifi_runtime_configure(&wifi->runtime_config, &wifi->boot);
	if (ret) {
		dev_err(dev, "failed WLAN post-capability basic config: %d\n", ret);
		return ret;
	}
	ret = mt6797_wifi_hif_runtime_start(&wifi->hif_irq, &wifi->boot);
	if (ret) {
		dev_err(dev, "failed to establish persistent WLAN HIF: %d\n", ret);
		return ret;
	}
	ret = mt6797_wifi_hif_runtime_configure_rxinf(&wifi->hif_irq,
		rxinf_before, rxinf_after);
	if (ret)
		mt6797_wifi_hif_runtime_stop(&wifi->hif_irq);
	return ret;
}

#if IS_ENABLED(CONFIG_MTK_MT6797_WIFI_RADIO)
static int mt6797_wifi_radio_start(struct mt6797_wifi *wifi)
{
	struct mt6797_wifi_radio *radio;

	radio = mt6797_wifi_radio_register(wifi->dev, &wifi->hif_irq,
					   &wifi->runtime_config,
					   &wifi->capability);
	if (IS_ERR(radio))
		return PTR_ERR(radio);
	wifi->radio = radio;
	return 0;
}

/* Refuse a restart when the MAX-th previous one began within WINDOW. */
static bool mt6797_wifi_recovery_budget(struct mt6797_wifi *wifi)
{
	unsigned long *stamp =
		&wifi->recovery_stamp[wifi->recoveries % MT6797_WIFI_RECOVERY_MAX];

	if (wifi->recoveries >= MT6797_WIFI_RECOVERY_MAX &&
	    time_before(jiffies, *stamp + MT6797_WIFI_RECOVERY_WINDOW))
		return false;
	*stamp = jiffies;
	wifi->recoveries++;
	return true;
}

static void mt6797_wifi_queue_recovery(struct mt6797_wifi *wifi,
				       unsigned long delay)
{
	spin_lock_irq(&wifi->recovery_lock);
	if (!wifi->removing)
		queue_delayed_work(system_long_wq, &wifi->recovery_work, delay);
	spin_unlock_irq(&wifi->recovery_lock);
}

static void mt6797_wifi_terminal_notify(void *context, int reason)
{
	mt6797_wifi_queue_recovery(context, 0);
}

static void mt6797_wifi_recovery_work(struct work_struct *work)
{
	struct mt6797_wifi *wifi = container_of(to_delayed_work(work),
						struct mt6797_wifi,
						recovery_work);
	struct mt6797_wifi_hif_irq *irq = &wifi->hif_irq;
	u32 rxinf_before, rxinf_after;
	int ret;

	mutex_lock(&wifi->lock);
	/* GEN3 wifi_reset_start(): remove the interface (cfg80211 disconnect). */
	if (wifi->radio) {
		mt6797_wifi_radio_unregister(wifi->radio);
		wifi->radio = NULL;
	}
	if (!mt6797_wifi_hif_runtime_recoverable(irq)) {
		dev_err(wifi->dev,
			"WLAN firmware state unknown after HIF fault; reboot required\n");
		goto out;
	}
	if (!mt6797_wifi_recovery_budget(wifi)) {
		dev_err(wifi->dev,
			"WLAN restarted %u times within %u s; leaving it down\n",
			MT6797_WIFI_RECOVERY_MAX,
			MT6797_WIFI_RECOVERY_WINDOW / HZ);
		goto out;
	}
	ret = mt6797_wifi_hif_runtime_reset(irq);
	if (ret)
		goto failed;
	memset(&wifi->runtime_config, 0, sizeof(wifi->runtime_config));
	ret = mt6797_wifi_capability_session_init(&wifi->capability_session,
						  irq);
	if (ret)
		goto failed;
	/* GEN3 wifi_reset_end(): download firmware and probe again. */
	ret = mt6797_wifi_bring_up(wifi, &rxinf_before, &rxinf_after);
	if (ret)
		goto failed;
	ret = mt6797_wifi_radio_start(wifi);
	if (ret) {
		/* Stop through the terminal path so firmware is powered off. */
		mutex_lock(&irq->lifecycle_lock);
		mt6797_wifi_hif_runtime_fail(irq, ret);
		mutex_unlock(&irq->lifecycle_lock);
		mt6797_wifi_hif_runtime_stop(irq);
		goto failed;
	}
	dev_info(wifi->dev, "WLAN restarted after HIF fault (restart %u)\n",
		 wifi->recoveries);
	goto out;

failed:
	wifi->recovery_failures++;
	dev_err(wifi->dev, "WLAN restart %u failed: %d\n", wifi->recoveries,
		ret);
	/* Only a proven firmware power-off permits another download. */
	if (mt6797_wifi_hif_runtime_recoverable(irq))
		mt6797_wifi_queue_recovery(wifi, MT6797_WIFI_RECOVERY_RETRY);
out:
	mutex_unlock(&wifi->lock);
}

static ssize_t mt6797_wifi_hif_fault_write(struct file *file,
					   const char __user *buffer,
					   size_t count, loff_t *position)
{
	struct mt6797_wifi *wifi = file->private_data;
	struct mt6797_wifi_hif_irq *irq = &wifi->hif_irq;
	int reason;
	int ret;

	ret = kstrtoint_from_user(buffer, count, 0, &reason);
	if (ret)
		return ret;
	if (reason >= 0 || reason < -MAX_ERRNO)
		return -EINVAL;
	mutex_lock(&wifi->lock);
	mutex_lock(&irq->lifecycle_lock);
	ret = mt6797_wifi_hif_irq_mode(irq) == MT6797_WIFI_HIF_IRQ_RUNTIME ?
		0 : -ENOTCONN;
	if (!ret)
		mt6797_wifi_hif_runtime_fail(irq, reason);
	mutex_unlock(&irq->lifecycle_lock);
	mutex_unlock(&wifi->lock);
	return ret ?: count;
}

static const struct file_operations mt6797_wifi_hif_fault_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = mt6797_wifi_hif_fault_write,
	.llseek = noop_llseek,
};

static int mt6797_wifi_recovery_show(struct seq_file *file, void *data)
{
	struct mt6797_wifi *wifi = file->private;

	mutex_lock(&wifi->lock);
	seq_printf(file, "restarts=%u failures=%u radio=%u mode=%d\n",
		   wifi->recoveries, wifi->recovery_failures, !!wifi->radio,
		   mt6797_wifi_hif_irq_mode(&wifi->hif_irq));
	mutex_unlock(&wifi->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6797_wifi_recovery);

static void mt6797_wifi_debugfs_init(struct mt6797_wifi *wifi)
{
	wifi->debugfs = debugfs_create_dir(dev_name(wifi->dev), NULL);
	/* Write a negative errno to declare a runtime HIF fault. */
	debugfs_create_file("hif_fault", 0200, wifi->debugfs, wifi,
			    &mt6797_wifi_hif_fault_fops);
	debugfs_create_file("recovery", 0400, wifi->debugfs, wifi,
			    &mt6797_wifi_recovery_fops);
}
#endif

static int mt6797_wifi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_wifi *wifi;
	struct resource *resource;
	void __iomem *base;
	u32 rxinf_before, rxinf_after;
	int ret;

	wifi = devm_kzalloc(dev, sizeof(*wifi), GFP_KERNEL);
	if (!wifi)
		return -ENOMEM;
#if IS_ENABLED(CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_QUALIFICATION)
	if (!mt6797_wifi_open_auth_source_revision_valid(
		CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_SOURCE_REVISION))
		return dev_err_probe(dev, -EINVAL,
			"invalid qualification source revision\n");
#endif
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource->start != MT6797_WIFI_HIF_BASE ||
	    resource_size(resource) != MT6797_WIFI_AHB_HIF_SIZE)
		return dev_err_probe(dev, -EINVAL, "invalid HIF resource\n");
	base = devm_ioremap_resource(dev, resource);
	if (IS_ERR(base))
		return PTR_ERR(base);
	ret = mt6797_wifi_ahb_hif_init(&wifi->hif, base);
	if (ret)
		return ret;

	ret = mt6797_wifi_validate_irq(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "invalid HIF interrupt\n");
	wifi->irq = platform_get_irq_byname(pdev, "hif");
	if (wifi->irq < 0)
		return wifi->irq;
	if ((irq_get_trigger_type(wifi->irq) & IRQ_TYPE_SENSE_MASK) !=
	    IRQ_TYPE_LEVEL_LOW)
		return dev_err_probe(dev, -EINVAL,
				     "HIF interrupt must be level-low\n");
	ret = mt6797_wifi_hif_irq_init(&wifi->hif_irq, dev, &wifi->hif,
				       wifi->irq);
	if (ret)
		return dev_err_probe(dev, ret, "failed to reserve HIF interrupt\n");
	ret = mt6797_wifi_capability_session_init(&wifi->capability_session,
						  &wifi->hif_irq);
	if (ret)
		return ret;

	ret = mt6797_wifi_firmware_request(dev, &wifi->firmware);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to acquire verified WLAN firmware\n");
	ret = devm_add_action_or_reset(dev, mt6797_wifi_release_firmware, wifi);
	if (ret)
		return ret;
	wifi->dev = dev;
	ret = mt6797_wifi_bring_up(wifi, &rxinf_before, &rxinf_after);
	if (ret)
		return dev_err_probe(dev, ret, "failed WLAN initialization\n");
	dev_info(dev, "HIF RX group1 register before=%#010x after=%#010x\n",
		rxinf_before, rxinf_after);

	platform_set_drvdata(pdev, wifi);
#if IS_ENABLED(CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_QUALIFICATION)
	ret = mt6797_wifi_open_auth_qualification_init(&wifi->qualification,
		&wifi->hif_irq, &wifi->runtime_config, &wifi->capability,
		CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_SOURCE_REVISION, NULL);
	if (ret)
		goto stop_runtime;
	ret = device_add_group(dev,
		&mt6797_wifi_open_auth_qualification_group);
	if (ret)
		goto stop_runtime;
#endif
#if IS_ENABLED(CONFIG_MTK_MT6797_WIFI_RADIO)
	ret = mt6797_wifi_radio_start(wifi);
	if (ret)
		goto stop_runtime;
	mutex_init(&wifi->lock);
	spin_lock_init(&wifi->recovery_lock);
	INIT_DELAYED_WORK(&wifi->recovery_work, mt6797_wifi_recovery_work);
	mt6797_wifi_hif_runtime_set_terminal_notify(&wifi->hif_irq,
		mt6797_wifi_terminal_notify, wifi);
	mt6797_wifi_debugfs_init(wifi);
#endif
	dev_info(dev,
		 "WLAN %#06x fw %#06x HIF_EVENT_PUMP_READY irq=%d budget=%u\n",
		 wifi->capability.product_id, wifi->capability.firmware_version,
		 wifi->irq, MT6797_WIFI_HIF_RX_BUDGET);
	return 0;

stop_runtime:
	mt6797_wifi_hif_runtime_stop(&wifi->hif_irq);
	return dev_err_probe(dev, ret,
		"failed WLAN runtime initialization\n");
}

static void mt6797_wifi_remove(struct platform_device *pdev)
{
	struct mt6797_wifi *wifi = platform_get_drvdata(pdev);
	int ret;

#if IS_ENABLED(CONFIG_MTK_MT6797_WIFI_RADIO)
	debugfs_remove_recursive(wifi->debugfs);
	spin_lock_irq(&wifi->recovery_lock);
	wifi->removing = true;
	spin_unlock_irq(&wifi->recovery_lock);
	/* A terminal worker that runs from here on observes removing. */
	cancel_delayed_work_sync(&wifi->recovery_work);
	if (wifi->radio)
		mt6797_wifi_radio_unregister(wifi->radio);
#endif
#if IS_ENABLED(CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_QUALIFICATION)
	device_remove_group(&pdev->dev,
		&mt6797_wifi_open_auth_qualification_group);
#endif
	ret = mt6797_wifi_hif_runtime_stop(&wifi->hif_irq);
	if (ret)
		dev_err(&pdev->dev,
			"failed to shut down WLAN firmware cleanly: %d\n", ret);
}

static const struct of_device_id mt6797_wifi_of_match[] = {
	{ .compatible = "mediatek,mt6797-wifi" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_wifi_of_match);

static struct platform_driver mt6797_wifi_driver = {
	.probe = mt6797_wifi_probe,
	.remove = mt6797_wifi_remove,
	.driver = {
		.name = "mt6797-wifi",
		.of_match_table = mt6797_wifi_of_match,
	},
};
module_platform_driver(mt6797_wifi_driver);

MODULE_DESCRIPTION("MediaTek MT6797 integrated GEN3 WLAN endpoint");
MODULE_LICENSE("GPL");
