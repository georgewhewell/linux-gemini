/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * One-shot MT6797 BTIF mandatory-to-full STP handover controller.
 *
 * This object owns no platform device, register mapping, Linux IRQ, clock,
 * reset, power domain, pad, firmware image, or radio function.  Its caller
 * supplies one acquired PIO lease, one exclusive long-lived CONSYS lifecycle
 * lease, and IRQ enable/close callbacks after those separate contracts have
 * been established.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_BTIF_STP_BOOTSTRAP_H
#define __LINUX_SOC_MEDIATEK_MT6797_BTIF_STP_BOOTSTRAP_H

#include <linux/types.h>

struct mt6797_btif_pio_lease;
struct mt6797_consys_runtime_lease;
struct mt6797_btif_stp_bootstrap;
struct mt6797_consys_stp_session_lease;
struct mt6797_btif_stp_bootstrap_full_wmt_lease;
struct mtk_wmt_match;

/*
 * enable_irq() is called only after PIO mandatory setup installed its RX
 * consumer; it must make an already-requested IRQ deliverable. close() must
 * synchronously mask that Linux IRQ before fencing the associated PIO lease.
 */
struct mt6797_btif_stp_bootstrap_ops {
	int (*enable_irq)(void *context);
	void (*close)(void *context);
	void *context;
};

/*
 * A post-full stage runs synchronously between the exact full-STP query and
 * session promotion.  The opaque lease has no constructor or endpoint getter:
 * it is valid only for this callback and exposes one serialized WMT operation
 * owned by the bootstrap.
 */
typedef int (*mt6797_btif_stp_bootstrap_full_wmt_stage_t)(void *context,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease);

/* True only while the bootstrap is synchronously executing that stage. */
bool mt6797_btif_stp_bootstrap_full_wmt_lease_active(
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease);

struct mt6797_btif_stp_bootstrap *
mt6797_btif_stp_bootstrap_create(
		const struct mt6797_btif_pio_lease *pio_lease,
		const struct mt6797_consys_runtime_lease *runtime_lease,
		const struct mt6797_btif_stp_bootstrap_ops *ops);

/*
 * Run exactly raw HCR/HVR/FVR identity reads, query-default, set-full, host
 * handoff, 10-ms wait, and query-full.  It is rejected unless the default-false
 * enable_experimental_stp_bootstrap parameter was set.  On every error it
 * leaves the supplied transport closed/fenced and returns an error pointer.
 */
const struct mt6797_consys_stp_session_lease *
mt6797_btif_stp_bootstrap_run(struct mt6797_btif_stp_bootstrap *bootstrap);

/*
 * As mt6797_btif_stp_bootstrap_run(), but runs @stage after the exact
 * full-STP query has succeeded and before the full session is published.  The
 * stage must neither retain @lease nor issue traffic except through
 * mt6797_btif_stp_bootstrap_full_wmt_transaction().  A stage error fences the
 * PIO/STP owner and prevents promotion.
 */
const struct mt6797_consys_stp_session_lease *
mt6797_btif_stp_bootstrap_run_with_full_wmt_stage(
		struct mt6797_btif_stp_bootstrap *bootstrap,
		mt6797_btif_stp_bootstrap_full_wmt_stage_t stage,
		void *stage_context);

/*
 * Send one exact WMT operation through the bootstrap's private full-STP WMT
 * endpoint.  This is valid only while its synchronous post-full stage owns
 * @lease.  The bootstrap retains the STP window, ACK, retry and recovery
 * lifecycle; callers cannot obtain or reset the underlying struct mtk_wmt.
 */
int mt6797_btif_stp_bootstrap_full_wmt_transaction(
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		u8 opcode, const u8 *data, size_t data_len,
		const struct mtk_wmt_match *expected, unsigned long timeout);

/* Capture a variable WMT event SDU without exposing the private endpoint. */
int mt6797_btif_stp_bootstrap_full_wmt_transaction_capture(
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		u8 opcode, const u8 *data, size_t data_len,
		u8 expected_opcode, size_t expected_data_len,
		u8 *reply, size_t reply_len, unsigned long timeout);

/*
 * The returned full session remains owned by bootstrap until destroy().
 * destroy() prevents another run and waits for an in-progress run to finish
 * before freeing the controller; its supplied close() callback must not call
 * destroy() recursively.
 */
void mt6797_btif_stp_bootstrap_destroy(
		struct mt6797_btif_stp_bootstrap *bootstrap);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_BTIF_STP_BOOTSTRAP_H */
