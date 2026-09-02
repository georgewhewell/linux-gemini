// SPDX-License-Identifier: GPL-2.0-only
/*
 * Structural V4L2 bridge for the MediaTek MT6797 SENINF/CSI-2 receiver.
 *
 * The source evidence does not establish a safe D-PHY power and lane-routing
 * sequence. Consequently this driver never enables a clock or accesses a
 * register, and streaming deliberately fails with -EOPNOTSUPP.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

#include "mtk_seninf_core.h"

struct mt6797_seninf {
	struct device *dev;
	struct clk *seninf_clk;
	void __iomem *core;
	void __iomem *csi0_ana;
	void __iomem *csi1_ana;
	unsigned int num_data_lanes;

	struct v4l2_subdev subdev;
	struct media_pad pads[MT6797_SENINF_NUM_PADS];
	struct v4l2_async_notifier notifier;
};

static inline struct mt6797_seninf *to_mt6797_seninf(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mt6797_seninf, subdev);
}

static int
mt6797_seninf_enum_mbus_code(struct v4l2_subdev *sd,
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

	if (!enable)
		return 0;

	dev_err_ratelimited(seninf->dev,
			    "streaming refused: MT6797 D-PHY sequence is not established\n");
	return -EOPNOTSUPP;
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

static int
mt6797_seninf_notify_bound(struct v4l2_async_notifier *notifier,
			   struct v4l2_subdev *subdev,
			   struct v4l2_async_connection *connection)
{
	struct mt6797_seninf *seninf =
		container_of(notifier, struct mt6797_seninf, notifier);

	return v4l2_create_fwnode_links_to_pad(subdev,
		&seninf->pads[MT6797_SENINF_PAD_SINK],
		MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
}

static const struct v4l2_async_notifier_operations mt6797_seninf_notify_ops = {
	.bound = mt6797_seninf_notify_bound,
};

static int
mt6797_seninf_resources_get(struct platform_device *pdev,
			    struct mt6797_seninf *seninf)
{
	static const char * const names[] = {
		"seninf", "csi0-ana", "csi1-ana",
	};
	struct mt6797_seninf_res resources = {};
	struct resource *resource;
	void __iomem **bases[] = {
		&seninf->core, &seninf->csi0_ana, &seninf->csi1_ana,
	};
	struct mt6797_seninf_range *ranges[] = {
		&resources.core, &resources.csi0_ana, &resources.csi1_ana,
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
		*bases[i] = devm_platform_ioremap_resource_byname(pdev, names[i]);
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

	seninf->pads[MT6797_SENINF_PAD_SINK].flags =
		MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
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

	dev_info(&pdev->dev,
		 "registered structural %u-lane receiver; streaming is unavailable\n",
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

	v4l2_async_unregister_subdev(&seninf->subdev);
	v4l2_async_nf_unregister(&seninf->notifier);
	v4l2_async_nf_cleanup(&seninf->notifier);
	v4l2_subdev_cleanup(&seninf->subdev);
	media_entity_cleanup(&seninf->subdev.entity);
}

static const struct of_device_id mt6797_seninf_of_match[] = {
	{ .compatible = "mediatek,mt6797-seninf" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_seninf_of_match);

static struct platform_driver mt6797_seninf_driver = {
	.probe = mt6797_seninf_probe,
	.remove = mt6797_seninf_remove,
	.driver = {
		.name = "mt6797-seninf",
		.of_match_table = mt6797_seninf_of_match,
	},
};
module_platform_driver(mt6797_seninf_driver);

MODULE_DESCRIPTION("MediaTek MT6797 structural SENINF/CSI-2 receiver");
MODULE_LICENSE("GPL");
