/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_HIF_RUNTIME_H
#define __MT6797_WIFI_HIF_RUNTIME_H

#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "hif_pump.h"
#include "scan.h"

struct device;
struct mt6797_wifi_ahb_hif;
struct mt6797_wifi_boot;

enum mt6797_wifi_hif_irq_mode {
	MT6797_WIFI_HIF_IRQ_QUIESCENT,
	MT6797_WIFI_HIF_IRQ_CAPABILITY,
	MT6797_WIFI_HIF_IRQ_RUNTIME,
	MT6797_WIFI_HIF_IRQ_QUIESCING,
	MT6797_WIFI_HIF_IRQ_TERMINAL,
};

#define MT6797_WIFI_HIF_WAIT_EVENT_PAYLOAD_MAX	16U

/*
 * One bounded event slot shared by the threaded IRQ and a lifecycle-locked
 * sleeping owner. The IRQ copies payload bytes under lock; it never publishes
 * a pointer into the HIF RX buffer.
 */
struct mt6797_wifi_hif_wait_event {
	/* Serializes IRQ offer against owner take, abort, and disarm. */
	spinlock_t lock;
	struct completion completion;
	u8 payload[MT6797_WIFI_HIF_WAIT_EVENT_PAYLOAD_MAX];
	size_t payload_length;
	size_t packet_length;
	int error;
	u8 id;
	u8 sequence;
	u8 match[2];
	u8 match_length;
	bool armed;
	bool ready;
};

struct mt6797_wifi_hif_event_copy {
	u8 payload[MT6797_WIFI_HIF_WAIT_EVENT_PAYLOAD_MAX];
	size_t payload_length;
	size_t packet_length;
	int error;
	u8 id;
	u8 sequence;
};

struct mt6797_wifi_hif_runtime_transaction {
	bool registers_saved;
	bool registers_dirty;
	bool device_touched;
	bool linux_enabled;
	bool active;
	bool terminal;
};

/*
 * One driver-private synchronous RX sink. Callbacks run under sink_lock in
 * threaded-IRQ or terminal context, must not sleep, and must not retain input
 * pointers. The lifecycle owner attaches and detaches while holding
 * lifecycle_lock; the sink context must outlive that interval.
 */
struct mt6797_wifi_hif_runtime_sink {
	int (*event)(void *context,
		     const struct mt6797_wifi_event_view *event);
	int (*frame)(void *context, const struct mt6797_wifi_rx_view *frame);
	void (*abort)(void *context, int reason);
	void *context;
};

struct mt6797_wifi_hif_runtime_ops {
	int (*precheck)(void *context);
	void (*mask_wrapper)(void *context);
	int (*save_registers)(void *context);
	int (*program_runtime)(void *context);
	int (*clear_status)(void *context);
	void (*publish_mode)(void *context,
			     enum mt6797_wifi_hif_irq_mode mode);
	int (*enable_device)(void *context);
	int (*enable_linux)(void *context);
	void (*unmask_wrapper)(void *context);
	int (*disable_device)(void *context);
	void (*disable_linux)(void *context);
	int (*restore_registers)(void *context);
	void (*fence)(void *context);
	int (*teardown)(void *context);
	void *context;
};

struct mt6797_wifi_hif_irq {
	struct device *dev;
	struct mt6797_wifi_ahb_hif *hif;
	struct mt6797_wifi_boot *boot;
	struct mutex lifecycle_lock; /* Serializes runtime start/stop/worker. */
	/* Publishes terminal mode and its reason as one observable state. */
	spinlock_t terminal_lock;
	/* Lock order: lifecycle_lock -> terminal_lock -> sink_lock. */
	spinlock_t sink_lock;
	struct work_struct terminal_work;
	irqreturn_t (*capability_thread)(void *context);
	void *capability_context;
	struct mt6797_wifi_hif_runtime_transaction transaction;
	struct mt6797_wifi_hif_pump_stats pump_stats;
	struct mt6797_wifi_hif_wait_event wait_event;
	struct mt6797_wifi_hif_runtime_sink sink;
	u8 *rx_buffer[2];
	ktime_t pump_deadline;
	u32 whcr_before;
	u32 whier_before;
	u32 acknowledged_status;
	int irq;
	int mode;
	int terminal_reason;
};

int mt6797_wifi_hif_runtime_transaction_start(
	const struct mt6797_wifi_hif_runtime_ops *ops,
	struct mt6797_wifi_hif_runtime_transaction *transaction);
int mt6797_wifi_hif_runtime_transaction_stop(
	const struct mt6797_wifi_hif_runtime_ops *ops,
	struct mt6797_wifi_hif_runtime_transaction *transaction,
	bool terminal);
bool mt6797_wifi_hif_runtime_mark_terminal(int *mode);
bool mt6797_wifi_hif_runtime_publish_terminal(
	struct mt6797_wifi_hif_irq *irq, int reason);
int mt6797_wifi_hif_runtime_status(struct mt6797_wifi_hif_irq *irq);
void mt6797_wifi_hif_runtime_fail(struct mt6797_wifi_hif_irq *irq,
				   int reason);
int mt6797_wifi_hif_runtime_route_event(
	struct mt6797_wifi_scan_transaction *transaction,
	const struct mt6797_wifi_event_view *event);
int mt6797_wifi_hif_runtime_sink_attach(
	struct mt6797_wifi_hif_irq *irq,
	const struct mt6797_wifi_hif_runtime_sink *sink);
int mt6797_wifi_hif_runtime_sink_detach(
	struct mt6797_wifi_hif_irq *irq, void *context);
int mt6797_wifi_hif_runtime_sink_event(
	struct mt6797_wifi_hif_irq *irq,
	const struct mt6797_wifi_event_view *event);
int mt6797_wifi_hif_runtime_sink_frame(
	struct mt6797_wifi_hif_irq *irq,
	const struct mt6797_wifi_rx_view *frame);
void mt6797_wifi_hif_runtime_sink_abort(
	struct mt6797_wifi_hif_irq *irq, int reason);
void mt6797_wifi_hif_wait_event_init(
	struct mt6797_wifi_hif_wait_event *wait_event);
int mt6797_wifi_hif_wait_event_arm(
	struct mt6797_wifi_hif_wait_event *wait_event, u8 id,
	const u8 *match, size_t match_length);
int mt6797_wifi_hif_wait_event_offer(
	struct mt6797_wifi_hif_wait_event *wait_event,
	const struct mt6797_wifi_event_view *event);
int mt6797_wifi_hif_wait_event_take(
	struct mt6797_wifi_hif_wait_event *wait_event,
	struct mt6797_wifi_hif_event_copy *copy);
void mt6797_wifi_hif_wait_event_abort(
	struct mt6797_wifi_hif_wait_event *wait_event, int reason);
void mt6797_wifi_hif_wait_event_disarm(
	struct mt6797_wifi_hif_wait_event *wait_event);

int mt6797_wifi_hif_irq_init(struct mt6797_wifi_hif_irq *irq,
			     struct device *dev,
			     struct mt6797_wifi_ahb_hif *hif,
			     int linux_irq);
int mt6797_wifi_hif_irq_enter_capability(
	struct mt6797_wifi_hif_irq *irq,
	irqreturn_t (*thread)(void *context), void *context);
void mt6797_wifi_hif_irq_leave_capability(struct mt6797_wifi_hif_irq *irq);
enum mt6797_wifi_hif_irq_mode
mt6797_wifi_hif_irq_mode(const struct mt6797_wifi_hif_irq *irq);

int mt6797_wifi_hif_runtime_start(struct mt6797_wifi_hif_irq *irq,
				   struct mt6797_wifi_boot *boot);
int mt6797_wifi_hif_runtime_stop(struct mt6797_wifi_hif_irq *irq);

#endif /* __MT6797_WIFI_HIF_RUNTIME_H */
