/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 BTIF PIO platform acquisition adapter.
 *
 * This interface has no discovery helper. A future controller must hold the
 * platform device lifetime and acquire this opaque lease after a separate
 * owner has established the missing CONSYS power, reset, wake, firmware and
 * mandatory-to-full-STP handover contracts.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_BTIF_PIO_PLATFORM_H
#define __LINUX_SOC_MEDIATEK_MT6797_BTIF_PIO_PLATFORM_H

struct device;
struct mt6797_btif_pio_platform_lease;
struct mt6797_consys_runtime_lease;
struct mt6797_consys_stp_session_lease;

/*
 * Reserve the single platform/PIO owner associated with dev. The caller must
 * serialize this with device removal and retain dev's platform-driver binding.
 * This performs no register access and does not enable the installed IRQ.
 */
const struct mt6797_btif_pio_platform_lease *
mt6797_btif_pio_platform_acquire(struct device *dev);

/*
 * Bind one already-active full-STP session, initialize the vendor PIO subset,
 * then enable the previously installed threaded IRQ. runtime_lease is pinned
 * across that live PIO interval and must be the exclusive long-lived CONSYS
 * lifecycle owner. This remains rejected unless the separate
 * enable_experimental_platform_adapter parameter was set.
 */
int mt6797_btif_pio_platform_attach_full_stp(
		const struct mt6797_btif_pio_platform_lease *platform_lease,
		const struct mt6797_consys_runtime_lease *runtime_lease,
		const struct mt6797_consys_stp_session_lease *stp_lease);

/*
 * Synchronously mask the Linux IRQ before PIO fencing. This may sleep and
 * cannot be called from the adapter's threaded IRQ. The lease stays fenced;
 * recovery requires a new platform instance and full-STP session.
 */
void mt6797_btif_pio_platform_close(
		const struct mt6797_btif_pio_platform_lease *platform_lease);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_BTIF_PIO_PLATFORM_H */
