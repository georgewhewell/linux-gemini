// SPDX-License-Identifier: GPL-2.0-only
/* Bounded MT6797 modem speech-control records. No automatic modem responses. */
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

#include "mtk_mt6797_ccci_speech.h"
#include "mtk_mt6797_ccci_speech_port.h"

#define MT6797_SPEECH_RX_FRAMES 32U
#define MT6797_SPEECH_RX_MEMORY (MT6797_SPEECH_RX_FRAMES * 8192U)

struct mt6797_speech_port {
	struct miscdevice misc;
	struct kref reference;
	struct mutex read_mutex;
	struct mutex tx_mutex;
	struct sk_buff_head rx;
	wait_queue_head_t wait;
	struct mt6797_speech_backend backend;
	void *context; /* tx_mutex; detached before the parent can disappear */
	/* rx.lock guards all following fields. Counts include a detached read. */
	unsigned int rx_count;
	unsigned int rx_memory;
	bool opened;
	bool ready;
	bool tx_room;
	bool stopped;
	bool speech_active;
	bool read_inflight;
	/* Counts include queued, detached and delivered requests until a real
	 * response submission. Credits cover successful current-owner reads only.
	 * Their total is bounded by RX_FRAMES, including reserved TX credits. */
	unsigned int requests[MT6797_SPEECH_REPLY_TYPES];
	unsigned int credits[MT6797_SPEECH_REPLY_TYPES];
	u16 waiting_id;
	int error;
};

/* Caller holds rx.lock. */
static unsigned int mt6797_speech_requests(struct mt6797_speech_port *port)
{
	return port->requests[MT6797_SPEECH_REPLY_RF] +
		port->requests[MT6797_SPEECH_REPLY_CODEC];
}

static bool mt6797_speech_has_credit(struct mt6797_speech_port *port)
{
	return port->credits[MT6797_SPEECH_REPLY_RF] ||
		port->credits[MT6797_SPEECH_REPLY_CODEC];
}

/* Each blocked writer waits for its own record's eligibility. POLLOUT may
 * advertise a credited reply while an unrelated command still waits for ACK. */
static bool mt6797_speech_write_waitable(struct mt6797_speech_port *port,
				       int reply)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&port->rx.lock, flags);
	ready = port->stopped || !port->ready ||
		(reply >= 0 && !port->credits[reply]) ||
		(port->tx_room && (reply >= 0 || !port->waiting_id));
	spin_unlock_irqrestore(&port->rx.lock, flags);
	return ready;
}

static void mt6797_speech_port_free(struct kref *reference)
{
	struct mt6797_speech_port *port = container_of(reference,
					struct mt6797_speech_port, reference);

	skb_queue_purge(&port->rx);
	kfree(port);
}

/* Caller holds rx.lock. A read detached from the queue keeps its own charge
 * and SKB; terminal/last-close cleanup never frees a reader's live buffer. */
static void mt6797_speech_purge_locked(struct mt6797_speech_port *port)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(&port->rx))) {
		port->rx_count--;
		port->rx_memory -= skb->truesize;
		kfree_skb(skb);
	}
}

static void mt6797_speech_stop_locked(struct mt6797_speech_port *port, int error)
{
	if (port->stopped)
		return;
	port->stopped = true;
	port->ready = false;
	port->tx_room = false;
	port->error = error < 0 ? error : -ESHUTDOWN;
	memset(port->requests, 0, sizeof(port->requests));
	memset(port->credits, 0, sizeof(port->credits));
	mt6797_speech_purge_locked(port);
}

void mt6797_speech_port_stop(struct mt6797_speech_port *port, int error)
{
	unsigned long flags;

	if (!port)
		return;
	spin_lock_irqsave(&port->rx.lock, flags);
	mt6797_speech_stop_locked(port, error);
	spin_unlock_irqrestore(&port->rx.lock, flags);
	wake_up_interruptible(&port->wait);
}

void mt6797_speech_port_ready(struct mt6797_speech_port *port, bool ready)
{
	unsigned long flags;

	if (!port)
		return;
	spin_lock_irqsave(&port->rx.lock, flags);
	if (!port->stopped) {
		port->ready = ready;
		port->tx_room = ready;
	}
	spin_unlock_irqrestore(&port->rx.lock, flags);
	wake_up_interruptible(&port->wait);
}

void mt6797_speech_port_tx_wake(struct mt6797_speech_port *port)
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

static int mt6797_speech_open(struct inode *inode, struct file *file)
{
	struct mt6797_speech_port *port = container_of(file->private_data,
					struct mt6797_speech_port, misc);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&port->rx.lock, flags);
	if (port->stopped)
		ret = port->error;
	else if (!port->ready)
		ret = -EAGAIN;
	else if (port->opened)
		ret = -EBUSY;
	else {
		kref_get(&port->reference);
		port->opened = true;
		file->private_data = port;
	}
	spin_unlock_irqrestore(&port->rx.lock, flags);
	if (ret)
		return ret;
	return nonseekable_open(inode, file);
}

static int mt6797_speech_release(struct inode *inode, struct file *file)
{
	struct mt6797_speech_port *port = file->private_data;
	unsigned long flags;

	/* VFS holds the file through every active read/write, so final release
 * cannot run with a syscall holding this descriptor's read/TX mutex. */
	spin_lock_irqsave(&port->rx.lock, flags);
	/* A new owner must never receive an old owner's still-pending ACK or
 * inherit a live speech engine. Recovery needs an actual modem reset;
 * closing a file does not manufacture the missing speech-off completion. */
	if (port->waiting_id || port->speech_active || mt6797_speech_requests(port))
		mt6797_speech_stop_locked(port, -ECANCELED);
	port->opened = false;
	mt6797_speech_purge_locked(port);
	spin_unlock_irqrestore(&port->rx.lock, flags);
	kref_put(&port->reference, mt6797_speech_port_free);
	return 0;
}

static ssize_t mt6797_speech_read(struct file *file, char __user *buffer,
				 size_t capacity, loff_t *position)
{
	struct mt6797_speech_port *port = file->private_data;
	struct sk_buff *skb;
	struct mt6797_speech_message message;
	unsigned long flags;
	int request = -ENOENT;
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
			if (capacity < skb->len) {
				ret = -EMSGSIZE;
				spin_unlock_irqrestore(&port->rx.lock, flags);
				goto unlock;
			}
			ret = mt6797_speech_parse(skb->data, skb->len, &message);
			if (!ret)
				request = mt6797_speech_request_index(&message);
			if (ret || request == -EPROTO) {
				ret = ret ? ret : -EPROTO;
				mt6797_speech_stop_locked(port, ret);
				spin_unlock_irqrestore(&port->rx.lock, flags);
				wake_up_interruptible(&port->wait);
				goto unlock;
			}
			port->read_inflight = true;
			__skb_unlink(skb, &port->rx);
			spin_unlock_irqrestore(&port->rx.lock, flags);
			break;
		}
		spin_unlock_irqrestore(&port->rx.lock, flags);
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto unlock;
		}
		ret = wait_event_interruptible(port->wait, READ_ONCE(port->stopped) ||
					      !skb_queue_empty(&port->rx));
		if (ret)
			goto unlock;
	}
	copied = !copy_to_user(buffer, skb->data, skb->len);
	spin_lock_irqsave(&port->rx.lock, flags);
	port->read_inflight = false;
	if (!copied && !port->stopped) {
		/* Its existing charge reserves room even if RX filled meanwhile. */
		__skb_queue_head(&port->rx, skb);
		ret = -EFAULT;
		skb = NULL;
	} else {
		if (!port->stopped && request >= 0)
			port->credits[request]++;
		port->rx_count--;
		port->rx_memory -= skb->truesize;
		ret = port->stopped ? (ssize_t)port->error : (ssize_t)skb->len;
	}
	spin_unlock_irqrestore(&port->rx.lock, flags);
	kfree_skb(skb);
	wake_up_interruptible(&port->wait);
unlock:
	mutex_unlock(&port->read_mutex);
	return ret;
}

static int mt6797_speech_validate_tx(const u8 *frame, size_t bytes)
{
	u32 first, packed;
	u16 id;

	if (bytes < MT6797_SPEECH_HEADER || bytes > MT6797_SPEECH_FRAME_MAX)
		return -EMSGSIZE;
	if ((mt6797_speech_read32(frame + 8) & 0xffff) != MT6797_SPEECH_TX)
		return -EPROTO;
	first = mt6797_speech_read32(frame);
	if (bytes == MT6797_SPEECH_HEADER) {
		if (first != 0xffffffffU)
			return -EPROTO;
		packed = mt6797_speech_read32(frame + 4);
		id = packed >> 16;
		if (mt6797_speech_reply_index(id) >= 0 &&
		    ((packed & 0xffff) || mt6797_speech_read32(frame + 12)))
			return -EPROTO;
		return (id & 0xff00) == 0x2f00 ? 0 : -EPROTO;
	}
	if (first || mt6797_speech_read32(frame + 4) != bytes)
		return -EPROTO;
	packed = mt6797_speech_read32(frame + 12);
	id = packed >> 16;
	/* Initial native data support is the exact in-band dynamic-parameter
 * format; PCM2way/recording data need their own observed owner contracts. */
	if (id != MT6797_SPEECH_DYNAMIC_PARAMETERS ||
	    (packed & 0xffff) != bytes - MT6797_SPEECH_HEADER ||
	    bytes <= MT6797_SPEECH_HEADER + MT6797_SPEECH_PARAMETERS_PREFIX ||
	    frame[16] != 0xa2 || frame[17] != 0xa2 ||
	    frame[18] != 19 || frame[19] ||
	    (size_t)((u16)frame[20] | ((u16)frame[21] << 8)) != bytes - 22)
		return -EPROTO;
	return 0;
}

static ssize_t mt6797_speech_write(struct file *file, const char __user *buffer,
				  size_t bytes, loff_t *position)
{
	struct mt6797_speech_port *port = file->private_data;
	unsigned long flags;
	u8 *frame;
	u16 id;
	bool expects_ack, armed, credited, attempted;
	int reply, ret;

	if (bytes < MT6797_SPEECH_HEADER || bytes > MT6797_SPEECH_FRAME_MAX)
		return -EMSGSIZE;
	frame = memdup_user(buffer, bytes);
	if (IS_ERR(frame))
		return PTR_ERR(frame);
	ret = mt6797_speech_validate_tx(frame, bytes);
	if (ret)
		goto free;
	id = mt6797_speech_read32(frame +
		(bytes == MT6797_SPEECH_HEADER ? 4 : 12)) >> 16;
	reply = mt6797_speech_reply_index(id);
	expects_ack = mt6797_speech_expects_ack(id);
	for (;;) {
		ret = mutex_lock_interruptible(&port->tx_mutex);
		if (ret)
			break;
		spin_lock_irqsave(&port->rx.lock, flags);
		armed = credited = attempted = false;
		ret = port->stopped ? port->error :
			(!port->ready ? -EAGAIN : 0);
		if (!ret && reply >= 0) {
			/* Only a request successfully copied to this exclusive owner
			 * authorizes its exact direct response. Never change ACK state. */
			if (!port->credits[reply])
				ret = -EPERM;
			else {
				port->credits[reply]--;
				credited = true;
			}
		} else if (!ret && port->waiting_id) {
			ret = -EBUSY;
		} else if (!ret && expects_ack) {
			port->waiting_id = id;
			armed = true;
		}
		spin_unlock_irqrestore(&port->rx.lock, flags);
		if (!ret && port->context) {
			attempted = true;
			ret = port->backend.send(port->context, frame, bytes);
		} else if (!ret) {
			ret = -ESHUTDOWN;
		}
		spin_lock_irqsave(&port->rx.lock, flags);
		if (credited && !port->stopped) {
			if (ret)
				port->credits[reply]++; /* unpublished; original request remains */
			else
				port->requests[reply]--;
		}
		if (ret && armed && port->waiting_id == id)
			port->waiting_id = 0;
		if (ret && port->stopped)
			ret = port->error;
		/* Clear before testing the ring, not after: completion between a
		 * busy return and room() must not be overwritten by a stale false. */
		if (attempted)
			port->tx_room = false;
		spin_unlock_irqrestore(&port->rx.lock, flags);
		if (attempted && port->backend.room(port->context))
			mt6797_speech_port_tx_wake(port);
		mutex_unlock(&port->tx_mutex);
		wake_up_interruptible(&port->wait);
		if (ret != -EBUSY)
			break;
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			break;
		}
		ret = wait_event_interruptible(port->wait,
				mt6797_speech_write_waitable(port, reply));
		if (ret)
			break;
	}
free:
	kfree(frame);
	return ret ? (ssize_t)ret : (ssize_t)bytes;
}

static __poll_t mt6797_speech_poll(struct file *file, poll_table *wait)
{
	struct mt6797_speech_port *port = file->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(file, &port->wait, wait);
	spin_lock_irqsave(&port->rx.lock, flags);
	if (port->stopped)
		mask = EPOLLERR | EPOLLHUP;
	else {
		if (!skb_queue_empty(&port->rx))
			mask |= EPOLLIN | EPOLLRDNORM;
		if (port->ready && port->tx_room &&
		    (!port->waiting_id || mt6797_speech_has_credit(port)))
			mask |= EPOLLOUT | EPOLLWRNORM;
	}
	spin_unlock_irqrestore(&port->rx.lock, flags);
	return mask;
}

static const struct file_operations mt6797_speech_fops = {
	.owner = THIS_MODULE,
	.open = mt6797_speech_open,
	.release = mt6797_speech_release,
	.read = mt6797_speech_read,
	.write = mt6797_speech_write,
	.poll = mt6797_speech_poll,
};

struct mt6797_speech_port *mt6797_speech_port_create(struct device *parent,
		const struct mt6797_speech_backend *backend, void *context)
{
	struct mt6797_speech_port *port;
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
	port->misc.name = "mt6797-speech";
	port->misc.fops = &mt6797_speech_fops;
	port->misc.parent = parent;
	port->misc.mode = 0600;
	ret = misc_register(&port->misc);
	if (ret) {
		kref_put(&port->reference, mt6797_speech_port_free);
		return ERR_PTR(ret);
	}
	return port;
}

int mt6797_speech_port_rx(struct mt6797_speech_port *port, struct sk_buff *skb)
{
	struct mt6797_speech_message message;
	unsigned long flags;
	int request, ret;

	if (!port || !skb)
		return -EINVAL;
	if (skb_is_nonlinear(skb)) {
		mt6797_speech_port_stop(port, -EPROTO);
		return -EPROTO;
	}
	ret = mt6797_speech_parse(skb->data, skb->len, &message);
	request = ret ? -ENOENT : mt6797_speech_request_index(&message);
	if (request == -EPROTO)
		ret = -EPROTO;
	if (ret) {
		/* A dropped completion would otherwise leave an apparently live
		 * owner waiting forever. Fence only speech on a corrupt record. */
		mt6797_speech_port_stop(port, ret);
		return ret;
	}
	spin_lock_irqsave(&port->rx.lock, flags);
	if (port->stopped)
		ret = port->error;
	else if ((request >= 0 && mt6797_speech_requests(port) >= MT6797_SPEECH_RX_FRAMES) ||
		 port->rx_count >= MT6797_SPEECH_RX_FRAMES ||
		 skb->truesize > MT6797_SPEECH_RX_MEMORY - port->rx_memory)
		ret = -ENOBUFS;
	else if (mt6797_speech_expects_ack(message.id & 0x7fff) &&
		 !mt6797_speech_ack_matches(&message, port->waiting_id))
		ret = -EPROTO;
	else if (message.id == MT6797_SPEECH_RESET_REQUEST &&
		 (port->waiting_id || port->speech_active || port->read_inflight ||
		  mt6797_speech_requests(port)))
		ret = -EPIPE;
	else {
		/* An idle real reset starts a new generation. Do not deliver queued
		 * pre-reset ACKs or notifications after its reset indication. */
		if (message.id == MT6797_SPEECH_RESET_REQUEST)
			mt6797_speech_purge_locked(port);
		if (request >= 0)
			port->requests[request]++;
		if (mt6797_speech_ack_matches(&message, port->waiting_id)) {
			if (port->waiting_id == MT6797_SPEECH_ON)
				port->speech_active = true;
			else if (port->waiting_id == MT6797_SPEECH_OFF)
				port->speech_active = false;
			port->waiting_id = 0;
		}
		port->rx_count++;
		port->rx_memory += skb->truesize;
		__skb_queue_tail(&port->rx, skb);
	}
	if (ret == -ENOBUFS || ret == -EPROTO || ret == -EPIPE)
		mt6797_speech_stop_locked(port, ret);
	spin_unlock_irqrestore(&port->rx.lock, flags);
	wake_up_interruptible(&port->wait);
	return ret;
}

void mt6797_speech_port_destroy(struct mt6797_speech_port *port)
{
	if (!port)
		return;
	mt6797_speech_port_stop(port, -ESHUTDOWN);
	/* misc_open serializes its open callback with misc_deregister; the
 * driver's initial kref pins the embedded misc object until it returns. */
	misc_deregister(&port->misc);
	mutex_lock(&port->tx_mutex);
	port->context = NULL;
	mutex_unlock(&port->tx_mutex);
	kref_put(&port->reference, mt6797_speech_port_free);
}
