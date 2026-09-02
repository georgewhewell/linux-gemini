// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 CAMSV0 capture engine.
 *
 * The implementation deliberately exposes only the Gemini camera1/sub path
 * recovered from the MT6797 camera HAL: CAMSV0, one-pixel RAW10 input and
 * packed SGRBG10 output at 2592x1944. The DMA API supplies 32-bit IOVAs via
 * the platform IOMMU; no Android buffer ABI is involved.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include "mtk_camsv_core.h"

#define MT6797_CAMSV_NAME	"mt6797-camsv"
#define MT6797_CAMSV_RESET_TIMEOUT_US	1000
#define MT6797_CAMSV_IDLE_TIMEOUT_US	350000

struct mt6797_camsv_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct mt6797_camsv {
	struct device *dev;
	void __iomem *base;
	struct clk_bulk_data clocks[4];
	int irq;

	struct media_device mdev;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct media_pad pad;
	struct vb2_queue queue;
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev *source;
	unsigned int source_pad;
	struct media_pipeline pipe;

	struct mutex lock; /* Serializes ioctls and stream transitions. */
	spinlock_t qlock; /* Protects buffers and IRQ-visible stream state. */
	struct list_head buffers;
	u32 sequence;
	bool streaming;
	bool fatal_error;
};

static inline struct mt6797_camsv_buffer *to_camsv_buffer(struct vb2_buffer *vb)
{
	return container_of(vb, struct mt6797_camsv_buffer, vb.vb2_buf);
}

static void mt6797_camsv_return_buffers(struct mt6797_camsv *camsv,
					enum vb2_buffer_state state)
{
	struct mt6797_camsv_buffer *buffer;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&camsv->qlock, flags);
		if (list_empty(&camsv->buffers)) {
			spin_unlock_irqrestore(&camsv->qlock, flags);
			break;
		}
		buffer = list_first_entry(&camsv->buffers,
					  struct mt6797_camsv_buffer, list);
		list_del(&buffer->list);
		spin_unlock_irqrestore(&camsv->qlock, flags);

		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
}

static int mt6797_camsv_reset(struct mt6797_camsv *camsv)
{
	u32 value;

	writel(4, camsv->base + MT6797_CAMSV_REG_SW_CTL);
	writel(0, camsv->base + MT6797_CAMSV_REG_SW_CTL);
	writel(1, camsv->base + MT6797_CAMSV_REG_SW_CTL);

	if (readl_poll_timeout(camsv->base + MT6797_CAMSV_REG_SW_CTL,
			       value, value == 3, 1,
			       MT6797_CAMSV_RESET_TIMEOUT_US)) {
		writel(0, camsv->base + MT6797_CAMSV_REG_SW_CTL);
		return -ETIMEDOUT;
	}

	writel(0, camsv->base + MT6797_CAMSV_REG_SW_CTL);
	return 0;
}

static void mt6797_camsv_hw_quiesce(struct mt6797_camsv *camsv)
{
	writel(0, camsv->base + MT6797_CAMSV_REG_INT_EN);
	writel(0, camsv->base + MT6797_CAMSV_REG_TG_VF_CON);
	writel(0, camsv->base + MT6797_CAMSV_REG_TG_SEN_MODE);
	writel(0, camsv->base + MT6797_CAMSV_REG_FBC_CTL1);
	writel(0, camsv->base + MT6797_CAMSV_REG_MODULE_EN);
	writel(0, camsv->base + MT6797_CAMSV_REG_CLK_EN);
}

static int mt6797_camsv_hw_configure(struct mt6797_camsv *camsv)
{
	struct mt6797_camsv_reg plan[24];
	unsigned int count;
	unsigned int i;
	int ret;

	ret = mt6797_camsv_reset(camsv);
	if (ret)
		return ret;

	count = mt6797_camsv_build_config_plan(plan, ARRAY_SIZE(plan));
	if (WARN_ON(count > ARRAY_SIZE(plan)))
		return -EOVERFLOW;

	for (i = 0; i < count; i++)
		writel(plan[i].value, camsv->base + plan[i].offset);

	return 0;
}

static void mt6797_camsv_hw_enqueue(struct mt6797_camsv *camsv,
				    struct mt6797_camsv_buffer *buffer)
{
	dma_addr_t address = vb2_dma_contig_plane_dma_addr(&buffer->vb.vb2_buf,
							      0);

	writel(lower_32_bits(address),
	       camsv->base + MT6797_CAMSV_REG_FBC_ENQ_ADDR);
	writel(1, camsv->base + MT6797_CAMSV_REG_IMGO_FBC);
}

static irqreturn_t mt6797_camsv_irq(int irq, void *data)
{
	struct mt6797_camsv *camsv = data;
	struct mt6797_camsv_buffer *buffer = NULL;
	u32 status;
	unsigned long flags;

	status = readl(camsv->base + MT6797_CAMSV_REG_INT_STATUS);
	if (!status)
		return IRQ_NONE;

	spin_lock_irqsave(&camsv->qlock, flags);
	if (!camsv->streaming) {
		spin_unlock_irqrestore(&camsv->qlock, flags);
		return IRQ_HANDLED;
	}

	if (status & MT6797_CAMSV_INT_ERRORS) {
		camsv->fatal_error = true;
		writel(0, camsv->base + MT6797_CAMSV_REG_INT_EN);
		writel(0, camsv->base + MT6797_CAMSV_REG_TG_VF_CON);
		writel(0, camsv->base + MT6797_CAMSV_REG_TG_SEN_MODE);
		spin_unlock_irqrestore(&camsv->qlock, flags);
		vb2_queue_error(&camsv->queue);
		mt6797_camsv_return_buffers(camsv, VB2_BUF_STATE_ERROR);
		return IRQ_HANDLED;
	}

	/* Stock userspace dequeues on SW_PASS1_DONE, after FBC/DMA retirement. */
	if ((status & MT6797_CAMSV_INT_SW_DONE) &&
	    !list_empty(&camsv->buffers)) {
		buffer = list_first_entry(&camsv->buffers,
					  struct mt6797_camsv_buffer, list);
		list_del(&buffer->list);
		buffer->vb.sequence = camsv->sequence++;
		buffer->vb.vb2_buf.timestamp = ktime_get_ns();
		vb2_set_plane_payload(&buffer->vb.vb2_buf, 0,
				      MT6797_CAMSV_SIZEIMAGE);
	}
	spin_unlock_irqrestore(&camsv->qlock, flags);

	if (buffer)
		vb2_buffer_done(&buffer->vb.vb2_buf, VB2_BUF_STATE_DONE);

	return IRQ_HANDLED;
}

static int mt6797_camsv_queue_setup(struct vb2_queue *queue,
				    unsigned int *num_buffers,
				    unsigned int *num_planes,
				    unsigned int sizes[],
				    struct device *alloc_devs[])
{
	if (*num_planes) {
		if (sizes[0] < MT6797_CAMSV_SIZEIMAGE)
			return -EINVAL;
		return 0;
	}

	*num_planes = 1;
	sizes[0] = MT6797_CAMSV_SIZEIMAGE;
	return 0;
}

static int mt6797_camsv_buffer_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < MT6797_CAMSV_SIZEIMAGE)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, MT6797_CAMSV_SIZEIMAGE);
	return 0;
}

static void mt6797_camsv_buffer_queue(struct vb2_buffer *vb)
{
	struct mt6797_camsv *camsv = vb2_get_drv_priv(vb->vb2_queue);
	struct mt6797_camsv_buffer *buffer = to_camsv_buffer(vb);
	unsigned long flags;

	spin_lock_irqsave(&camsv->qlock, flags);
	list_add_tail(&buffer->list, &camsv->buffers);
	if (camsv->streaming && !camsv->fatal_error)
		mt6797_camsv_hw_enqueue(camsv, buffer);
	spin_unlock_irqrestore(&camsv->qlock, flags);
}

static int mt6797_camsv_start_streaming(struct vb2_queue *queue,
					unsigned int count)
{
	struct mt6797_camsv *camsv = vb2_get_drv_priv(queue);
	struct mt6797_camsv_buffer *buffer;
	unsigned long flags;
	bool source_started = false;
	int ret;

	ret = video_device_pipeline_start(&camsv->vdev, &camsv->pipe);
	if (ret)
		goto err_buffers;

	ret = pm_runtime_resume_and_get(camsv->dev);
	if (ret < 0)
		goto err_pipeline;

	ret = mt6797_camsv_hw_configure(camsv);
	if (ret) {
		dev_err(camsv->dev, "CAMSV reset/configuration failed: %d\n", ret);
		goto err_pm;
	}

	spin_lock_irqsave(&camsv->qlock, flags);
	list_for_each_entry(buffer, &camsv->buffers, list)
		mt6797_camsv_hw_enqueue(camsv, buffer);
	camsv->sequence = 0;
	camsv->fatal_error = false;
	spin_unlock_irqrestore(&camsv->qlock, flags);

	if (!camsv->source) {
		ret = -ENOLINK;
		goto err_hw;
	}

	ret = v4l2_subdev_enable_streams(camsv->source, camsv->source_pad,
					 BIT(0));
	if (ret) {
		dev_err(camsv->dev, "failed to start upstream pipeline: %d\n", ret);
		goto err_hw;
	}
	source_started = true;

	spin_lock_irqsave(&camsv->qlock, flags);
	camsv->streaming = true;
	writel(1, camsv->base + MT6797_CAMSV_REG_TG_VF_CON);
	spin_unlock_irqrestore(&camsv->qlock, flags);
	return 0;

err_hw:
	if (source_started)
		v4l2_subdev_disable_streams(camsv->source, camsv->source_pad,
					    BIT(0));
	mt6797_camsv_hw_quiesce(camsv);
	mt6797_camsv_reset(camsv);
err_pm:
	pm_runtime_put_sync(camsv->dev);
err_pipeline:
	video_device_pipeline_stop(&camsv->vdev);
err_buffers:
	mt6797_camsv_return_buffers(camsv, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void mt6797_camsv_stop_streaming(struct vb2_queue *queue)
{
	struct mt6797_camsv *camsv = vb2_get_drv_priv(queue);
	u32 value;
	unsigned long flags;
	bool was_streaming;

	spin_lock_irqsave(&camsv->qlock, flags);
	was_streaming = camsv->streaming;
	camsv->streaming = false;
	writel(0, camsv->base + MT6797_CAMSV_REG_INT_EN);
	writel(0, camsv->base + MT6797_CAMSV_REG_TG_VF_CON);
	spin_unlock_irqrestore(&camsv->qlock, flags);

	if (was_streaming &&
	    readl_poll_timeout(camsv->base + MT6797_CAMSV_REG_TG_INTER_ST,
			       value, (value & 0x3f00) == 0x100, 1000,
			       MT6797_CAMSV_IDLE_TIMEOUT_US))
		dev_warn(camsv->dev, "TG did not become idle before shutdown\n");

	if (was_streaming)
		v4l2_subdev_disable_streams(camsv->source, camsv->source_pad,
					    BIT(0));

	mt6797_camsv_hw_quiesce(camsv);
	if (mt6797_camsv_reset(camsv))
		dev_warn(camsv->dev, "CAMSV reset timed out during shutdown\n");

	pm_runtime_put_sync(camsv->dev);
	video_device_pipeline_stop(&camsv->vdev);
	mt6797_camsv_return_buffers(camsv, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops mt6797_camsv_vb2_ops = {
	.queue_setup = mt6797_camsv_queue_setup,
	.buf_prepare = mt6797_camsv_buffer_prepare,
	.buf_queue = mt6797_camsv_buffer_queue,
	.start_streaming = mt6797_camsv_start_streaming,
	.stop_streaming = mt6797_camsv_stop_streaming,
};

static void mt6797_camsv_fill_pix(struct v4l2_pix_format *pix)
{
	pix->width = MT6797_CAMSV_WIDTH;
	pix->height = MT6797_CAMSV_HEIGHT;
	pix->pixelformat = V4L2_PIX_FMT_SGRBG10P;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = MT6797_CAMSV_BYTESPERLINE;
	pix->sizeimage = MT6797_CAMSV_SIZEIMAGE;
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int mt6797_camsv_querycap(struct file *file, void *priv,
				 struct v4l2_capability *cap)
{
	strscpy(cap->driver, MT6797_CAMSV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "MT6797 CAMSV0", sizeof(cap->card));
	return 0;
}

static int mt6797_camsv_enum_fmt(struct file *file, void *priv,
				 struct v4l2_fmtdesc *fmt)
{
	if (fmt->index)
		return -EINVAL;

	fmt->pixelformat = V4L2_PIX_FMT_SGRBG10P;
	return 0;
}

static int mt6797_camsv_g_fmt(struct file *file, void *priv,
			      struct v4l2_format *format)
{
	mt6797_camsv_fill_pix(&format->fmt.pix);
	return 0;
}

static int mt6797_camsv_try_fmt(struct file *file, void *priv,
				struct v4l2_format *format)
{
	mt6797_camsv_fill_pix(&format->fmt.pix);
	return 0;
}

static int mt6797_camsv_s_fmt(struct file *file, void *priv,
			      struct v4l2_format *format)
{
	struct mt6797_camsv *camsv = video_drvdata(file);

	if (vb2_is_busy(&camsv->queue))
		return -EBUSY;

	mt6797_camsv_fill_pix(&format->fmt.pix);
	return 0;
}

static const struct v4l2_ioctl_ops mt6797_camsv_ioctl_ops = {
	.vidioc_querycap = mt6797_camsv_querycap,
	.vidioc_enum_fmt_vid_cap = mt6797_camsv_enum_fmt,
	.vidioc_g_fmt_vid_cap = mt6797_camsv_g_fmt,
	.vidioc_try_fmt_vid_cap = mt6797_camsv_try_fmt,
	.vidioc_s_fmt_vid_cap = mt6797_camsv_s_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations mt6797_camsv_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
};

static int mt6797_camsv_link_validate(struct media_link *link)
{
	struct video_device *vdev = media_entity_to_video_device(link->sink->entity);
	struct mt6797_camsv *camsv = video_get_drvdata(vdev);
	struct v4l2_subdev *subdev = media_entity_to_v4l2_subdev(link->source->entity);
	struct v4l2_subdev_state *state;
	struct v4l2_mbus_framefmt *format;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(subdev);
	format = v4l2_subdev_state_get_format(state, camsv->source_pad);
	if (!format || format->code != MEDIA_BUS_FMT_SGRBG10_1X10 ||
	    format->width != MT6797_CAMSV_WIDTH ||
	    format->height != MT6797_CAMSV_HEIGHT)
		ret = -EPIPE;
	v4l2_subdev_unlock_state(state);

	return ret;
}

static const struct media_entity_operations mt6797_camsv_media_ops = {
	.link_validate = mt6797_camsv_link_validate,
};

static int mt6797_camsv_video_register(struct mt6797_camsv *camsv)
{
	struct video_device *vdev = &camsv->vdev;
	struct vb2_queue *queue = &camsv->queue;
	int ret;

	queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	queue->io_modes = VB2_MMAP | VB2_DMABUF;
	queue->drv_priv = camsv;
	queue->ops = &mt6797_camsv_vb2_ops;
	queue->mem_ops = &vb2_dma_contig_memops;
	queue->buf_struct_size = sizeof(struct mt6797_camsv_buffer);
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	queue->lock = &camsv->lock;
	queue->min_queued_buffers = 2;
	queue->max_num_buffers = MT6797_CAMSV_MAX_BUFFERS;
	queue->dev = camsv->dev;
	ret = vb2_queue_init(queue);
	if (ret)
		return ret;

	vdev->fops = &mt6797_camsv_fops;
	vdev->ioctl_ops = &mt6797_camsv_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->v4l2_dev = &camsv->v4l2_dev;
	vdev->queue = queue;
	vdev->lock = &camsv->lock;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_IO_MC;
	vdev->entity.ops = &mt6797_camsv_media_ops;
	vdev->entity.flags |= MEDIA_ENT_FL_DEFAULT;
	strscpy(vdev->name, "mt6797-camsv0", sizeof(vdev->name));
	video_set_drvdata(vdev, camsv);

	camsv->pad.flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	ret = media_entity_pads_init(&vdev->entity, 1, &camsv->pad);
	if (ret)
		return ret;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		media_entity_cleanup(&vdev->entity);

	return ret;
}

static int mt6797_camsv_notify_bound(struct v4l2_async_notifier *notifier,
				     struct v4l2_subdev *subdev,
				     struct v4l2_async_connection *connection)
{
	struct mt6797_camsv *camsv =
		container_of(notifier, struct mt6797_camsv, notifier);
	struct media_pad *source;
	int ret;

	ret = v4l2_create_fwnode_links_to_pad(subdev, &camsv->pad,
					      MEDIA_LNK_FL_ENABLED |
					      MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		return ret;

	source = media_pad_remote_pad_unique(&camsv->pad);
	if (IS_ERR(source))
		return PTR_ERR(source);

	camsv->source = subdev;
	camsv->source_pad = source->index;
	return 0;
}

static int mt6797_camsv_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct mt6797_camsv *camsv =
		container_of(notifier, struct mt6797_camsv, notifier);
	int ret;

	ret = v4l2_device_register_subdev_nodes(&camsv->v4l2_dev);
	return ret;
}

static const struct v4l2_async_notifier_operations mt6797_camsv_notify_ops = {
	.bound = mt6797_camsv_notify_bound,
	.complete = mt6797_camsv_notify_complete,
};

static int mt6797_camsv_notifier_register(struct mt6797_camsv *camsv)
{
	struct v4l2_async_connection *connection;
	struct fwnode_handle *endpoint;
	int ret;

	endpoint = fwnode_graph_get_endpoint_by_id(dev_fwnode(camsv->dev), 0, 0,
						   FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!endpoint)
		return dev_err_probe(camsv->dev, -ENODEV,
				     "enabled CAMSV needs a port@0 endpoint\n");

	v4l2_async_nf_init(&camsv->notifier, &camsv->v4l2_dev);
	camsv->notifier.ops = &mt6797_camsv_notify_ops;
	connection = v4l2_async_nf_add_fwnode_remote(&camsv->notifier,
						     endpoint,
						     struct v4l2_async_connection);
	fwnode_handle_put(endpoint);
	if (IS_ERR(connection)) {
		ret = PTR_ERR(connection);
		goto err_cleanup;
	}

	ret = v4l2_async_nf_register(&camsv->notifier);
	if (ret)
		goto err_cleanup;

	return 0;

err_cleanup:
	v4l2_async_nf_cleanup(&camsv->notifier);
	return ret;
}

static int mt6797_camsv_runtime_resume(struct device *dev)
{
	struct mt6797_camsv *camsv = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(ARRAY_SIZE(camsv->clocks), camsv->clocks);
}

static int mt6797_camsv_runtime_suspend(struct device *dev)
{
	struct mt6797_camsv *camsv = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(ARRAY_SIZE(camsv->clocks), camsv->clocks);
	return 0;
}

static int mt6797_camsv_probe(struct platform_device *pdev)
{
	static const char *const clock_names[] = {
		"larb", "camsys", "camtg", "camsv",
	};
	struct mt6797_camsv *camsv;
	unsigned int i;
	int ret;

	camsv = devm_kzalloc(&pdev->dev, sizeof(*camsv), GFP_KERNEL);
	if (!camsv)
		return -ENOMEM;

	camsv->dev = &pdev->dev;
	platform_set_drvdata(pdev, camsv);
	mutex_init(&camsv->lock);
	spin_lock_init(&camsv->qlock);
	INIT_LIST_HEAD(&camsv->buffers);

	camsv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(camsv->base))
		return PTR_ERR(camsv->base);

	camsv->irq = platform_get_irq(pdev, 0);
	if (camsv->irq < 0)
		return camsv->irq;

	for (i = 0; i < ARRAY_SIZE(camsv->clocks); i++)
		camsv->clocks[i].id = clock_names[i];
	ret = devm_clk_bulk_get(&pdev->dev, ARRAY_SIZE(camsv->clocks),
				camsv->clocks);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to get clocks\n");

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "32-bit DMA addressing unavailable\n");

	ret = devm_request_irq(&pdev->dev, camsv->irq, mt6797_camsv_irq, 0,
			       dev_name(&pdev->dev), camsv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to request IRQ\n");

	strscpy(camsv->mdev.model, "MediaTek MT6797 CAMSV",
		sizeof(camsv->mdev.model));
	camsv->mdev.dev = &pdev->dev;
	media_device_init(&camsv->mdev);
	camsv->v4l2_dev.mdev = &camsv->mdev;

	ret = v4l2_device_register(&pdev->dev, &camsv->v4l2_dev);
	if (ret)
		goto err_media_cleanup;

	ret = media_device_register(&camsv->mdev);
	if (ret)
		goto err_v4l2_unregister;

	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		goto err_media_unregister;

	/* The sink pad must exist before the notifier creates its graph link. */
	ret = mt6797_camsv_video_register(camsv);
	if (ret)
		goto err_media_unregister;

	ret = mt6797_camsv_notifier_register(camsv);
	if (ret)
		goto err_video_unregister;

	dev_info(&pdev->dev, "registered fail-closed CAMSV0 RAW10 capture\n");
	return 0;

err_video_unregister:
	vb2_video_unregister_device(&camsv->vdev);
err_media_unregister:
	media_device_unregister(&camsv->mdev);
err_v4l2_unregister:
	v4l2_device_unregister(&camsv->v4l2_dev);
err_media_cleanup:
	media_device_cleanup(&camsv->mdev);
	mutex_destroy(&camsv->lock);
	return ret;
}

static void mt6797_camsv_remove(struct platform_device *pdev)
{
	struct mt6797_camsv *camsv = platform_get_drvdata(pdev);

	if (video_is_registered(&camsv->vdev))
		vb2_video_unregister_device(&camsv->vdev);
	v4l2_async_nf_unregister(&camsv->notifier);
	v4l2_async_nf_cleanup(&camsv->notifier);
	media_device_unregister(&camsv->mdev);
	v4l2_device_unregister(&camsv->v4l2_dev);
	media_device_cleanup(&camsv->mdev);
	mutex_destroy(&camsv->lock);
}

static const struct of_device_id mt6797_camsv_of_match[] = {
	{ .compatible = "mediatek,mt6797-camsv" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6797_camsv_of_match);

static DEFINE_RUNTIME_DEV_PM_OPS(mt6797_camsv_pm_ops,
				 mt6797_camsv_runtime_suspend,
				 mt6797_camsv_runtime_resume, NULL);

static struct platform_driver mt6797_camsv_driver = {
	.probe = mt6797_camsv_probe,
	.remove = mt6797_camsv_remove,
	.driver = {
		.name = MT6797_CAMSV_NAME,
		.of_match_table = mt6797_camsv_of_match,
		.pm = pm_ptr(&mt6797_camsv_pm_ops),
	},
};
module_platform_driver(mt6797_camsv_driver);

MODULE_DESCRIPTION("MediaTek MT6797 CAMSV0 V4L2 capture driver");
MODULE_LICENSE("GPL");
