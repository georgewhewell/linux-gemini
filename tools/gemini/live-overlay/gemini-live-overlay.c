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
		return ret;
	}

	pr_info("gemini-live-overlay: applied %s as overlay %d\n",
		firmware, overlay_id);
	return 0;
}

static void __exit gemini_live_overlay_exit(void)
{
	int removed_id = overlay_id;
	int ret;

	ret = of_overlay_remove(&overlay_id);
	if (ret)
		pr_err("gemini-live-overlay: cannot remove overlay %d: %d\n",
		       removed_id, ret);
	else
		pr_info("gemini-live-overlay: removed overlay %d\n", removed_id);
}

module_init(gemini_live_overlay_init);
module_exit(gemini_live_overlay_exit);

MODULE_AUTHOR("Gemini PDA mainline bring-up project");
MODULE_DESCRIPTION("Runtime device-tree overlay loader for Gemini bring-up");
MODULE_LICENSE("GPL");
