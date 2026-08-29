// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bind read-only MT6797 efuse/PTP identity to owner provenance.
 *
 * The table epoch and calibration handle remain explicit provider callbacks;
 * this bridge only decodes the selected variant and validates provenance.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-identity-source.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ptp.h>

static int mt6797_dvfsp_identity_source_check(
	const struct mt6797_dvfsp_identity_source *source,
	const struct mt6797_dvfsp_ptp_state *ptp_state)
{
	if (!source || !ptp_state ||
	    ptp_state->abi != MT6797_DVFSP_PTP_STATE_ABI ||
	    ptp_state->bank_mask != MT6797_DVFSP_PTP_BANK_MASK_ALL ||
	    !source->devices.calibration_source || !source->provenance_ops ||
	    source->provenance_ops->abi != MT6797_DVFSP_IDENTITY_SOURCE_ABI ||
	    !source->provenance_ops->read_table_epoch ||
	    !source->provenance_ops->read_calibration_handle)
		return -EINVAL;

	return 0;
}

int mt6797_dvfsp_identity_source_read(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	struct mt6797_dvfsp_state_owner_source_identity *identity)
{
	struct mt6797_dvfsp_identity_source *source = context;
	struct mt6797_dvfsp_efuse_identity raw;
	struct mt6797_dvfsp_ptp_identity decoded;
	u64 table_epoch;
	u64 calibration_handle;
	int ret;

	if (!identity)
		return -EINVAL;
	ret = mt6797_dvfsp_identity_source_check(source, ptp_state);
	if (ret)
		return ret;

	memset(&raw, 0, sizeof(raw));
	ret = mt6797_dvfsp_state_source_backend_read_ptp_identity(
		&source->devices, &raw);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_ptp_identity_decode(&raw, &decoded);
	if (ret || !decoded.variant_id)
		return ret ? ret : -EPROTO;

	table_epoch = 0;
	ret = source->provenance_ops->read_table_epoch(
		source->provenance_context, &decoded, &table_epoch);
	if (ret || !table_epoch)
		return ret ? ret : -EPROTO;

	calibration_handle = 0;
	ret = source->provenance_ops->read_calibration_handle(
		source->provenance_context, &decoded, &calibration_handle);
	if (ret || !calibration_handle)
		return ret ? ret : -EPROTO;

	memset(identity, 0, sizeof(*identity));
	identity->abi = MT6797_DVFSP_STATE_OWNER_SOURCE_ABI;
	identity->variant_id = decoded.variant_id;
	identity->table_epoch = table_epoch;
	identity->calibration_handle = calibration_handle;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_identity_source_read);

MODULE_DESCRIPTION("MT6797 read-only efuse identity source bridge");
MODULE_LICENSE("GPL");
