// SPDX-License-Identifier: GPL-2.0-only
/*
 * Load one flattened device-tree overlay through the in-kernel overlay API.
 *
 * This deliberately small bring-up helper fills the gap between OF_OVERLAY
 * and systems which do not expose a configfs overlay frontend.  Put a dtbo
 * below firmware_class.path, load this module with firmware=<name>, and
 * unload it to remove the overlay again.
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of.h>

static char *firmware;
module_param(firmware, charp, 0400);
MODULE_PARM_DESC(firmware, "DT overlay firmware name");

static int overlay_id = -1;

static int gemini_live_overlay_remove(const char *value,
				      const struct kernel_param *)
{
	bool requested;
	int removed_id;
	int ret;

	ret = kstrtobool(value, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	if (overlay_id <= 0)
		return -ENOENT;

	removed_id = overlay_id;
	ret = of_overlay_remove(&overlay_id);
	if (overlay_id > 0) {
		pr_err("gemini-live-overlay: cannot remove overlay %d: %d; retry is available\n",
		       removed_id, ret);
		return ret;
	}

	pr_info("gemini-live-overlay: removed overlay %d\n", removed_id);
	module_put(THIS_MODULE);

	return ret;
}

static const struct kernel_param_ops remove_ops = {
	.set = gemini_live_overlay_remove,
};

module_param_cb(remove_overlay, &remove_ops, NULL, 0200);
MODULE_PARM_DESC(remove_overlay, "write 1 to remove the live overlay");

static int __init gemini_live_overlay_init(void)
{
	const struct firmware *fw;
	int remove_ret;
	int ret;

	if (!firmware || !*firmware)
		return -EINVAL;

	ret = request_firmware_direct(&fw, firmware, NULL);
	if (ret) {
		pr_err("gemini-live-overlay: cannot load %s: %d\n",
		       firmware, ret);
		return ret;
	}

	ret = of_overlay_fdt_apply(fw->data, fw->size, &overlay_id, NULL);
	release_firmware(fw);
	if (ret) {
		pr_err("gemini-live-overlay: cannot apply %s: %d (id=%d)\n",
		       firmware, ret, overlay_id);
		if (overlay_id > 0) {
			remove_ret = of_overlay_remove(&overlay_id);
			if (remove_ret)
				pr_err("gemini-live-overlay: partial overlay removal failed: %d\n",
				       remove_ret);
		}
		/*
		 * A partially applied overlay must retain this module so its removal
		 * can be retried through remove_overlay.  Returning an error here
		 * would discard the only owner and its executable cleanup path.
		 */
		if (overlay_id > 0) {
			__module_get(THIS_MODULE);
			return 0;
		}
		return ret;
	}

	/* Refuse rmmod until explicit, successful overlay removal. */
	__module_get(THIS_MODULE);
	pr_info("gemini-live-overlay: applied %s as overlay %d\n",
		firmware, overlay_id);
	return 0;
}

static void __exit gemini_live_overlay_exit(void)
{
	WARN_ON_ONCE(overlay_id > 0);
}

module_init(gemini_live_overlay_init);
module_exit(gemini_live_overlay_exit);

MODULE_AUTHOR("Gemini PDA mainline bring-up project");
MODULE_DESCRIPTION("Runtime device-tree overlay loader for Gemini bring-up");
MODULE_LICENSE("GPL");
