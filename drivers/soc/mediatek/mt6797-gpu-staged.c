// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded MT6797 Mali-T880 rail, MFG-domain and ID diagnostic.
 *
 * This is intentionally not a DRM driver.  Stage 2 reads only registers that
 * Panfrost describes as read-only, and every stage that changes hardware
 * drops its own vote before probe returns.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#define MT6797_GPU_BASE			0x13040000
#define MT6797_GPU_SIZE			SZ_16K
#define MT6797_GPU_VOLTAGE_UV		812500
#define MT6797_SPM_PWR_STATUS		0x180
#define MT6797_SPM_PWR_STATUS_2ND	0x184
#define MT6797_MFG_STATUS_MASK		GENMASK(13, 8)

/* Read-only Mali Midgard GPU-control registers; see panfrost_regs.h. */
#define MT6797_GPU_ID			0x000
#define MT6797_GPU_L2_FEATURES		0x004
#define MT6797_GPU_CORE_FEATURES	0x008
#define MT6797_GPU_TILER_FEATURES	0x00c
#define MT6797_GPU_MEM_FEATURES		0x010
#define MT6797_GPU_MMU_FEATURES		0x014
#define MT6797_GPU_SHADER_PRESENT_LO	0x100

#define MT6797_GPU_MAX_STAGE		2
#define MT6797_GPU_READONLY_STAGE	-1
#define MT6797_GPU_DEFAULT_TIMEOUT_MS	1000
#define MT6797_GPU_MAX_TIMEOUT_MS	10000
#define MT6797_GPU_DEVINFO_WORDS	3

#define MT6797_GPU_SPEED_WORD		0
#define MT6797_GPU_FUNCTION_WORD	1
#define MT6797_GPU_PTP_WORD		2

#define MT6797_GPU_SPEED_SHIFT		8
#define MT6797_GPU_SPEED_MASK		GENMASK(3, 0)
#define MT6797_GPU_FUNCTION0_SHIFT	24
#define MT6797_GPU_FUNCTION_MASK	GENMASK(3, 0)
#define MT6797_GPU_PTP_SHIFT		4
#define MT6797_GPU_PTP_MASK		GENMASK(3, 0)

static bool enable_experimental_stages;
module_param_named(enable_experimental_stages, enable_experimental_stages, bool, 0400);
MODULE_PARM_DESC(enable_experimental_stages,
		 "allow the MT6797 GPU staged diagnostic");

static int stage = -1;
module_param(stage, int, 0400);
MODULE_PARM_DESC(stage,
		 "highest staged diagnostic gate to run (-1=devinfo/clock capture, 0=rail ID, 1=MFG, 2=GPU ID)");

static unsigned int max_stage_ms = MT6797_GPU_DEFAULT_TIMEOUT_MS;
module_param(max_stage_ms, uint, 0400);
MODULE_PARM_DESC(max_stage_ms, "maximum elapsed time accepted after each GPU diagnostic stage");

struct mt6797_gpu_staged {
	struct device *dev;
	struct regulator *vgpu;
	struct regmap *spm;
	struct clk *core_clk;
	struct device *domain_devs[4];
	struct device_link *domain_links[4];
	void __iomem *gpu;
	int original_vgpu_uv;
	bool vgpu_vote;
	bool vgpu_voltage_set;
	bool runtime_enabled;
	bool domain_vote;
	bool core_clk_vote;
};

static const char * const mt6797_gpu_domain_names[] = {
	"core0", "core1", "core2", "core3",
};

static unsigned int mt6797_gpu_staged_vendor_table_type(u32 function_word,
						 u32 ptp_word)
{
	unsigned int function_code = function_word & MT6797_GPU_FUNCTION_MASK;
	unsigned int ptp_version = (ptp_word >> MT6797_GPU_PTP_SHIFT) &
				   MT6797_GPU_PTP_MASK;
	unsigned int type;

	if (function_code == 0 || function_code == 3)
		type = 0;
	else if (function_code == 1 || function_code == 6)
		type = 1;
	else if (function_code == 2 || function_code == 7)
		type = 2;
	else
		type = 0;

	if (ptp_version <= 5)
		type += 4;
	if (ptp_version > 6)
		type += 8;
	if (function_code == 0xf)
		type = 11;
	else if (function_code == 4)
		type = 12;

	return type;
}

static int mt6797_gpu_staged_read_devinfo(struct mt6797_gpu_staged *diag)
{
	struct nvmem_cell *cell;
	void *buffer;
	size_t length;
	u32 speed_word;
	u32 function_word;
	u32 ptp_word;
	unsigned int speed_bin;
	unsigned int function0;
	unsigned int function1;
	unsigned int ptp_version;

	cell = devm_nvmem_cell_get(diag->dev, "gpu-devinfo");
	if (IS_ERR(cell))
		return dev_err_probe(diag->dev, PTR_ERR(cell),
				     "missing GPU devinfo cell\n");

	buffer = nvmem_cell_read(cell, &length);
	if (IS_ERR(buffer))
		return dev_err_probe(diag->dev, PTR_ERR(buffer),
				     "failed to read GPU devinfo\n");
	if (length != MT6797_GPU_DEVINFO_WORDS * sizeof(u32)) {
		kfree(buffer);
		return dev_err_probe(diag->dev, -EINVAL,
				     "invalid GPU devinfo length %zu\n", length);
	}

	speed_word = get_unaligned_le32((u8 *)buffer +
					MT6797_GPU_SPEED_WORD * sizeof(u32));
	function_word = get_unaligned_le32((u8 *)buffer +
					   MT6797_GPU_FUNCTION_WORD * sizeof(u32));
	ptp_word = get_unaligned_le32((u8 *)buffer +
					      MT6797_GPU_PTP_WORD * sizeof(u32));
	kfree(buffer);

	speed_bin = (speed_word >> MT6797_GPU_SPEED_SHIFT) & MT6797_GPU_SPEED_MASK;
	function0 = (function_word >> MT6797_GPU_FUNCTION0_SHIFT) &
			    MT6797_GPU_FUNCTION_MASK;
	function1 = function_word & MT6797_GPU_FUNCTION_MASK;
	ptp_version = (ptp_word >> MT6797_GPU_PTP_SHIFT) & MT6797_GPU_PTP_MASK;

	dev_info(diag->dev,
		 "read-only GPU devinfo: word8=%08x word22=%08x word61=%08x speed-bin=%x function=%x/%x ptp-version=%x vendor-table=%u\n",
		 speed_word, function_word, ptp_word, speed_bin, function0, function1,
		 ptp_version,
		 mt6797_gpu_staged_vendor_table_type(function_word, ptp_word));
	return 0;
}

static int mt6797_gpu_staged_log_clock(struct mt6797_gpu_staged *diag,
					 const char *id)
{
	struct clk *clk;
	struct clk *parent;

	clk = devm_clk_get(diag->dev, id);
	if (IS_ERR(clk))
		return dev_err_probe(diag->dev, PTR_ERR(clk),
				     "failed to get %s clock\n", id);

	parent = clk_get_parent(clk);
	dev_info(diag->dev,
		 "read-only clock: %s=%luHz parent=%s%s%luHz\n", id,
		 clk_get_rate(clk), parent ? __clk_get_name(parent) : "(none)",
		 parent ? "@" : "", parent ? clk_get_rate(parent) : 0UL);
	return 0;
}

static int mt6797_gpu_staged_read_only_capture(struct mt6797_gpu_staged *diag)
{
	int ret;

	ret = mt6797_gpu_staged_read_devinfo(diag);
	if (ret)
		return ret;
	ret = mt6797_gpu_staged_log_clock(diag, "mfg-sel");
	if (ret)
		return ret;
	return mt6797_gpu_staged_log_clock(diag, "mfgpll");
}

static int mt6797_gpu_staged_check_timeout(struct mt6797_gpu_staged *diag,
					    ktime_t started, const char *name)
{
	s64 elapsed = ktime_ms_delta(ktime_get(), started);

	if (elapsed <= max_stage_ms)
		return 0;

	return dev_err_probe(diag->dev, -ETIMEDOUT,
				     "%s exceeded %u ms (%lld ms)\n", name,
				     max_stage_ms, elapsed);
}

static int mt6797_gpu_staged_attach_domains(struct mt6797_gpu_staged *diag)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(diag->domain_devs); i++) {
		diag->domain_devs[i] = dev_pm_domain_attach_by_name(diag->dev,
							      mt6797_gpu_domain_names[i]);
		if (IS_ERR_OR_NULL(diag->domain_devs[i])) {
			int ret = PTR_ERR(diag->domain_devs[i]) ? : -ENODATA;

			diag->domain_devs[i] = NULL;
			return dev_err_probe(diag->dev, ret, "failed to attach %s domain\n",
					     mt6797_gpu_domain_names[i]);
		}

		diag->domain_links[i] = device_link_add(diag->dev,
						      diag->domain_devs[i],
						      DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS |
						      DL_FLAG_RPM_ACTIVE);
		if (!diag->domain_links[i])
			return dev_err_probe(diag->dev, -ENODEV,
					     "failed to link %s domain\n",
					     mt6797_gpu_domain_names[i]);
	}

	pm_runtime_enable(diag->dev);
	diag->runtime_enabled = true;
	return 0;
}

static int mt6797_gpu_staged_enable_mfg(struct mt6797_gpu_staged *diag)
{
	unsigned int status;
	unsigned int status_2nd;
	int ret;

	ret = mt6797_gpu_staged_attach_domains(diag);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(diag->dev);
	if (ret < 0)
		return dev_err_probe(diag->dev, ret, "failed to power MFG domains\n");
	diag->domain_vote = true;

	ret = regmap_read(diag->spm, MT6797_SPM_PWR_STATUS, &status);
	if (ret)
		return dev_err_probe(diag->dev, ret, "failed to read MFG status\n");
	ret = regmap_read(diag->spm, MT6797_SPM_PWR_STATUS_2ND, &status_2nd);
	if (ret)
		return dev_err_probe(diag->dev, ret, "failed to read MFG status 2\n");

	if ((status & MT6797_MFG_STATUS_MASK) != MT6797_MFG_STATUS_MASK ||
	    (status_2nd & MT6797_MFG_STATUS_MASK) != MT6797_MFG_STATUS_MASK)
		return dev_err_probe(diag->dev, -EIO,
				     "MFG acknowledgement mismatch: %08x/%08x\n",
				     status, status_2nd);

	dev_info(diag->dev, "stage 1 pass: MFG status=%08x/%08x\n",
		 status, status_2nd);
	return 0;
}

static int mt6797_gpu_staged_read_gpu(struct mt6797_gpu_staged *diag)
{
	struct resource res;
	struct device_node *gpu_node;
	u32 gpu_id;
	int ret;

	diag->core_clk = devm_clk_get(diag->dev, "core");
	if (IS_ERR(diag->core_clk))
		return dev_err_probe(diag->dev, PTR_ERR(diag->core_clk),
				     "failed to get BG3D clock\n");
	ret = clk_prepare_enable(diag->core_clk);
	if (ret)
		return dev_err_probe(diag->dev, ret, "failed to enable BG3D clock\n");
	diag->core_clk_vote = true;

	gpu_node = of_parse_phandle(diag->dev->of_node, "mediatek,gpu", 0);
	if (!gpu_node)
		return dev_err_probe(diag->dev, -EINVAL, "missing GPU target\n");
	if (of_device_is_available(gpu_node)) {
		of_node_put(gpu_node);
		return dev_err_probe(diag->dev, -EBUSY,
				     "Panfrost target must remain disabled\n");
	}

	ret = of_address_to_resource(gpu_node, 0, &res);
	of_node_put(gpu_node);
	if (ret)
		return dev_err_probe(diag->dev, ret, "missing GPU resource\n");
	if (res.start != MT6797_GPU_BASE || resource_size(&res) != MT6797_GPU_SIZE)
		return dev_err_probe(diag->dev, -EINVAL, "invalid GPU resource\n");

	diag->gpu = devm_ioremap_resource(diag->dev, &res);
	if (IS_ERR(diag->gpu))
		return PTR_ERR(diag->gpu);

	gpu_id = readl(diag->gpu + MT6797_GPU_ID);
	if (!gpu_id || gpu_id == U32_MAX)
		return dev_err_probe(diag->dev, -ENODEV, "invalid GPU ID %08x\n", gpu_id);

	dev_info(diag->dev,
		 "stage 2 pass: id=%08x l2=%08x core=%08x tiler=%08x mem=%08x mmu=%08x shader=%08x (read-only)\n",
		 gpu_id, readl(diag->gpu + MT6797_GPU_L2_FEATURES),
		 readl(diag->gpu + MT6797_GPU_CORE_FEATURES),
		 readl(diag->gpu + MT6797_GPU_TILER_FEATURES),
		 readl(diag->gpu + MT6797_GPU_MEM_FEATURES),
		 readl(diag->gpu + MT6797_GPU_MMU_FEATURES),
		 readl(diag->gpu + MT6797_GPU_SHADER_PRESENT_LO));
	return 0;
}

static int mt6797_gpu_staged_release(struct mt6797_gpu_staged *diag)
{
	int ret = 0;
	int tmp;
	unsigned int i;

	if (diag->core_clk_vote) {
		clk_disable_unprepare(diag->core_clk);
		diag->core_clk_vote = false;
	}
	if (diag->domain_vote) {
		ret = pm_runtime_put_sync_suspend(diag->dev);
		if (ret < 0) {
			pm_runtime_get_noresume(diag->dev);
			return ret;
		}
		diag->domain_vote = false;
	}
	if (diag->runtime_enabled) {
		pm_runtime_disable(diag->dev);
		diag->runtime_enabled = false;
	}
	for (i = ARRAY_SIZE(diag->domain_devs); i-- > 0;) {
		if (diag->domain_links[i]) {
			device_link_del(diag->domain_links[i]);
			diag->domain_links[i] = NULL;
		}
		if (diag->domain_devs[i]) {
			dev_pm_domain_detach(diag->domain_devs[i], true);
			diag->domain_devs[i] = NULL;
		}
	}
	if (diag->vgpu_vote) {
		tmp = regulator_disable(diag->vgpu);
		if (!tmp)
			diag->vgpu_vote = false;
		else if (!ret)
			ret = tmp;
	}
	if (diag->vgpu_voltage_set) {
		tmp = regulator_set_voltage(diag->vgpu, diag->original_vgpu_uv,
					  diag->original_vgpu_uv);
		if (!tmp)
			diag->vgpu_voltage_set = false;
		else if (!ret)
			ret = tmp;
	}
	return ret;
}

static int mt6797_gpu_staged_probe(struct platform_device *pdev)
{
	struct mt6797_gpu_staged diag = { .dev = &pdev->dev };
	ktime_t started;
	int ret;
	int release_ret;

	if (!enable_experimental_stages)
		return dev_err_probe(diag.dev, -EPERM, "experimental gate is false\n");
	if (stage < MT6797_GPU_READONLY_STAGE || stage > MT6797_GPU_MAX_STAGE)
		return dev_err_probe(diag.dev, -EINVAL, "stage must be -1, 0, 1, or 2\n");
	if (!max_stage_ms || max_stage_ms > MT6797_GPU_MAX_TIMEOUT_MS)
		return dev_err_probe(diag.dev, -EINVAL, "invalid stage timeout\n");

	started = ktime_get();
	ret = mt6797_gpu_staged_read_only_capture(&diag);
	if (ret)
		return ret;
	if (stage == MT6797_GPU_READONLY_STAGE)
		return 0;

	diag.vgpu = devm_regulator_get_exclusive(diag.dev, "vgpu");
	if (IS_ERR(diag.vgpu))
		return dev_err_probe(diag.dev, PTR_ERR(diag.vgpu),
				     "stage 0 failed: RT5735 is not registered\n");
	dev_info(diag.dev, "stage 0 pass: RT5735 registered, enabled=%d voltage=%duV\n",
		 regulator_is_enabled(diag.vgpu), regulator_get_voltage(diag.vgpu));
	ret = mt6797_gpu_staged_check_timeout(&diag, started, "stage 0");
	if (ret || stage == 0)
		return ret;

	diag.spm = syscon_regmap_lookup_by_phandle(diag.dev->of_node, "mediatek,spm");
	if (IS_ERR(diag.spm))
		return dev_err_probe(diag.dev, PTR_ERR(diag.spm), "missing SPM syscon\n");
	diag.original_vgpu_uv = regulator_get_voltage(diag.vgpu);
	if (diag.original_vgpu_uv < 0)
		return dev_err_probe(diag.dev, diag.original_vgpu_uv,
				     "cannot restore unknown VGPU voltage\n");
	ret = regulator_set_voltage(diag.vgpu, MT6797_GPU_VOLTAGE_UV,
				  MT6797_GPU_VOLTAGE_UV);
	if (ret)
		goto out_release;
	diag.vgpu_voltage_set = true;
	ret = regulator_enable(diag.vgpu);
	if (ret)
		goto out_release;
	diag.vgpu_vote = true;

	ret = mt6797_gpu_staged_enable_mfg(&diag);
	if (!ret)
		ret = mt6797_gpu_staged_check_timeout(&diag, started, "stage 1");
	if (!ret && stage == 2)
		ret = mt6797_gpu_staged_read_gpu(&diag);
	if (!ret && stage == 2)
		ret = mt6797_gpu_staged_check_timeout(&diag, started, "stage 2");

out_release:
	if (ret)
		dev_err(diag.dev, "stage %d failed: %d; releasing diagnostic votes\n",
			stage, ret);
	release_ret = mt6797_gpu_staged_release(&diag);
	if (release_ret)
		dev_err(diag.dev, "stage cleanup failed: %d; state is unknown\n", release_ret);
	else
		dev_info(diag.dev, "stage cleanup pass: VGPU and MFG votes released\n");
	return ret ?: release_ret;
}

static const struct of_device_id mt6797_gpu_staged_of_match[] = {
	{ .compatible = "mediatek,mt6797-gpu-staged" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_gpu_staged_of_match);

static struct platform_driver mt6797_gpu_staged_driver = {
	.probe = mt6797_gpu_staged_probe,
	.driver = {
		.name = "mt6797-gpu-staged",
		.of_match_table = mt6797_gpu_staged_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_gpu_staged_driver);

MODULE_DESCRIPTION("MediaTek MT6797 staged Mali-T880 diagnostic");
MODULE_LICENSE("GPL");
