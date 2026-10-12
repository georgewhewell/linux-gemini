// SPDX-License-Identifier: GPL-2.0-only
/* Exclusive MT6797 IMS control records. No automatic protocol responses. */
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "mtk_mt6797_ccci_imsc_port.h"

#define MT6797_IMSC_RX_FRAMES 32U
#define MT6797_IMSC_RX_MEMORY (MT6797_IMSC_RX_FRAMES * 8192U)

struct mt6797_imsc_port {
	struct miscdevice misc;
	struct kref reference;
	struct mutex read_mutex; /* one detached userspace copy */
	struct mutex tx_mutex; /* backend calls and parent context lifetime */
	struct sk_buff_head rx;
	wait_queue_head_t wait;
	struct mt6797_imsc_backend backend;
	void *context; /* tx_mutex; detached before the parent can disappear */
	/* rx.lock guards all following fields. Counts include a detached read. */
	unsigned int rx_count;
	unsigned int rx_memory;
	bool opened;
	bool ready;
	bool tx_room;
	bool stopped;
	bool touched; /* current owner delivered RX or published TX */
	int error;
};

static bool mt6797_imsc_write_waitable(struct mt6797_imsc_port *port)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&port->rx.lock, flags);
	ready = port->stopped || !port->ready || port->tx_room;
	spin_unlock_irqrestore(&port->rx.lock, flags);
	return ready;
}

static void mt6797_imsc_port_free(struct kref *reference)
{
	struct mt6797_imsc_port *port = container_of(reference,
		 struct mt6797_imsc_port, reference);

	skb_queue_purge(&port->rx);
	kfree(port);
}

/*
 * Caller holds rx.lock. A read detached from the queue keeps its own charge
 * and SKB; terminal/last-close cleanup never frees a reader's live buffer.
 */
static void mt6797_imsc_purge_locked(struct mt6797_imsc_port *port)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(&port->rx))) {
		port->rx_count--;
		port->rx_memory -= skb->truesize;
		kfree_skb(skb);
	}
}

static void mt6797_imsc_stop_locked(struct mt6797_imsc_port *port, int error)
{
	if (port->stopped)
		return;
	port->stopped = true;
	port->ready = false;
	port->tx_room = false;
	port->error = error < 0 ? error : -ESHUTDOWN;
	mt6797_imsc_purge_locked(port);
}

void mt6797_imsc_port_stop(struct mt6797_imsc_port *port, int error)
{
	unsigned long flags;

	if (!port)
		return;
	spin_lock_irqsave(&port->rx.lock, flags);
	mt6797_imsc_stop_locked(port, error);
	spin_unlock_irqrestore(&port->rx.lock, flags);
	wake_up_interruptible(&port->wait);
}

void mt6797_imsc_port_ready(struct mt6797_imsc_port *port, bool ready)
{
	unsigned long flags;

	if (!port)
		return;
	spin_lock_irqsave(&port->rx.lock, flags);
	if (!port->stopped && port->ready && !ready)
		mt6797_imsc_stop_locked(port, -EPIPE);
	if (!port->stopped) {
		port->ready = ready;
		port->tx_room = ready;
	}
	spin_unlock_irqrestore(&port->rx.lock, flags);
	wake_up_interruptible(&port->wait);
}

void mt6797_imsc_port_tx_wake(struct mt6797_imsc_port *port)
{
	unsigned long flags;

	if (!port)
		return;
	spin_lock_irqsave(&port->rx.lock, flags);
	if (port->ready && !port->stopped)
		port->tx_room = true;
	spin_unlock_irqrestore(&port->rx.lock, flags);
	wake_up_interruptible(&port->wait);
}

static int mt6797_imsc_open(struct inode *inode, struct file *file)
{
	struct mt6797_imsc_port *port = container_of(file->private_data,
		 struct mt6797_imsc_port, misc);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&port->rx.lock, flags);
	if (port->stopped) {
		ret = port->error;
	} else if (!port->ready) {
		ret = -EAGAIN;
	} else if (port->opened) {
		ret = -EBUSY;
	} else {
		kref_get(&port->reference);
		port->opened = true;
		file->private_data = port;
	}
	spin_unlock_irqrestore(&port->rx.lock, flags);
	if (ret)
		return ret;
	return nonseekable_open(inode, file);
}

static int mt6797_imsc_release(struct inode *inode, struct file *file)
{
	struct mt6797_imsc_port *port = file->private_data;
	unsigned long flags;

	/*
	 * VFS holds the file through every active read/write, so final release
	 * cannot run with a syscall holding this descriptor's read/TX mutex.
	 */
	spin_lock_irqsave(&port->rx.lock, flags);
	/*
	 * No established IMCB disconnect/reset contract permits a new owner to
	 * inherit published requests or delivered/abandoned records. Untouched,
	 * empty open/close is reusable. Otherwise require a real modem restart.
	 */
	if (port->touched || port->rx_count)
		mt6797_imsc_stop_locked(port, -ECANCELED);
	port->opened = false;
	mt6797_imsc_purge_locked(port);
	spin_unlock_irqrestore(&port->rx.lock, flags);
	kref_put(&port->reference, mt6797_imsc_port_free);
	return 0;
}

static ssize_t mt6797_imsc_read(struct file *file, char __user *buffer,
				size_t capacity, loff_t *position)
{
	struct mt6797_imsc_port *port = file->private_data;
	struct sk_buff *skb;
	unsigned long flags;
	ssize_t ret;
	bool copied;

	if (!capacity)
		return 0;
	ret = mutex_lock_interruptible(&port->read_mutex);
	if (ret)
		return ret;
	for (;;) {
		spin_lock_irqsave(&port->rx.lock, flags);
		if (port->stopped) {
			ret = port->error;
			spin_unlock_irqrestore(&port->rx.lock, flags);
			goto unlock;
		}
		skb = skb_peek(&port->rx);
		if (skb) {
			if (capacity < skb->len - MT6797_IMSC_CCCI_HEADER) {
				ret = -EMSGSIZE;
				spin_unlock_irqrestore(&port->rx.lock, flags);
				goto unlock;
			}
			__skb_unlink(skb, &port->rx);
			spin_unlock_irqrestore(&port->rx.lock, flags);
			break;
		}
		spin_unlock_irqrestore(&port->rx.lock, flags);
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto unlock;
		}
		ret = wait_event_interruptible(port->wait,
					       READ_ONCE(port->stopped) ||
					       !skb_queue_empty(&port->rx));
		if (ret)
			goto unlock;
	}
	copied = !copy_to_user(buffer, skb->data + MT6797_IMSC_CCCI_HEADER,
			      skb->len - MT6797_IMSC_CCCI_HEADER);
	spin_lock_irqsave(&port->rx.lock, flags);
	if (!copied && !port->stopped) {
		/* Its existing charge reserves room even if RX filled meanwhile. */
		__skb_queue_head(&port->rx, skb);
		ret = -EFAULT;
		skb = NULL;
	} else {
		if (!port->stopped)
			port->touched = true;
		port->rx_count--;
		port->rx_memory -= skb->truesize;
		ret = port->stopped ? (ssize_t)port->error :
			(ssize_t)(skb->len - MT6797_IMSC_CCCI_HEADER);
	}
	spin_unlock_irqrestore(&port->rx.lock, flags);
	kfree_skb(skb);
	wake_up_interruptible(&port->wait);
unlock:
	mutex_unlock(&port->read_mutex);
	return ret;
}

static ssize_t mt6797_imsc_write(struct file *file, const char __user *buffer,
				 size_t bytes, loff_t *position)
{
	struct mt6797_imsc_port *port = file->private_data;
	unsigned long flags;
	u8 *payload;
	bool attempted;
	int ret;

	/*
	 * A write is one complete current IMCB control record; never split it or
	 * accept a caller-supplied CCCI header. The backend adds that header.
	 */
	if (bytes != MT6797_IMSC_PAYLOAD)
		return -EMSGSIZE;
	payload = memdup_user(buffer, bytes);
	if (IS_ERR(payload))
		return PTR_ERR(payload);
	for (;;) {
		ret = mutex_lock_interruptible(&port->tx_mutex);
		if (ret)
			break;
		spin_lock_irqsave(&port->rx.lock, flags);
		ret = port->stopped ? port->error : (!port->ready ? -EAGAIN : 0);
		spin_unlock_irqrestore(&port->rx.lock, flags);
		attempted = false;
		if (!ret && port->context) {
			attempted = true;
			ret = port->backend.send(port->context, payload, bytes);
		} else if (!ret) {
			ret = -ESHUTDOWN;
		}
		spin_lock_irqsave(&port->rx.lock, flags);
		if (!ret)
			port->touched = true;
		else if (port->stopped)
			ret = port->error;
		/*
		 * Clear before checking the ring. A reclaim racing a busy return must
		 * not be overwritten after room() with a stale false value.
		 */
		if (attempted)
			port->tx_room = false;
		spin_unlock_irqrestore(&port->rx.lock, flags);
		if (attempted && port->backend.room(port->context))
			mt6797_imsc_port_tx_wake(port);
		mutex_unlock(&port->tx_mutex);
		wake_up_interruptible(&port->wait);
		if (ret != -EBUSY)
			break;
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			break;
		}
		ret = wait_event_interruptible(port->wait,
					       mt6797_imsc_write_waitable(port));
		if (ret)
			break;
	}
	kfree(payload);
	/*
	 * A published transfer remains a successful write even if teardown
	 * starts while it completes; never invite an uncertain retransmission.
	 */
	return ret ? (ssize_t)ret : (ssize_t)bytes;
}

static __poll_t mt6797_imsc_poll(struct file *file, poll_table *wait)
{
	struct mt6797_imsc_port *port = file->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(file, &port->wait, wait);
	spin_lock_irqsave(&port->rx.lock, flags);
	if (port->stopped) {
		mask = EPOLLERR | EPOLLHUP;
	} else {
		if (!skb_queue_empty(&port->rx))
			mask |= EPOLLIN | EPOLLRDNORM;
		if (port->ready && port->tx_room)
			mask |= EPOLLOUT | EPOLLWRNORM;
	}
	spin_unlock_irqrestore(&port->rx.lock, flags);
	return mask;
}

static const struct file_operations mt6797_imsc_fops = {
	.owner = THIS_MODULE,
	.open = mt6797_imsc_open,
	.release = mt6797_imsc_release,
	.read = mt6797_imsc_read,
	.write = mt6797_imsc_write,
	.poll = mt6797_imsc_poll,
};

struct mt6797_imsc_port *
mt6797_imsc_port_create(struct device *parent,
			const struct mt6797_imsc_backend *backend, void *context)
{
	struct mt6797_imsc_port *port;
	int ret;

	if (!backend || !backend->send || !backend->room || !context)
		return ERR_PTR(-EINVAL);
	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return ERR_PTR(-ENOMEM);
	kref_init(&port->reference);
	mutex_init(&port->read_mutex);
	mutex_init(&port->tx_mutex);
	skb_queue_head_init(&port->rx);
	init_waitqueue_head(&port->wait);
	port->backend = *backend;
	port->context = context;
	port->misc.minor = MISC_DYNAMIC_MINOR;
	port->misc.name = "mt6797-imsc";
	port->misc.fops = &mt6797_imsc_fops;
	port->misc.parent = parent;
	port->misc.mode = 0600;
	ret = misc_register(&port->misc);
	if (ret) {
		kref_put(&port->reference, mt6797_imsc_port_free);
		return ERR_PTR(ret);
	}
	return port;
}

static u32 mt6797_imsc_get32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

int mt6797_imsc_port_rx(struct mt6797_imsc_port *port, struct sk_buff *skb)
{
	unsigned long flags;
	int ret = 0;

	if (!port || !skb)
		return -EINVAL;
	/*
	 * Validate transport framing only. IMCB event/account semantics belong to
	 * the exclusive userspace owner; unfamiliar events must not be discarded.
	 */
	if (skb_is_nonlinear(skb) ||
	    skb->len < MT6797_IMSC_CCCI_HEADER + MT6797_IMSC_ENVELOPE ||
	    skb->len > MT6797_IMSC_CCCI_HEADER + MT6797_IMSC_PAYLOAD ||
	    mt6797_imsc_get32(skb->data) ||
	    mt6797_imsc_get32(skb->data + 4) != skb->len ||
	    (mt6797_imsc_get32(skb->data + 8) & 0xffff) != MT6797_IMSC_RX) {
		mt6797_imsc_port_stop(port, -EPROTO);
		return -EPROTO;
	}
	spin_lock_irqsave(&port->rx.lock, flags);
	if (port->stopped) {
		ret = port->error;
	} else if (port->rx_count >= MT6797_IMSC_RX_FRAMES ||
		   skb->truesize > MT6797_IMSC_RX_MEMORY - port->rx_memory) {
		ret = -ENOBUFS;
		mt6797_imsc_stop_locked(port, ret);
	} else {
		port->rx_count++;
		port->rx_memory += skb->truesize;
		__skb_queue_tail(&port->rx, skb);
	}
	spin_unlock_irqrestore(&port->rx.lock, flags);
	wake_up_interruptible(&port->wait);
	return ret;
}

void mt6797_imsc_port_destroy(struct mt6797_imsc_port *port)
{
	if (!port)
		return;
	mt6797_imsc_port_stop(port, -ESHUTDOWN);
	/*
	 * misc_open serializes its open callback with misc_deregister; the
	 * driver's initial kref pins the embedded misc object until it returns.
	 */
	misc_deregister(&port->misc);
	mutex_lock(&port->tx_mutex);
	port->context = NULL;
	mutex_unlock(&port->tx_mutex);
	kref_put(&port->reference, mt6797_imsc_port_free);
}
