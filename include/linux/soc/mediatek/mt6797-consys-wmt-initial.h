/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797's mandatory-STP WMT identity and chip-side mode preflight.
 *
 * This is deliberately not a platform, power, firmware, or radio API.  A
 * future single transport owner may use it only after it owns the mandatory
 * STP endpoint, its WMT receive callback, and recovery lifecycle.  It must
 * not run independently beside mt6797-btif-stp-bootstrap: that exclusive PIO
 * owner consumes the identity/default/set stages exactly once before its
 * host-handoff/full-query tail.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_INITIAL_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_INITIAL_H

#include <linux/types.h>

struct mtk_wmt;
struct mt6797_consys_wmt_initial;

/*
 * Exchange an exact raw WMT identity-read packet.  The callback receives no
 * operation-controlled address or data; this controller supplies the three
 * vendor-evidenced HCR, HVR, and FVR reads in its fixed order.  On success it
 * must provide the complete raw event and its exact size.  The caller owns all
 * STP framing, byte I/O, timeout and recovery mechanics underneath this
 * endpoint.
 */
struct mt6797_consys_wmt_initial_ops {
	int (*raw_exchange)(void *context, const u8 *command, size_t command_len,
			    u8 *event, size_t event_capacity, size_t *event_len,
			    unsigned long timeout);
	void *context;
};

/* Values are masked by the vendor GEN_HCR_MASK or GEN_VER_MASK respectively. */
struct mt6797_consys_wmt_initial_identity {
	u16 chip_id;
	u16 hardware_version;
	u16 firmware_version;
};

struct mt6797_consys_wmt_initial *
mt6797_consys_wmt_initial_create(struct mtk_wmt *wmt,
				 const struct mt6797_consys_wmt_initial_ops *ops);
void mt6797_consys_wmt_initial_destroy(struct mt6797_consys_wmt_initial *initial);

/*
 * Run HCR, HVR, and FVR verification followed by the exact mandatory-STP
 * default-query. identity reports the masked values obtained before the
 * query. On any error the controller instance becomes unusable; its owner
 * must recover and instantiate a new endpoint before trying again.
 */
int mt6797_consys_wmt_initial_run(struct mt6797_consys_wmt_initial *initial,
				  struct mt6797_consys_wmt_initial_identity *identity,
				  unsigned long timeout);

/*
 * Send only the vendor's chip-side set-full WMT command after a successful
 * run(). The local endpoint remains mandatory: this neither changes host STP
 * framing nor delays, performs a full-mode query, creates a full-session
 * lease, or authorizes any subsequent firmware or radio operation. A future
 * single owner must perform those remaining handoff steps exactly once. This
 * call is valid only after run() succeeds; a malformed, timed-out, or
 * out-of-order call permanently fails the instance.
 */
int mt6797_consys_wmt_initial_configure_chip_full(
		struct mt6797_consys_wmt_initial *initial, unsigned long timeout);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_INITIAL_H */
