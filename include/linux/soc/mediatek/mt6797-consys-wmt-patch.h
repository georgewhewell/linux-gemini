/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Bounded MT6797 WMT patch-download controller.
 *
 * This interface deliberately accepts only bootstrap's already-owned opaque
 * WMT transaction lease. It does not discover BTIF, CONSYS, Device Tree or
 * power resources, and it cannot establish a transport by itself.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_PATCH_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_PATCH_H

struct mt6797_consys_fw_bundle;
struct mt6797_btif_stp_bootstrap_full_wmt_lease;

/*
 * Consume the canonical owner's already requested/hash-pinned Gemini bundle,
 * then issue only the vendor-evidenced power-on DLM, MT6797 MCU-clock,
 * patch-address, patch-data, reset, and clock-down WMT transactions.
 * @lease is an opaque capability supplied only to the bootstrap's synchronous
 * post-full callback; it proves one owner retains the full STP window, ACK,
 * retry and recovery lifecycle for this call.
 */
int
mt6797_consys_wmt_patch_download(const struct mt6797_consys_fw_bundle *bundle,
				 const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_PATCH_H */
