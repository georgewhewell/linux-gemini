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

struct mt6797_btif_pio_lease;
struct mt6797_consys_runtime_lease;
struct mt6797_btif_stp_bootstrap;
struct mt6797_consys_stp_session_lease;

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

struct mt6797_btif_stp_bootstrap *
mt6797_btif_stp_bootstrap_create(
		const struct mt6797_btif_pio_lease *pio_lease,
		const struct mt6797_consys_runtime_lease *runtime_lease,
		const struct mt6797_btif_stp_bootstrap_ops *ops);

/*
 * Run exactly query-default, set-full, host handoff, 10-ms wait, and
 * query-full.  It is rejected unless the default-false
 * enable_experimental_stp_bootstrap parameter was set.  On every error it
 * leaves the supplied transport closed/fenced and returns an error pointer.
 */
const struct mt6797_consys_stp_session_lease *
mt6797_btif_stp_bootstrap_run(struct mt6797_btif_stp_bootstrap *bootstrap);

/*
 * The returned full session remains owned by bootstrap until destroy().
 * destroy() prevents another run and waits for an in-progress run to finish
 * before freeing the controller; its supplied close() callback must not call
 * destroy() recursively.
 */
void mt6797_btif_stp_bootstrap_destroy(
		struct mt6797_btif_stp_bootstrap *bootstrap);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_BTIF_STP_BOOTSTRAP_H */
