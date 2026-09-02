// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 CONSYS runtime/BTIF bootstrap coordinator.
 *
 * This composes, but does not enlarge, the established evidence boundaries:
 *
 * - the runtime provider publishes resources only; an earlier true-chronology
 *   handoff owner must already hold the vendor identity prefix through chip
 *   ID 0x0279 (mtk_wcn_consys_hw.c:301-354,401-420);
 * - the PIO adapter owns the exact BTIF core and its disabled core IRQ
 *   (arch/arm64/boot/dts/mt6797.dtsi:2511-2516 and mtk_btif.c:1472-1481);
 * - the bootstrap performs precisely query-default, set-full, a 10-ms host
 *   switch, and query-full (wmt_ic_soc.c:122-124,190-191,1008-1044).
 *
 * The first radio lease starts the common hardware and the last lease tears it
 * down. Before publishing full STP, the controller owns resources, identity,
 * post-ID, patch download and post-patch calibration in their strict order.
 * Wi-Fi, Bluetooth and GNSS receive only their channel and bounded function
 * capabilities; none can stop a controller still leased by another radio.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_RUNTIME_CONTROLLER_KUNIT_TEST)
#include <kunit/test.h>
#include <linux/kthread.h>
#endif

#include <linux/soc/mediatek/mt6797-btif-pio-platform.h>
#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mt6797-consys-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-consys-postid.h>
#include <linux/soc/mediatek/mt6797-consys-runtime-controller.h>
#include <linux/soc/mediatek/mt6797-consys-runtime.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>
#include <linux/soc/mediatek/mt6797-consys-wmt-patch.h>
#include <linux/soc/mediatek/mt6797-consys-wmt-postpatch.h>
#include <linux/soc/mediatek/mtk-wmt.h>

enum mt6797_consys_runtime_controller_state {
	MT6797_CONSYS_RUNTIME_CONTROLLER_READY,
	MT6797_CONSYS_RUNTIME_CONTROLLER_STARTING,
	MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE,
	MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING,
};

struct mt6797_consys_runtime_controller_ops {
	const struct mt6797_consys_runtime_lease *(*runtime_lease)(void *context);
	const struct mt6797_btif_pio_platform_lease *(*platform_acquire)(void *context);
	struct mt6797_btif_stp_bootstrap *(*bootstrap_create)(void *context,
			const struct mt6797_btif_pio_platform_lease *platform_lease,
			const struct mt6797_consys_runtime_lease *runtime_lease);
	const struct mt6797_consys_stp_session_lease *(*bootstrap_run)(void *context,
			struct mt6797_btif_stp_bootstrap *bootstrap);
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *
		(*bootstrap_wmt_lease)(void *context,
			struct mt6797_btif_stp_bootstrap *bootstrap);
	int (*patch_download)(void *context, struct device *dev,
			const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease);
	int (*postpatch)(void *context, struct device *dev,
			const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
			struct regulator *bt_paldo, struct regulator *wifi_paldo);
	void (*bootstrap_destroy)(void *context,
			struct mt6797_btif_stp_bootstrap *bootstrap);
	void (*platform_close)(void *context,
			const struct mt6797_btif_pio_platform_lease *platform_lease);
	int (*prepare)(void *context);
	void (*unprepare)(void *context);
};

struct mt6797_consys_runtime_controller {
	struct device *dev;
	struct device *runtime_dev;
	struct device *btif_dev;
	struct regulator *bt_paldo;
	struct regulator *wifi_paldo;
	const struct mt6797_consys_runtime_controller_ops *ops;
	void *ops_context;
	struct mutex lock;
	struct completion run_complete;
	struct completion gnss_released;
	struct completion wifi_released;
	struct completion bt_released;
	struct completion stop_complete;
	const struct mt6797_consys_runtime_lease *runtime_lease;
	const struct mt6797_btif_pio_platform_lease *platform_lease;
	struct mt6797_btif_stp_bootstrap *bootstrap;
	const struct mt6797_consys_stp_session_lease *session_lease;
	struct mt6797_consys_runtime_resource_lease resources;
	struct mt6797_consys_runtime_provider_lease identity;
	struct mt6797_consys_postid_owner postid;
	struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease_owner;
	struct mt6797_consys_runtime_controller_wifi_lease *wifi_lease_owner;
	struct mt6797_consys_runtime_controller_bt_lease *bt_lease_owner;
	u64 session_generation;
	bool run_in_progress;
	bool stop_requested;
	bool resources_held;
	bool identity_held;
	bool postid_active;
	bool emi_claimed;
	bool platform_failed;
	bool removing;
	enum mt6797_consys_runtime_controller_state state;
};

enum mt6797_consys_runtime_controller_lifecycle_step {
	MT6797_CONSYS_LIFECYCLE_ACQUIRE_RESOURCES,
	MT6797_CONSYS_LIFECYCLE_CLAIM_EMI,
	MT6797_CONSYS_LIFECYCLE_ACQUIRE_IDENTITY,
	MT6797_CONSYS_LIFECYCLE_INIT_POSTID,
	MT6797_CONSYS_LIFECYCLE_START_POSTID,
	MT6797_CONSYS_LIFECYCLE_STOP_POSTID,
	MT6797_CONSYS_LIFECYCLE_RELEASE_IDENTITY,
	MT6797_CONSYS_LIFECYCLE_RELEASE_RESOURCES,
	MT6797_CONSYS_LIFECYCLE_LATCH_FATAL,
};

typedef int (*mt6797_consys_runtime_controller_lifecycle_t)(
		struct mt6797_consys_runtime_controller *controller,
		enum mt6797_consys_runtime_controller_lifecycle_step step,
		void *context);

static bool mt6797_consys_runtime_controller_ops_valid(
		const struct mt6797_consys_runtime_controller_ops *ops)
{
	return ops && ops->runtime_lease && ops->platform_acquire &&
		ops->bootstrap_create && ops->bootstrap_run &&
		ops->bootstrap_wmt_lease && ops->patch_download && ops->postpatch &&
		ops->bootstrap_destroy && ops->platform_close;
}

static int mt6797_consys_runtime_controller_init(
		struct mt6797_consys_runtime_controller *controller,
		const struct mt6797_consys_runtime_controller_ops *ops,
		void *ops_context)
{
	if (!controller || !mt6797_consys_runtime_controller_ops_valid(ops))
		return -EINVAL;

	controller->ops = ops;
	controller->ops_context = ops_context;
	controller->state = MT6797_CONSYS_RUNTIME_CONTROLLER_READY;
	mutex_init(&controller->lock);
	init_completion(&controller->run_complete);
	init_completion(&controller->gnss_released);
	complete_all(&controller->gnss_released);
	init_completion(&controller->wifi_released);
	complete_all(&controller->wifi_released);
	init_completion(&controller->bt_released);
	complete_all(&controller->bt_released);
	init_completion(&controller->stop_complete);
	complete_all(&controller->stop_complete);
	return 0;
}

static bool mt6797_consys_runtime_controller_stop_requested(
		struct mt6797_consys_runtime_controller *controller)
{
	bool requested;

	mutex_lock(&controller->lock);
	requested = controller->stop_requested;
	mutex_unlock(&controller->lock);
	return requested;
}

static void mt6797_consys_runtime_controller_complete_idle(
		struct mt6797_consys_runtime_controller *controller)
{
	mutex_lock(&controller->lock);
	controller->state = MT6797_CONSYS_RUNTIME_CONTROLLER_READY;
	controller->run_in_progress = false;
	controller->stop_requested = false;
	complete_all(&controller->run_complete);
	mutex_unlock(&controller->lock);
}

static void mt6797_consys_runtime_controller_abort(
		struct mt6797_consys_runtime_controller *controller,
		struct mt6797_btif_stp_bootstrap *bootstrap,
		const struct mt6797_btif_pio_platform_lease *platform_lease)
{
	/* bootstrap_destroy synchronizes its WMT runner before closing PIO. */
	if (bootstrap) {
		controller->ops->bootstrap_destroy(controller->ops_context, bootstrap);
	} else if (platform_lease) {
		/* Creation failed before the bootstrap took ownership. */
		controller->ops->platform_close(controller->ops_context, platform_lease);
	}
	if (controller->ops->unprepare)
		controller->ops->unprepare(controller->ops_context);
	mt6797_consys_runtime_controller_complete_idle(controller);
}

static int mt6797_consys_runtime_controller_start_unchecked(
		struct mt6797_consys_runtime_controller *controller)
{
	const struct mt6797_consys_runtime_lease *runtime_lease = NULL;
	const struct mt6797_btif_pio_platform_lease *platform_lease = NULL;
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *wmt_lease;
	const struct mt6797_consys_stp_session_lease *session_lease;
	struct mt6797_btif_stp_bootstrap *bootstrap = NULL;
	int ret;

	if (!controller)
		return -EINVAL;
	mutex_lock(&controller->lock);
	if (controller->state != MT6797_CONSYS_RUNTIME_CONTROLLER_READY) {
		mutex_unlock(&controller->lock);
		return -EBUSY;
	}
	controller->state = MT6797_CONSYS_RUNTIME_CONTROLLER_STARTING;
	controller->run_in_progress = true;
	reinit_completion(&controller->run_complete);
	mutex_unlock(&controller->lock);

	if (controller->ops->prepare) {
		ret = controller->ops->prepare(controller->ops_context);
		if (ret)
			goto abort;
	}

	runtime_lease = controller->ops->runtime_lease(controller->ops_context);
	if (IS_ERR_OR_NULL(runtime_lease)) {
		ret = runtime_lease ? PTR_ERR(runtime_lease) : -ENODEV;
		goto abort;
	}
	if (mt6797_consys_runtime_controller_stop_requested(controller)) {
		ret = -ESHUTDOWN;
		goto abort;
	}

	platform_lease = controller->ops->platform_acquire(controller->ops_context);
	if (IS_ERR_OR_NULL(platform_lease)) {
		ret = platform_lease ? PTR_ERR(platform_lease) : -ENODEV;
		goto abort;
	}
	if (mt6797_consys_runtime_controller_stop_requested(controller)) {
		ret = -ESHUTDOWN;
		goto abort;
	}

	bootstrap = controller->ops->bootstrap_create(controller->ops_context,
						      platform_lease, runtime_lease);
	if (IS_ERR_OR_NULL(bootstrap)) {
		ret = bootstrap ? PTR_ERR(bootstrap) : -ENOMEM;
		goto abort;
	}
	if (mt6797_consys_runtime_controller_stop_requested(controller)) {
		ret = -ESHUTDOWN;
		goto abort;
	}

	session_lease = controller->ops->bootstrap_run(controller->ops_context,
						       bootstrap);
	if (IS_ERR_OR_NULL(session_lease)) {
		ret = session_lease ? PTR_ERR(session_lease) : -EIO;
		goto abort;
	}
	if (mt6797_consys_runtime_controller_stop_requested(controller)) {
		ret = -ESHUTDOWN;
		goto abort;
	}
	wmt_lease = controller->ops->bootstrap_wmt_lease(controller->ops_context,
								bootstrap);
	if (IS_ERR_OR_NULL(wmt_lease)) {
		ret = wmt_lease ? PTR_ERR(wmt_lease) : -EIO;
		goto abort;
	}

	/* Patch and calibration complete before any client lease is published. */
	ret = controller->ops->patch_download(controller->ops_context,
					     controller->dev, wmt_lease);
	if (ret)
		goto abort;
	if (mt6797_consys_runtime_controller_stop_requested(controller)) {
		ret = -ESHUTDOWN;
		goto abort;
	}
	ret = controller->ops->postpatch(controller->ops_context, controller->dev,
					 wmt_lease, controller->bt_paldo,
					 controller->wifi_paldo);
	if (ret)
		goto abort;
	if (mt6797_consys_runtime_controller_stop_requested(controller)) {
		ret = -ESHUTDOWN;
		goto abort;
	}

	mutex_lock(&controller->lock);
	if (controller->stop_requested) {
		mutex_unlock(&controller->lock);
		ret = -ESHUTDOWN;
		goto abort;
	}
	controller->runtime_lease = runtime_lease;
	controller->platform_lease = platform_lease;
	controller->bootstrap = bootstrap;
	controller->session_lease = session_lease;
	controller->session_generation++;
	controller->state = MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE;
	controller->run_in_progress = false;
	complete_all(&controller->run_complete);
	mutex_unlock(&controller->lock);
	return 0;

abort:
	mt6797_consys_runtime_controller_abort(controller,
						      !IS_ERR_OR_NULL(bootstrap) ? bootstrap : NULL,
						      !IS_ERR_OR_NULL(platform_lease) ?
						      platform_lease : NULL);
	return ret;
}

static int mt6797_consys_runtime_controller_start_controller(
		struct mt6797_consys_runtime_controller *controller)
{
	int ret;

	if (!controller)
		return -EINVAL;
	for (;;) {
		mutex_lock(&controller->lock);
		if (controller->platform_failed || controller->removing) {
			mutex_unlock(&controller->lock);
			return -ESHUTDOWN;
		}
		if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE) {
			mutex_unlock(&controller->lock);
			return 0;
		}
		if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_STARTING) {
			mutex_unlock(&controller->lock);
			wait_for_completion(&controller->run_complete);
			continue;
		}
		if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING) {
			mutex_unlock(&controller->lock);
			wait_for_completion(&controller->stop_complete);
			continue;
		}
		mutex_unlock(&controller->lock);

		ret = mt6797_consys_runtime_controller_start_unchecked(controller);
		if (ret == -EBUSY)
			continue;
		return ret;
	}
}

static void mt6797_consys_runtime_controller_stop_if_idle(
		struct mt6797_consys_runtime_controller *controller)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = NULL;
	const struct mt6797_btif_pio_platform_lease *platform_lease = NULL;

	mutex_lock(&controller->lock);
	if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE &&
	    !controller->gnss_lease_owner && !controller->wifi_lease_owner &&
	    !controller->bt_lease_owner) {
		/*
		 * The lease-owner check and ACTIVE -> QUIESCING transition share
		 * one lock with every get(). A racing get therefore either wins a
		 * lease first or observes QUIESCING and retries after stop_complete.
		 */
		controller->stop_requested = true;
		controller->state = MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING;
		reinit_completion(&controller->stop_complete);
		bootstrap = controller->bootstrap;
		platform_lease = controller->platform_lease;
		controller->bootstrap = NULL;
		controller->platform_lease = NULL;
		controller->runtime_lease = NULL;
		controller->session_lease = NULL;
	}
	mutex_unlock(&controller->lock);

	if (!bootstrap)
		return;
	mt6797_consys_runtime_controller_abort(controller, bootstrap,
						 platform_lease);
	complete_all(&controller->stop_complete);
}

static int mt6797_consys_runtime_controller_gnss_session_get_controller(
		struct mt6797_consys_runtime_controller *controller,
		struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease,
		const struct mt6797_consys_stp_session_lease **session_lease)
{
	int ret;

	/*
	 * Vendor wmt_core.c:395-481 proves only the six-byte GPS function
	 * request/event.  wmt_func.c:405-606 separately owns GPS_SYNC, LNA and
	 * VCN28/co-clock controls, so this transport-only hand-off cannot start
	 * GNSS or grow a generic radio-power operation.
	 */
	if (!controller || !gnss_lease || !session_lease)
		return -EINVAL;
	*session_lease = NULL;
	if (gnss_lease->controller)
		return -EBUSY;

	mutex_lock(&controller->lock);
	if (controller->state != MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE ||
	    controller->stop_requested || !controller->session_lease) {
		ret = controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_READY ||
			controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_STARTING ?
			-EAGAIN : -ESHUTDOWN;
		goto unlock;
	}
	if (controller->gnss_lease_owner) {
		ret = -EBUSY;
		goto unlock;
	}

	/* Provider removal waits on this owner before destroying full STP. */
	reinit_completion(&controller->gnss_released);
	controller->gnss_lease_owner = gnss_lease;
	gnss_lease->controller = controller;
	gnss_lease->generation = controller->session_generation;
	*session_lease = controller->session_lease;
	ret = 0;
unlock:
	mutex_unlock(&controller->lock);
	return ret;
}

int mt6797_consys_runtime_controller_gnss_session_get(struct device *dev,
		struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease,
		const struct mt6797_consys_stp_session_lease **session_lease)
{
	struct mt6797_consys_runtime_controller *controller;
	int ret;

	if (!dev)
		return -EINVAL;
	controller = dev_get_drvdata(dev);
	if (!controller)
		return -ENODEV;

	for (;;) {
		ret = mt6797_consys_runtime_controller_start_controller(controller);
		if (ret)
			return ret;
		ret = mt6797_consys_runtime_controller_gnss_session_get_controller(
			controller, gnss_lease, session_lease);
		if (ret != -EAGAIN && ret != -ESHUTDOWN)
			return ret;
	}
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_gnss_session_get);

static void mt6797_consys_runtime_controller_gnss_session_put_controller(
		struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease)
{
	struct mt6797_consys_runtime_controller *controller;
	bool released = false;

	if (!gnss_lease || !gnss_lease->controller)
		return;

	controller = gnss_lease->controller;
	mutex_lock(&controller->lock);
	if (controller->gnss_lease_owner == gnss_lease &&
	    controller->session_generation == gnss_lease->generation) {
		controller->gnss_lease_owner = NULL;
		complete_all(&controller->gnss_released);
		released = true;
	}
	gnss_lease->controller = NULL;
	gnss_lease->generation = 0;
	mutex_unlock(&controller->lock);
	if (released)
		mt6797_consys_runtime_controller_stop_if_idle(controller);
}

void mt6797_consys_runtime_controller_gnss_session_put(
		struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease)
{
	mt6797_consys_runtime_controller_gnss_session_put_controller(gnss_lease);
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_gnss_session_put);

#define MT6797_CONSYS_GNSS_FUNC_CTRL	6
#define MT6797_CONSYS_GNSS_FUNC_TIMEOUT	msecs_to_jiffies(3000)

static const u8 mt6797_consys_gnss_func_ok[] = { 0 };
static const struct mtk_wmt_match mt6797_consys_gnss_func_match = {
	.opcode = MT6797_CONSYS_GNSS_FUNC_CTRL,
	.data = mt6797_consys_gnss_func_ok,
	.data_len = sizeof(mt6797_consys_gnss_func_ok),
};

int mt6797_consys_runtime_controller_gnss_function_set(
		struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease,
		bool enable)
{
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *wmt_lease;
	struct mt6797_consys_runtime_controller *controller;
	const u8 data[] = { 2, enable ? 1 : 0 };
	struct mt6797_btif_stp_bootstrap *bootstrap;

	if (!gnss_lease || !gnss_lease->controller)
		return -EINVAL;
	controller = gnss_lease->controller;
	mutex_lock(&controller->lock);
	if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE &&
	    !controller->stop_requested &&
	    controller->gnss_lease_owner == gnss_lease &&
	    controller->session_generation == gnss_lease->generation)
		bootstrap = controller->bootstrap;
	else
		bootstrap = NULL;
	mutex_unlock(&controller->lock);
	if (!bootstrap)
		return -ESHUTDOWN;

	wmt_lease = mt6797_btif_stp_bootstrap_full_wmt_lease(bootstrap);
	if (IS_ERR(wmt_lease))
		return PTR_ERR(wmt_lease);

	return mt6797_btif_stp_bootstrap_full_wmt_transaction(wmt_lease,
		MT6797_CONSYS_GNSS_FUNC_CTRL, data, sizeof(data),
		&mt6797_consys_gnss_func_match,
		MT6797_CONSYS_GNSS_FUNC_TIMEOUT);
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_gnss_function_set);

#define MT6797_CONSYS_BT_FUNC_CTRL	6
#define MT6797_CONSYS_BT_FUNC_TIMEOUT	msecs_to_jiffies(3000)

static const u8 mt6797_consys_bt_func_ok[] = { 0 };
static const struct mtk_wmt_match mt6797_consys_bt_func_match = {
	.opcode = MT6797_CONSYS_BT_FUNC_CTRL,
	.data = mt6797_consys_bt_func_ok,
	.data_len = sizeof(mt6797_consys_bt_func_ok),
};

static int mt6797_consys_bt_session_get(
	struct mt6797_consys_runtime_controller *controller,
	struct mt6797_consys_runtime_controller_bt_lease *bt_lease,
	const struct mt6797_consys_stp_session_lease **session_lease)
{
	int ret;

	if (!controller || !bt_lease || !session_lease)
		return -EINVAL;
	*session_lease = NULL;
	if (bt_lease->controller)
		return -EBUSY;

	mutex_lock(&controller->lock);
	if (controller->state != MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE ||
	    controller->stop_requested || !controller->session_lease ||
	    !controller->bootstrap) {
		ret = controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_READY ||
			controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_STARTING ?
			-EAGAIN : -ESHUTDOWN;
	} else if (controller->bt_lease_owner) {
		ret = -EBUSY;
	} else {
		reinit_completion(&controller->bt_released);
		controller->bt_lease_owner = bt_lease;
		bt_lease->controller = controller;
		bt_lease->generation = controller->session_generation;
		*session_lease = controller->session_lease;
		ret = 0;
	}
	mutex_unlock(&controller->lock);
	return ret;
}

int mt6797_consys_runtime_controller_bt_session_get(struct device *dev,
		struct mt6797_consys_runtime_controller_bt_lease *bt_lease,
		const struct mt6797_consys_stp_session_lease **session_lease)
{
	struct mt6797_consys_runtime_controller *controller;
	int ret;

	if (!dev)
		return -EINVAL;
	controller = dev_get_drvdata(dev);
	if (!controller)
		return -ENODEV;
	for (;;) {
		ret = mt6797_consys_runtime_controller_start_controller(controller);
		if (ret)
			return ret;
		ret = mt6797_consys_bt_session_get(controller, bt_lease,
						    session_lease);
		if (ret != -EAGAIN && ret != -ESHUTDOWN)
			return ret;
	}
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_bt_session_get);

void mt6797_consys_runtime_controller_bt_session_put(
		struct mt6797_consys_runtime_controller_bt_lease *bt_lease)
{
	struct mt6797_consys_runtime_controller *controller;
	bool released = false;

	if (!bt_lease || !bt_lease->controller)
		return;
	controller = bt_lease->controller;
	mutex_lock(&controller->lock);
	if (controller->bt_lease_owner == bt_lease &&
	    controller->session_generation == bt_lease->generation) {
		controller->bt_lease_owner = NULL;
		complete_all(&controller->bt_released);
		released = true;
	}
	bt_lease->controller = NULL;
	bt_lease->generation = 0;
	mutex_unlock(&controller->lock);
	if (released)
		mt6797_consys_runtime_controller_stop_if_idle(controller);
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_bt_session_put);

int mt6797_consys_runtime_controller_bt_function_set(
		struct mt6797_consys_runtime_controller_bt_lease *bt_lease,
		bool enable)
{
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *wmt_lease;
	struct mt6797_consys_runtime_controller *controller;
	const u8 data[] = { 0, enable ? 1 : 0 };
	struct mt6797_btif_stp_bootstrap *bootstrap;

	if (!bt_lease || !bt_lease->controller)
		return -EINVAL;
	controller = bt_lease->controller;
	mutex_lock(&controller->lock);
	if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE &&
	    !controller->stop_requested &&
	    controller->bt_lease_owner == bt_lease &&
	    controller->session_generation == bt_lease->generation)
		bootstrap = controller->bootstrap;
	else
		bootstrap = NULL;
	mutex_unlock(&controller->lock);
	if (!bootstrap)
		return -ESHUTDOWN;

	wmt_lease = mt6797_btif_stp_bootstrap_full_wmt_lease(bootstrap);
	if (IS_ERR(wmt_lease))
		return PTR_ERR(wmt_lease);

	return mt6797_btif_stp_bootstrap_full_wmt_transaction(wmt_lease,
		MT6797_CONSYS_BT_FUNC_CTRL, data, sizeof(data),
		&mt6797_consys_bt_func_match, MT6797_CONSYS_BT_FUNC_TIMEOUT);
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_bt_function_set);

static int mt6797_consys_runtime_controller_wifi_session_get_controller(
		struct mt6797_consys_runtime_controller *controller,
		struct mt6797_consys_runtime_controller_wifi_lease *wifi_lease,
		const struct mt6797_consys_stp_session_lease **session_lease)
{
	int ret;

	if (!controller || !wifi_lease || !session_lease)
		return -EINVAL;
	*session_lease = NULL;
	if (wifi_lease->controller)
		return -EBUSY;
	mutex_lock(&controller->lock);
	if (controller->state != MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE ||
	    controller->stop_requested || !controller->session_lease) {
		ret = controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_READY ||
			controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_STARTING ?
			-EAGAIN : -ESHUTDOWN;
		goto unlock;
	}
	if (controller->wifi_lease_owner) {
		ret = -EBUSY;
		goto unlock;
	}
	reinit_completion(&controller->wifi_released);
	controller->wifi_lease_owner = wifi_lease;
	wifi_lease->controller = controller;
	wifi_lease->generation = controller->session_generation;
	*session_lease = controller->session_lease;
	ret = 0;
unlock:
	mutex_unlock(&controller->lock);
	return ret;
}

int mt6797_consys_runtime_controller_wifi_session_get(struct device *dev,
		struct mt6797_consys_runtime_controller_wifi_lease *wifi_lease,
		const struct mt6797_consys_stp_session_lease **session_lease)
{
	struct mt6797_consys_runtime_controller *controller;
	int ret;

	if (!dev)
		return -EINVAL;
	controller = dev_get_drvdata(dev);
	if (!controller)
		return -ENODEV;
	for (;;) {
		ret = mt6797_consys_runtime_controller_start_controller(controller);
		if (ret)
			return ret;
		ret = mt6797_consys_runtime_controller_wifi_session_get_controller(
			controller, wifi_lease, session_lease);
		if (ret != -EAGAIN && ret != -ESHUTDOWN)
			return ret;
	}
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_wifi_session_get);

void mt6797_consys_runtime_controller_wifi_session_put(
		struct mt6797_consys_runtime_controller_wifi_lease *wifi_lease)
{
	struct mt6797_consys_runtime_controller *controller;
	bool released = false;

	if (!wifi_lease || !wifi_lease->controller)
		return;
	controller = wifi_lease->controller;
	mutex_lock(&controller->lock);
	if (controller->wifi_lease_owner == wifi_lease &&
	    controller->session_generation == wifi_lease->generation) {
		controller->wifi_lease_owner = NULL;
		complete_all(&controller->wifi_released);
		released = true;
	}
	wifi_lease->controller = NULL;
	wifi_lease->generation = 0;
	mutex_unlock(&controller->lock);
	if (released)
		mt6797_consys_runtime_controller_stop_if_idle(controller);
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_wifi_session_put);

static void mt6797_consys_runtime_controller_stop_controller(
		struct mt6797_consys_runtime_controller *controller)
{
	struct mt6797_btif_stp_bootstrap *bootstrap;
	const struct mt6797_btif_pio_platform_lease *platform_lease;
	bool wait_for_run = false;

	if (!controller)
		return;

	mutex_lock(&controller->lock);
	if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_STARTING) {
		controller->stop_requested = true;
		wait_for_run = controller->run_in_progress;
		mutex_unlock(&controller->lock);
		if (wait_for_run)
			wait_for_completion(&controller->run_complete);
		return;
	}
	if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING) {
		mutex_unlock(&controller->lock);
		wait_for_completion(&controller->stop_complete);
		return;
	}
	if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_READY) {
		mutex_unlock(&controller->lock);
		return;
	}

	/* ACTIVE has one winner; it drains every retained STP channel borrower. */
	controller->stop_requested = true;
	controller->state = MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING;
	reinit_completion(&controller->stop_complete);
	mutex_unlock(&controller->lock);
	wait_for_completion(&controller->gnss_released);
	wait_for_completion(&controller->wifi_released);
	wait_for_completion(&controller->bt_released);

	mutex_lock(&controller->lock);
	/* The active-stop winner alone owns bootstrap destruction and PIO close. */
	bootstrap = controller->bootstrap;
	platform_lease = controller->platform_lease;
	controller->bootstrap = NULL;
	controller->platform_lease = NULL;
	controller->runtime_lease = NULL;
	controller->session_lease = NULL;
	mutex_unlock(&controller->lock);

	/* Keep QUIESCING visible until bootstrap destroy and PIO close have returned. */
	mt6797_consys_runtime_controller_abort(controller, bootstrap, platform_lease);
	complete_all(&controller->stop_complete);
}

static const struct mt6797_consys_runtime_lease *
mt6797_consys_runtime_controller_get_runtime_lease(void *context)
{
	struct mt6797_consys_runtime_controller *controller = context;

	/* This returns only identity held by an earlier chronology owner. */
	return mt6797_consys_runtime_platform_lease(controller->runtime_dev);
}

static const struct mt6797_btif_pio_platform_lease *
mt6797_consys_runtime_controller_acquire_platform(void *context)
{
	struct mt6797_consys_runtime_controller *controller = context;

	return mt6797_btif_pio_platform_acquire(controller->btif_dev);
}

static struct mt6797_btif_stp_bootstrap *
mt6797_consys_runtime_controller_create_bootstrap(void *context,
		const struct mt6797_btif_pio_platform_lease *platform_lease,
		const struct mt6797_consys_runtime_lease *runtime_lease)
{
	return mt6797_btif_pio_platform_bootstrap_create(platform_lease,
							    runtime_lease);
}

static const struct mt6797_consys_stp_session_lease *
mt6797_consys_runtime_controller_run_bootstrap(void *context,
		struct mt6797_btif_stp_bootstrap *bootstrap)
{
	return mt6797_btif_stp_bootstrap_run(bootstrap);
}

static const struct mt6797_btif_stp_bootstrap_full_wmt_lease *
mt6797_consys_runtime_controller_bootstrap_wmt_lease(void *context,
		struct mt6797_btif_stp_bootstrap *bootstrap)
{
	return mt6797_btif_stp_bootstrap_full_wmt_lease(bootstrap);
}

static int mt6797_consys_runtime_controller_patch_download(void *context,
		struct device *dev,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	return mt6797_consys_wmt_patch_download(dev, lease);
}

static int mt6797_consys_runtime_controller_postpatch(void *context,
		struct device *dev,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		struct regulator *bt_paldo, struct regulator *wifi_paldo)
{
	return mt6797_consys_wmt_postpatch_run(dev, lease, bt_paldo, wifi_paldo);
}

static int mt6797_consys_runtime_controller_prepare_stages(
		struct mt6797_consys_runtime_controller *controller,
		mt6797_consys_runtime_controller_lifecycle_t run, void *context)
{
	int ret;

	if (controller->platform_failed)
		return -ESHUTDOWN;
	ret = run(controller, MT6797_CONSYS_LIFECYCLE_ACQUIRE_RESOURCES,
		  context);
	if (ret)
		return ret;
	controller->resources_held = true;

	if (!controller->emi_claimed) {
		ret = run(controller, MT6797_CONSYS_LIFECYCLE_CLAIM_EMI,
			  context);
		if (ret)
			return ret;
		controller->emi_claimed = true;
	}

	ret = run(controller, MT6797_CONSYS_LIFECYCLE_ACQUIRE_IDENTITY,
		  context);
	if (ret)
		goto fatal;
	controller->identity_held = true;

	ret = run(controller, MT6797_CONSYS_LIFECYCLE_INIT_POSTID, context);
	if (ret)
		goto fatal;
	/* A failed start may have changed hardware and therefore requires stop. */
	controller->postid_active = true;
	ret = run(controller, MT6797_CONSYS_LIFECYCLE_START_POSTID, context);
	if (ret)
		goto fatal;
	return 0;

fatal:
	controller->platform_failed = true;
	run(controller, MT6797_CONSYS_LIFECYCLE_LATCH_FATAL, context);
	return ret;
}

static int mt6797_consys_runtime_controller_unprepare_stages(
		struct mt6797_consys_runtime_controller *controller,
		mt6797_consys_runtime_controller_lifecycle_t run, void *context)
{
	int ret;

	if (controller->postid_active) {
		ret = run(controller, MT6797_CONSYS_LIFECYCLE_STOP_POSTID,
			  context);
		if (ret)
			goto fatal;
		controller->postid_active = false;
	}
	if (controller->identity_held) {
		ret = run(controller, MT6797_CONSYS_LIFECYCLE_RELEASE_IDENTITY,
			  context);
		if (ret)
			goto fatal;
		controller->identity_held = false;
	}
	if (controller->resources_held) {
		ret = run(controller, MT6797_CONSYS_LIFECYCLE_RELEASE_RESOURCES,
			  context);
		if (ret)
			goto fatal;
		controller->resources_held = false;
	}

	return 0;

fatal:
	controller->platform_failed = true;
	run(controller, MT6797_CONSYS_LIFECYCLE_LATCH_FATAL, context);
	return ret;
}

static int mt6797_consys_runtime_controller_platform_lifecycle(
		struct mt6797_consys_runtime_controller *controller,
		enum mt6797_consys_runtime_controller_lifecycle_step step,
		void *context)
{
	(void)context;

	switch (step) {
	case MT6797_CONSYS_LIFECYCLE_ACQUIRE_RESOURCES:
		return mt6797_consys_runtime_resource_lease_acquire(
			controller->runtime_dev, &controller->resources);
	case MT6797_CONSYS_LIFECYCLE_CLAIM_EMI:
		return mt6797_consys_emi_mpu_claim(controller->dev);
	case MT6797_CONSYS_LIFECYCLE_ACQUIRE_IDENTITY:
		return mt6797_consys_runtime_platform_identity_acquire(
			&controller->resources, &controller->identity);
	case MT6797_CONSYS_LIFECYCLE_INIT_POSTID:
		return mt6797_consys_runtime_postid_owner_init(
			controller->runtime_dev, controller->identity.identity,
			&controller->postid);
	case MT6797_CONSYS_LIFECYCLE_START_POSTID:
		return mt6797_consys_postid_start(&controller->postid,
			controller->identity.identity);
	case MT6797_CONSYS_LIFECYCLE_STOP_POSTID:
		return mt6797_consys_postid_stop(&controller->postid);
	case MT6797_CONSYS_LIFECYCLE_RELEASE_IDENTITY:
		return mt6797_consys_runtime_platform_identity_release(
			&controller->identity);
	case MT6797_CONSYS_LIFECYCLE_RELEASE_RESOURCES:
		return mt6797_consys_runtime_resource_lease_release(
			&controller->resources);
	case MT6797_CONSYS_LIFECYCLE_LATCH_FATAL:
		mt6797_consys_emi_mpu_latch_fatal();
		return 0;
	}
	return -EINVAL;
}

static int mt6797_consys_runtime_controller_prepare(void *context)
{
	struct mt6797_consys_runtime_controller *controller = context;

	return mt6797_consys_runtime_controller_prepare_stages(controller,
		mt6797_consys_runtime_controller_platform_lifecycle, NULL);
}

static void mt6797_consys_runtime_controller_unprepare(void *context)
{
	struct mt6797_consys_runtime_controller *controller = context;
	int ret;

	ret = mt6797_consys_runtime_controller_unprepare_stages(controller,
		mt6797_consys_runtime_controller_platform_lifecycle, NULL);
	if (ret)
		dev_err(controller->dev,
			"failed to release common CONSYS lifecycle: %d\n", ret);
}

static void mt6797_consys_runtime_controller_destroy_bootstrap(void *context,
		struct mt6797_btif_stp_bootstrap *bootstrap)
{
	mt6797_btif_stp_bootstrap_destroy(bootstrap);
}

static void mt6797_consys_runtime_controller_close_platform(void *context,
		const struct mt6797_btif_pio_platform_lease *platform_lease)
{
	mt6797_btif_pio_platform_close(platform_lease);
}

static const struct mt6797_consys_runtime_controller_ops
		mt6797_consys_runtime_controller_platform_ops = {
	.runtime_lease = mt6797_consys_runtime_controller_get_runtime_lease,
	.platform_acquire = mt6797_consys_runtime_controller_acquire_platform,
	.bootstrap_create = mt6797_consys_runtime_controller_create_bootstrap,
	.bootstrap_run = mt6797_consys_runtime_controller_run_bootstrap,
	.bootstrap_wmt_lease =
		mt6797_consys_runtime_controller_bootstrap_wmt_lease,
	.patch_download = mt6797_consys_runtime_controller_patch_download,
	.postpatch = mt6797_consys_runtime_controller_postpatch,
	.bootstrap_destroy = mt6797_consys_runtime_controller_destroy_bootstrap,
	.platform_close = mt6797_consys_runtime_controller_close_platform,
	.prepare = mt6797_consys_runtime_controller_prepare,
	.unprepare = mt6797_consys_runtime_controller_unprepare,
};

static int mt6797_consys_runtime_controller_get_supplier(struct device *dev,
		const char *property, struct device **supplier)
{
	struct device_node *node;
	struct platform_device *pdev;

	node = of_parse_phandle(dev->of_node, property, 0);
	if (!node)
		return dev_err_probe(dev, -EINVAL, "missing %s provider\n", property);
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "%s provider is not registered\n", property);
	if (&pdev->dev == dev) {
		put_device(&pdev->dev);
		return dev_err_probe(dev, -EINVAL,
				     "%s cannot refer to the controller\n", property);
	}
	*supplier = &pdev->dev;
	return 0;
}

static int mt6797_consys_runtime_controller_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_consys_runtime_controller *controller;
	int ret;

	controller = devm_kzalloc(dev, sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return -ENOMEM;
	controller->dev = dev;
	ret = mt6797_consys_runtime_controller_init(controller,
					    &mt6797_consys_runtime_controller_platform_ops,
					    controller);
	if (ret)
		return ret;

	ret = mt6797_consys_runtime_controller_get_supplier(dev,
						    "mediatek,runtime-provider",
						    &controller->runtime_dev);
	if (ret)
		return ret;
	ret = mt6797_consys_runtime_controller_get_supplier(dev,
						    "mediatek,btif-provider",
						    &controller->btif_dev);
	if (ret)
		goto put_runtime;
	if (!device_link_add(dev, controller->runtime_dev,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		ret = dev_err_probe(dev, -ENOMEM,
				    "failed to link runtime provider\n");
		goto put_btif;
	}
	if (!device_link_add(dev, controller->btif_dev,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		ret = dev_err_probe(dev, -ENOMEM,
				    "failed to link BTIF provider\n");
		goto put_btif;
	}
	controller->bt_paldo = devm_regulator_get(dev, "bt-paldo");
	if (IS_ERR(controller->bt_paldo)) {
		ret = dev_err_probe(dev, PTR_ERR(controller->bt_paldo),
				    "failed to get Bluetooth PALDO\n");
		goto put_btif;
	}
	controller->wifi_paldo = devm_regulator_get(dev, "wifi-paldo");
	if (IS_ERR(controller->wifi_paldo)) {
		ret = dev_err_probe(dev, PTR_ERR(controller->wifi_paldo),
				    "failed to get Wi-Fi PALDO\n");
		goto put_btif;
	}
	if (regulator_is_equal(controller->bt_paldo,
			       controller->wifi_paldo)) {
		ret = dev_err_probe(dev, -EINVAL,
				    "Bluetooth and Wi-Fi PALDOs must be distinct\n");
		goto put_btif;
	}

	platform_set_drvdata(pdev, controller);
	dev_info(dev, "STP controller registered\n");
	return 0;

put_btif:
	put_device(controller->btif_dev);
put_runtime:
	put_device(controller->runtime_dev);
	return ret;
}

static void mt6797_consys_runtime_controller_remove(struct platform_device *pdev)
{
	struct mt6797_consys_runtime_controller *controller =
		platform_get_drvdata(pdev);

	if (!controller)
		return;
	mutex_lock(&controller->lock);
	controller->removing = true;
	mutex_unlock(&controller->lock);
	mt6797_consys_runtime_controller_stop_controller(controller);
	platform_set_drvdata(pdev, NULL);
	put_device(controller->btif_dev);
	put_device(controller->runtime_dev);
}

static const struct of_device_id mt6797_consys_runtime_controller_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys-runtime-controller" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_consys_runtime_controller_of_match);

static struct platform_driver mt6797_consys_runtime_controller_driver = {
	.probe = mt6797_consys_runtime_controller_probe,
	.remove = mt6797_consys_runtime_controller_remove,
	.driver = {
		.name = "mt6797-consys-runtime-controller",
		.of_match_table = mt6797_consys_runtime_controller_of_match,
		/* A successful handover owns resources until its strict teardown. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_consys_runtime_controller_driver);

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_RUNTIME_CONTROLLER_KUNIT_TEST)

enum mt6797_consys_runtime_controller_test_call {
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN,
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_WMT,
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PATCH,
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_POSTPATCH,
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CLOSE,
};

struct mt6797_consys_runtime_controller_test_backend {
	enum mt6797_consys_runtime_controller_test_call calls[24];
	struct mt6797_consys_runtime_lease runtime_token;
	u8 platform_token;
	u8 bootstrap_token;
	u8 session_token;
	struct completion run_entered;
	struct completion allow_run;
	struct completion patch_entered;
	struct completion allow_patch;
	struct completion destroy_entered;
	struct completion allow_destroy;
	unsigned int call_count;
	int runtime_error;
	int platform_error;
	int create_error;
	int run_error;
	int patch_error;
	int postpatch_error;
	bool block_run;
	bool block_patch;
	bool block_destroy;
};

struct mt6797_consys_runtime_controller_test_task {
	struct mt6797_consys_runtime_controller *controller;
	struct mt6797_consys_runtime_controller_bt_lease *bt_lease;
	const struct mt6797_consys_stp_session_lease *session;
	struct completion done;
	int result;
	bool stop;
	bool put_bt;
	bool get_bt;
};

struct mt6797_consys_runtime_controller_lifecycle_test {
	enum mt6797_consys_runtime_controller_lifecycle_step calls[16];
	unsigned int call_count;
	int fail_step;
};

static int mt6797_consys_runtime_controller_test_lifecycle(
		struct mt6797_consys_runtime_controller *controller,
		enum mt6797_consys_runtime_controller_lifecycle_step step,
		void *context)
{
	struct mt6797_consys_runtime_controller_lifecycle_test *test = context;

	if (test->call_count < ARRAY_SIZE(test->calls))
		test->calls[test->call_count++] = step;
	return test->fail_step == step ? -EIO : 0;
}

static void mt6797_consys_runtime_controller_test_prepare_failure_unwinds(
		struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_lifecycle_step failures[] = {
		MT6797_CONSYS_LIFECYCLE_ACQUIRE_RESOURCES,
		MT6797_CONSYS_LIFECYCLE_CLAIM_EMI,
		MT6797_CONSYS_LIFECYCLE_ACQUIRE_IDENTITY,
		MT6797_CONSYS_LIFECYCLE_INIT_POSTID,
		MT6797_CONSYS_LIFECYCLE_START_POSTID,
	};
	struct mt6797_consys_runtime_controller_lifecycle_test backend;
	struct mt6797_consys_runtime_controller controller;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(failures); i++) {
		memset(&controller, 0, sizeof(controller));
		memset(&backend, 0, sizeof(backend));
		backend.fail_step = failures[i];
		KUNIT_EXPECT_EQ(test,
			mt6797_consys_runtime_controller_prepare_stages(&controller,
				mt6797_consys_runtime_controller_test_lifecycle,
				&backend), -EIO);
		backend.fail_step = -1;
		KUNIT_EXPECT_EQ(test,
			mt6797_consys_runtime_controller_unprepare_stages(&controller,
				mt6797_consys_runtime_controller_test_lifecycle,
				&backend), 0);
		KUNIT_EXPECT_FALSE(test, controller.resources_held);
		KUNIT_EXPECT_FALSE(test, controller.identity_held);
		KUNIT_EXPECT_FALSE(test, controller.postid_active);
		KUNIT_EXPECT_EQ(test, controller.platform_failed,
			failures[i] >= MT6797_CONSYS_LIFECYCLE_ACQUIRE_IDENTITY);
		if (failures[i] >= MT6797_CONSYS_LIFECYCLE_ACQUIRE_IDENTITY)
			KUNIT_EXPECT_EQ(test,
				backend.calls[i + 1],
				MT6797_CONSYS_LIFECYCLE_LATCH_FATAL);
	}
}

static void mt6797_consys_runtime_controller_test_cleanup_fails_closed(
		struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_lifecycle_step failures[] = {
		MT6797_CONSYS_LIFECYCLE_STOP_POSTID,
		MT6797_CONSYS_LIFECYCLE_RELEASE_IDENTITY,
		MT6797_CONSYS_LIFECYCLE_RELEASE_RESOURCES,
	};
	struct mt6797_consys_runtime_controller_lifecycle_test backend;
	struct mt6797_consys_runtime_controller controller;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(failures); i++) {
		memset(&controller, 0, sizeof(controller));
		memset(&backend, 0, sizeof(backend));
		backend.fail_step = -1;
		KUNIT_ASSERT_EQ(test,
			mt6797_consys_runtime_controller_prepare_stages(&controller,
				mt6797_consys_runtime_controller_test_lifecycle,
				&backend), 0);
		backend.fail_step = failures[i];
		KUNIT_EXPECT_EQ(test,
			mt6797_consys_runtime_controller_unprepare_stages(&controller,
				mt6797_consys_runtime_controller_test_lifecycle,
				&backend), -EIO);
		KUNIT_EXPECT_TRUE(test, controller.platform_failed);
		KUNIT_EXPECT_EQ(test, backend.calls[backend.call_count - 1],
			MT6797_CONSYS_LIFECYCLE_LATCH_FATAL);
		KUNIT_EXPECT_EQ(test, controller.postid_active,
			failures[i] == MT6797_CONSYS_LIFECYCLE_STOP_POSTID);
		KUNIT_EXPECT_EQ(test, controller.identity_held,
			failures[i] <= MT6797_CONSYS_LIFECYCLE_RELEASE_IDENTITY);
		KUNIT_EXPECT_TRUE(test, controller.resources_held);
	}
}

static void mt6797_consys_runtime_controller_test_lifecycle_restarts(
		struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_lifecycle_step expected[] = {
		MT6797_CONSYS_LIFECYCLE_ACQUIRE_RESOURCES,
		MT6797_CONSYS_LIFECYCLE_CLAIM_EMI,
		MT6797_CONSYS_LIFECYCLE_ACQUIRE_IDENTITY,
		MT6797_CONSYS_LIFECYCLE_INIT_POSTID,
		MT6797_CONSYS_LIFECYCLE_START_POSTID,
		MT6797_CONSYS_LIFECYCLE_STOP_POSTID,
		MT6797_CONSYS_LIFECYCLE_RELEASE_IDENTITY,
		MT6797_CONSYS_LIFECYCLE_RELEASE_RESOURCES,
		MT6797_CONSYS_LIFECYCLE_ACQUIRE_RESOURCES,
		MT6797_CONSYS_LIFECYCLE_ACQUIRE_IDENTITY,
		MT6797_CONSYS_LIFECYCLE_INIT_POSTID,
		MT6797_CONSYS_LIFECYCLE_START_POSTID,
		MT6797_CONSYS_LIFECYCLE_STOP_POSTID,
		MT6797_CONSYS_LIFECYCLE_RELEASE_IDENTITY,
		MT6797_CONSYS_LIFECYCLE_RELEASE_RESOURCES,
	};
	struct mt6797_consys_runtime_controller_lifecycle_test backend = {
		.fail_step = -1,
	};
	struct mt6797_consys_runtime_controller controller = { };
	unsigned int i;

	for (i = 0; i < 2; i++) {
		KUNIT_ASSERT_EQ(test,
			mt6797_consys_runtime_controller_prepare_stages(&controller,
				mt6797_consys_runtime_controller_test_lifecycle,
				&backend), 0);
		KUNIT_ASSERT_EQ(test,
			mt6797_consys_runtime_controller_unprepare_stages(&controller,
				mt6797_consys_runtime_controller_test_lifecycle,
				&backend), 0);
	}
	KUNIT_EXPECT_TRUE(test, controller.emi_claimed);
	KUNIT_EXPECT_FALSE(test, controller.platform_failed);
	KUNIT_EXPECT_EQ(test, backend.call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend.calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_controller_test_record(void *context,
		enum mt6797_consys_runtime_controller_test_call call)
{
	struct mt6797_consys_runtime_controller_test_backend *backend = context;

	if (backend->call_count < ARRAY_SIZE(backend->calls))
		backend->calls[backend->call_count++] = call;
}

static const struct mt6797_consys_runtime_lease *
mt6797_consys_runtime_controller_test_runtime(void *context)
{
	struct mt6797_consys_runtime_controller_test_backend *backend = context;

	mt6797_consys_runtime_controller_test_record(backend,
			MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME);
	if (backend->runtime_error)
		return ERR_PTR(backend->runtime_error);
	return &backend->runtime_token;
}

static const struct mt6797_btif_pio_platform_lease *
mt6797_consys_runtime_controller_test_platform(void *context)
{
	struct mt6797_consys_runtime_controller_test_backend *backend = context;

	mt6797_consys_runtime_controller_test_record(backend,
			MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM);
	if (backend->platform_error)
		return ERR_PTR(backend->platform_error);
	return (const void *)&backend->platform_token;
}

static struct mt6797_btif_stp_bootstrap *
mt6797_consys_runtime_controller_test_create(void *context,
		const struct mt6797_btif_pio_platform_lease *platform_lease,
		const struct mt6797_consys_runtime_lease *runtime_lease)
{
	struct mt6797_consys_runtime_controller_test_backend *backend = context;

	mt6797_consys_runtime_controller_test_record(backend,
			MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE);
	if (backend->create_error)
		return ERR_PTR(backend->create_error);
	return (void *)&backend->bootstrap_token;
}

static const struct mt6797_consys_stp_session_lease *
mt6797_consys_runtime_controller_test_run(void *context,
		struct mt6797_btif_stp_bootstrap *bootstrap)
{
	struct mt6797_consys_runtime_controller_test_backend *backend = context;

	mt6797_consys_runtime_controller_test_record(backend,
			MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN);
	if (backend->block_run) {
		complete_all(&backend->run_entered);
		wait_for_completion(&backend->allow_run);
	}
	if (backend->run_error)
		return ERR_PTR(backend->run_error);
	return (const void *)&backend->session_token;
}

static const struct mt6797_btif_stp_bootstrap_full_wmt_lease *
mt6797_consys_runtime_controller_test_wmt(void *context,
		struct mt6797_btif_stp_bootstrap *bootstrap)
{
	struct mt6797_consys_runtime_controller_test_backend *backend = context;

	mt6797_consys_runtime_controller_test_record(backend,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_WMT);
	return (const void *)&backend->session_token;
}

static int mt6797_consys_runtime_controller_test_patch(void *context,
		struct device *dev,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	struct mt6797_consys_runtime_controller_test_backend *backend = context;

	mt6797_consys_runtime_controller_test_record(backend,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PATCH);
	if (backend->block_patch) {
		complete_all(&backend->patch_entered);
		wait_for_completion(&backend->allow_patch);
	}
	return backend->patch_error;
}

static int mt6797_consys_runtime_controller_test_postpatch(void *context,
		struct device *dev,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		struct regulator *bt_paldo, struct regulator *wifi_paldo)
{
	struct mt6797_consys_runtime_controller_test_backend *backend = context;

	mt6797_consys_runtime_controller_test_record(backend,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_POSTPATCH);
	return backend->postpatch_error;
}

static void mt6797_consys_runtime_controller_test_destroy(void *context,
		struct mt6797_btif_stp_bootstrap *bootstrap)
{
	struct mt6797_consys_runtime_controller_test_backend *backend = context;

	mt6797_consys_runtime_controller_test_record(backend,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY);
	if (backend->block_destroy) {
		complete_all(&backend->destroy_entered);
		wait_for_completion(&backend->allow_destroy);
	}
}

static void mt6797_consys_runtime_controller_test_close(void *context,
		const struct mt6797_btif_pio_platform_lease *platform_lease)
{
	mt6797_consys_runtime_controller_test_record(context,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CLOSE);
}

static const struct mt6797_consys_runtime_controller_ops
		mt6797_consys_runtime_controller_test_ops = {
	.runtime_lease = mt6797_consys_runtime_controller_test_runtime,
	.platform_acquire = mt6797_consys_runtime_controller_test_platform,
	.bootstrap_create = mt6797_consys_runtime_controller_test_create,
	.bootstrap_run = mt6797_consys_runtime_controller_test_run,
	.bootstrap_wmt_lease = mt6797_consys_runtime_controller_test_wmt,
	.patch_download = mt6797_consys_runtime_controller_test_patch,
	.postpatch = mt6797_consys_runtime_controller_test_postpatch,
	.bootstrap_destroy = mt6797_consys_runtime_controller_test_destroy,
	.platform_close = mt6797_consys_runtime_controller_test_close,
};

static struct mt6797_consys_runtime_controller *
mt6797_consys_runtime_controller_test_init(struct kunit *test,
		struct mt6797_consys_runtime_controller_test_backend **backend_out)
{
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;

	backend = kunit_kzalloc(test, sizeof(*backend), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, backend);
	init_completion(&backend->run_entered);
	init_completion(&backend->allow_run);
	init_completion(&backend->patch_entered);
	init_completion(&backend->allow_patch);
	init_completion(&backend->destroy_entered);
	init_completion(&backend->allow_destroy);
	controller = kunit_kzalloc(test, sizeof(*controller), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, controller);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_controller_init(controller,
							 &mt6797_consys_runtime_controller_test_ops,
							 backend), 0);
	*backend_out = backend;
	return controller;
}

static int mt6797_consys_runtime_controller_test_thread(void *context)
{
	struct mt6797_consys_runtime_controller_test_task *task = context;

	if (task->stop) {
		mt6797_consys_runtime_controller_stop_controller(task->controller);
		task->result = 0;
	} else if (task->put_bt) {
		mt6797_consys_runtime_controller_bt_session_put(task->bt_lease);
		task->result = 0;
	} else if (task->get_bt) {
		task->result = mt6797_consys_runtime_controller_start_controller(
			task->controller);
		if (!task->result)
			task->result = mt6797_consys_bt_session_get(
				task->controller, task->bt_lease, &task->session);
	} else {
		task->result = mt6797_consys_runtime_controller_start_unchecked(
			task->controller);
	}
	complete_all(&task->done);
	return 0;
}

static void mt6797_consys_runtime_controller_test_open_races_last_close(
		struct kunit *test)
{
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;
	struct mt6797_consys_runtime_controller_bt_lease first_lease = { };
	struct mt6797_consys_runtime_controller_bt_lease second_lease = { };
	const struct mt6797_consys_stp_session_lease *session;
	struct mt6797_consys_runtime_controller_test_task close_task = { };
	struct mt6797_consys_runtime_controller_test_task open_task = { };
	struct task_struct *close_thread;
	struct task_struct *open_thread;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_runtime_controller_start_controller(controller), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_bt_session_get(controller, &first_lease, &session), 0);
	backend->block_destroy = true;
	close_task.controller = controller;
	close_task.bt_lease = &first_lease;
	close_task.put_bt = true;
	init_completion(&close_task.done);
	close_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
		&close_task, "mt6797-ctrl-last-close");
	KUNIT_ASSERT_FALSE(test, IS_ERR(close_thread));
	KUNIT_ASSERT_NE(test,
		wait_for_completion_timeout(&backend->destroy_entered,
			msecs_to_jiffies(1000)), 0UL);

	open_task.controller = controller;
	open_task.bt_lease = &second_lease;
	open_task.get_bt = true;
	init_completion(&open_task.done);
	open_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
		&open_task, "mt6797-ctrl-racing-open");
	KUNIT_ASSERT_FALSE(test, IS_ERR(open_thread));
	usleep_range(10000, 20000);
	KUNIT_EXPECT_FALSE(test, completion_done(&open_task.done));

	complete_all(&backend->allow_destroy);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&close_task.done,
		msecs_to_jiffies(1000)), 0UL);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&open_task.done,
		msecs_to_jiffies(1000)), 0UL);
	KUNIT_EXPECT_EQ(test, open_task.result, 0);
	KUNIT_EXPECT_PTR_EQ(test, controller->bt_lease_owner, &second_lease);
	KUNIT_EXPECT_EQ(test, controller->state,
		MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE);
	mt6797_consys_runtime_controller_bt_session_put(&second_lease);
	KUNIT_EXPECT_EQ(test, controller->state,
		MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
}

static void mt6797_consys_runtime_controller_test_order(struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_WMT,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PATCH,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_POSTPATCH,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
	};
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_runtime_controller_start_unchecked(controller), 0);
	KUNIT_EXPECT_EQ(test, controller->state,
				MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE);
	mt6797_consys_runtime_controller_stop_controller(controller);
	KUNIT_EXPECT_EQ(test, controller->state,
				MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_controller_test_failure_unwinds(struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
	};
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	backend->run_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_runtime_controller_start_unchecked(controller),
		-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, controller->state,
				MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_controller_test_create_failure_closes_platform(
		struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CLOSE,
	};
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	backend->create_error = -ENOMEM;
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_runtime_controller_start_unchecked(controller),
		-ENOMEM);
	KUNIT_EXPECT_EQ(test, controller->state,
			 MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_controller_test_patch_failure_recovers(
		struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_WMT,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PATCH,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
	};
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	backend->patch_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_runtime_controller_start_unchecked(controller),
		-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, controller->state,
			MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_controller_test_postpatch_failure_recovers(
		struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_WMT,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PATCH,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_POSTPATCH,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
	};
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	backend->postpatch_error = -EIO;
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_runtime_controller_start_unchecked(controller),
		-EIO);
	KUNIT_EXPECT_EQ(test, controller->state,
			 MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_controller_test_stop_waits_for_run(
		struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
	};
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;
	struct mt6797_consys_runtime_controller_test_task run_task = { };
	struct mt6797_consys_runtime_controller_test_task stop_task = { };
	struct task_struct *run_thread;
	struct task_struct *stop_thread;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	backend->block_run = true;
	run_task.controller = controller;
	init_completion(&run_task.done);
	run_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
					 &run_task, "mt6797-ctrl-run");
	KUNIT_ASSERT_FALSE(test, IS_ERR(run_thread));
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&backend->run_entered,
							  msecs_to_jiffies(1000)), 0UL);

	stop_task.controller = controller;
	stop_task.stop = true;
	init_completion(&stop_task.done);
	stop_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
					  &stop_task, "mt6797-ctrl-stop");
	KUNIT_ASSERT_FALSE(test, IS_ERR(stop_thread));
	usleep_range(10000, 20000);
	KUNIT_EXPECT_EQ(test, backend->call_count, 4U);
	complete_all(&backend->allow_run);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&run_task.done,
							  msecs_to_jiffies(1000)), 0UL);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&stop_task.done,
							  msecs_to_jiffies(1000)), 0UL);
	KUNIT_EXPECT_EQ(test, run_task.result, -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, controller->state,
				MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

/*
 * remove() enters the same stop_controller() path. Once patch download has
 * begun, stop must wait for the startup runner to return before it can
 * destroy that private full-STP endpoint or close BTIF.
 */
static void mt6797_consys_runtime_controller_test_stop_waits_for_patch(
		struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_WMT,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PATCH,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
	};
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;
	struct mt6797_consys_runtime_controller_gnss_lease gnss_lease = { };
	const struct mt6797_consys_stp_session_lease *session_lease;
	struct mt6797_consys_runtime_controller_test_task run_task = { };
	struct mt6797_consys_runtime_controller_test_task stop_task = { };
	struct task_struct *run_thread;
	struct task_struct *stop_thread;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	backend->block_patch = true;
	run_task.controller = controller;
	init_completion(&run_task.done);
	run_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
				 &run_task, "mt6797-ctrl-patch-run");
	KUNIT_ASSERT_FALSE(test, IS_ERR(run_thread));
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&backend->patch_entered,
						  msecs_to_jiffies(1000)), 0UL);

	/* The session remains private until patch and postpatch return. */
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_runtime_controller_gnss_session_get_controller(controller,
			&gnss_lease, &session_lease), -EAGAIN);
	KUNIT_EXPECT_EQ(test, backend->call_count, 6U);

	stop_task.controller = controller;
	stop_task.stop = true;
	init_completion(&stop_task.done);
	stop_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
				  &stop_task, "mt6797-ctrl-patch-stop");
	KUNIT_ASSERT_FALSE(test, IS_ERR(stop_thread));
	usleep_range(10000, 20000);
	KUNIT_EXPECT_FALSE(test, completion_done(&stop_task.done));
	KUNIT_EXPECT_EQ(test, backend->call_count, 6U);

	complete_all(&backend->allow_patch);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&run_task.done,
						  msecs_to_jiffies(1000)), 0UL);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&stop_task.done,
						  msecs_to_jiffies(1000)), 0UL);
	KUNIT_EXPECT_EQ(test, run_task.result, -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, controller->state,
			MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_controller_test_gnss_lease_drains_before_stop(
		struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_WMT,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PATCH,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_POSTPATCH,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
	};
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;
	struct mt6797_consys_runtime_controller_gnss_lease gnss_lease = { };
	struct mt6797_consys_runtime_controller_gnss_lease second_lease = { };
	const struct mt6797_consys_stp_session_lease *session_lease = NULL;
	struct mt6797_consys_runtime_controller_test_task stop_task = { };
	struct task_struct *stop_thread;
	unsigned int retries;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_runtime_controller_start_unchecked(controller), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_runtime_controller_gnss_session_get_controller(controller,
			&gnss_lease, &session_lease), 0);
	KUNIT_EXPECT_PTR_EQ(test, session_lease,
		(const void *)&backend->session_token);
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_runtime_controller_gnss_session_get_controller(controller,
			&second_lease, &session_lease), -EBUSY);

	stop_task.controller = controller;
	stop_task.stop = true;
	init_completion(&stop_task.done);
	stop_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
				  &stop_task, "mt6797-ctrl-gnss-stop");
	KUNIT_ASSERT_FALSE(test, IS_ERR(stop_thread));
	for (retries = 0; retries < 100; retries++) {
		if (READ_ONCE(controller->state) ==
			MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING)
			break;
		usleep_range(1000, 2000);
	}
	KUNIT_ASSERT_EQ(test, READ_ONCE(controller->state),
			MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING);
	KUNIT_EXPECT_EQ(test, backend->call_count, 7U);
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_runtime_controller_gnss_session_get_controller(controller,
			&second_lease, &session_lease), -ESHUTDOWN);

	mt6797_consys_runtime_controller_gnss_session_put_controller(&gnss_lease);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&stop_task.done,
						  msecs_to_jiffies(1000)), 0UL);
	KUNIT_EXPECT_EQ(test, controller->state,
			MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_controller_test_concurrent_stop_waits_for_close(
		struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_WMT,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PATCH,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_POSTPATCH,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
	};
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;
	struct mt6797_consys_runtime_controller_test_task first_stop = { };
	struct mt6797_consys_runtime_controller_test_task second_stop = { };
	struct task_struct *first_thread;
	struct task_struct *second_thread;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_runtime_controller_start_unchecked(controller), 0);
	backend->block_destroy = true;
	first_stop.controller = controller;
	first_stop.stop = true;
	init_completion(&first_stop.done);
	first_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
				   &first_stop, "mt6797-ctrl-first-stop");
	KUNIT_ASSERT_FALSE(test, IS_ERR(first_thread));
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&backend->destroy_entered,
						  msecs_to_jiffies(1000)), 0UL);
	KUNIT_EXPECT_EQ(test, controller->state,
			MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING);
	KUNIT_EXPECT_EQ(test, backend->call_count, 8U);

	second_stop.controller = controller;
	second_stop.stop = true;
	init_completion(&second_stop.done);
	second_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
				    &second_stop, "mt6797-ctrl-second-stop");
	KUNIT_ASSERT_FALSE(test, IS_ERR(second_thread));
	usleep_range(10000, 20000);
	KUNIT_EXPECT_FALSE(test, completion_done(&second_stop.done));
	KUNIT_EXPECT_EQ(test, backend->call_count, 8U);

	complete_all(&backend->allow_destroy);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&first_stop.done,
						  msecs_to_jiffies(1000)), 0UL);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&second_stop.done,
						  msecs_to_jiffies(1000)), 0UL);
	KUNIT_EXPECT_EQ(test, controller->state,
			MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_controller_test_wifi_release_preserves_radios(
		struct kunit *test)
{
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;
	struct mt6797_consys_runtime_controller_wifi_lease wifi_lease = { };
	struct mt6797_consys_runtime_controller_bt_lease bt_lease = { };
	struct mt6797_consys_runtime_controller_bt_lease second_bt = { };
	struct mt6797_consys_runtime_controller_gnss_lease gnss_lease = { };
	u64 first_generation;
	const struct mt6797_consys_stp_session_lease *wifi_session;
	const struct mt6797_consys_stp_session_lease *bt_session;
	const struct mt6797_consys_stp_session_lease *gnss_session;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_runtime_controller_start_unchecked(controller), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_runtime_controller_wifi_session_get_controller(controller,
			&wifi_lease, &wifi_session), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_bt_session_get(controller,
			&bt_lease, &bt_session), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_runtime_controller_gnss_session_get_controller(
			controller, &gnss_lease, &gnss_session), 0);
	KUNIT_EXPECT_PTR_EQ(test, wifi_session, bt_session);
	KUNIT_EXPECT_PTR_EQ(test, wifi_session, gnss_session);
	first_generation = wifi_lease.generation;
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_bt_session_get(controller,
			&second_bt, &bt_session), -EBUSY);
	/* ndo_stop releases WLAN without synchronously draining other radios. */
	mt6797_consys_runtime_controller_wifi_session_put(&wifi_lease);
	KUNIT_EXPECT_EQ(test, controller->state,
		MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE);
	KUNIT_EXPECT_PTR_EQ(test, controller->bt_lease_owner, &bt_lease);
	KUNIT_EXPECT_PTR_EQ(test, controller->gnss_lease_owner, &gnss_lease);
	mt6797_consys_runtime_controller_bt_session_put(&bt_lease);
	KUNIT_EXPECT_EQ(test, controller->state,
		MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE);
	mt6797_consys_runtime_controller_gnss_session_put_controller(&gnss_lease);
	KUNIT_EXPECT_EQ(test, controller->state,
		MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, 8U);
	KUNIT_EXPECT_EQ(test, backend->calls[backend->call_count - 1],
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY);

	/* A later first borrower starts a fresh full-STP generation. */
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_runtime_controller_start_controller(controller), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_bt_session_get(controller, &second_bt, &bt_session), 0);
	KUNIT_EXPECT_GT(test, second_bt.generation, first_generation);
	mt6797_consys_runtime_controller_bt_session_put(&second_bt);
	KUNIT_EXPECT_EQ(test, controller->state,
		MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
	KUNIT_EXPECT_EQ(test, backend->call_count, 16U);
}

static void mt6797_consys_runtime_controller_test_stop_waits_for_bt(
		struct kunit *test)
{
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;
	struct mt6797_consys_runtime_controller_bt_lease bt_lease = { };
	const struct mt6797_consys_stp_session_lease *session;
	struct mt6797_consys_runtime_controller_test_task stop_task = { };
	struct task_struct *stop_thread;
	unsigned int retries;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_runtime_controller_start_unchecked(controller), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_consys_bt_session_get(controller,
			&bt_lease, &session), 0);

	stop_task.controller = controller;
	stop_task.stop = true;
	init_completion(&stop_task.done);
	stop_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
		&stop_task, "mt6797-ctrl-bt-stop");
	KUNIT_ASSERT_FALSE(test, IS_ERR(stop_thread));
	for (retries = 0; retries < 100; retries++) {
		if (READ_ONCE(controller->state) ==
		    MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING)
			break;
		usleep_range(1000, 2000);
	}
	KUNIT_ASSERT_EQ(test, READ_ONCE(controller->state),
		MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING);
	KUNIT_EXPECT_FALSE(test, completion_done(&stop_task.done));
	mt6797_consys_runtime_controller_bt_session_put(&bt_lease);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&stop_task.done,
		msecs_to_jiffies(1000)), 0UL);
	KUNIT_EXPECT_EQ(test, controller->state,
		MT6797_CONSYS_RUNTIME_CONTROLLER_READY);
}

static struct kunit_case mt6797_consys_runtime_controller_test_cases[] = {
	KUNIT_CASE(mt6797_consys_runtime_controller_test_prepare_failure_unwinds),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_cleanup_fails_closed),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_lifecycle_restarts),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_order),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_failure_unwinds),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_create_failure_closes_platform),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_patch_failure_recovers),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_postpatch_failure_recovers),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_stop_waits_for_run),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_stop_waits_for_patch),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_gnss_lease_drains_before_stop),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_concurrent_stop_waits_for_close),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_wifi_release_preserves_radios),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_open_races_last_close),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_stop_waits_for_bt),
	{ }
};

static struct kunit_suite mt6797_consys_runtime_controller_test_suite = {
	.name = "mt6797-consys-runtime-controller",
	.test_cases = mt6797_consys_runtime_controller_test_cases,
};

kunit_test_suite(mt6797_consys_runtime_controller_test_suite);

#endif

MODULE_DESCRIPTION("MediaTek MT6797 CONSYS BTIF STP-bootstrap coordinator");
MODULE_LICENSE("GPL");
