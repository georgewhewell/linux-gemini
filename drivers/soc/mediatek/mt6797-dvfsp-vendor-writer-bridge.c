// SPDX-License-Identifier: GPL-2.0-only
/*
 * Source-independent owner bridge for the MT6797 vendor writer.
 *
 * The callback table is consumed by an external vendor-side binding. This
 * file only delegates to the already-bound mainline writer and never calls a
 * vendor setter or performs hardware I/O.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-writer-bridge.h>

static bool mt6797_dvfsp_vendor_writer_bridge_site_valid(unsigned int site)
{
	return site < MT6797_DVFSP_VENDOR_WRITER_SITE_COUNT;
}

static int mt6797_dvfsp_vendor_writer_bridge_begin(
	void *context, unsigned int site, u64 *generation)
{
	struct mt6797_dvfsp_vendor_writer_bridge *bridge = context;
	int ret;

	if (!bridge || !bridge->writer || !generation ||
	    !mt6797_dvfsp_vendor_writer_bridge_site_valid(site) ||
	    bridge->active)
		return -EINVAL;

	ret = mt6797_dvfsp_vendor_writer_begin(bridge->writer, generation);
	if (ret)
		return ret;

	bridge->active_site = site;
	bridge->active_generation = *generation;
	bridge->active = true;
	return 0;
}

static int mt6797_dvfsp_vendor_writer_bridge_commit(
	void *context, unsigned int site, u64 generation)
{
	struct mt6797_dvfsp_vendor_writer_bridge *bridge = context;

	if (!bridge || !bridge->active || bridge->active_site != site ||
	    bridge->active_generation != generation)
		return -ESTALE;

	{
		int ret = mt6797_dvfsp_vendor_writer_commit(bridge->writer);

		if (ret)
			return ret;
	}

	bridge->active = false;
	return 0;
}

static int mt6797_dvfsp_vendor_writer_bridge_abort(
	void *context, unsigned int site, u64 generation)
{
	struct mt6797_dvfsp_vendor_writer_bridge *bridge = context;
	bool identity_match;
	int ret;

	if (!bridge || !bridge->active)
		return -EINVAL;

	identity_match = bridge->active_site == site &&
		bridge->active_generation == generation;
	ret = mt6797_dvfsp_vendor_writer_abort(bridge->writer);
	if (ret)
		return ret;

	bridge->active = false;
	return identity_match ? 0 : -ESTALE;
}

static int mt6797_dvfsp_vendor_writer_bridge_read_identity(
	void *context, u64 *owner_handle, u64 *transition_handle)
{
	struct mt6797_dvfsp_vendor_writer_bridge *bridge = context;

	if (!bridge || !bridge->writer || !owner_handle ||
	    !transition_handle || !bridge->writer->initialized ||
	    !bridge->writer->owner_handle ||
	    !bridge->writer->transition_handle)
		return -EINVAL;

	*owner_handle = bridge->writer->owner_handle;
	*transition_handle = bridge->writer->transition_handle;
	return 0;
}

static const struct mt6797_dvfsp_vendor_writer_bridge_ops
mt6797_dvfsp_vendor_writer_bridge_callbacks = {
	.abi = MT6797_DVFSP_VENDOR_WRITER_BRIDGE_ABI,
	.begin = mt6797_dvfsp_vendor_writer_bridge_begin,
	.commit = mt6797_dvfsp_vendor_writer_bridge_commit,
	.abort = mt6797_dvfsp_vendor_writer_bridge_abort,
	.read_identity = mt6797_dvfsp_vendor_writer_bridge_read_identity,
};

int mt6797_dvfsp_vendor_writer_bridge_init(
	struct mt6797_dvfsp_vendor_writer_bridge *bridge,
	struct mt6797_dvfsp_vendor_writer *writer)
{
	if (!bridge || !writer || !writer->initialized ||
	    !writer->transition_lock || !writer->generation ||
	    !writer->owner_handle || !writer->transition_handle)
		return -EINVAL;

	memset(bridge, 0, sizeof(*bridge));
	bridge->writer = writer;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_writer_bridge_init);

const struct mt6797_dvfsp_vendor_writer_bridge_ops *
mt6797_dvfsp_vendor_writer_bridge_ops(void)
{
	return &mt6797_dvfsp_vendor_writer_bridge_callbacks;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_writer_bridge_ops);

MODULE_DESCRIPTION("MT6797 vendor writer owner bridge");
MODULE_LICENSE("GPL");
