// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek CONSYS GNSS endpoint for the STP multiplexer.
 *
 * Vendor MT6797 code proves only that GPS uses STP task 2 and that opening the
 * vendor character device brackets that stream with WMT GPS function on/off.
 * It does not prove BTIF transport ownership, firmware protocol, register map,
 * power rails, NMEA dialect, or shared-memory contract. Those responsibilities
 * stay with a future controller owner. This endpoint uses an opaque active
 * full-STP lease, so it cannot forge sequence/ACK fields or coexist with a
 * generic full-STP encoder.
 */

#include <linux/errno.h>
#include <linux/gnss.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/soc/mediatek/mtk-gnss.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>

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

/*
 * Teardown cannot leave a live callback or power vote behind merely because
 * the caller has a pending signal.  Like the interruptible helper above this
 * returns with gnss->lock held, but it retries until the current transition
 * has completed.
 */
static void mtk_gnss_wait_for_transition_uninterruptible(struct mtk_gnss *gnss)
{
	for (;;) {
		mutex_lock(&gnss->lock);
		if (!gnss->transitioning)
			return;
		mutex_unlock(&gnss->lock);

		wait_event(gnss->transition_queue, !READ_ONCE(gnss->transitioning));
	}
}

int mtk_gnss_start(struct mtk_gnss *gnss)
{
	int ret;

	if (!gnss)
		return -EINVAL;
	if (!READ_ONCE(gnss->initialized))
		return -ENODEV;

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

	if (!gnss || !READ_ONCE(gnss->initialized))
		return;

	mtk_gnss_wait_for_transition_uninterruptible(gnss);
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
	int ret;

	if (!gnss || (!data && data_len))
		return -EINVAL;
	if (!READ_ONCE(gnss->initialized))
		return -ENODEV;

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

	gnss->transitioning = true;
	mutex_unlock(&gnss->lock);
	ret = mt6797_consys_stp_session_transmit(gnss->session_lease,
						 MTK_STP_GNSS, data, data_len);
	mutex_lock(&gnss->lock);
	if (ret) {
		/*
		 * A failed opaque full-STP submit has no endpoint-local recovery,
		 * but it must not strand the owner\'s GPS function/LNA vote.  Hold
		 * transitioning across the unclocked fail-low callback so close() and
		 * unregister() cannot race it, then preserve the transmit failure as
		 * this endpoint\'s terminal fault.
		 */
		mutex_unlock(&gnss->lock);
		gnss->transport.set_power(gnss->transport.context, false);
		mutex_lock(&gnss->lock);
		gnss->active = false;
		gnss->faulted = true;
	} else {
		ret = data_len;
	}
	mtk_gnss_complete_transition(gnss);
unlock:
	mutex_unlock(&gnss->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_gnss_write);

void mtk_gnss_transport_reset(struct mtk_gnss *gnss)
{
	if (!gnss || !READ_ONCE(gnss->initialized))
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
		      const struct mt6797_consys_stp_session_lease *session_lease,
		      const struct mtk_gnss_transport *transport)
{
	struct gnss_device *gdev;
	int ret;

	if (!gnss || !parent || !session_lease || !transport ||
	    !transport->set_power)
		return -EINVAL;

	if (!gnss->initialized) {
		mutex_init(&gnss->lock);
		init_waitqueue_head(&gnss->transition_queue);
		gnss->initialized = true;
	}
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
	gnss->session_lease = session_lease;
	gnss->gdev = gdev;
	gnss->transport = *transport;
	gnss->active = false;
	gnss->faulted = false;
	gnss->transitioning = false;
	gnss->unregistering = false;

	ret = mt6797_consys_stp_session_bind_gnss(session_lease,
						    mtk_gnss_receive, gnss);
	if (ret)
		goto err_put_device;

	/* Publish complete state before the character device can be opened. */
	gnss->registered = true;
	ret = gnss_register_device(gdev);
	if (ret)
		goto err_unregister_stp;

	mutex_unlock(&gnss->lock);
	return 0;

err_unregister_stp:
	/*
	 * A concurrent full-STP RX holds the session lock while its GNSS callback
	 * takes gnss->lock.  Make this callback unable to accept input, then drop
	 * gnss->lock before waiting for the session lock in unbind below.
	 */
	gnss->registered = false;
	gnss->gdev = NULL;
	gnss->session_lease = NULL;
	mutex_unlock(&gnss->lock);
	mt6797_consys_stp_session_unbind_gnss(session_lease, mtk_gnss_receive,
						      gnss);
	gnss_put_device(gdev);
	return ret;

err_put_device:
	gnss->gdev = NULL;
	gnss->session_lease = NULL;
	mutex_unlock(&gnss->lock);
	gnss_put_device(gdev);
	return ret;

unlock:
	mutex_unlock(&gnss->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_gnss_register);

void mtk_gnss_unregister(struct mtk_gnss *gnss)
{
	struct gnss_device *gdev;
	const struct mt6797_consys_stp_session_lease *session_lease;

	if (!gnss || !READ_ONCE(gnss->initialized))
		return;

	for (;;) {
		mtk_gnss_wait_for_transition_uninterruptible(gnss);
		if (!gnss->registered) {
			mutex_unlock(&gnss->lock);
			return;
		}
		if (!gnss->unregistering)
			break;
		mutex_unlock(&gnss->lock);
		/* A competing teardown owns the device and session callback. */
		wait_event(gnss->transition_queue, !READ_ONCE(gnss->unregistering));
	}
	gdev = gnss->gdev;
	session_lease = gnss->session_lease;
	gnss->unregistering = true;
	mutex_unlock(&gnss->lock);

	/* gnss_deregister_device() invokes close() for an open endpoint. */
	gnss_deregister_device(gdev);
	/* Also balance a session started directly by a transport owner. */
	mtk_gnss_stop(gnss);
	mt6797_consys_stp_session_unbind_gnss(session_lease, mtk_gnss_receive,
						      gnss);

	mutex_lock(&gnss->lock);
	gnss->registered = false;
	gnss->active = false;
	gnss->faulted = false;
	gnss->unregistering = false;
	gnss->gdev = NULL;
	gnss->session_lease = NULL;
	wake_up_all(&gnss->transition_queue);
	mutex_unlock(&gnss->lock);

	gnss_put_device(gdev);
}
EXPORT_SYMBOL_GPL(mtk_gnss_unregister);

MODULE_DESCRIPTION("MediaTek CONSYS GNSS STP endpoint");
MODULE_LICENSE("GPL");
