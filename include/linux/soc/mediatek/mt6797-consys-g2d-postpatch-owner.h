/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6797 G2d true-chronology identity handoff for the W4 HIF consumer. */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_G2D_POSTPATCH_OWNER_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_G2D_POSTPATCH_OWNER_H

struct device;

/* The W4 consumer may start only after G2d has retained post-ID identity. */
int mt6797_consys_g2d_postpatch_owner_w4_validate(struct device *dev);

/* Called after W4 has stopped/fenced its controller and HIF session. */
int mt6797_consys_g2d_postpatch_owner_w4_release(struct device *dev);

/* Attach/detach the G2g channel-2 endpoint to W4's active full-STP session. */
int mt6797_consys_g2d_postpatch_owner_gnss_attach(struct device *dev);
void mt6797_consys_g2d_postpatch_owner_gnss_detach(struct device *dev);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_G2D_POSTPATCH_OWNER_H */
