// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek CONSYS GNSS endpoint for the STP multiplexer.
 *
 * Vendor MT6797 code proves only that GPS uses STP task 2 and that opening the
 * vendor character device brackets that stream with WMT GPS function on/off.
 * It does not prove the WMT request/reply bytes, BTIF transport ownership,
 * firmware protocol, register map, power rails, NMEA dialect, or shared-memory
 * contract.  Those responsibilities therefore stay with a future transport
 * owner.  This file contains the safe, reusable half: a standard GNSS class
 * endpoint which moves opaque bytes over the reviewed STP channel.
 */

#include <linux/errno.h>
#include <linux/gnss.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <linux/soc/mediatek/mtk-gnss.h>

static int mtk_gnss_receive(void *context, const u8 *payload, size_t length)
{
	struct mtk_gnss *gnss = context;
	bool accepting;
	int ret = 0;

	if (!gnss || (!payload && length))
		return -EINVAL;

	mutex_lock(&gnss->lock);
	accepting = gnss->registered && gnss->active;
	if (accepting)
		ret = gnss_insert_raw(gnss->gdev, payload, length);
	mutex_unlock(&gnss->lock);

	if (!accepting)
		return 0;
	/* STP has already consumed this frame; make FIFO loss visible to its owner. */
	if (ret < 0)
		return ret;
	if (ret != length)
		return -ENOSPC;

	return 0;
}

static void mtk_gnss_complete_transition(struct mtk_gnss *gnss)
{
	gnss->transitioning = false;
	wake_up_all(&gnss->transition_queue);
}

static int mtk_gnss_wait_for_transition(struct mtk_gnss *gnss)
{
	int ret;

	for (;;) {
		mutex_lock(&gnss->lock);
		if (!gnss->transitioning)
			return 0;
		mutex_unlock(&gnss->lock);

		ret = wait_event_interruptible(gnss->transition_queue,
					       !READ_ONCE(gnss->transitioning));
		if (ret)
			return ret;
	}
}

int mtk_gnss_start(struct mtk_gnss *gnss)
{
	int ret;

	if (!gnss)
		return -EINVAL;

	ret = mtk_gnss_wait_for_transition(gnss);
	if (ret)
		return ret;
	if (!gnss->registered || gnss->unregistering) {
		ret = -ENODEV;
		goto unlock;
	} else if (gnss->faulted) {
		ret = -EIO;
		goto unlock;
	} else if (gnss->active) {
		ret = 0;
		goto unlock;
	} else {
		gnss->transitioning = true;
		mutex_unlock(&gnss->lock);
		ret = gnss->transport.set_power(gnss->transport.context, true);
		mutex_lock(&gnss->lock);
		if (!ret)
			gnss->active = true;
		mtk_gnss_complete_transition(gnss);
	}
unlock:
	mutex_unlock(&gnss->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_gnss_start);

void mtk_gnss_stop(struct mtk_gnss *gnss)
{
	int ret;

	if (!gnss)
		return;

	ret = mtk_gnss_wait_for_transition(gnss);
	if (ret)
		return;
	if (gnss->registered && gnss->active) {
		gnss->transitioning = true;
		mutex_unlock(&gnss->lock);
		ret = gnss->transport.set_power(gnss->transport.context, false);
		mutex_lock(&gnss->lock);
		gnss->active = false;
		if (ret)
			gnss->faulted = true;
		mtk_gnss_complete_transition(gnss);
	}
	mutex_unlock(&gnss->lock);
}
EXPORT_SYMBOL_GPL(mtk_gnss_stop);

int mtk_gnss_write(struct mtk_gnss *gnss, const u8 *data, size_t data_len)
{
	u8 *frame;
	size_t frame_len;
	int ret;

	if (!gnss || (!data && data_len))
		return -EINVAL;

	ret = mtk_gnss_wait_for_transition(gnss);
	if (ret)
		return ret;
	if (!gnss->registered || gnss->unregistering) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!gnss->active) {
		ret = gnss->faulted ? -EIO : -EPIPE;
		goto unlock;
	}

	frame = kmalloc(MTK_STP_HEADER_SIZE + MTK_STP_MAX_PAYLOAD +
			MTK_STP_TRAILER_SIZE, GFP_KERNEL);
	if (!frame) {
		ret = -ENOMEM;
		goto unlock;
	}

	ret = mtk_stp_encode(gnss->stp_mode, MTK_STP_GNSS, 0, 0, data,
			     data_len, frame,
			     MTK_STP_HEADER_SIZE + MTK_STP_MAX_PAYLOAD +
			     MTK_STP_TRAILER_SIZE, &frame_len);
	if (ret)
		goto free_frame;

	gnss->transitioning = true;
	mutex_unlock(&gnss->lock);
	ret = gnss->transport.transmit(gnss->transport.context, frame, frame_len);
	mutex_lock(&gnss->lock);
	if (ret) {
		/* A partial or failed transport write has no recoverable boundary. */
		gnss->active = false;
		gnss->faulted = true;
	} else {
		ret = data_len;
	}
	mtk_gnss_complete_transition(gnss);
free_frame:
	kfree(frame);
unlock:
	mutex_unlock(&gnss->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_gnss_write);

void mtk_gnss_transport_reset(struct mtk_gnss *gnss)
{
	if (!gnss)
		return;

	if (mtk_gnss_wait_for_transition(gnss))
		return;
	gnss->active = false;
	gnss->faulted = false;
	mutex_unlock(&gnss->lock);
}
EXPORT_SYMBOL_GPL(mtk_gnss_transport_reset);

static int mtk_gnss_open(struct gnss_device *gdev)
{
	return mtk_gnss_start(gnss_get_drvdata(gdev));
}

static void mtk_gnss_close(struct gnss_device *gdev)
{
	mtk_gnss_stop(gnss_get_drvdata(gdev));
}

static int mtk_gnss_write_raw(struct gnss_device *gdev,
			      const unsigned char *data, size_t data_len)
{
	return mtk_gnss_write(gnss_get_drvdata(gdev), data, data_len);
}

static const struct gnss_operations mtk_gnss_ops = {
	.open = mtk_gnss_open,
	.close = mtk_gnss_close,
	.write_raw = mtk_gnss_write_raw,
};

int mtk_gnss_register(struct mtk_gnss *gnss, struct device *parent,
		      struct mtk_stp *stp, enum mtk_stp_mode stp_mode,
		      const struct mtk_gnss_transport *transport)
{
	struct gnss_device *gdev;
	int ret;

	if (!gnss || !parent || !stp || !transport || !transport->transmit ||
	    !transport->set_power)
		return -EINVAL;
	if (stp_mode != MTK_STP_MODE_MANDATORY && stp_mode != MTK_STP_MODE_FULL)
		return -EINVAL;
	if (stp->mode != stp_mode)
		return -EINVAL;

	mutex_init(&gnss->lock);
	init_waitqueue_head(&gnss->transition_queue);
	mutex_lock(&gnss->lock);
	if (gnss->registered) {
		ret = -EBUSY;
		goto unlock;
	}

	gdev = gnss_allocate_device(parent);
	if (!gdev) {
		ret = -ENOMEM;
		goto unlock;
	}

	gdev->type = GNSS_TYPE_MTK;
	gdev->ops = &mtk_gnss_ops;
	gnss_set_drvdata(gdev, gnss);
	gnss->stp = stp;
	gnss->gdev = gdev;
	gnss->transport = *transport;
	gnss->stp_mode = stp_mode;
	gnss->active = false;
	gnss->faulted = false;
	gnss->transitioning = false;
	gnss->unregistering = false;

	ret = mtk_stp_register_client(stp, MTK_STP_GNSS, mtk_gnss_receive, gnss);
	if (ret)
		goto err_put_device;

	ret = gnss_register_device(gdev);
	if (ret)
		goto err_unregister_stp;

	gnss->registered = true;
	mutex_unlock(&gnss->lock);
	return 0;

err_unregister_stp:
	mtk_stp_unregister_client(stp, MTK_STP_GNSS);
err_put_device:
	gnss->gdev = NULL;
	gnss_put_device(gdev);
unlock:
	mutex_unlock(&gnss->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_gnss_register);

void mtk_gnss_unregister(struct mtk_gnss *gnss)
{
	struct gnss_device *gdev;
	struct mtk_stp *stp;

	if (!gnss)
		return;

	if (mtk_gnss_wait_for_transition(gnss))
		return;
	if (!gnss->registered) {
		mutex_unlock(&gnss->lock);
		return;
	}
	gdev = gnss->gdev;
	stp = gnss->stp;
	gnss->unregistering = true;
	mutex_unlock(&gnss->lock);

	/* gnss_deregister_device() invokes close() for an open endpoint. */
	gnss_deregister_device(gdev);
	/* Also balance a session started directly by a transport owner. */
	mtk_gnss_stop(gnss);
	mtk_stp_unregister_client(stp, MTK_STP_GNSS);

	mutex_lock(&gnss->lock);
	gnss->registered = false;
	gnss->active = false;
	gnss->faulted = false;
	gnss->unregistering = false;
	gnss->gdev = NULL;
	gnss->stp = NULL;
	mutex_unlock(&gnss->lock);

	gnss_put_device(gdev);
}
EXPORT_SYMBOL_GPL(mtk_gnss_unregister);

MODULE_DESCRIPTION("MediaTek CONSYS GNSS STP endpoint");
MODULE_LICENSE("GPL");
