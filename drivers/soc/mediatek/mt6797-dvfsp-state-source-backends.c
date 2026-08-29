// SPDX-License-Identifier: GPL-2.0-only
/*
 * Connect the reviewed MT6797 readback transports to the state-source seam.
 *
 * This file deliberately has no platform driver and retains no device
 * references. The real owner supplies the device tuple and transition lock;
 * calibration and live-state ownership remain separate mandatory callbacks.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/nvmem-consumer.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mt6797-bigidvfs-backend.h>
#include <linux/soc/mediatek/mt6797-dvfsp-clock-backend.h>
#include <linux/soc/mediatek/mt6797-dvfsp-state-source-backends.h>
#include <linux/soc/mediatek/mt6797-eem-readback.h>

int mt6797_dvfsp_state_source_backend_read_clock(
	void *context, struct mt6797_dvfsp_clock_readback *readback)
{
	const struct mt6797_dvfsp_state_source_devices *devices = context;

	if (!devices || !devices->clock_backend || !readback)
		return -EINVAL;
#if IS_ENABLED(CONFIG_MTK_MT6797_DVFSP_CLOCK_BACKEND)
	return mt6797_dvfsp_clock_backend_read(devices->clock_backend,
						readback);
#else
	return -ENODEV;
#endif
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_source_backend_read_clock);

int mt6797_dvfsp_state_source_backend_read_big(
	void *context, struct mt6797_bigidvfs_readback *readback)
{
	const struct mt6797_dvfsp_state_source_devices *devices = context;

	if (!devices || !devices->bigidvfs_backend || !readback)
		return -EINVAL;
#if IS_ENABLED(CONFIG_MTK_MT6797_DVFSP_BIGIDVFS_BACKEND)
	return mt6797_bigidvfs_backend_read(devices->bigidvfs_backend,
						readback);
#else
	return -ENODEV;
#endif
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_source_backend_read_big);

int mt6797_dvfsp_state_source_backend_read_eem(
	void *context, struct mt6797_eem_readback *readback)
{
	const struct mt6797_dvfsp_state_source_devices *devices = context;

	if (!devices || !devices->thermal || !readback)
		return -EINVAL;
	return mt6797_thermal_eem_readback(devices->thermal, readback);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_source_backend_read_eem);

int mt6797_dvfsp_state_source_backend_read_ptp(
	void *context, struct mt6797_dvfsp_ptp_handoff *handoff)
{
	const struct mt6797_dvfsp_state_source_devices *devices = context;
	struct nvmem_cell *cell;
	void *buffer;
	size_t length;
	unsigned int i;

	if (!devices || !devices->calibration_source || !handoff)
		return -EINVAL;
#if !IS_ENABLED(CONFIG_NVMEM_MTK_ATAG_DEVINFO)
	return -ENODEV;
#else
	cell = nvmem_cell_get(devices->calibration_source,
			      "ptp-calibration-data");
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buffer = nvmem_cell_read(cell, &length);
	nvmem_cell_put(cell);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	if (length != MT6797_DVFSP_PTP_HANDOFF_WORDS * sizeof(u32)) {
		kfree(buffer);
		return -EINVAL;
	}

	memset(handoff, 0, sizeof(*handoff));
	handoff->abi = MT6797_DVFSP_PTP_HANDOFF_ABI;
	for (i = 0; i < MT6797_DVFSP_PTP_HANDOFF_WORDS; i++)
		handoff->m_hw_res[i] =
			get_unaligned_le32((u8 *)buffer + i * sizeof(u32));
	kfree(buffer);
	return 0;
#endif
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_source_backend_read_ptp);

int mt6797_dvfsp_state_source_backend_read_ptp_identity(
	void *context, struct mt6797_dvfsp_efuse_identity *identity)
{
	const struct mt6797_dvfsp_state_source_devices *devices = context;
	struct nvmem_cell *cell;
	void *buffer;
	size_t length;
	unsigned int i;
	u32 *words = (u32 *)identity;

	if (!devices || !devices->calibration_source || !identity)
		return -EINVAL;
#if !IS_ENABLED(CONFIG_NVMEM_MTK_ATAG_DEVINFO)
	return -ENODEV;
#else
	cell = nvmem_cell_get(devices->calibration_source,
				     "cpu-efuse-identity");
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buffer = nvmem_cell_read(cell, &length);
	nvmem_cell_put(cell);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	if (length != sizeof(*identity)) {
		kfree(buffer);
		return -EINVAL;
	}

	for (i = 0; i < sizeof(*identity) / sizeof(u32); i++)
		words[i] = get_unaligned_le32((u8 *)buffer + i * sizeof(u32));
	kfree(buffer);
	if (identity->abi != MT6797_DVFSP_EFUSE_IDENTITY_ABI)
		return -EPROTO;
	return 0;
#endif
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_source_backend_read_ptp_identity);

int mt6797_dvfsp_state_source_backend_ops_init(
	const struct mt6797_dvfsp_state_source_devices *devices,
	struct mt6797_dvfsp_state_source_ops *ops)
{
	if (!devices || !ops || !devices->clock_backend ||
	    !devices->bigidvfs_backend || !devices->thermal ||
	    !devices->calibration_source)
		return -EINVAL;

#if !IS_ENABLED(CONFIG_MTK_MT6797_DVFSP_CLOCK_BACKEND) || \
	!IS_ENABLED(CONFIG_MTK_MT6797_DVFSP_BIGIDVFS_BACKEND) || \
	!IS_ENABLED(CONFIG_MTK_SOC_THERMAL) || \
	!IS_ENABLED(CONFIG_NVMEM_MTK_ATAG_DEVINFO)
	return -ENODEV;
#else
	memset(ops, 0, sizeof(*ops));
	ops->abi = MT6797_DVFSP_STATE_SOURCE_ABI;
	ops->read_clock = mt6797_dvfsp_state_source_backend_read_clock;
	ops->read_big = mt6797_dvfsp_state_source_backend_read_big;
	ops->read_eem = mt6797_dvfsp_state_source_backend_read_eem;
	ops->read_ptp = mt6797_dvfsp_state_source_backend_read_ptp;
	return 0;
#endif
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_source_backend_ops_init);

MODULE_DESCRIPTION("MT6797 protected readback source callbacks");
MODULE_LICENSE("GPL");
