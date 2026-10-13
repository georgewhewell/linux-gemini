/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD3_SERVICES_H
#define __MT6797_MD3_SERVICES_H
#include <linux/types.h>
#include <linux/kconfig.h>
#include <linux/errno.h>
#include <linux/sysfs.h>
struct platform_device;
struct mt6797_md3_ccif;
struct mt6797_md3_execution;
struct mt6797_md3_services;
#if IS_ENABLED(CONFIG_MTK_MT6797_MD3_SERVICES)
int mt6797_md3_services_acquire(struct platform_device *pdev,
				struct mt6797_md3_ccif *ccif,
				struct mt6797_md3_execution *execution,
				struct mt6797_md3_services **out);
int mt6797_md3_services_claim(struct mt6797_md3_services *services);
int mt6797_md3_services_start(struct mt6797_md3_services *services);
void mt6797_md3_services_shutdown(struct mt6797_md3_services *services);
ssize_t mt6797_md3_services_show(struct mt6797_md3_services *services,
				 char *buf);
#else
static inline int mt6797_md3_services_acquire(struct platform_device *p,
					      struct mt6797_md3_ccif *c,
					      struct mt6797_md3_execution *e,
					      struct mt6797_md3_services **out)
{
	return -EOPNOTSUPP;
}
static inline int mt6797_md3_services_claim(struct mt6797_md3_services *s)
{
	return -EOPNOTSUPP;
}
static inline int mt6797_md3_services_start(struct mt6797_md3_services *s)
{
	return -EOPNOTSUPP;
}
static inline void mt6797_md3_services_shutdown(struct mt6797_md3_services *s)
{
}
static inline ssize_t mt6797_md3_services_show(struct mt6797_md3_services *s,
					       char *b)
{
	return sysfs_emit(b, "requested=0 runtime_sent=0 hs2=0 css_ready=0\n");
}
#endif
#endif
