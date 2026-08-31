// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 BTIF mandatory-to-full STP bootstrap controller.
 *
 * The retained Gemini vendor sequence is deliberately the whole contract:
 *
 * - wmt_core.c:747-809 selects BTIF mandatory mode before this sequence;
 * - wmt_core.c:585-631 issues the raw HCR/HVR/FVR reads before the BTIF
 *   script, and wmt_ic_soc.c:1614-1659 selects the supported HVR values;
 * - wmt_ic_soc.c:122-124 defines the query command and both exact replies;
 * - wmt_ic_soc.c:190-191 defines the exact set command/reply;
 * - wmt_ic_soc.c:1008-1044 executes query-default, set, host full-mode,
 *   waits 10 ms (with >=2 ms documented), then query-full;
 * - wmt_core.c:634-673 sends every init-script command then compares its
 *   complete reply, and wmt_ctrl.c:248-286 uses WMT_LIB_RX_TIMEOUT;
 * - include/wmt_lib.h:70 defines that reply timeout as 2000 ms;
 * - stp_core.c:577-604 and 632-754 establish the full STP 0/7 initial window
 *   and 180-ms retained-frame resync/retry behavior.
 *
 * This is neither a CONSYS power sequencer nor a radio-function controller.
 * It performs no discovery and has no DT match.  The only transmission path
 * is default-false parameter gated.  Its regular run API emits only the
 * vendor STP-switch exchanges below.  An opaque, synchronous post-full stage
 * may issue an independently gated firmware sequence through the bootstrap's
 * private full-STP WMT endpoint before the session is published.  In
 * particular, this object never enables Wi-Fi, Bluetooth, GNSS, or FM.
 * The caller supplies the same long-lived CONSYS runtime lease that the PIO
 * owner pins for the entire mandatory-to-full handoff.
 */

#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_STP_BOOTSTRAP_KUNIT_TEST)
#include <kunit/test.h>
#include <linux/kthread.h>
#endif

#include <linux/soc/mediatek/mt6797-btif-pio.h>
#include <linux/soc/mediatek/mt6797-bt-h4-collector.h>
#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mt6797-consys.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>
#include <linux/soc/mediatek/mt6797-consys-wmt-initial.h>
#include <linux/soc/mediatek/mtk-stp.h>
#include <linux/soc/mediatek/mtk-wmt.h>

#define MT6797_BTIF_STP_WMT_OPCODE		0x04
#define MT6797_BTIF_STP_WMT_TIMEOUT_MS		2000
#define MT6797_BTIF_STP_HOST_SWITCH_DELAY_MS	10
#define MT6797_BTIF_STP_RAW_REG_COMMAND_SIZE	20

static bool enable_experimental_stp_bootstrap;
module_param_named(enable_experimental_stp_bootstrap,
		   enable_experimental_stp_bootstrap, bool, 0400);
MODULE_PARM_DESC(enable_experimental_stp_bootstrap,
		 "allow the dormant MT6797 BTIF STP bootstrap experiment (default: false)");

enum mt6797_btif_stp_bootstrap_state {
	MT6797_BTIF_STP_BOOTSTRAP_NEW,
	MT6797_BTIF_STP_BOOTSTRAP_MANDATORY,
	MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY,
	MT6797_BTIF_STP_BOOTSTRAP_ACTIVE,
	MT6797_BTIF_STP_BOOTSTRAP_FENCED,
};

struct mt6797_btif_stp_bootstrap_full_wmt_lease {
	struct mt6797_btif_stp_bootstrap *bootstrap;
};

struct mt6797_btif_stp_bootstrap {
	struct mutex lifecycle_lock;
	/* destroy() waits for the sole runner before it frees this object. */
	struct completion run_complete;
	const struct mt6797_btif_pio_lease *pio_lease;
	const struct mt6797_consys_runtime_lease *runtime_lease;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct mtk_stp mandatory_stp;
	struct mtk_wmt mandatory_wmt;
	struct mt6797_consys_wmt_initial *initial;
	struct completion raw_response;
	/* Protects the raw request stack references from threaded RX. */
	spinlock_t raw_lock;
	u8 *raw_event;
	size_t raw_event_capacity;
	size_t *raw_event_length;
	int raw_result;
	bool raw_pending;
	u64 raw_wmt_events;
	u64 raw_wmt_event_bytes;
	struct mtk_wmt full_wmt;
	struct mt6797_bt_h4_collector h4_collector;
	struct mt6797_consys_stp_session *session;
	const struct mt6797_consys_stp_session_bootstrap_lease *full_bootstrap;
	const struct mt6797_consys_stp_session_lease *full_session;
	struct delayed_work full_retry_work;
	bool full_retry_armed;
	bool full_wmt_stage_running;
	struct mt6797_btif_stp_bootstrap_full_wmt_lease full_wmt_lease;
	bool run_in_progress;
	bool destroy_requested;
	enum mt6797_btif_stp_bootstrap_state state;
};

/* The only local WMT transaction is the vendor full-mode verification query. */
static const u8 mt6797_btif_stp_query_data[] = { 0x04 };
static const u8 mt6797_btif_stp_query_full_data[] = {
	0x00, 0x04, 0xdf, 0x0e, 0x68, 0x01,
};

static void mt6797_btif_stp_bootstrap_fence(
		struct mt6797_btif_stp_bootstrap *bootstrap, bool from_retry_work)
{
	bool started;

	mutex_lock(&bootstrap->lifecycle_lock);
	if (bootstrap->state == MT6797_BTIF_STP_BOOTSTRAP_FENCED) {
		mutex_unlock(&bootstrap->lifecycle_lock);
		/* A retry worker may have set FENCED before finishing close(). */
		if (!from_retry_work)
			cancel_delayed_work_sync(&bootstrap->full_retry_work);
		return;
	}
	started = bootstrap->state != MT6797_BTIF_STP_BOOTSTRAP_NEW;
	bootstrap->full_retry_armed = false;
	bootstrap->state = MT6797_BTIF_STP_BOOTSTRAP_FENCED;
	mutex_unlock(&bootstrap->lifecycle_lock);
	if (!from_retry_work)
		cancel_delayed_work_sync(&bootstrap->full_retry_work);
	mt6797_consys_stp_session_quiesce(bootstrap->session);
	mt6797_bt_h4_collector_fence(&bootstrap->h4_collector);
	/* close() is the only supplied path that synchronizes the Linux IRQ. */
	if (started)
		bootstrap->ops.close(bootstrap->ops.context);
}

/*
 * wmt_core.c:585-631 exchanges a complete 20-byte raw WMT register command
 * with a 16-byte event whose retained WMT length field is only four.  The
 * generic mtk_wmt decoder correctly rejects that length lie, so this one
 * mandatory-STP owner consumes raw events only while an identity read is
 * armed.  All ordinary WMT events remain with the strict mtk_wmt matcher.
 */
static int
mt6797_btif_stp_bootstrap_raw_exchange(void *context, const u8 *command,
				       size_t command_length, u8 *event,
				       size_t event_capacity, size_t *event_length,
				       unsigned long timeout)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = context;
	u8 frame[MTK_STP_HEADER_SIZE + MT6797_BTIF_STP_RAW_REG_COMMAND_SIZE +
		 MTK_STP_TRAILER_SIZE];
	unsigned long flags;
	size_t frame_length;
	long wait;
	u64 raw_wmt_events;
	u64 raw_wmt_event_bytes;
	int ret;
	struct mt6797_btif_pio_diagnostics diagnostics;

	if (!command || !event || !event_length || !timeout)
		return -EINVAL;
	if (command_length != MT6797_BTIF_STP_RAW_REG_COMMAND_SIZE)
		return -EPROTO;

	spin_lock_irqsave(&bootstrap->raw_lock, flags);
	if (bootstrap->raw_pending) {
		spin_unlock_irqrestore(&bootstrap->raw_lock, flags);
		return -EBUSY;
	}
	reinit_completion(&bootstrap->raw_response);
	bootstrap->raw_event = event;
	bootstrap->raw_event_capacity = event_capacity;
	bootstrap->raw_event_length = event_length;
	bootstrap->raw_result = -EINPROGRESS;
	bootstrap->raw_pending = true;
	spin_unlock_irqrestore(&bootstrap->raw_lock, flags);

	ret = mtk_stp_encode(MTK_STP_MODE_MANDATORY, MTK_STP_WMT, 0, 0, command,
			     command_length, frame, sizeof(frame), &frame_length);
	if (!ret)
		ret = mt6797_btif_pio_transmit((void *)bootstrap->pio_lease, frame,
					       frame_length);
	if (ret)
		goto out_disarm;

	wait = wait_for_completion_interruptible_timeout(&bootstrap->raw_response,
							 timeout);
	if (wait < 0) {
		ret = wait;
	} else if (!wait) {
		ret = -ETIMEDOUT;
		spin_lock_irqsave(&bootstrap->raw_lock, flags);
		raw_wmt_events = bootstrap->raw_wmt_events;
		raw_wmt_event_bytes = bootstrap->raw_wmt_event_bytes;
		spin_unlock_irqrestore(&bootstrap->raw_lock, flags);
		if (!mt6797_btif_pio_diagnostics_snapshot(bootstrap->pio_lease,
							   &diagnostics))
			pr_warn("mt6797-consys-g2b: raw WMT timeout tx_enq=%llu/%lluB "
				"thr=%llu queued=%u irq=%llu iir=%llu(rx=%llu txe=%llu nint=%llu) "
				"rx=%lluB/%llu cb chunks=%llu last=%uB yields=%llu "
				"cb_fail=%llu proto_fail=%llu fences=%llu yield=%uB first=%*ph last=%*ph raw=%llu/%lluB regs="
				"IER=%#08x IIR=%#08x LCR=%#08x LSR=%#08x DMA=%#08x "
				"TRI=%#08x HANDSHAKE=%#08x\n",
				diagnostics.tx_enqueue_count,
				diagnostics.tx_enqueue_bytes, diagnostics.tx_thr_writes,
				diagnostics.tx_queued, diagnostics.irq_count,
				diagnostics.iir_reads, diagnostics.iir_rx,
				diagnostics.iir_tx_empty, diagnostics.iir_spurious,
				diagnostics.rx_bytes, diagnostics.rx_callbacks,
				diagnostics.rx_chunks, diagnostics.rx_last_irq_bytes,
				diagnostics.rx_work_ceiling_yields,
				diagnostics.rx_callback_failures,
				diagnostics.rx_protocol_failures, diagnostics.rx_fences,
				diagnostics.rx_yield_evidence_bytes,
				diagnostics.rx_yield_prefix_length,
				diagnostics.rx_yield_prefix,
				diagnostics.rx_yield_tail_length,
				diagnostics.rx_yield_tail,
				raw_wmt_events, raw_wmt_event_bytes,
				diagnostics.ier, diagnostics.iir, diagnostics.fakelcr,
				diagnostics.lsr, diagnostics.dma_en, diagnostics.tri_lvl,
				diagnostics.handshake);
	} else {
		spin_lock_irqsave(&bootstrap->raw_lock, flags);
		ret = bootstrap->raw_result;
		spin_unlock_irqrestore(&bootstrap->raw_lock, flags);
	}

out_disarm:
	spin_lock_irqsave(&bootstrap->raw_lock, flags);
	bootstrap->raw_pending = false;
	bootstrap->raw_event = NULL;
	bootstrap->raw_event_capacity = 0;
	bootstrap->raw_event_length = NULL;
	spin_unlock_irqrestore(&bootstrap->raw_lock, flags);
	return ret;
}

static int
mt6797_btif_stp_bootstrap_mandatory_wmt_receive(void *context,
						const u8 *payload, size_t payload_length)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = context;
	unsigned long flags;
	bool raw = false;

	spin_lock_irqsave(&bootstrap->raw_lock, flags);
	if (bootstrap->raw_pending) {
		raw = true;
		bootstrap->raw_wmt_events++;
		bootstrap->raw_wmt_event_bytes += payload_length;
		if (payload_length > bootstrap->raw_event_capacity) {
			bootstrap->raw_result = -EMSGSIZE;
		} else {
			memcpy(bootstrap->raw_event, payload, payload_length);
			*bootstrap->raw_event_length = payload_length;
			bootstrap->raw_result = 0;
		}
		bootstrap->raw_pending = false;
	}
	spin_unlock_irqrestore(&bootstrap->raw_lock, flags);
	if (raw) {
		complete(&bootstrap->raw_response);
		return 0;
	}

	return mtk_wmt_receive(&bootstrap->mandatory_wmt, payload, payload_length);
}

static int mt6797_btif_stp_bootstrap_receive(void *context, const u8 *data,
					     size_t length)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = context;

	if (READ_ONCE(bootstrap->state) == MT6797_BTIF_STP_BOOTSTRAP_MANDATORY)
		return mtk_stp_rx(&bootstrap->mandatory_stp, data, length);
	if (READ_ONCE(bootstrap->state) == MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY)
		return mt6797_consys_stp_session_bootstrap_receive(
				bootstrap->full_bootstrap, data, length);
	if (READ_ONCE(bootstrap->state) == MT6797_BTIF_STP_BOOTSTRAP_ACTIVE)
		return mt6797_consys_stp_session_receive_lease(
				bootstrap->full_session, data, length);
	return -ESHUTDOWN;
}

static int mt6797_btif_stp_bootstrap_full_wmt_receive(void *context,
						      enum mtk_stp_channel channel,
							      const u8 *payload, size_t payload_length)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = context;

	if (channel == MTK_STP_WMT)
		return mtk_wmt_receive(&bootstrap->full_wmt, payload, payload_length);
	if (channel == MTK_STP_BT)
		return mt6797_bt_h4_collector_receive(&bootstrap->h4_collector,
					      payload, payload_length);
	return -EOPNOTSUPP;
}

static int mt6797_btif_stp_bootstrap_full_wmt_transmit(void *context,
						       const u8 *packet, size_t packet_length)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = context;
	int ret;

	if (READ_ONCE(bootstrap->state) != MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY)
		return -ESHUTDOWN;
	ret = mt6797_consys_stp_session_bootstrap_transmit(
		bootstrap->full_bootstrap, MTK_STP_WMT, packet, packet_length);
	if (!ret && READ_ONCE(bootstrap->full_retry_armed))
		schedule_delayed_work(&bootstrap->full_retry_work,
				      msecs_to_jiffies(MT6797_CONSYS_STP_TX_TIMEOUT_MS));
	return ret;
}

static void mt6797_btif_stp_bootstrap_full_retry(struct work_struct *work)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = container_of(
		to_delayed_work(work), struct mt6797_btif_stp_bootstrap,
		full_retry_work);
	int ret;

	if (!READ_ONCE(bootstrap->full_retry_armed) ||
	    READ_ONCE(bootstrap->state) != MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY)
		return;

	ret = mt6797_consys_stp_session_bootstrap_timeout(bootstrap->full_bootstrap);
	if (ret) {
		mt6797_btif_stp_bootstrap_fence(bootstrap, true);
		return;
	}
	if (READ_ONCE(bootstrap->full_retry_armed) &&
	    READ_ONCE(bootstrap->state) == MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY)
		schedule_delayed_work(&bootstrap->full_retry_work,
				      msecs_to_jiffies(MT6797_CONSYS_STP_TX_TIMEOUT_MS));
}

static void mt6797_btif_stp_bootstrap_log_full_wmt_failure(
		struct mt6797_btif_stp_bootstrap *bootstrap, u8 opcode, int ret)
{
	struct mt6797_btif_pio_diagnostics diagnostics;

	if (!ret || mt6797_btif_pio_diagnostics_snapshot(bootstrap->pio_lease,
								 &diagnostics))
		return;
	pr_warn("mt6797-consys-g2b: full WMT %#02x failed=%d rx=%lluB/%llu cb "
		"chunks=%llu last=%uB yields=%llu cb_fail=%llu proto_fail=%llu "
		"fences=%llu yield=%uB first=%*ph last=%*ph\n",
		opcode, ret, diagnostics.rx_bytes, diagnostics.rx_callbacks,
		diagnostics.rx_chunks, diagnostics.rx_last_irq_bytes,
		diagnostics.rx_work_ceiling_yields,
		diagnostics.rx_callback_failures,
		diagnostics.rx_protocol_failures, diagnostics.rx_fences,
		diagnostics.rx_yield_evidence_bytes,
		diagnostics.rx_yield_prefix_length, diagnostics.rx_yield_prefix,
		diagnostics.rx_yield_tail_length, diagnostics.rx_yield_tail);
}

static void mt6797_btif_stp_bootstrap_log_calibration_trace(
		const struct mt6797_btif_pio_diagnostics *pio_before,
		const struct mt6797_btif_pio_diagnostics *pio_after,
		const struct mt6797_consys_stp_session_diagnostics *stp_before,
		const struct mt6797_consys_stp_session_diagnostics *stp_after,
		const struct mtk_wmt_trace *trace, int ret)
{
	unsigned int index;

	pr_warn("mt6797-consys-g2b: WMT14 result=%d pio-local=%lluB/%llu_cb "
		"stp-local=%lluB data=%llu wmt=%llu ack=%llu tx=%llu retry=%llu "
		"crc=%llu parse=%llu seq=%u/%u/%u/%u credits=%u retries=%u raw=%*ph/%*ph\n",
		ret, pio_after->rx_bytes - pio_before->rx_bytes,
		pio_after->rx_callbacks - pio_before->rx_callbacks,
		stp_after->rx_bytes - stp_before->rx_bytes,
		stp_after->rx_data_frames - stp_before->rx_data_frames,
		stp_after->rx_wmt_frames - stp_before->rx_wmt_frames,
		stp_after->rx_ack_frames - stp_before->rx_ack_frames,
		stp_after->tx_frames - stp_before->tx_frames,
		stp_after->retry_ticks - stp_before->retry_ticks,
		stp_after->rx_crc_failures - stp_before->rx_crc_failures,
		stp_after->rx_parser_failures - stp_before->rx_parser_failures,
		stp_after->txseq, stp_after->txack, stp_after->rxack,
		stp_after->expected_rxseq, stp_after->credits, stp_after->retries,
		stp_after->ingress_prefix_length, stp_after->ingress_prefix,
		stp_after->ingress_tail_length, stp_after->ingress_tail);
	pr_warn("mt6797-consys-g2b: WMT14 events=%llu malformed=%llu len=%u raw=%*ph/%*ph\n",
		trace->decoded_events, trace->decode_failures, trace->malformed_length,
		trace->malformed_prefix_length, trace->malformed_prefix,
		trace->malformed_tail_length, trace->malformed_tail);
	for (index = 0; index < trace->event_count; index++) {
		const struct mtk_wmt_trace_event *event = &trace->events[index];

		pr_warn("mt6797-consys-g2b: WMT14 event[%u] +%ums op=%#02x len=%u raw=%*ph/%*ph\n",
			index, event->elapsed_ms, event->opcode, event->data_len,
			event->prefix_length, event->prefix, event->tail_length,
			event->tail);
	}
}

static int mt6797_btif_stp_bootstrap_full_wmt_transaction_unchecked(
		struct mt6797_btif_stp_bootstrap *bootstrap, u8 opcode,
		const u8 *data, size_t data_len,
		const struct mtk_wmt_match *expected, unsigned long timeout)
{
	int ret;

	if (!bootstrap || !expected || (!data && data_len) || !timeout)
		return -EINVAL;
	if (READ_ONCE(bootstrap->state) != MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY)
		return -ESHUTDOWN;

	/*
	 * Vendor stp_core.c:632-754 retries retained full-STP frames every
	 * 180 ms; wmt_ic_soc.c:2171-2284 sends each patch command once and
	 * fails its sequence on an error.  Keep the session retry owner armed
	 * around every WMT transaction, without inventing a second WMT retry.
	 */
	WRITE_ONCE(bootstrap->full_retry_armed, true);
	ret = mtk_wmt_transaction(&bootstrap->full_wmt, opcode, data, data_len,
			  expected, timeout);
	WRITE_ONCE(bootstrap->full_retry_armed, false);
	cancel_delayed_work_sync(&bootstrap->full_retry_work);
	mt6797_btif_stp_bootstrap_log_full_wmt_failure(bootstrap, opcode, ret);
	return ret;
}

int mt6797_btif_stp_bootstrap_full_wmt_transaction(
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		u8 opcode, const u8 *data, size_t data_len,
		const struct mtk_wmt_match *expected, unsigned long timeout)
{
	struct mt6797_btif_stp_bootstrap *bootstrap;
	bool valid;

	if (!lease || !lease->bootstrap)
		return -EINVAL;
	bootstrap = lease->bootstrap;

	mutex_lock(&bootstrap->lifecycle_lock);
	valid = bootstrap->run_in_progress && bootstrap->full_wmt_stage_running &&
		!bootstrap->destroy_requested &&
		bootstrap->state == MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY;
	mutex_unlock(&bootstrap->lifecycle_lock);
	if (!valid)
		return -ESHUTDOWN;

	return mt6797_btif_stp_bootstrap_full_wmt_transaction_unchecked(
		bootstrap, opcode, data, data_len, expected, timeout);
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_full_wmt_transaction);

static int mt6797_btif_stp_bootstrap_full_wmt_transaction_capture_common(
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		u8 opcode, const u8 *data, size_t data_len,
		u8 expected_opcode, size_t min_data_len, size_t max_data_len,
		u8 *reply, size_t reply_len, size_t *actual_data_len,
		unsigned long timeout)
{
	struct mt6797_btif_stp_bootstrap *bootstrap;
	struct mt6797_btif_pio_diagnostics pio_before, pio_after;
	struct mt6797_consys_stp_session_diagnostics stp_before, stp_after;
	struct mtk_wmt_trace trace;
	bool trace_calibration;
	bool trace_ready = false;
	bool valid;
	int ret;

	if (!lease || !lease->bootstrap || (!reply && reply_len) ||
	    !actual_data_len || reply_len > min_data_len ||
	    min_data_len > max_data_len || (!data && data_len) || !timeout)
		return -EINVAL;
	bootstrap = lease->bootstrap;
	mutex_lock(&bootstrap->lifecycle_lock);
	valid = bootstrap->run_in_progress && bootstrap->full_wmt_stage_running &&
		!bootstrap->destroy_requested &&
		bootstrap->state == MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY;
	mutex_unlock(&bootstrap->lifecycle_lock);
	if (!valid)
		return -ESHUTDOWN;

	trace_calibration = opcode == 0x14;
	if (trace_calibration) {
		if (!mt6797_btif_pio_diagnostics_snapshot(bootstrap->pio_lease,
							 &pio_before) &&
		    !mt6797_consys_stp_session_bootstrap_diagnostics_snapshot(
					bootstrap->full_bootstrap, &stp_before)) {
			mtk_wmt_trace_begin(&bootstrap->full_wmt, opcode);
			trace_ready = true;
		}
	}
	WRITE_ONCE(bootstrap->full_retry_armed, true);
	ret = mtk_wmt_transaction_capture_range(&bootstrap->full_wmt, opcode, data,
			data_len, expected_opcode, min_data_len, max_data_len, reply,
			reply_len, actual_data_len, timeout);
	WRITE_ONCE(bootstrap->full_retry_armed, false);
	cancel_delayed_work_sync(&bootstrap->full_retry_work);
	if (trace_ready) {
		mtk_wmt_trace_end(&bootstrap->full_wmt, &trace);
		if (!mt6797_btif_pio_diagnostics_snapshot(bootstrap->pio_lease,
							 &pio_after) &&
		    !mt6797_consys_stp_session_bootstrap_diagnostics_snapshot(
					bootstrap->full_bootstrap, &stp_after))
			mt6797_btif_stp_bootstrap_log_calibration_trace(&pio_before,
					&pio_after, &stp_before, &stp_after, &trace, ret);
	} else {
		mt6797_btif_stp_bootstrap_log_full_wmt_failure(bootstrap, opcode, ret);
	}
	return ret;
}

int mt6797_btif_stp_bootstrap_full_wmt_transaction_capture(
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		u8 opcode, const u8 *data, size_t data_len,
		u8 expected_opcode, size_t expected_data_len,
		u8 *reply, size_t reply_len, unsigned long timeout)
{
	size_t actual_data_len;

	if (reply_len != expected_data_len)
		return -EINVAL;
	return mt6797_btif_stp_bootstrap_full_wmt_transaction_capture_common(
		lease, opcode, data, data_len, expected_opcode,
		expected_data_len, expected_data_len, reply, reply_len,
		&actual_data_len, timeout);
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_full_wmt_transaction_capture);

int mt6797_btif_stp_bootstrap_full_wmt_transaction_capture_range(
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		u8 opcode, const u8 *data, size_t data_len,
		u8 expected_opcode, size_t min_data_len, size_t max_data_len,
		u8 *reply, size_t reply_len, size_t *actual_data_len,
		unsigned long timeout)
{
	return mt6797_btif_stp_bootstrap_full_wmt_transaction_capture_common(
		lease, opcode, data, data_len, expected_opcode, min_data_len,
		max_data_len, reply, reply_len, actual_data_len, timeout);
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_full_wmt_transaction_capture_range);

int mt6797_btif_stp_bootstrap_full_bt_hci_exchange(
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		const u8 *command, size_t command_len, u16 expected_opcode,
		size_t expected_reply_len, u8 *reply, size_t reply_size,
		size_t *reply_len, unsigned long timeout)
{
	struct mt6797_btif_stp_bootstrap *bootstrap;
	bool valid;
	int ret;

	if (!lease || !lease->bootstrap || !command || command_len < 4 ||
	    command[0] != 0x01 || command_len != 4 + command[3] || !reply ||
	    !reply_len || !timeout || !expected_reply_len ||
	    expected_reply_len > reply_size)
		return -EINVAL;
	bootstrap = lease->bootstrap;
	mutex_lock(&bootstrap->lifecycle_lock);
	valid = bootstrap->run_in_progress && bootstrap->full_wmt_stage_running &&
		!bootstrap->destroy_requested &&
		bootstrap->state == MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY;
	mutex_unlock(&bootstrap->lifecycle_lock);
	if (!valid)
		return -ESHUTDOWN;
	*reply_len = 0;
	ret = mt6797_bt_h4_collector_begin(&bootstrap->h4_collector, expected_opcode);
	if (ret)
		return ret;
	WRITE_ONCE(bootstrap->full_retry_armed, true);
	ret = mt6797_consys_stp_session_bootstrap_transmit(
		bootstrap->full_bootstrap, MTK_STP_BT, command, command_len);
	if (!ret)
		ret = mt6797_bt_h4_collector_wait(&bootstrap->h4_collector, timeout,
					  reply, reply_size, reply_len);
	WRITE_ONCE(bootstrap->full_retry_armed, false);
	cancel_delayed_work_sync(&bootstrap->full_retry_work);
	if (!ret && *reply_len != expected_reply_len)
		ret = -EPROTO;
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_full_bt_hci_exchange);

struct mt6797_btif_stp_bootstrap *
mt6797_btif_stp_bootstrap_create(
		const struct mt6797_btif_pio_lease *pio_lease,
		const struct mt6797_consys_runtime_lease *runtime_lease,
		const struct mt6797_btif_stp_bootstrap_ops *ops)
{
	struct mt6797_btif_stp_bootstrap *bootstrap;

	if (!pio_lease || !runtime_lease || !ops || !ops->enable_irq || !ops->close)
		return ERR_PTR(-EINVAL);

	bootstrap = kzalloc_obj(*bootstrap, GFP_KERNEL);
	if (!bootstrap)
		return ERR_PTR(-ENOMEM);
	mutex_init(&bootstrap->lifecycle_lock);
	init_completion(&bootstrap->run_complete);
	init_completion(&bootstrap->raw_response);
	spin_lock_init(&bootstrap->raw_lock);
	mt6797_bt_h4_collector_init(&bootstrap->h4_collector);
	INIT_DELAYED_WORK(&bootstrap->full_retry_work,
			  mt6797_btif_stp_bootstrap_full_retry);
	bootstrap->pio_lease = pio_lease;
	bootstrap->runtime_lease = runtime_lease;
	bootstrap->ops = *ops;
	bootstrap->full_wmt_lease.bootstrap = bootstrap;
	return bootstrap;
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_create);

static const struct mt6797_consys_stp_session_lease *
mt6797_btif_stp_bootstrap_run_unchecked(
		struct mt6797_btif_stp_bootstrap *bootstrap,
		mt6797_btif_stp_bootstrap_full_wmt_stage_t stage,
		void *stage_context)
{
	const struct mt6797_consys_stp_session_lease *session_lease;
	struct mt6797_consys_stp_session_ops session_ops;
	struct mt6797_consys_wmt_initial_ops initial_ops;
	struct mt6797_consys_wmt_initial_identity identity;
	struct mtk_wmt_match expected;
	unsigned long timeout = msecs_to_jiffies(MT6797_BTIF_STP_WMT_TIMEOUT_MS);
	int ret;

	mutex_lock(&bootstrap->lifecycle_lock);
	if (bootstrap->destroy_requested) {
		session_lease = ERR_PTR(-ESHUTDOWN);
		goto out_unlock;
	}
	if (bootstrap->state == MT6797_BTIF_STP_BOOTSTRAP_FENCED) {
		session_lease = ERR_PTR(-EPIPE);
		goto out_unlock;
	}
	if (bootstrap->state != MT6797_BTIF_STP_BOOTSTRAP_NEW) {
		session_lease = ERR_PTR(-EBUSY);
		goto out_unlock;
	}
	bootstrap->run_in_progress = true;
	reinit_completion(&bootstrap->run_complete);

	mtk_stp_init(&bootstrap->mandatory_stp, MTK_STP_MODE_MANDATORY);
	mtk_wmt_init(&bootstrap->mandatory_wmt, MTK_STP_MODE_MANDATORY,
		     mt6797_btif_pio_transmit, (void *)bootstrap->pio_lease);
	initial_ops = (struct mt6797_consys_wmt_initial_ops) {
		.raw_exchange = mt6797_btif_stp_bootstrap_raw_exchange,
		.context = bootstrap,
	};
	bootstrap->initial =
		mt6797_consys_wmt_initial_create(&bootstrap->mandatory_wmt,
						 &initial_ops);
	if (IS_ERR(bootstrap->initial)) {
		ret = PTR_ERR(bootstrap->initial);
		bootstrap->initial = NULL;
		goto fail_locked;
	}
	ret = mtk_stp_register_client(&bootstrap->mandatory_stp, MTK_STP_WMT,
				      mt6797_btif_stp_bootstrap_mandatory_wmt_receive,
				     bootstrap);
	if (ret)
		goto fail_locked;
	ret = mt6797_btif_pio_begin_mandatory(bootstrap->pio_lease,
					      bootstrap->runtime_lease,
					      mt6797_btif_stp_bootstrap_receive,
					      bootstrap);
	if (ret)
		goto fail_locked;
	/* The RX consumer is valid before an installed IRQ becomes deliverable. */
	WRITE_ONCE(bootstrap->state, MT6797_BTIF_STP_BOOTSTRAP_MANDATORY);
	ret = bootstrap->ops.enable_irq(bootstrap->ops.context);
	if (ret)
		goto fail_locked;
	mutex_unlock(&bootstrap->lifecycle_lock);

	ret = mt6797_consys_wmt_initial_run_exclusive(bootstrap->initial,
						      &identity, timeout);
	if (ret)
		goto fail;
	ret = mt6797_consys_wmt_initial_configure_chip_full_exclusive(bootstrap->initial,
								      timeout);
	if (ret)
		goto fail;

	session_ops = (struct mt6797_consys_stp_session_ops) {
		.transmit = mt6797_btif_pio_transmit,
		.receive = mt6797_btif_stp_bootstrap_full_wmt_receive,
		.transmit_context = (void *)bootstrap->pio_lease,
		.receive_context = bootstrap,
	};
	bootstrap->session = mt6797_consys_stp_session_create(&session_ops);
	if (IS_ERR(bootstrap->session)) {
		ret = PTR_ERR(bootstrap->session);
		bootstrap->session = NULL;
		goto fail;
	}
	bootstrap->full_bootstrap =
		mt6797_consys_stp_session_begin_full_bootstrap(bootstrap->session);
	if (IS_ERR(bootstrap->full_bootstrap)) {
		ret = PTR_ERR(bootstrap->full_bootstrap);
		bootstrap->full_bootstrap = NULL;
		goto fail;
	}
	mtk_wmt_init_payload_transport(&bootstrap->full_wmt, MTK_STP_MODE_FULL,
				       mt6797_btif_stp_bootstrap_full_wmt_transmit,
				       bootstrap);
	/*
	 * PIO keeps this router from the mandatory phase onwards.  Publishing the
	 * full parser first gives every RX byte one unambiguous owner while PIO
	 * serializes its own state transition; there is no callback/state gap.
	 */
	WRITE_ONCE(bootstrap->state, MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY);
	ret = mt6797_btif_pio_handoff_full_bootstrap(bootstrap->pio_lease,
						     mt6797_btif_stp_bootstrap_receive,
					     bootstrap);
	if (ret)
		goto fail;
	/* Vendor wmt_ic_soc.c says 10 ms, although its stated minimum is 2 ms. */
	pr_info("mt6797-consys-g2b: before host full-mode settle\n");
	msleep(MT6797_BTIF_STP_HOST_SWITCH_DELAY_MS);
	pr_info("mt6797-consys-g2b: after host full-mode settle\n");

	expected = (struct mtk_wmt_match) {
		.opcode = MT6797_BTIF_STP_WMT_OPCODE,
		.data = mt6797_btif_stp_query_full_data,
		.data_len = sizeof(mt6797_btif_stp_query_full_data),
	};
	pr_info("mt6797-consys-g2b: before full-mode query\n");
	ret = mt6797_btif_stp_bootstrap_full_wmt_transaction_unchecked(
			bootstrap, MT6797_BTIF_STP_WMT_OPCODE, mt6797_btif_stp_query_data,
			sizeof(mt6797_btif_stp_query_data), &expected, timeout);
	pr_info("mt6797-consys-g2b: after full-mode query ret=%d\n", ret);
	if (ret)
		goto fail;

	/* mtk_wmt_transaction() returned zero only after its exact reply matcher. */
	ret = mt6797_consys_stp_session_bootstrap_confirm(
		bootstrap->full_bootstrap);
	if (ret)
		goto fail;
	if (stage) {
		mutex_lock(&bootstrap->lifecycle_lock);
		bootstrap->full_wmt_stage_running = true;
		mutex_unlock(&bootstrap->lifecycle_lock);
		ret = stage(stage_context, &bootstrap->full_wmt_lease);
		mutex_lock(&bootstrap->lifecycle_lock);
		bootstrap->full_wmt_stage_running = false;
		mutex_unlock(&bootstrap->lifecycle_lock);
		/* Channel zero is private to this callback, never the promoted session. */
		mt6797_bt_h4_collector_fence(&bootstrap->h4_collector);
		if (ret)
			goto fail;
	}
	bootstrap->full_session = mt6797_consys_stp_session_bootstrap_promote(
		bootstrap->full_bootstrap);
	if (IS_ERR(bootstrap->full_session)) {
		ret = PTR_ERR(bootstrap->full_session);
		bootstrap->full_session = NULL;
		goto fail;
	}
	/* The stable router changes state before PIO's final commit. */
	WRITE_ONCE(bootstrap->state, MT6797_BTIF_STP_BOOTSTRAP_ACTIVE);
	ret = mt6797_btif_pio_promote_full(bootstrap->pio_lease,
					   mt6797_btif_stp_bootstrap_receive,
					      bootstrap);
	if (ret)
		goto fail;
	session_lease = bootstrap->full_session;
	goto out_complete;

fail:
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	session_lease = ERR_PTR(ret);
	goto out_complete;
fail_locked:
	mutex_unlock(&bootstrap->lifecycle_lock);
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	session_lease = ERR_PTR(ret);
	goto out_complete;
out_complete:
	mutex_lock(&bootstrap->lifecycle_lock);
	bootstrap->run_in_progress = false;
	complete_all(&bootstrap->run_complete);
	mutex_unlock(&bootstrap->lifecycle_lock);
	return session_lease;
out_unlock:
	mutex_unlock(&bootstrap->lifecycle_lock);
	return session_lease;
}

const struct mt6797_consys_stp_session_lease *
mt6797_btif_stp_bootstrap_run(struct mt6797_btif_stp_bootstrap *bootstrap)
{
	if (!bootstrap)
		return ERR_PTR(-EINVAL);
	if (!READ_ONCE(enable_experimental_stp_bootstrap))
		return ERR_PTR(-EPERM);
	return mt6797_btif_stp_bootstrap_run_unchecked(bootstrap, NULL, NULL);
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_run);

const struct mt6797_consys_stp_session_lease *
mt6797_btif_stp_bootstrap_run_with_full_wmt_stage(
		struct mt6797_btif_stp_bootstrap *bootstrap,
		mt6797_btif_stp_bootstrap_full_wmt_stage_t stage,
		void *stage_context)
{
	if (!bootstrap || !stage)
		return ERR_PTR(-EINVAL);
	if (!READ_ONCE(enable_experimental_stp_bootstrap))
		return ERR_PTR(-EPERM);

	return mt6797_btif_stp_bootstrap_run_unchecked(bootstrap, stage,
						       stage_context);
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_run_with_full_wmt_stage);

void mt6797_btif_stp_bootstrap_destroy(
		struct mt6797_btif_stp_bootstrap *bootstrap)
{
	bool run_in_progress;

	if (!bootstrap || IS_ERR(bootstrap))
		return;

	/*
	 * run() drops lifecycle_lock around bounded WMT transactions.  Publish the
	 * terminal intent under that lock and wait for that runner before fencing
	 * or freeing the embedded WMT/session objects it still uses.  The supplied
	 * close() callback must not re-enter destroy().
	 */
	mutex_lock(&bootstrap->lifecycle_lock);
	bootstrap->destroy_requested = true;
	run_in_progress = bootstrap->run_in_progress;
	mutex_unlock(&bootstrap->lifecycle_lock);
	if (run_in_progress)
		wait_for_completion(&bootstrap->run_complete);
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	mt6797_consys_wmt_initial_destroy(bootstrap->initial);
	mt6797_consys_stp_session_destroy(bootstrap->session);
	kfree(bootstrap);
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_destroy);

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_STP_BOOTSTRAP_KUNIT_TEST)

#define MT6797_BTIF_STP_TEST_IER		0x04
#define MT6797_BTIF_STP_TEST_IIR		0x08
#define MT6797_BTIF_STP_TEST_LSR		0x14
#define MT6797_BTIF_STP_TEST_IIR_NINT		BIT(0)
#define MT6797_BTIF_STP_TEST_IIR_TX_EMPTY	BIT(1)
#define MT6797_BTIF_STP_TEST_IIR_RX		BIT(2)
#define MT6797_BTIF_STP_TEST_LSR_TEMT		BIT(6)
#define MT6797_BTIF_STP_TEST_REGS		(0x70 / sizeof(u32))

struct mt6797_btif_stp_test_bus {
	u32 regs[MT6797_BTIF_STP_TEST_REGS];
	u32 iir;
	u8 rx[64];
	size_t rx_length;
	size_t rx_position;
	u8 tx[128];
	size_t tx_length;
};

struct mt6797_btif_stp_test_peer {
	struct mt6797_btif_stp_test_bus *bus;
	const struct mt6797_btif_pio_lease *pio_lease;
	struct mt6797_btif_pio *pio;
	struct task_struct *task;
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease runtime_lease;
	struct completion done;
	size_t tx_position;
	unsigned int stage;
	int fail_stage;
	unsigned int close_count;
	int error;
};

struct mt6797_btif_stp_test_stage {
	bool called;
	int result;
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease;
};

static int mt6797_btif_stp_test_lifecycle_op(void *context)
{
	return 0;
}

static int mt6797_btif_stp_test_lifecycle_poll(
		void *context, enum mt6797_consys_poll poll)
{
	return poll == MT6797_CONSYS_POLL_CHIP_READY ? 0 : -EINVAL;
}

static const struct mt6797_consys_ops mt6797_btif_stp_test_lifecycle_ops = {
	.abi = MT6797_CONSYS_LIFECYCLE_ABI,
	.enable_vcn18 = mt6797_btif_stp_test_lifecycle_op,
	.disable_vcn18 = mt6797_btif_stp_test_lifecycle_op,
	.enable_vcn28 = mt6797_btif_stp_test_lifecycle_op,
	.disable_vcn28 = mt6797_btif_stp_test_lifecycle_op,
	.configure_sleep_mask = mt6797_btif_stp_test_lifecycle_op,
	.assert_reset = mt6797_btif_stp_test_lifecycle_op,
	.configure_pwron = mt6797_btif_stp_test_lifecycle_op,
	.enable_scp_sys_conn = mt6797_btif_stp_test_lifecycle_op,
	.disable_scp_sys_conn = mt6797_btif_stp_test_lifecycle_op,
	.poll = mt6797_btif_stp_test_lifecycle_poll,
};

static const struct mt6797_consys_timeouts mt6797_btif_stp_test_timeouts = {
	.poll_interval_us = 1,
	.chip_ready_timeout_us = 1,
};

static void mt6797_btif_stp_test_runtime_acquire(
		struct kunit *test, struct mt6797_btif_stp_test_peer *peer)
{
	KUNIT_ASSERT_EQ(test, mt6797_consys_lifecycle_init(&peer->lifecycle,
							   &mt6797_btif_stp_test_lifecycle_ops,
							    NULL,
							    &mt6797_btif_stp_test_timeouts), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_acquire(&peer->lifecycle,
								  &peer->runtime_lease), 0);
}

struct mt6797_btif_stp_test_destroy_race {
	struct mt6797_btif_stp_bootstrap *bootstrap;
	struct mt6797_btif_pio *pio;
	struct completion enable_irq_entered;
	struct completion allow_enable_irq;
	struct completion run_done;
	struct completion destroy_done;
	unsigned int close_count;
	int run_result;
};

static u32 mt6797_btif_stp_test_read32(void *context, u32 offset)
{
	struct mt6797_btif_stp_test_bus *bus = context;

	if (offset == MT6797_BTIF_STP_TEST_IIR) {
		if (bus->rx_position < bus->rx_length)
			return MT6797_BTIF_STP_TEST_IIR_RX;
		return bus->iir;
	}
	return bus->regs[offset / sizeof(u32)];
}

static void mt6797_btif_stp_test_write32(void *context, u32 offset, u32 value)
{
	struct mt6797_btif_stp_test_bus *bus = context;

	bus->regs[offset / sizeof(u32)] = value;
}

static u8 mt6797_btif_stp_test_read8(void *context, u32 offset)
{
	struct mt6797_btif_stp_test_bus *bus = context;

	if (offset != 0 || bus->rx_position == bus->rx_length)
		return 0;
	return bus->rx[bus->rx_position++];
}

static void mt6797_btif_stp_test_write8(void *context, u32 offset, u8 value)
{
	struct mt6797_btif_stp_test_bus *bus = context;

	if (offset == 0 && bus->tx_length < sizeof(bus->tx))
		bus->tx[bus->tx_length++] = value;
}

static int mt6797_btif_stp_test_frame(unsigned int stage, bool reply,
				      u8 *frame, size_t frame_size, size_t *frame_length)
{
	const u8 *packet;
	size_t packet_length;
	enum mtk_stp_mode mode = MTK_STP_MODE_MANDATORY;
	u8 sequence = 0;
	u8 ack = 0;
	static const u8 commands[][MT6797_BTIF_STP_RAW_REG_COMMAND_SIZE] = {
		{
			0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01,
			0x08, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
			0xff, 0xff, 0x00, 0x00,
		}, {
			0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01,
			0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
			0xff, 0xff, 0x00, 0x00,
		}, {
			0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01,
			0x04, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
			0xff, 0xff, 0x00, 0x00,
		}, {
			0x01, 0x04, 0x01, 0x00, 0x04,
		}, {
			0x01, 0x04, 0x05, 0x00, 0x03, 0xdf, 0x0e, 0x68, 0x01,
		}, {
			0x01, 0x04, 0x01, 0x00, 0x04,
		},
	};
	static const u8 command_lengths[] = { 20, 20, 20, 5, 9, 5 };
	static const u8 replies[][MT6797_BTIF_STP_RAW_REG_COMMAND_SIZE] = {
		{
			0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
			0x08, 0x00, 0x00, 0x80, 0x79, 0x02, 0x00, 0x00,
		}, {
			0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
			0x00, 0x00, 0x00, 0x80, 0x01, 0x8b, 0xa5, 0xa5,
		}, {
			0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
			0x04, 0x00, 0x00, 0x80, 0x34, 0x5a, 0xa5, 0xa5,
		}, {
			0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0x11, 0x00,
			0x00, 0x00,
		}, {
			0x02, 0x04, 0x02, 0x00, 0x00, 0x03,
		}, {
			0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xdf, 0x0e,
			0x68, 0x01,
		},
	};
	static const u8 reply_lengths[] = { 16, 16, 16, 10, 6, 10 };

	if (stage >= ARRAY_SIZE(commands))
		return -EINVAL;
	if (stage == 5) {
		mode = MTK_STP_MODE_FULL;
		if (reply)
			ack = 0;
		else
			ack = 7;
	}
	if (reply) {
		packet = replies[stage];
		packet_length = reply_lengths[stage];
	} else {
		packet = commands[stage];
		packet_length = command_lengths[stage];
	}
	return mtk_stp_encode(mode, MTK_STP_WMT, sequence, ack, packet,
			      packet_length, frame, frame_size, frame_length);
}

static int mt6797_btif_stp_test_peer_thread(void *context)
{
	struct mt6797_btif_stp_test_peer *peer = context;
	u8 expected[32];
	u8 reply[32];
	size_t expected_length;
	size_t reply_length;
	int ret;

	while (!kthread_should_stop() && peer->stage < 6) {
		peer->bus->iir = MT6797_BTIF_STP_TEST_IIR_TX_EMPTY;
		peer->bus->regs[MT6797_BTIF_STP_TEST_LSR / sizeof(u32)] =
			MT6797_BTIF_STP_TEST_LSR_TEMT;
		ret = mt6797_btif_pio_irq(peer->pio_lease);
		if (ret && ret != -EAGAIN) {
			peer->error = ret;
			break;
		}
		ret = mt6797_btif_stp_test_frame(peer->stage, false, expected,
						 sizeof(expected), &expected_length);
		if (ret || peer->bus->tx_length - peer->tx_position < expected_length) {
			if (ret)
				peer->error = ret;
			usleep_range(1000, 2000);
			continue;
		}
		if (memcmp(peer->bus->tx + peer->tx_position, expected,
			   expected_length)) {
			peer->error = -EPROTO;
			break;
		}
		peer->tx_position += expected_length;
		if (peer->fail_stage == peer->stage)
			break;
		ret = mt6797_btif_stp_test_frame(peer->stage, true, reply,
						 sizeof(reply), &reply_length);
		if (ret || reply_length > sizeof(peer->bus->rx)) {
			peer->error = ret ?: -EMSGSIZE;
			break;
		}
		memcpy(peer->bus->rx, reply, reply_length);
		peer->bus->rx_length = reply_length;
		peer->bus->rx_position = 0;
		peer->bus->iir = MT6797_BTIF_STP_TEST_IIR_NINT;
		ret = mt6797_btif_pio_irq(peer->pio_lease);
		if (ret) {
			peer->error = ret;
			break;
		}
		peer->stage++;
	}
	complete(&peer->done);
	/* close() owns the peer task reference and must stop a live task. */
	while (!kthread_should_stop())
		schedule();
	return 0;
}

static int mt6797_btif_stp_test_enable_irq(void *context)
{
	struct mt6797_btif_stp_test_peer *peer = context;

	peer->task = kthread_run(mt6797_btif_stp_test_peer_thread, peer,
				 "mt6797-stp-peer");
	return IS_ERR(peer->task) ? PTR_ERR(peer->task) : 0;
}

static int mt6797_btif_stp_test_block_enable_irq(void *context)
{
	struct mt6797_btif_stp_test_destroy_race *race = context;

	complete(&race->enable_irq_entered);
	wait_for_completion(&race->allow_enable_irq);
	return -EIO;
}

static void mt6797_btif_stp_test_destroy_race_close(void *context)
{
	struct mt6797_btif_stp_test_destroy_race *race = context;

	race->close_count++;
	mt6797_btif_pio_close(race->pio);
}

static int mt6797_btif_stp_test_run_thread(void *context)
{
	struct mt6797_btif_stp_test_destroy_race *race = context;
	const struct mt6797_consys_stp_session_lease *session_lease;

	session_lease = mt6797_btif_stp_bootstrap_run_unchecked(race->bootstrap,
								 NULL, NULL);
	race->run_result = IS_ERR(session_lease) ? PTR_ERR(session_lease) : 0;
	complete(&race->run_done);
	return 0;
}

static int mt6797_btif_stp_test_destroy_thread(void *context)
{
	struct mt6797_btif_stp_test_destroy_race *race = context;

	mt6797_btif_stp_bootstrap_destroy(race->bootstrap);
	complete(&race->destroy_done);
	return 0;
}

static void mt6797_btif_stp_test_close(void *context)
{
	struct mt6797_btif_stp_test_peer *peer = context;

	peer->close_count++;
	if (peer->task && !IS_ERR(peer->task))
		kthread_stop(peer->task);
	mt6797_btif_pio_close(peer->pio);
}

static int mt6797_btif_stp_test_full_wmt_stage(void *context,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	struct mt6797_btif_stp_test_stage *stage = context;

	if (!stage || !lease)
		return -EINVAL;
	stage->called = true;
	stage->lease = lease;
	return stage->result;
}

static void mt6797_btif_stp_test_exact_handoff_stage_lease_expires(
		struct kunit *test)
{
	struct mt6797_btif_stp_test_peer *peer;
	struct mt6797_btif_stp_test_bus *bus;
	struct mt6797_btif_pio_io_ops io_ops;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct mt6797_btif_stp_bootstrap *bootstrap;
	const struct mt6797_consys_stp_session_lease *session_lease;
	static const u8 reset_data[] = { 0x04 };
	static const u8 reset_reply[] = { 0x00 };
	const struct mtk_wmt_match reset = {
		.opcode = 0x07,
		.data = reset_reply,
		.data_len = sizeof(reset_reply),
	};
	struct mt6797_btif_stp_test_stage stage = { };
	unsigned long waited;
	size_t tx_length;

	peer = kunit_kzalloc(test, sizeof(*peer), GFP_KERNEL);
	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, peer);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	peer->bus = bus;
	peer->fail_stage = -1;
	mt6797_btif_stp_test_runtime_acquire(test, peer);
	bus->iir = MT6797_BTIF_STP_TEST_IIR_NINT;
	init_completion(&peer->done);
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_stp_test_read32,
		.write32 = mt6797_btif_stp_test_write32,
		.read8 = mt6797_btif_stp_test_read8,
		.write8 = mt6797_btif_stp_test_write8,
		.context = bus,
	};
	peer->pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio));
	peer->pio_lease = mt6797_btif_pio_acquire(peer->pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio_lease));
	ops = (struct mt6797_btif_stp_bootstrap_ops) {
		.enable_irq = mt6797_btif_stp_test_enable_irq,
		.close = mt6797_btif_stp_test_close,
		.context = peer,
	};
	bootstrap = mt6797_btif_stp_bootstrap_create(peer->pio_lease,
						     &peer->runtime_lease, &ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(bootstrap));
	session_lease = mt6797_btif_stp_bootstrap_run_unchecked(bootstrap,
			mt6797_btif_stp_test_full_wmt_stage, &stage);
	KUNIT_ASSERT_FALSE(test, IS_ERR(session_lease));
	KUNIT_ASSERT_TRUE(test, stage.called);
	KUNIT_ASSERT_NOT_NULL(test, stage.lease);
	waited = wait_for_completion_timeout(&peer->done, msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	KUNIT_EXPECT_EQ(test, peer->error, 0);
	KUNIT_EXPECT_EQ(test, peer->stage, 6U);
	KUNIT_EXPECT_TRUE(test,
			  mt6797_consys_stp_session_lease_active(session_lease));
	/* The callback capability expires before the session becomes visible. */
	tx_length = bus->tx_length;
	KUNIT_EXPECT_EQ(test,
		mt6797_btif_stp_bootstrap_full_wmt_transaction(stage.lease, 0x07,
			reset_data, sizeof(reset_data), &reset,
			msecs_to_jiffies(MT6797_BTIF_STP_WMT_TIMEOUT_MS)), -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, bus->tx_length, tx_length);
	mt6797_btif_stp_bootstrap_destroy(bootstrap);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(
									&peer->runtime_lease), 0);
	mt6797_btif_pio_destroy(peer->pio);
}

static void mt6797_btif_stp_test_stage_failures_fence(struct kunit *test)
{
	struct mt6797_btif_stp_test_peer *peer;
	struct mt6797_btif_stp_test_bus *bus;
	struct mt6797_btif_pio_io_ops io_ops;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct mt6797_btif_stp_bootstrap *bootstrap;
	const struct mt6797_consys_stp_session_lease *session_lease;
	unsigned long waited;
	unsigned int stage;
	int ret;

	for (stage = 0; stage < 6; stage++) {
		peer = kunit_kzalloc(test, sizeof(*peer), GFP_KERNEL);
		bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, peer);
		KUNIT_ASSERT_NOT_NULL(test, bus);
		peer->bus = bus;
		peer->fail_stage = stage;
		mt6797_btif_stp_test_runtime_acquire(test, peer);
		bus->iir = MT6797_BTIF_STP_TEST_IIR_NINT;
		init_completion(&peer->done);
		io_ops = (struct mt6797_btif_pio_io_ops) {
			.read32 = mt6797_btif_stp_test_read32,
			.write32 = mt6797_btif_stp_test_write32,
			.read8 = mt6797_btif_stp_test_read8,
			.write8 = mt6797_btif_stp_test_write8,
			.context = bus,
		};
		peer->pio = mt6797_btif_pio_create(&io_ops);
		KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio));
		peer->pio_lease = mt6797_btif_pio_acquire(peer->pio);
		KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio_lease));
		ops = (struct mt6797_btif_stp_bootstrap_ops) {
			.enable_irq = mt6797_btif_stp_test_enable_irq,
			.close = mt6797_btif_stp_test_close,
			.context = peer,
		};
		bootstrap = mt6797_btif_stp_bootstrap_create(peer->pio_lease,
							     &peer->runtime_lease, &ops);
		KUNIT_ASSERT_FALSE(test, IS_ERR(bootstrap));

		session_lease = mt6797_btif_stp_bootstrap_run_unchecked(bootstrap, NULL,
									 NULL);
		KUNIT_EXPECT_TRUE(test, IS_ERR(session_lease));
		KUNIT_EXPECT_EQ(test, PTR_ERR(session_lease), -ETIMEDOUT);
		waited = wait_for_completion_timeout(&peer->done,
						     msecs_to_jiffies(1000));
		KUNIT_EXPECT_NE(test, waited, 0UL);
		KUNIT_EXPECT_EQ(test, peer->error, 0);
		KUNIT_EXPECT_EQ(test, peer->stage, stage);
		KUNIT_EXPECT_EQ(test, peer->close_count, 1U);
		mt6797_btif_stp_bootstrap_destroy(bootstrap);
		KUNIT_EXPECT_EQ(test, peer->close_count, 1U);
		ret = mt6797_consys_runtime_lease_release(&peer->runtime_lease);
		KUNIT_ASSERT_EQ(test, ret, 0);
		mt6797_btif_pio_destroy(peer->pio);
	}
}

static void mt6797_btif_stp_test_full_wmt_stage_failure_fences(struct kunit *test)
{
	struct mt6797_btif_stp_test_peer *peer;
	struct mt6797_btif_stp_test_bus *bus;
	struct mt6797_btif_pio_io_ops io_ops;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct mt6797_btif_stp_bootstrap *bootstrap;
	struct mt6797_btif_stp_test_stage stage = { .result = -EIO };
	const struct mt6797_consys_stp_session_lease *session_lease;
	unsigned long waited;

	peer = kunit_kzalloc(test, sizeof(*peer), GFP_KERNEL);
	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, peer);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	peer->bus = bus;
	peer->fail_stage = -1;
	mt6797_btif_stp_test_runtime_acquire(test, peer);
	bus->iir = MT6797_BTIF_STP_TEST_IIR_NINT;
	init_completion(&peer->done);
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_stp_test_read32,
		.write32 = mt6797_btif_stp_test_write32,
		.read8 = mt6797_btif_stp_test_read8,
		.write8 = mt6797_btif_stp_test_write8,
		.context = bus,
	};
	peer->pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio));
	peer->pio_lease = mt6797_btif_pio_acquire(peer->pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio_lease));
	ops = (struct mt6797_btif_stp_bootstrap_ops) {
		.enable_irq = mt6797_btif_stp_test_enable_irq,
		.close = mt6797_btif_stp_test_close,
		.context = peer,
	};
	bootstrap = mt6797_btif_stp_bootstrap_create(peer->pio_lease,
						     &peer->runtime_lease, &ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(bootstrap));

	session_lease = mt6797_btif_stp_bootstrap_run_unchecked(bootstrap,
			mt6797_btif_stp_test_full_wmt_stage, &stage);
	KUNIT_EXPECT_TRUE(test, IS_ERR(session_lease));
	KUNIT_EXPECT_EQ(test, PTR_ERR(session_lease), -EIO);
	KUNIT_EXPECT_TRUE(test, stage.called);
	waited = wait_for_completion_timeout(&peer->done, msecs_to_jiffies(1000));
	KUNIT_EXPECT_NE(test, waited, 0UL);
	KUNIT_EXPECT_EQ(test, peer->error, 0);
	KUNIT_EXPECT_EQ(test, peer->stage, 6U);
	KUNIT_EXPECT_PTR_EQ(test, bootstrap->full_session, NULL);
	KUNIT_EXPECT_EQ(test, bootstrap->state, MT6797_BTIF_STP_BOOTSTRAP_FENCED);
	KUNIT_EXPECT_EQ(test, peer->close_count, 1U);
	mt6797_btif_stp_bootstrap_destroy(bootstrap);
	KUNIT_EXPECT_EQ(test, peer->close_count, 1U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(
									&peer->runtime_lease), 0);
	mt6797_btif_pio_destroy(peer->pio);
}

static void mt6797_btif_stp_test_fence_is_one_shot(struct kunit *test)
{
	struct mt6797_btif_stp_test_peer *peer;
	struct mt6797_btif_stp_test_bus *bus;
	struct mt6797_btif_pio_io_ops io_ops;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct mt6797_btif_stp_bootstrap *bootstrap;

	peer = kunit_kzalloc(test, sizeof(*peer), GFP_KERNEL);
	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, peer);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	peer->bus = bus;
	mt6797_btif_stp_test_runtime_acquire(test, peer);
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_stp_test_read32,
		.write32 = mt6797_btif_stp_test_write32,
		.read8 = mt6797_btif_stp_test_read8,
		.write8 = mt6797_btif_stp_test_write8,
		.context = bus,
	};
	peer->pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio));
	peer->pio_lease = mt6797_btif_pio_acquire(peer->pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio_lease));
	ops = (struct mt6797_btif_stp_bootstrap_ops) {
		.enable_irq = mt6797_btif_stp_test_enable_irq,
		.close = mt6797_btif_stp_test_close,
		.context = peer,
	};
	bootstrap = mt6797_btif_stp_bootstrap_create(peer->pio_lease,
						     &peer->runtime_lease, &ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(bootstrap));
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	KUNIT_EXPECT_EQ(test, peer->close_count, 0U);
	bootstrap->state = MT6797_BTIF_STP_BOOTSTRAP_MANDATORY;
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	KUNIT_EXPECT_EQ(test, peer->close_count, 1U);
	mt6797_btif_stp_bootstrap_destroy(bootstrap);
	KUNIT_EXPECT_EQ(test, peer->close_count, 1U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(
									&peer->runtime_lease), 0);
	mt6797_btif_pio_destroy(peer->pio);
}

static void mt6797_btif_stp_test_destroy_waits_for_runner(struct kunit *test)
{
	struct mt6797_btif_stp_test_destroy_race *race;
	struct mt6797_btif_stp_test_peer *peer;
	struct mt6797_btif_stp_test_bus *bus;
	struct mt6797_btif_pio_io_ops io_ops;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct task_struct *destroy_task;
	struct task_struct *run_task;
	unsigned long waited;

	race = kunit_kzalloc(test, sizeof(*race), GFP_KERNEL);
	peer = kunit_kzalloc(test, sizeof(*peer), GFP_KERNEL);
	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, race);
	KUNIT_ASSERT_NOT_NULL(test, peer);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	peer->bus = bus;
	mt6797_btif_stp_test_runtime_acquire(test, peer);
	bus->iir = MT6797_BTIF_STP_TEST_IIR_NINT;
	init_completion(&race->enable_irq_entered);
	init_completion(&race->allow_enable_irq);
	init_completion(&race->run_done);
	init_completion(&race->destroy_done);
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_stp_test_read32,
		.write32 = mt6797_btif_stp_test_write32,
		.read8 = mt6797_btif_stp_test_read8,
		.write8 = mt6797_btif_stp_test_write8,
		.context = bus,
	};
	peer->pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio));
	race->pio = peer->pio;
	peer->pio_lease = mt6797_btif_pio_acquire(peer->pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio_lease));
	ops = (struct mt6797_btif_stp_bootstrap_ops) {
		.enable_irq = mt6797_btif_stp_test_block_enable_irq,
		.close = mt6797_btif_stp_test_destroy_race_close,
		.context = race,
	};
	race->bootstrap = mt6797_btif_stp_bootstrap_create(peer->pio_lease,
							   &peer->runtime_lease, &ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(race->bootstrap));

	run_task = kthread_run(mt6797_btif_stp_test_run_thread, race,
			       "mt6797-stp-run");
	KUNIT_ASSERT_FALSE(test, IS_ERR(run_task));
	waited = wait_for_completion_timeout(&race->enable_irq_entered,
					     msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	destroy_task = kthread_run(mt6797_btif_stp_test_destroy_thread, race,
				   "mt6797-stp-destroy");
	KUNIT_ASSERT_FALSE(test, IS_ERR(destroy_task));
	/* destroy() must remain blocked until the in-flight runner returns. */
	waited = wait_for_completion_timeout(&race->destroy_done,
					     msecs_to_jiffies(20));
	KUNIT_EXPECT_EQ(test, waited, 0UL);
	complete(&race->allow_enable_irq);
	waited = wait_for_completion_timeout(&race->run_done, msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	waited = wait_for_completion_timeout(&race->destroy_done,
					     msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	KUNIT_EXPECT_EQ(test, race->run_result, -EIO);
	KUNIT_EXPECT_EQ(test, race->close_count, 1U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(
									&peer->runtime_lease), 0);
	mt6797_btif_pio_destroy(peer->pio);
}

static struct kunit_case mt6797_btif_stp_bootstrap_test_cases[] = {
	KUNIT_CASE(mt6797_btif_stp_test_exact_handoff_stage_lease_expires),
	KUNIT_CASE(mt6797_btif_stp_test_stage_failures_fence),
	KUNIT_CASE(mt6797_btif_stp_test_full_wmt_stage_failure_fences),
	KUNIT_CASE(mt6797_btif_stp_test_fence_is_one_shot),
	KUNIT_CASE(mt6797_btif_stp_test_destroy_waits_for_runner),
	{ }
};

static struct kunit_suite mt6797_btif_stp_bootstrap_test_suite = {
	.name = "mt6797-btif-stp-bootstrap",
	.test_cases = mt6797_btif_stp_bootstrap_test_cases,
};

kunit_test_suite(mt6797_btif_stp_bootstrap_test_suite);

#endif

MODULE_DESCRIPTION("Dormant MT6797 BTIF mandatory-to-full STP handover");
MODULE_LICENSE("GPL");
