// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 GNSS endpoint over controller-owned full STP. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/gnss.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>

#if IS_ENABLED(CONFIG_GNSS_MTK_CONSYS_KUNIT_TEST)
#include <kunit/test.h>
#endif

#include <linux/soc/mediatek/mt6797-consys-runtime-controller.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>

struct mtk_consys_gnss;

struct mtk_consys_gnss_ops {
	int (*session_get)(struct mtk_consys_gnss *gnss);
	void (*session_put)(struct mtk_consys_gnss *gnss);
	int (*bind)(struct mtk_consys_gnss *gnss);
	int (*unbind)(struct mtk_consys_gnss *gnss);
	int (*function_set)(struct mtk_consys_gnss *gnss, bool enable);
	int (*lna_set)(struct mtk_consys_gnss *gnss, bool enable);
	int (*transmit)(struct mtk_consys_gnss *gnss, const u8 *data,
			size_t length);
};

struct mtk_consys_gnss {
	struct device *dev;
	struct device *controller;
	struct regulator *lna;
	struct gnss_device *gdev;
	/* Serialize open, close, I/O, suspend and remove state transitions. */
	struct mutex lock;
	const struct mtk_consys_gnss_ops *ops;
	void *ops_context;
	struct mt6797_consys_runtime_controller_gnss_lease controller_lease;
	const struct mt6797_consys_stp_session_lease *stp_lease;
	bool bound;
	bool lna_on;
	bool function_on;
	bool active;
	bool accepting;
	bool suspended;
	bool faulted;
};

static int mtk_consys_gnss_receive(void *context, const u8 *data,
				   size_t length)
{
	struct mtk_consys_gnss *gnss = context;
	int ret;

	if (!gnss || (!data && length))
		return -EINVAL;
	if (!READ_ONCE(gnss->accepting))
		return 0;

	ret = gnss_insert_raw(gnss->gdev, data, length);
	if (ret < 0)
		return ret;
	return ret == length ? 0 : -ENOSPC;
}

static int mtk_consys_gnss_session_get(struct mtk_consys_gnss *gnss)
{
	return mt6797_consys_runtime_controller_gnss_session_get(gnss->controller,
		&gnss->controller_lease, &gnss->stp_lease);
}

static void mtk_consys_gnss_session_put(struct mtk_consys_gnss *gnss)
{
	mt6797_consys_runtime_controller_gnss_session_put(&gnss->controller_lease);
	gnss->stp_lease = NULL;
}

static int mtk_consys_gnss_bind(struct mtk_consys_gnss *gnss)
{
	return mt6797_consys_stp_session_bind_gnss(gnss->stp_lease,
		mtk_consys_gnss_receive, gnss);
}

static int mtk_consys_gnss_unbind(struct mtk_consys_gnss *gnss)
{
	return mt6797_consys_stp_session_unbind_gnss(gnss->stp_lease,
		mtk_consys_gnss_receive, gnss);
}

static int mtk_consys_gnss_function_set(struct mtk_consys_gnss *gnss,
					bool enable)
{
	struct mt6797_consys_runtime_controller_gnss_lease *lease;

	lease = &gnss->controller_lease;
	return mt6797_consys_runtime_controller_gnss_function_set(lease, enable);
}

static int mtk_consys_gnss_lna_set(struct mtk_consys_gnss *gnss, bool enable)
{
	return enable ? regulator_enable(gnss->lna) : regulator_disable(gnss->lna);
}

static int mtk_consys_gnss_transmit(struct mtk_consys_gnss *gnss,
				    const u8 *data, size_t length)
{
	return mt6797_consys_stp_session_transmit_gnss(gnss->stp_lease,
		mtk_consys_gnss_receive, gnss, data, length);
}

static const struct mtk_consys_gnss_ops mtk_consys_gnss_platform_ops = {
	.session_get = mtk_consys_gnss_session_get,
	.session_put = mtk_consys_gnss_session_put,
	.bind = mtk_consys_gnss_bind,
	.unbind = mtk_consys_gnss_unbind,
	.function_set = mtk_consys_gnss_function_set,
	.lna_set = mtk_consys_gnss_lna_set,
	.transmit = mtk_consys_gnss_transmit,
};

static int mtk_consys_gnss_release_locked(struct mtk_consys_gnss *gnss)
{
	int first_error = 0;
	int ret;

	WRITE_ONCE(gnss->accepting, false);
	gnss->active = false;
	gnss->suspended = false;
	if (gnss->function_on) {
		ret = gnss->ops->function_set(gnss, false);
		if (ret)
			first_error = ret;
		gnss->function_on = false;
	}
	if (gnss->lna_on) {
		ret = gnss->ops->lna_set(gnss, false);
		if (!first_error && ret)
			first_error = ret;
		gnss->lna_on = false;
	}
	if (gnss->bound) {
		ret = gnss->ops->unbind(gnss);
		if (!first_error && ret && ret != -ESHUTDOWN)
			first_error = ret;
		gnss->bound = false;
	}
	if (gnss->controller_lease.controller)
		gnss->ops->session_put(gnss);
	if (first_error)
		gnss->faulted = true;
	return first_error;
}

static int mtk_consys_gnss_start_locked(struct mtk_consys_gnss *gnss)
{
	int cleanup;
	int ret;

	if (gnss->faulted)
		return -EIO;
	if (gnss->active)
		return 0;

	ret = gnss->ops->session_get(gnss);
	if (ret)
		return ret;
	ret = gnss->ops->bind(gnss);
	if (ret)
		goto fail;
	gnss->bound = true;

	/* Vendor MT6797 enables the external LNA before GPS FUNC_CTRL. */
	gnss->lna_on = true;
	ret = gnss->ops->lna_set(gnss, true);
	if (ret)
		goto fail;
	gnss->function_on = true;
	ret = gnss->ops->function_set(gnss, true);
	if (ret)
		goto fail;

	gnss->active = true;
	WRITE_ONCE(gnss->accepting, true);
	return 0;

fail:
	cleanup = mtk_consys_gnss_release_locked(gnss);
	if (cleanup)
		dev_err(gnss->dev, "GNSS startup cleanup failed: %d\n", cleanup);
	return ret;
}

static int mtk_consys_gnss_open(struct gnss_device *gdev)
{
	struct mtk_consys_gnss *gnss = gnss_get_drvdata(gdev);
	int ret;

	mutex_lock(&gnss->lock);
	ret = mtk_consys_gnss_start_locked(gnss);
	mutex_unlock(&gnss->lock);
	return ret;
}

static void mtk_consys_gnss_close(struct gnss_device *gdev)
{
	struct mtk_consys_gnss *gnss = gnss_get_drvdata(gdev);
	int ret;

	mutex_lock(&gnss->lock);
	ret = mtk_consys_gnss_release_locked(gnss);
	mutex_unlock(&gnss->lock);
	if (ret)
		dev_err(gnss->dev, "GNSS shutdown failed: %d\n", ret);
}

static int mtk_consys_gnss_write_locked(struct mtk_consys_gnss *gnss,
					const u8 *data, size_t length)
{
	int ret;

	if (gnss->faulted) {
		ret = -EIO;
	} else if (!gnss->active || gnss->suspended) {
		ret = -EPIPE;
	} else {
		ret = gnss->ops->transmit(gnss, data, length);
		if (!ret) {
			ret = length;
		} else {
			int cleanup = mtk_consys_gnss_release_locked(gnss);

			gnss->faulted = true;
			if (cleanup)
				dev_err(gnss->dev,
					"GNSS transmit cleanup failed: %d\n",
					cleanup);
		}
	}
	return ret;
}

static int mtk_consys_gnss_write_raw(struct gnss_device *gdev,
				     const unsigned char *data, size_t length)
{
	struct mtk_consys_gnss *gnss = gnss_get_drvdata(gdev);
	int ret;

	mutex_lock(&gnss->lock);
	ret = mtk_consys_gnss_write_locked(gnss, data, length);
	mutex_unlock(&gnss->lock);
	return ret;
}

static const struct gnss_operations mtk_consys_gnss_ops = {
	.open = mtk_consys_gnss_open,
	.close = mtk_consys_gnss_close,
	.write_raw = mtk_consys_gnss_write_raw,
};

static int mtk_consys_gnss_suspend_locked(struct mtk_consys_gnss *gnss)
{
	int ret = 0;

	if (!gnss->active || gnss->suspended)
		return 0;
	WRITE_ONCE(gnss->accepting, false);
	if (gnss->function_on) {
		ret = gnss->ops->function_set(gnss, false);
		if (!ret)
			gnss->function_on = false;
	}
	if (!ret && gnss->lna_on) {
		ret = gnss->ops->lna_set(gnss, false);
		if (!ret)
			gnss->lna_on = false;
	}
	if (!ret && gnss->bound) {
		ret = gnss->ops->unbind(gnss);
		if (!ret)
			gnss->bound = false;
	}
	if (ret) {
		mtk_consys_gnss_release_locked(gnss);
		gnss->faulted = true;
	} else {
		gnss->ops->session_put(gnss);
		gnss->suspended = true;
	}
	return ret;
}

static int mtk_consys_gnss_resume_locked(struct mtk_consys_gnss *gnss)
{
	int ret = 0;

	if (!gnss->active || !gnss->suspended)
		return 0;
	ret = gnss->ops->session_get(gnss);
	if (ret)
		goto fail;
	ret = gnss->ops->bind(gnss);
	if (ret)
		goto fail;
	gnss->bound = true;
	gnss->lna_on = true;
	ret = gnss->ops->lna_set(gnss, true);
	if (ret)
		goto fail;
	gnss->function_on = true;
	ret = gnss->ops->function_set(gnss, true);
	if (ret)
		goto fail;
	gnss->suspended = false;
	WRITE_ONCE(gnss->accepting, true);
	return 0;
fail:
	mtk_consys_gnss_release_locked(gnss);
	gnss->faulted = true;
	return ret;
}

static int mtk_consys_gnss_suspend(struct device *dev)
{
	struct mtk_consys_gnss *gnss = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&gnss->lock);
	ret = mtk_consys_gnss_suspend_locked(gnss);
	mutex_unlock(&gnss->lock);
	return ret;
}

static int mtk_consys_gnss_resume(struct device *dev)
{
	struct mtk_consys_gnss *gnss = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&gnss->lock);
	ret = mtk_consys_gnss_resume_locked(gnss);
	mutex_unlock(&gnss->lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mtk_consys_gnss_pm_ops,
		mtk_consys_gnss_suspend, mtk_consys_gnss_resume);

static int mtk_consys_gnss_probe(struct platform_device *pdev)
{
	struct platform_device *controller;
	struct mtk_consys_gnss *gnss;
	struct device_node *node;
	int ret;

	node = of_parse_phandle(pdev->dev.of_node, "mediatek,controller", 0);
	if (!node)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing mediatek,controller\n");
	controller = of_find_device_by_node(node);
	of_node_put(node);
	if (!controller)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "CONSYS controller is not registered\n");

	gnss = devm_kzalloc(&pdev->dev, sizeof(*gnss), GFP_KERNEL);
	if (!gnss) {
		ret = -ENOMEM;
		goto put_controller;
	}
	gnss->dev = &pdev->dev;
	gnss->controller = &controller->dev;
	gnss->ops = &mtk_consys_gnss_platform_ops;
	mutex_init(&gnss->lock);
	if (!device_link_add(&pdev->dev, gnss->controller,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		ret = -ENOMEM;
		goto put_controller;
	}
	gnss->lna = devm_regulator_get(&pdev->dev, "lna");
	if (IS_ERR(gnss->lna)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(gnss->lna),
				    "failed to get antenna LNA supply\n");
		goto put_controller;
	}
	gnss->gdev = gnss_allocate_device(&pdev->dev);
	if (!gnss->gdev) {
		ret = -ENOMEM;
		goto put_controller;
	}
	gnss->gdev->type = GNSS_TYPE_MTK;
	gnss->gdev->ops = &mtk_consys_gnss_ops;
	gnss_set_drvdata(gnss->gdev, gnss);
	platform_set_drvdata(pdev, gnss);
	ret = gnss_register_device(gnss->gdev);
	if (ret)
		goto put_gnss;
	return 0;

put_gnss:
	platform_set_drvdata(pdev, NULL);
	gnss_put_device(gnss->gdev);
put_controller:
	put_device(&controller->dev);
	return ret;
}

static void mtk_consys_gnss_remove(struct platform_device *pdev)
{
	struct mtk_consys_gnss *gnss = platform_get_drvdata(pdev);

	if (!gnss)
		return;
	gnss_deregister_device(gnss->gdev);
	mutex_lock(&gnss->lock);
	mtk_consys_gnss_release_locked(gnss);
	mutex_unlock(&gnss->lock);
	gnss_put_device(gnss->gdev);
	put_device(gnss->controller);
}

static const struct of_device_id mtk_consys_gnss_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys-gnss" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_consys_gnss_of_match);

static struct platform_driver mtk_consys_gnss_driver = {
	.probe = mtk_consys_gnss_probe,
	.remove = mtk_consys_gnss_remove,
	.driver = {
		.name = "mt6797-consys-gnss",
		.of_match_table = mtk_consys_gnss_of_match,
		.pm = pm_sleep_ptr(&mtk_consys_gnss_pm_ops),
	},
};
module_platform_driver(mtk_consys_gnss_driver);

MODULE_DESCRIPTION("MediaTek MT6797 GNSS endpoint over full STP");
MODULE_LICENSE("GPL");

#if IS_ENABLED(CONFIG_GNSS_MTK_CONSYS_KUNIT_TEST)

struct mtk_consys_gnss_test {
	unsigned int get_calls;
	unsigned int put_calls;
	unsigned int bind_calls;
	unsigned int unbind_calls;
	unsigned int function_on_calls;
	unsigned int function_off_calls;
	unsigned int lna_on_calls;
	unsigned int lna_off_calls;
	unsigned int tx_calls;
	int function_on_error;
	int function_off_error;
	int transmit_error;
};

static struct mtk_consys_gnss_test *
mtk_consys_gnss_test_data(struct mtk_consys_gnss *gnss)
{
	return gnss->ops_context;
}

static int mtk_consys_gnss_test_session_get(struct mtk_consys_gnss *gnss)
{
	struct mtk_consys_gnss_test *test = mtk_consys_gnss_test_data(gnss);

	test->get_calls++;
	gnss->controller_lease.controller = (void *)gnss;
	gnss->stp_lease = (void *)gnss;
	return 0;
}

static void mtk_consys_gnss_test_session_put(struct mtk_consys_gnss *gnss)
{
	mtk_consys_gnss_test_data(gnss)->put_calls++;
	gnss->controller_lease.controller = NULL;
	gnss->stp_lease = NULL;
}

static int mtk_consys_gnss_test_bind(struct mtk_consys_gnss *gnss)
{
	mtk_consys_gnss_test_data(gnss)->bind_calls++;
	return 0;
}

static int mtk_consys_gnss_test_unbind(struct mtk_consys_gnss *gnss)
{
	mtk_consys_gnss_test_data(gnss)->unbind_calls++;
	return 0;
}

static int mtk_consys_gnss_test_function_set(struct mtk_consys_gnss *gnss,
					     bool enable)
{
	struct mtk_consys_gnss_test *test = mtk_consys_gnss_test_data(gnss);

	if (enable) {
		test->function_on_calls++;
		return test->function_on_error;
	}
	test->function_off_calls++;
	return test->function_off_error;
}

static int mtk_consys_gnss_test_lna_set(struct mtk_consys_gnss *gnss,
					bool enable)
{
	struct mtk_consys_gnss_test *test = mtk_consys_gnss_test_data(gnss);

	if (enable)
		test->lna_on_calls++;
	else
		test->lna_off_calls++;
	return 0;
}

static int mtk_consys_gnss_test_transmit(struct mtk_consys_gnss *gnss,
					 const u8 *data, size_t length)
{
	struct mtk_consys_gnss_test *test = mtk_consys_gnss_test_data(gnss);

	test->tx_calls++;
	return test->transmit_error;
}

static const struct mtk_consys_gnss_ops mtk_consys_gnss_test_ops = {
	.session_get = mtk_consys_gnss_test_session_get,
	.session_put = mtk_consys_gnss_test_session_put,
	.bind = mtk_consys_gnss_test_bind,
	.unbind = mtk_consys_gnss_test_unbind,
	.function_set = mtk_consys_gnss_test_function_set,
	.lna_set = mtk_consys_gnss_test_lna_set,
	.transmit = mtk_consys_gnss_test_transmit,
};

static struct mtk_consys_gnss *
mtk_consys_gnss_test_init(struct kunit *test,
			  struct mtk_consys_gnss_test **backend)
{
	struct mtk_consys_gnss *gnss;

	gnss = kunit_kzalloc(test, sizeof(*gnss), GFP_KERNEL);
	if (!gnss)
		return NULL;
	*backend = kunit_kzalloc(test, sizeof(**backend), GFP_KERNEL);
	if (!*backend)
		return NULL;
	gnss->ops = &mtk_consys_gnss_test_ops;
	gnss->ops_context = *backend;
	mutex_init(&gnss->lock);
	return gnss;
}

static void mtk_consys_gnss_test_lifecycle(struct kunit *test)
{
	struct mtk_consys_gnss_test *backend;
	struct mtk_consys_gnss *gnss = mtk_consys_gnss_test_init(test, &backend);

	KUNIT_ASSERT_NOT_NULL(test, gnss);
	KUNIT_ASSERT_EQ(test, mtk_consys_gnss_start_locked(gnss), 0);
	KUNIT_EXPECT_TRUE(test, gnss->active);
	KUNIT_EXPECT_TRUE(test, gnss->accepting);
	KUNIT_EXPECT_EQ(test, backend->function_on_calls, 1U);
	KUNIT_EXPECT_EQ(test, mtk_consys_gnss_release_locked(gnss), 0);
	KUNIT_EXPECT_EQ(test, backend->function_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, backend->lna_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, backend->unbind_calls, 1U);
	KUNIT_EXPECT_EQ(test, backend->put_calls, 1U);
}

static void mtk_consys_gnss_test_failed_start_cleans_up(struct kunit *test)
{
	struct mtk_consys_gnss_test *backend;
	struct mtk_consys_gnss *gnss = mtk_consys_gnss_test_init(test, &backend);

	KUNIT_ASSERT_NOT_NULL(test, gnss);
	backend->function_on_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, mtk_consys_gnss_start_locked(gnss), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, backend->function_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, backend->lna_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, backend->unbind_calls, 1U);
	KUNIT_EXPECT_EQ(test, backend->put_calls, 1U);
	KUNIT_EXPECT_FALSE(test, gnss->active);
	KUNIT_EXPECT_FALSE(test, gnss->faulted);
}

static void mtk_consys_gnss_test_suspend_resume(struct kunit *test)
{
	struct mtk_consys_gnss_test *backend;
	struct mtk_consys_gnss *gnss = mtk_consys_gnss_test_init(test, &backend);

	KUNIT_ASSERT_NOT_NULL(test, gnss);
	KUNIT_ASSERT_EQ(test, mtk_consys_gnss_start_locked(gnss), 0);
	KUNIT_ASSERT_EQ(test, mtk_consys_gnss_suspend_locked(gnss), 0);
	KUNIT_EXPECT_TRUE(test, gnss->suspended);
	KUNIT_EXPECT_FALSE(test, gnss->accepting);
	KUNIT_EXPECT_EQ(test, backend->unbind_calls, 1U);
	KUNIT_EXPECT_EQ(test, backend->put_calls, 1U);
	KUNIT_ASSERT_EQ(test, mtk_consys_gnss_resume_locked(gnss), 0);
	KUNIT_EXPECT_FALSE(test, gnss->suspended);
	KUNIT_EXPECT_TRUE(test, gnss->accepting);
	KUNIT_EXPECT_EQ(test, backend->function_on_calls, 2U);
	KUNIT_EXPECT_EQ(test, backend->function_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, backend->get_calls, 2U);
	KUNIT_EXPECT_EQ(test, backend->bind_calls, 2U);
	mtk_consys_gnss_release_locked(gnss);
}

static void mtk_consys_gnss_test_tx_failure_is_terminal(struct kunit *test)
{
	static const u8 payload[] = { 0x24, 0x47 };
	struct mtk_consys_gnss_test *backend;
	struct mtk_consys_gnss *gnss = mtk_consys_gnss_test_init(test, &backend);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, gnss);
	KUNIT_ASSERT_EQ(test, mtk_consys_gnss_start_locked(gnss), 0);
	backend->transmit_error = -EIO;
	ret = mtk_consys_gnss_write_locked(gnss, payload, sizeof(payload));
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_TRUE(test, gnss->faulted);
	KUNIT_EXPECT_FALSE(test, gnss->active);
	KUNIT_EXPECT_EQ(test, backend->function_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, backend->put_calls, 1U);
}

static struct kunit_case mtk_consys_gnss_test_cases[] = {
	KUNIT_CASE(mtk_consys_gnss_test_lifecycle),
	KUNIT_CASE(mtk_consys_gnss_test_failed_start_cleans_up),
	KUNIT_CASE(mtk_consys_gnss_test_suspend_resume),
	KUNIT_CASE(mtk_consys_gnss_test_tx_failure_is_terminal),
	{ }
};

static struct kunit_suite mtk_consys_gnss_test_suite = {
	.name = "mtk-consys-gnss",
	.test_cases = mtk_consys_gnss_test_cases,
};

kunit_test_suite(mtk_consys_gnss_test_suite);

#endif
