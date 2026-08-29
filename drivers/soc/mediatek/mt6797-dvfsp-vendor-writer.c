// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared transition owner for a cooperative MT6797 vendor writer.
 *
 * The caller at the vendor PTP, voltage, and PPM writers owns the legacy
 * callback slots. It must hold this lock while chaining those existing
 * callbacks, and must commit only after the complete update is published.
 * This file never calls a vendor setter and performs no hardware I/O.
 */

#include <linux/errno.h>
#include <linux/irqflags.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-writer.h>

static int mt6797_dvfsp_vendor_writer_check(
	const struct mt6797_dvfsp_vendor_writer *writer)
{
	if (!writer || !writer->initialized || !writer->transition_lock ||
	    !writer->generation ||
	    writer->ops.abi != MT6797_DVFSP_VENDOR_WRITER_ABI ||
	    !writer->ops.read_provenance || !writer->owner_handle ||
	    !writer->transition_handle)
		return -EINVAL;

	return 0;
}

int mt6797_dvfsp_vendor_writer_init(
	struct mt6797_dvfsp_vendor_writer *writer,
	struct mutex *transition_lock,
	u64 *generation,
	const struct mt6797_dvfsp_vendor_writer_ops *ops,
	void *context, u64 owner_handle, u64 transition_handle)
{
	if (!writer || !transition_lock || !generation || !ops ||
	    ops->abi != MT6797_DVFSP_VENDOR_WRITER_ABI ||
	    !ops->read_provenance || !owner_handle || !transition_handle)
		return -EINVAL;

	memset(writer, 0, sizeof(*writer));
	writer->transition_lock = transition_lock;
	writer->ops = *ops;
	writer->context = context;
	writer->generation = generation;
	writer->owner_handle = owner_handle;
	writer->transition_handle = transition_handle;
	writer->initialized = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_writer_init);

int mt6797_dvfsp_vendor_writer_begin(
	struct mt6797_dvfsp_vendor_writer *writer, u64 *generation)
{
	int ret = mt6797_dvfsp_vendor_writer_check(writer);

	if (ret || !generation)
		return ret ? ret : -EINVAL;
	if (in_atomic() || irqs_disabled())
		return -EWOULDBLOCK;
	if (READ_ONCE(writer->update_held))
		return -EALREADY;

	mutex_lock(writer->transition_lock);
	if (writer->update_held) {
		mutex_unlock(writer->transition_lock);
		return -EALREADY;
	}
	if (READ_ONCE(*writer->generation) == ~0ULL) {
		mutex_unlock(writer->transition_lock);
		return -EOVERFLOW;
	}

	writer->update_held = true;
	*generation = *writer->generation;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_writer_begin);

int mt6797_dvfsp_vendor_writer_run(
	struct mt6797_dvfsp_vendor_writer *writer,
	mt6797_dvfsp_vendor_writer_update_fn update, void *context)
{
	u64 generation;
	int ret;
	int abort_ret;

	if (!update)
		return -EINVAL;

	ret = mt6797_dvfsp_vendor_writer_begin(writer, &generation);
	if (ret)
		return ret;

	ret = update(context, generation);
	if (ret) {
		abort_ret = mt6797_dvfsp_vendor_writer_abort(writer);
		return abort_ret ? abort_ret : ret;
	}

	ret = mt6797_dvfsp_vendor_writer_commit(writer);
	if (!ret)
		return 0;

	abort_ret = mt6797_dvfsp_vendor_writer_abort(writer);
	return abort_ret ? abort_ret : ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_writer_run);

int mt6797_dvfsp_vendor_writer_run_site(
	struct mt6797_dvfsp_vendor_writer *writer,
	const struct mt6797_dvfsp_vendor_writer_site *site)
{
	if (!site || site->abi != MT6797_DVFSP_VENDOR_WRITER_SITE_ABI ||
	    (unsigned int)site->id >= MT6797_DVFSP_VENDOR_WRITER_SITE_COUNT ||
	    !site->update)
		return -EINVAL;

	return mt6797_dvfsp_vendor_writer_run(writer, site->update,
					       site->context);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_writer_run_site);

int mt6797_dvfsp_vendor_writer_commit(
	struct mt6797_dvfsp_vendor_writer *writer)
{
	int ret = mt6797_dvfsp_vendor_writer_check(writer);

	if (ret)
		return ret;
	if (!writer->update_held)
		return -EINVAL;
	if (*writer->generation == ~0ULL)
		return -EOVERFLOW;

	(*writer->generation)++;
	writer->update_held = false;
	mutex_unlock(writer->transition_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_writer_commit);

int mt6797_dvfsp_vendor_writer_abort(
	struct mt6797_dvfsp_vendor_writer *writer)
{
	int ret = mt6797_dvfsp_vendor_writer_check(writer);

	if (ret)
		return ret;
	if (!writer->update_held)
		return -EINVAL;

	writer->update_held = false;
	mutex_unlock(writer->transition_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_writer_abort);

int mt6797_dvfsp_vendor_writer_read_generation(
	void *context, const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm, u64 *generation,
	u64 *owner_handle, u64 *transition_handle,
	struct mt6797_dvfsp_state_provenance *provenance)
{
	struct mt6797_dvfsp_vendor_writer *writer = context;
	int ret;

	if (!clock || !calibration || !cspm || !ppm || !generation ||
	    !owner_handle || !transition_handle || !provenance)
		return -EINVAL;
	ret = mt6797_dvfsp_vendor_writer_check(writer);
	if (ret)
		return ret;

	memset(provenance, 0, sizeof(*provenance));
	ret = writer->ops.read_provenance(writer->context, provenance);
	if (ret)
		return ret;
	if (provenance->abi != MT6797_DVFSP_STATE_PROVENANCE_ABI ||
	    provenance->source_mask != MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL ||
	    !provenance->variant_id || !provenance->table_epoch ||
	    !provenance->calibration_handle)
		return -EPROTO;

	*generation = READ_ONCE(*writer->generation);
	*owner_handle = writer->owner_handle;
	*transition_handle = writer->transition_handle;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_writer_read_generation);

MODULE_DESCRIPTION("MT6797 cooperative vendor writer transition owner");
MODULE_LICENSE("GPL");
