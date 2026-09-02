// SPDX-License-Identifier: GPL-2.0-only
/*
 * V4L2 bridge for the MediaTek MT6797 SENINF/CSI-2 receiver.
 *
 * Only the Gemini camera1/sub NCSI2 route is enabled. Other sensor-interface
 * instances and modes remain unavailable until their hardware sequences are
 * independently established.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

#include "mtk_seninf_core.h"

#define MT6797_SENINF_MCLK2_CTRL	0x1600
#define MT6797_SENINF_MCLK2_ON		(BIT(31) | BIT(29))
#define MT6797_SENINF_MCLK2_GATE	BIT(29)
#define MT6797_SENINF_MCLK_RATE		24000000UL

struct mt6797_seninf {
	struct device *dev;
	struct clk *seninf_clk;
	struct clk *camtg_clk;
	struct clk *mclk_parent;
	struct clk_hw mclk_hw;
	void __iomem *core;
	void __iomem *csi0_ana;
	void __iomem *csi1_ana;
	u32 calibration[MT6797_SENINF_CAL_WORDS];
	unsigned int data_lanes[4];
	unsigned int num_data_lanes;
	bool streaming;
	struct mutex lock; /* Protects streaming state and receiver programming. */

	struct v4l2_subdev subdev;
	struct media_pad pads[MT6797_SENINF_NUM_PADS];
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev *source;
	unsigned int source_pad;
};

static inline struct mt6797_seninf *
to_mt6797_seninf_mclk(struct clk_hw *hw)
{
	return container_of(hw, struct mt6797_seninf, mclk_hw);
}

static int mt6797_seninf_mclk_prepare(struct clk_hw *hw)
{
	struct mt6797_seninf *seninf = to_mt6797_seninf_mclk(hw);
	u32 value;
	int ret;

	ret = pm_runtime_resume_and_get(seninf->dev);
	if (ret < 0)
		return ret;

	ret = clk_prepare_enable(seninf->camtg_clk);
	if (ret) {
		pm_runtime_put(seninf->dev);
		return ret;
	}

	value = readl(seninf->core + MT6797_SENINF_MCLK2_CTRL);
	writel(value | MT6797_SENINF_MCLK2_ON,
	       seninf->core + MT6797_SENINF_MCLK2_CTRL);
	return 0;
}

static void mt6797_seninf_mclk_unprepare(struct clk_hw *hw)
{
	struct mt6797_seninf *seninf = to_mt6797_seninf_mclk(hw);
	u32 value;

	value = readl(seninf->core + MT6797_SENINF_MCLK2_CTRL);
	writel(value & ~MT6797_SENINF_MCLK2_GATE,
	       seninf->core + MT6797_SENINF_MCLK2_CTRL);
	clk_disable_unprepare(seninf->camtg_clk);
	pm_runtime_put(seninf->dev);
}

static unsigned long mt6797_seninf_mclk_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	return parent_rate / 2;
}

static const struct clk_ops mt6797_seninf_mclk_ops = {
	.prepare = mt6797_seninf_mclk_prepare,
	.unprepare = mt6797_seninf_mclk_unprepare,
	.recalc_rate = mt6797_seninf_mclk_recalc_rate,
};

static inline struct mt6797_seninf *to_mt6797_seninf(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mt6797_seninf, subdev);
}

static u8 __iomem *mt6797_seninf_bank_base(struct mt6797_seninf *seninf,
					   enum mt6797_seninf_bank bank)
{
	return bank == MT6797_SENINF_BANK_CORE ? seninf->core :
						 seninf->csi0_ana;
}

static u32 mt6797_seninf_io_read(void *context, enum mt6797_seninf_bank bank,
				 u32 offset)
{
	struct mt6797_seninf *seninf = context;

	return readl(mt6797_seninf_bank_base(seninf, bank) + offset);
}

static void mt6797_seninf_io_write(void *context, enum mt6797_seninf_bank bank,
				   u32 offset, u32 value)
{
	struct mt6797_seninf *seninf = context;

	writel(value, mt6797_seninf_bank_base(seninf, bank) + offset);
}

static void mt6797_seninf_io_delay(void *context, unsigned int min,
				   unsigned int max)
{
	(void)context;

	if (max <= 30)
		udelay(min);
	else
		usleep_range(min, max + 1);
}

static int mt6797_seninf_enum_mbus_code(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state,
					struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad >= MT6797_SENINF_NUM_PADS || code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int mt6797_seninf_set_fmt(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *sink;
	struct v4l2_mbus_framefmt *source;

	if (format->pad >= MT6797_SENINF_NUM_PADS)
		return -EINVAL;

	sink = v4l2_subdev_state_get_format(state, MT6797_SENINF_PAD_SINK);
	source = v4l2_subdev_state_get_format(state, MT6797_SENINF_PAD_SOURCE);
	if (!sink || !source)
		return -EINVAL;

	if (format->pad == MT6797_SENINF_PAD_SINK) {
		mt6797_seninf_format_adjust(&format->format);
		*sink = format->format;
		*source = *sink;
	} else {
		format->format = *source;
	}

	return 0;
}

static int mt6797_seninf_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mt6797_seninf *seninf = to_mt6797_seninf(sd);
	const struct mt6797_seninf_io io = {
		.read = mt6797_seninf_io_read,
		.write = mt6797_seninf_io_write,
		.delay_us = mt6797_seninf_io_delay,
		.context = seninf,
	};
	struct mt6797_seninf_config config = {
		.num_data_lanes = seninf->num_data_lanes,
		.settle_ns = MT6797_SENINF_FULL_SETTLE_NS,
	};
	struct v4l2_mbus_framefmt format;
	struct v4l2_subdev_state *state;
	struct media_pad *remote;
	s64 link_freq;
	int ret = 0;

	mutex_lock(&seninf->lock);
	if (!!enable == seninf->streaming)
		goto out_unlock;

	if (!enable) {
		v4l2_subdev_disable_streams(seninf->source,
					    seninf->source_pad, BIT(0));
		mt6797_seninf_hw_stop(&io);
		pm_runtime_put(seninf->dev);
		seninf->streaming = false;
		goto out_unlock;
	}

	state = v4l2_subdev_lock_and_get_active_state(sd);
	format = *v4l2_subdev_state_get_format(state, MT6797_SENINF_PAD_SINK);
	v4l2_subdev_unlock_state(state);

	remote = media_pad_remote_pad_first(&seninf->pads[MT6797_SENINF_PAD_SINK]);
	if (!remote || !seninf->source) {
		ret = -ENOLINK;
		goto out_unlock;
	}

	link_freq = v4l2_get_link_freq(remote, 0, 0);
	if (link_freq < 0) {
		ret = link_freq;
		goto out_unlock;
	}

	ret = mt6797_seninf_route_validate(seninf->data_lanes,
					   seninf->num_data_lanes, &format,
					   link_freq);
	if (ret)
		goto out_unlock;

	memcpy(config.calibration, seninf->calibration,
	       sizeof(config.calibration));
	ret = pm_runtime_resume_and_get(seninf->dev);
	if (ret < 0)
		goto out_unlock;

	ret = mt6797_seninf_hw_start(&io, &config);
	if (ret) {
		dev_err(seninf->dev, "NCSI2 calibration failed: %d\n", ret);
		pm_runtime_put(seninf->dev);
		goto out_unlock;
	}

	ret = v4l2_subdev_enable_streams(seninf->source,
					 seninf->source_pad, BIT(0));
	if (ret) {
		mt6797_seninf_hw_stop(&io);
		pm_runtime_put(seninf->dev);
		goto out_unlock;
	}

	seninf->streaming = true;

out_unlock:
	mutex_unlock(&seninf->lock);
	return ret;
}

static const struct v4l2_subdev_pad_ops mt6797_seninf_pad_ops = {
	.enum_mbus_code = mt6797_seninf_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = mt6797_seninf_set_fmt,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_video_ops mt6797_seninf_video_ops = {
	.s_stream = mt6797_seninf_s_stream,
};

static const struct v4l2_subdev_ops mt6797_seninf_subdev_ops = {
	.pad = &mt6797_seninf_pad_ops,
	.video = &mt6797_seninf_video_ops,
};

static int mt6797_seninf_init_state(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt format = {};
	struct v4l2_mbus_framefmt *sink;
	struct v4l2_mbus_framefmt *source;

	mt6797_seninf_format_adjust(&format);
	sink = v4l2_subdev_state_get_format(state, MT6797_SENINF_PAD_SINK);
	source = v4l2_subdev_state_get_format(state, MT6797_SENINF_PAD_SOURCE);
	if (!sink || !source)
		return -EINVAL;

	*sink = format;
	*source = format;
	return 0;
}

static const struct v4l2_subdev_internal_ops mt6797_seninf_internal_ops = {
	.init_state = mt6797_seninf_init_state,
};

static const struct media_entity_operations mt6797_seninf_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int mt6797_seninf_notify_bound(struct v4l2_async_notifier *notifier,
				      struct v4l2_subdev *subdev,
				      struct v4l2_async_connection *connection)
{
	struct mt6797_seninf *seninf =
		container_of(notifier, struct mt6797_seninf, notifier);
	struct media_pad *sink = &seninf->pads[MT6797_SENINF_PAD_SINK];
	struct media_pad *source;
	int ret;

	ret = v4l2_create_fwnode_links_to_pad(subdev, sink,
					      MEDIA_LNK_FL_ENABLED |
					      MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		return ret;

	source = media_pad_remote_pad_unique(sink);
	if (IS_ERR(source))
		return PTR_ERR(source);

	seninf->source = subdev;
	seninf->source_pad = source->index;
	return 0;
}

static const struct v4l2_async_notifier_operations mt6797_seninf_notify_ops = {
	.bound = mt6797_seninf_notify_bound,
};

static int mt6797_seninf_resources_get(struct platform_device *pdev,
				       struct mt6797_seninf *seninf)
{
	static const char *const names[] = {
		"seninf",
		"csi0-ana",
		"csi1-ana",
	};
	struct mt6797_seninf_res resources = {};
	struct resource *resource;
	void __iomem **bases[] = {
		&seninf->core,
		&seninf->csi0_ana,
		&seninf->csi1_ana,
	};
	struct mt6797_seninf_range *ranges[] = {
		&resources.core,
		&resources.csi0_ana,
		&resources.csi1_ana,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							names[i]);
		if (!resource)
			return -EINVAL;
		ranges[i]->start = resource->start;
		ranges[i]->size = resource_size(resource);
	}

	ret = mt6797_seninf_resources_validate(&resources);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid SENINF/CSI analog resource layout\n");

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		*bases[i] =
			devm_platform_ioremap_resource_byname(pdev, names[i]);
		if (IS_ERR(*bases[i]))
			return PTR_ERR(*bases[i]);
	}

	return 0;
}

static int mt6797_seninf_notifier_register(struct mt6797_seninf *seninf)
{
	struct v4l2_fwnode_endpoint endpoint = {
		.bus_type = V4L2_MBUS_UNKNOWN,
	};
	struct v4l2_async_connection *connection;
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(seninf->dev), 0, 0, 0);
	if (!ep)
		return dev_err_probe(seninf->dev, -ENODEV,
				     "enabled receiver needs a port@0 endpoint\n");

	ret = v4l2_fwnode_endpoint_parse(ep, &endpoint);
	if (ret)
		goto out_put_endpoint;
	ret = mt6797_seninf_endpoint_validate(endpoint.bus_type,
					      endpoint.bus.mipi_csi2.num_data_lanes);
	if (ret) {
		ret = dev_err_probe(seninf->dev, ret,
				    "endpoint must be a 1..4 lane CSI-2 D-PHY\n");
		goto out_free_endpoint;
	}
	seninf->num_data_lanes = endpoint.bus.mipi_csi2.num_data_lanes;
	memcpy(seninf->data_lanes, endpoint.bus.mipi_csi2.data_lanes,
	       seninf->num_data_lanes * sizeof(seninf->data_lanes[0]));

	v4l2_async_subdev_nf_init(&seninf->notifier, &seninf->subdev);
	seninf->notifier.ops = &mt6797_seninf_notify_ops;
	connection = v4l2_async_nf_add_fwnode_remote(&seninf->notifier, ep,
						     struct v4l2_async_connection);
	if (IS_ERR(connection)) {
		ret = PTR_ERR(connection);
		goto out_cleanup_notifier;
	}

	ret = v4l2_async_nf_register(&seninf->notifier);
	if (ret)
		goto out_cleanup_notifier;

	v4l2_fwnode_endpoint_free(&endpoint);
	fwnode_handle_put(ep);
	return 0;

out_cleanup_notifier:
	v4l2_async_nf_cleanup(&seninf->notifier);
out_free_endpoint:
	v4l2_fwnode_endpoint_free(&endpoint);
out_put_endpoint:
	fwnode_handle_put(ep);
	return ret;
}

static int mt6797_seninf_calibration_get(struct mt6797_seninf *seninf)
{
	struct nvmem_cell *cell;
	const u8 *buffer;
	size_t length;
	unsigned int i;
	int ret = 0;

	cell = devm_nvmem_cell_get(seninf->dev, "csi-calibration-data");
	if (IS_ERR(cell))
		return dev_err_probe(seninf->dev, PTR_ERR(cell),
				     "failed to get CSI calibration\n");

	buffer = nvmem_cell_read(cell, &length);
	if (IS_ERR(buffer))
		return dev_err_probe(seninf->dev, PTR_ERR(buffer),
				     "failed to read CSI calibration\n");
	if (length != sizeof(seninf->calibration)) {
		ret = dev_err_probe(seninf->dev, -EINVAL,
				    "CSI calibration must contain five words\n");
		goto out_free;
	}

	for (i = 0; i < ARRAY_SIZE(seninf->calibration); i++)
		seninf->calibration[i] =
			get_unaligned_le32(buffer + i * sizeof(u32));

out_free:
	kfree(buffer);
	return ret;
}

static int mt6797_seninf_mclk_register(struct mt6797_seninf *seninf)
{
	const struct clk_hw *parent_hw = __clk_get_hw(seninf->mclk_parent);
	struct clk_init_data init = {};
	const char *name;
	int ret;

	if (clk_get_rate(seninf->mclk_parent) != MT6797_SENINF_MCLK_RATE * 2)
		return dev_err_probe(seninf->dev, -EINVAL,
				     "MCLK parent must be 48 MHz\n");

	name = devm_kasprintf(seninf->dev, GFP_KERNEL, "%s-mclk2",
			      dev_name(seninf->dev));
	if (!name)
		return -ENOMEM;

	init.name = name;
	init.ops = &mt6797_seninf_mclk_ops;
	init.parent_hws = &parent_hw;
	init.num_parents = 1;
	seninf->mclk_hw.init = &init;

	ret = devm_clk_hw_register(seninf->dev, &seninf->mclk_hw);
	if (ret)
		return dev_err_probe(seninf->dev, ret,
				     "failed to register camera MCLK2\n");

	return devm_of_clk_add_hw_provider(seninf->dev, of_clk_hw_simple_get,
					   &seninf->mclk_hw);
}

static int mt6797_seninf_runtime_resume(struct device *dev)
{
	struct mt6797_seninf *seninf = dev_get_drvdata(dev);

	return clk_prepare_enable(seninf->seninf_clk);
}

static int mt6797_seninf_runtime_suspend(struct device *dev)
{
	struct mt6797_seninf *seninf = dev_get_drvdata(dev);

	clk_disable_unprepare(seninf->seninf_clk);
	return 0;
}

static int mt6797_seninf_probe(struct platform_device *pdev)
{
	struct mt6797_seninf *seninf;
	struct v4l2_subdev *sd;
	int ret;

	seninf = devm_kzalloc(&pdev->dev, sizeof(*seninf), GFP_KERNEL);
	if (!seninf)
		return -ENOMEM;

	seninf->dev = &pdev->dev;
	platform_set_drvdata(pdev, seninf);
	ret = mt6797_seninf_resources_get(pdev, seninf);
	if (ret)
		return ret;

	seninf->seninf_clk = devm_clk_get(&pdev->dev, "seninf");
	if (IS_ERR(seninf->seninf_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(seninf->seninf_clk),
				     "failed to get SENINF clock\n");
	seninf->camtg_clk = devm_clk_get(&pdev->dev, "camtg");
	if (IS_ERR(seninf->camtg_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(seninf->camtg_clk),
				     "failed to get camera TG clock\n");
	seninf->mclk_parent = devm_clk_get(&pdev->dev, "mclk-parent");
	if (IS_ERR(seninf->mclk_parent))
		return dev_err_probe(&pdev->dev, PTR_ERR(seninf->mclk_parent),
				     "failed to get camera MCLK parent\n");

	ret = mt6797_seninf_calibration_get(seninf);
	if (ret)
		return ret;

	mutex_init(&seninf->lock);
	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable runtime PM\n");

	ret = mt6797_seninf_mclk_register(seninf);
	if (ret)
		return ret;

	sd = &seninf->subdev;
	v4l2_subdev_init(sd, &mt6797_seninf_subdev_ops);
	sd->dev = &pdev->dev;
	sd->owner = THIS_MODULE;
	sd->internal_ops = &mt6797_seninf_internal_ops;
	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->entity.ops = &mt6797_seninf_media_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(sd->name, sizeof(sd->name), "mt6797-seninf %s",
		 dev_name(&pdev->dev));

	seninf->pads[MT6797_SENINF_PAD_SINK].flags = MEDIA_PAD_FL_SINK |
						     MEDIA_PAD_FL_MUST_CONNECT;
	seninf->pads[MT6797_SENINF_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sd->entity, MT6797_SENINF_NUM_PADS,
				     seninf->pads);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_cleanup_entity;

	ret = mt6797_seninf_notifier_register(seninf);
	if (ret)
		goto err_cleanup_subdev;

	ret = v4l2_async_register_subdev(sd);
	if (ret)
		goto err_unregister_notifier;

	dev_info(&pdev->dev, "registered %u-lane NCSI2 receiver\n",
		 seninf->num_data_lanes);
	return 0;

err_unregister_notifier:
	v4l2_async_nf_unregister(&seninf->notifier);
	v4l2_async_nf_cleanup(&seninf->notifier);
err_cleanup_subdev:
	v4l2_subdev_cleanup(sd);
err_cleanup_entity:
	media_entity_cleanup(&sd->entity);
	return ret;
}

static void mt6797_seninf_remove(struct platform_device *pdev)
{
	struct mt6797_seninf *seninf = platform_get_drvdata(pdev);
	const struct mt6797_seninf_io io = {
		.read = mt6797_seninf_io_read,
		.write = mt6797_seninf_io_write,
		.delay_us = mt6797_seninf_io_delay,
		.context = seninf,
	};

	mutex_lock(&seninf->lock);
	if (seninf->streaming) {
		v4l2_subdev_disable_streams(seninf->source,
					    seninf->source_pad, BIT(0));
		mt6797_seninf_hw_stop(&io);
		pm_runtime_put(seninf->dev);
		seninf->streaming = false;
	}
	mutex_unlock(&seninf->lock);

	v4l2_async_unregister_subdev(&seninf->subdev);
	v4l2_async_nf_unregister(&seninf->notifier);
	v4l2_async_nf_cleanup(&seninf->notifier);
	v4l2_subdev_cleanup(&seninf->subdev);
	media_entity_cleanup(&seninf->subdev.entity);
}

static const struct of_device_id mt6797_seninf_of_match[] = {
	{ .compatible = "mediatek,mt6797-seninf" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6797_seninf_of_match);

static DEFINE_RUNTIME_DEV_PM_OPS(mt6797_seninf_pm_ops,
				 mt6797_seninf_runtime_suspend,
				 mt6797_seninf_runtime_resume, NULL);

static struct platform_driver mt6797_seninf_driver = {
	.probe = mt6797_seninf_probe,
	.remove = mt6797_seninf_remove,
	.driver = {
		.name = "mt6797-seninf",
		.of_match_table = mt6797_seninf_of_match,
		.pm = pm_ptr(&mt6797_seninf_pm_ops),
	},
};
module_platform_driver(mt6797_seninf_driver);

MODULE_DESCRIPTION("MediaTek MT6797 SENINF/CSI-2 receiver");
MODULE_LICENSE("GPL");
