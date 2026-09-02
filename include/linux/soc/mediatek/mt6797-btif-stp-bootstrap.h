/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 BTIF mandatory-to-full STP handover controller.
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
 * synchronously mask that Linux IRQ before invalidating the associated lease.
 */
struct mt6797_btif_stp_bootstrap_ops {
	int (*enable_irq)(void *context);
	void (*close)(void *context);
	void *context;
};

/* True while the bootstrap owns an active full-STP WMT endpoint. */
bool mt6797_btif_stp_bootstrap_full_wmt_lease_active(
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease);

struct mt6797_btif_stp_bootstrap *
mt6797_btif_stp_bootstrap_create(
		const struct mt6797_btif_pio_lease *pio_lease,
		const struct mt6797_consys_runtime_lease *runtime_lease,
		const struct mt6797_btif_stp_bootstrap_ops *ops);

/*
 * Run exactly raw HCR/HVR/FVR identity reads, query-default, set-full, host
 * handoff, 10-ms wait, and query-full. On every error it closes the supplied
 * transport and returns an error pointer.
 */
const struct mt6797_consys_stp_session_lease *
mt6797_btif_stp_bootstrap_run(struct mt6797_btif_stp_bootstrap *bootstrap);
/* Return the active bootstrap-owned WMT capability after run() succeeds. */
const struct mt6797_btif_stp_bootstrap_full_wmt_lease *
mt6797_btif_stp_bootstrap_full_wmt_lease(
		struct mt6797_btif_stp_bootstrap *bootstrap);

/*
 * Send one exact WMT operation through the bootstrap's private full-STP WMT
 * endpoint. This is valid only while the bootstrap owns @lease. The bootstrap
 * retains the STP window, ACK, retry and recovery
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

int mt6797_btif_stp_bootstrap_full_wmt_transaction_capture_range(
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		u8 opcode, const u8 *data, size_t data_len,
		u8 expected_opcode, size_t min_data_len, size_t max_data_len,
		u8 *reply, size_t reply_len, size_t *actual_data_len,
		unsigned long timeout);

/*
 * The returned full session remains owned by bootstrap until destroy().
 * destroy() prevents another run and waits for an in-progress run to finish
 * before freeing the controller; its supplied close() callback must not call
 * destroy() recursively.
 */
void mt6797_btif_stp_bootstrap_destroy(
		struct mt6797_btif_stp_bootstrap *bootstrap);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_BTIF_STP_BOOTSTRAP_H */
