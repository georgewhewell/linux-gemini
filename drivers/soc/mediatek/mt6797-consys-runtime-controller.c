// SPDX-License-Identifier: GPL-2.0-only
/*
 * Default-disabled MT6797 CONSYS runtime/BTIF bootstrap coordinator.
 *
 * This composes, but does not enlarge, the established evidence boundaries:
 *
 * - the runtime provider keeps the vendor identity prefix held through chip
 *   ID 0x0279 (mtk_wcn_consys_hw.c:301-354,401-420);
 * - the PIO adapter owns the exact BTIF core and its disabled core IRQ
 *   (arch/arm64/boot/dts/mt6797.dtsi:2511-2516 and mtk_btif.c:1472-1481);
 * - the bootstrap performs precisely query-default, set-full, a 10-ms host
 *   switch, and query-full (wmt_ic_soc.c:122-124,190-191,1008-1044).
 *
 * No DT node in the shipped Gemini image matches this driver. Even a custom
 * node needs a boolean property and this read-only default-false parameter.
 * Binding does not start a transaction. A separate in-kernel caller must
 * start it, at which point the runtime, platform, and bootstrap's own gates
 * all still apply. This driver has no firmware loader, client registration,
 * radio-function command, reset release, AFE write, shared-EMI mapping, or
 * userspace ABI.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_RUNTIME_CONTROLLER_KUNIT_TEST)
#include <kunit/test.h>
#include <linux/kthread.h>
#endif

#include <linux/soc/mediatek/mt6797-btif-pio-platform.h>
#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mt6797-consys-runtime-controller.h>
#include <linux/soc/mediatek/mt6797-consys-runtime.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>

static bool enable_experimental_runtime_controller;
module_param_named(enable_experimental_runtime_controller,
			   enable_experimental_runtime_controller, bool, 0400);
MODULE_PARM_DESC(enable_experimental_runtime_controller,
		 "allow the gated MT6797 CONSYS STP-bootstrap coordinator (default: false)");

enum mt6797_consys_runtime_controller_state {
	MT6797_CONSYS_RUNTIME_CONTROLLER_READY,
	MT6797_CONSYS_RUNTIME_CONTROLLER_STARTING,
	MT6797_CONSYS_RUNTIME_CONTROLLER_ACTIVE,
	MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING,
	MT6797_CONSYS_RUNTIME_CONTROLLER_FENCED,
};

struct mt6797_consys_runtime_controller_ops {
	const struct mt6797_consys_runtime_lease *(*runtime_lease)(void *context);
	const struct mt6797_btif_pio_platform_lease *(*platform_acquire)(void *context);
	struct mt6797_btif_stp_bootstrap *(*bootstrap_create)(void *context,
			const struct mt6797_btif_pio_platform_lease *platform_lease,
			const struct mt6797_consys_runtime_lease *runtime_lease);
	const struct mt6797_consys_stp_session_lease *(*bootstrap_run)(void *context,
			struct mt6797_btif_stp_bootstrap *bootstrap);
	void (*bootstrap_destroy)(void *context,
			struct mt6797_btif_stp_bootstrap *bootstrap);
	void (*platform_close)(void *context,
			const struct mt6797_btif_pio_platform_lease *platform_lease);
};

struct mt6797_consys_runtime_controller {
	struct device *dev;
	struct device *runtime_dev;
	struct device *btif_dev;
	const struct mt6797_consys_runtime_controller_ops *ops;
	void *ops_context;
	struct mutex lock;
	struct completion run_complete;
	struct completion gnss_released;
	struct completion stop_complete;
	const struct mt6797_consys_runtime_lease *runtime_lease;
	const struct mt6797_btif_pio_platform_lease *platform_lease;
	struct mt6797_btif_stp_bootstrap *bootstrap;
	const struct mt6797_consys_stp_session_lease *session_lease;
	struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease_owner;
	u64 session_generation;
	bool run_in_progress;
	bool stop_requested;
	enum mt6797_consys_runtime_controller_state state;
};

static bool mt6797_consys_runtime_controller_ops_valid(
		const struct mt6797_consys_runtime_controller_ops *ops)
{
	return ops && ops->runtime_lease && ops->platform_acquire &&
		ops->bootstrap_create && ops->bootstrap_run &&
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

static void mt6797_consys_runtime_controller_complete_fenced(
		struct mt6797_consys_runtime_controller *controller)
{
	mutex_lock(&controller->lock);
	controller->state = MT6797_CONSYS_RUNTIME_CONTROLLER_FENCED;
	controller->run_in_progress = false;
	complete_all(&controller->run_complete);
	mutex_unlock(&controller->lock);
}

static void mt6797_consys_runtime_controller_abort(
		struct mt6797_consys_runtime_controller *controller,
		struct mt6797_btif_stp_bootstrap *bootstrap,
		const struct mt6797_btif_pio_platform_lease *platform_lease)
{
	/* bootstrap_destroy synchronizes its WMT runner before closing PIO. */
	if (bootstrap)
		controller->ops->bootstrap_destroy(controller->ops_context, bootstrap);
	/* This additionally covers a reserved adapter if bootstrap never ran. */
	if (platform_lease)
		controller->ops->platform_close(controller->ops_context, platform_lease);
	mt6797_consys_runtime_controller_complete_fenced(controller);
}

static int mt6797_consys_runtime_controller_start_unchecked(
		struct mt6797_consys_runtime_controller *controller)
{
	const struct mt6797_consys_runtime_lease *runtime_lease = NULL;
	const struct mt6797_btif_pio_platform_lease *platform_lease = NULL;
	const struct mt6797_consys_stp_session_lease *session_lease;
	struct mt6797_btif_stp_bootstrap *bootstrap = NULL;
	int ret;

	mutex_lock(&controller->lock);
	if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_FENCED) {
		mutex_unlock(&controller->lock);
		return -EPIPE;
	}
	if (controller->state != MT6797_CONSYS_RUNTIME_CONTROLLER_READY) {
		mutex_unlock(&controller->lock);
		return -EBUSY;
	}
	controller->state = MT6797_CONSYS_RUNTIME_CONTROLLER_STARTING;
	controller->run_in_progress = true;
	reinit_completion(&controller->run_complete);
	mutex_unlock(&controller->lock);

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
	/* A FENCED controller is one-shot, so this cannot wrap during its life. */
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
	if (!controller)
		return -EINVAL;
	if (!READ_ONCE(enable_experimental_runtime_controller))
		return -EPERM;

	return mt6797_consys_runtime_controller_start_unchecked(controller);
}

int mt6797_consys_runtime_controller_start(struct device *dev)
{
	struct mt6797_consys_runtime_controller *controller;

	if (!dev)
		return -EINVAL;
	controller = dev_get_drvdata(dev);
	if (!controller)
		return -ENODEV;

	return mt6797_consys_runtime_controller_start_controller(controller);
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_start);

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

	/* stop() waits on this exact owner before it can destroy full STP. */
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

	if (!dev)
		return -EINVAL;
	controller = dev_get_drvdata(dev);
	if (!controller)
		return -ENODEV;

	return mt6797_consys_runtime_controller_gnss_session_get_controller(
		controller, gnss_lease, session_lease);
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_gnss_session_get);

static void mt6797_consys_runtime_controller_gnss_session_put_controller(
		struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease)
{
	struct mt6797_consys_runtime_controller *controller;

	if (!gnss_lease || !gnss_lease->controller)
		return;

	controller = gnss_lease->controller;
	mutex_lock(&controller->lock);
	if (controller->gnss_lease_owner == gnss_lease &&
	    controller->session_generation == gnss_lease->generation) {
		controller->gnss_lease_owner = NULL;
		complete_all(&controller->gnss_released);
	}
	gnss_lease->controller = NULL;
	gnss_lease->generation = 0;
	mutex_unlock(&controller->lock);
}

void mt6797_consys_runtime_controller_gnss_session_put(
		struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease)
{
	mt6797_consys_runtime_controller_gnss_session_put_controller(gnss_lease);
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_gnss_session_put);

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
	if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_FENCED) {
		mutex_unlock(&controller->lock);
		return;
	}
	if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING) {
		mutex_unlock(&controller->lock);
		wait_for_completion(&controller->stop_complete);
		return;
	}
	if (controller->state == MT6797_CONSYS_RUNTIME_CONTROLLER_READY) {
		controller->state = MT6797_CONSYS_RUNTIME_CONTROLLER_FENCED;
		mutex_unlock(&controller->lock);
		return;
	}

	/* ACTIVE has one winner; it first drains an opaque GNSS session borrower. */
	controller->stop_requested = true;
	controller->state = MT6797_CONSYS_RUNTIME_CONTROLLER_QUIESCING;
	reinit_completion(&controller->stop_complete);
	mutex_unlock(&controller->lock);
	wait_for_completion(&controller->gnss_released);

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

void mt6797_consys_runtime_controller_stop(struct device *dev)
{
	if (!dev)
		return;

	mt6797_consys_runtime_controller_stop_controller(dev_get_drvdata(dev));
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_controller_stop);

static const struct mt6797_consys_runtime_lease *
mt6797_consys_runtime_controller_get_runtime_lease(void *context)
{
	struct mt6797_consys_runtime_controller *controller = context;

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
	.bootstrap_destroy = mt6797_consys_runtime_controller_destroy_bootstrap,
	.platform_close = mt6797_consys_runtime_controller_close_platform,
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

	if (!READ_ONCE(enable_experimental_runtime_controller) ||
	    !device_property_read_bool(dev, "mediatek,experimental-bootstrap"))
		return dev_err_probe(dev, -EPERM,
				     "controller requires explicit DT and parameter opt-in\n");

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

	platform_set_drvdata(pdev, controller);
	dev_info(dev, "gated STP controller armed; no handover or client started\n");
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
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
	MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CLOSE,
};

struct mt6797_consys_runtime_controller_test_backend {
	enum mt6797_consys_runtime_controller_test_call calls[8];
	struct mt6797_consys_runtime_lease runtime_token;
	u8 platform_token;
	u8 bootstrap_token;
	u8 session_token;
	struct completion run_entered;
	struct completion allow_run;
	struct completion destroy_entered;
	struct completion allow_destroy;
	unsigned int call_count;
	int runtime_error;
	int platform_error;
	int create_error;
	int run_error;
	bool block_run;
	bool block_destroy;
};

struct mt6797_consys_runtime_controller_test_task {
	struct mt6797_consys_runtime_controller *controller;
	struct completion done;
	int result;
	bool stop;
};

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
	} else {
		task->result = mt6797_consys_runtime_controller_start_unchecked(
			task->controller);
	}
	complete_all(&task->done);
	return 0;
}

static void mt6797_consys_runtime_controller_test_order(struct kunit *test)
{
	static const enum mt6797_consys_runtime_controller_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUNTIME,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_PLATFORM,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CREATE,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_RUN,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CLOSE,
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
				MT6797_CONSYS_RUNTIME_CONTROLLER_FENCED);
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
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CLOSE,
	};
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	backend->run_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_runtime_controller_start_unchecked(controller),
		-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, controller->state,
				MT6797_CONSYS_RUNTIME_CONTROLLER_FENCED);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_controller_test_gate(struct kunit *test)
{
	struct mt6797_consys_runtime_controller_test_backend *backend;
	struct mt6797_consys_runtime_controller *controller;
	bool enabled = READ_ONCE(enable_experimental_runtime_controller);

	controller = mt6797_consys_runtime_controller_test_init(test, &backend);
	WRITE_ONCE(enable_experimental_runtime_controller, false);
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_runtime_controller_start_controller(controller), -EPERM);
	KUNIT_EXPECT_EQ(test, backend->call_count, 0U);
	WRITE_ONCE(enable_experimental_runtime_controller, enabled);
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
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CLOSE,
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
				MT6797_CONSYS_RUNTIME_CONTROLLER_FENCED);
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
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CLOSE,
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
	KUNIT_EXPECT_EQ(test, backend->call_count, 4U);
	KUNIT_EXPECT_EQ(test,
		mt6797_consys_runtime_controller_gnss_session_get_controller(controller,
			&second_lease, &session_lease), -ESHUTDOWN);

	mt6797_consys_runtime_controller_gnss_session_put_controller(&gnss_lease);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&stop_task.done,
						  msecs_to_jiffies(1000)), 0UL);
	KUNIT_EXPECT_EQ(test, controller->state,
			MT6797_CONSYS_RUNTIME_CONTROLLER_FENCED);
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
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_DESTROY,
		MT6797_CONSYS_RUNTIME_CONTROLLER_TEST_CLOSE,
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
	KUNIT_EXPECT_EQ(test, backend->call_count, 5U);

	second_stop.controller = controller;
	second_stop.stop = true;
	init_completion(&second_stop.done);
	second_thread = kthread_run(mt6797_consys_runtime_controller_test_thread,
				    &second_stop, "mt6797-ctrl-second-stop");
	KUNIT_ASSERT_FALSE(test, IS_ERR(second_thread));
	usleep_range(10000, 20000);
	KUNIT_EXPECT_FALSE(test, completion_done(&second_stop.done));
	KUNIT_EXPECT_EQ(test, backend->call_count, 5U);

	complete_all(&backend->allow_destroy);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&first_stop.done,
						  msecs_to_jiffies(1000)), 0UL);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&second_stop.done,
						  msecs_to_jiffies(1000)), 0UL);
	KUNIT_EXPECT_EQ(test, controller->state,
			MT6797_CONSYS_RUNTIME_CONTROLLER_FENCED);
	KUNIT_EXPECT_EQ(test, backend->call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend->calls, expected, sizeof(expected));
}

static struct kunit_case mt6797_consys_runtime_controller_test_cases[] = {
	KUNIT_CASE(mt6797_consys_runtime_controller_test_order),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_failure_unwinds),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_gate),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_stop_waits_for_run),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_gnss_lease_drains_before_stop),
	KUNIT_CASE(mt6797_consys_runtime_controller_test_concurrent_stop_waits_for_close),
	{ }
};

static struct kunit_suite mt6797_consys_runtime_controller_test_suite = {
	.name = "mt6797-consys-runtime-controller",
	.test_cases = mt6797_consys_runtime_controller_test_cases,
};

kunit_test_suite(mt6797_consys_runtime_controller_test_suite);

#endif

MODULE_DESCRIPTION("Gated MT6797 CONSYS BTIF STP-bootstrap coordinator");
MODULE_LICENSE("GPL");
