// SPDX-License-Identifier: GPL-2.0
/* Copyright 2018 Marty E. Plummer <hanetzer@startmail.com> */
/* Copyright 2019 Linaro, Ltd., Rob Herring <robh@kernel.org> */
/* Copyright 2019 Collabora ltd. */
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ptp.h>

#include <drm/drm_print.h>

#include "panfrost_device.h"
#include "panfrost_features.h"
#include "panfrost_issues.h"
#include "panfrost_gpu.h"
#include "panfrost_perfcnt.h"
#include "panfrost_regs.h"

/* Default-off GPULDO observation: no control writes or power transitions. */
static bool mt6797_gpuldo_diag;
module_param_named(mt6797_gpuldo_diag, mt6797_gpuldo_diag, bool, 0400);
MODULE_PARM_DESC(mt6797_gpuldo_diag,
	"Restricted MT6797 debug: read three GPULDO words once before first AS enable");

static void panfrost_mt6797_gpuldo_prepare(struct panfrost_device *pfdev)
{
	struct device_node *np, *duplicate;
	struct resource res;

	if (!mt6797_gpuldo_diag ||
	    pfdev->comp->vendor_init != panfrost_gpu_mt6797_init)
		return;

	pfdev->mt6797_gpuldo_status = -ENODEV;
	if (!of_machine_is_compatible("planet,gemini-pda") ||
	    !of_machine_is_compatible("mediatek,mt6797"))
		return;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-infracfg");
	if (!np)
		return;
	/* of_find_compatible_node drops its starting reference. */
	duplicate = of_find_compatible_node(of_node_get(np), NULL,
					    "mediatek,mt6797-infracfg");
	if (duplicate) {
		of_node_put(duplicate);
		goto out;
	}
	if (!of_device_is_available(np) ||
	    !of_device_is_compatible(np, "syscon") ||
	    of_address_to_resource(np, 0, &res) ||
	    res.start != 0x10001000 || resource_size(&res) != 0x1000 ||
	    of_find_property(np, "clocks", NULL) ||
	    of_find_property(np, "resets", NULL))
		goto out;

	/* Generic shared syscon only, with no resources to enable/deassert. */
	pfdev->mt6797_gpuldo_map = device_node_to_regmap(np);
	if (IS_ERR(pfdev->mt6797_gpuldo_map)) {
		pfdev->mt6797_gpuldo_status = PTR_ERR(pfdev->mt6797_gpuldo_map);
		pfdev->mt6797_gpuldo_map = NULL;
	} else {
		pfdev->mt6797_gpuldo_status = 0;
	}
out:
	of_node_put(np);
}

void panfrost_gpu_mt6797_gpuldo_diag(struct panfrost_device *pfdev)
{
	unsigned int v0 = 0, v1 = 0, v2 = 0;
	int r0, r1, r2;

	if (!mt6797_gpuldo_diag ||
	    pfdev->comp->vendor_init != panfrost_gpu_mt6797_init ||
	    test_and_set_bit(0, &pfdev->mt6797_gpuldo_seen))
		return;
	if (!pfdev->mt6797_gpuldo_map) {
		dev_info(pfdev->base.dev, "MT6797_GPULDO stage=pre-as unavailable=%d\n",
			 pfdev->mt6797_gpuldo_status);
		return;
	}
	lockdep_assert_not_held(&pfdev->as_lock);
	might_sleep();
	/* Caller owns a successful job PM reference, outside job_lock/as_lock.
	 * Vendor debug show reads these words. Other LDO controls are excluded.
	 * Each status qualifies its value: an error is not a measured zero.
	 */
	r0 = regmap_read(pfdev->mt6797_gpuldo_map, 0xfc0, &v0);
	r1 = regmap_read(pfdev->mt6797_gpuldo_map, 0xfc4, &v1);
	r2 = regmap_read(pfdev->mt6797_gpuldo_map, 0xfc8, &v2);
	if (r0)
		v0 = 0;
	if (r1)
		v1 = 0;
	if (r2)
		v2 = 0;
	dev_info(pfdev->base.dev,
		 "MT6797_GPULDO stage=pre-as fc0_rc=%d fc0=%08x fc4_rc=%d fc4=%08x fc8_rc=%d fc8=%08x\n",
		 r0, v0, r1, v1, r2, v2);
}

/* MT6797_DIAG_BEGIN: bounded read-only observations, not a recovery policy. */
enum panfrost_mt6797_diag_event {
	MT6797_DIAG_SUBMIT,
	MT6797_DIAG_FAULT,
	MT6797_DIAG_TIMEOUT,
};

static bool panfrost_mt6797_diag_claim(struct panfrost_device *pfdev,
				      unsigned int event)
{
	/* Callers already own a powered submit/MMU path or a live GPU IRQ. */
	if (pfdev->comp->vendor_init != panfrost_gpu_mt6797_init ||
	    !bitmap_empty(pfdev->is_suspended, PANFROST_COMP_BIT_MAX))
		return false;

	return !test_and_set_bit(event, &pfdev->mt6797_diag_seen);
}

static u64 panfrost_mt6797_diag_read64(struct panfrost_device *pfdev, u32 reg)
{
	/* Sequential observation, not an atomic hardware snapshot. */
	u32 lo = gpu_read(pfdev, reg);
	u32 hi = gpu_read(pfdev, reg + 4);

	return ((u64)hi << 32) | lo;
}

static void panfrost_mt6797_diag_status(struct panfrost_device *pfdev,
				       const char *event)
{
	dev_info(pfdev->base.dev,
		 "MT6797_GPU_DIAG event=%s gpu_raw=%08x mmu_raw=%08x job_raw=%08x l2_mmu=%08x\n",
		 event, gpu_read(pfdev, GPU_INT_RAWSTAT),
		 gpu_read(pfdev, MMU_INT_RAWSTAT), gpu_read(pfdev, JOB_INT_RAWSTAT),
		 gpu_read(pfdev, GPU_L2_MMU_CONFIG));
}

static void panfrost_mt6797_diag_as(struct panfrost_device *pfdev,
				   const char *event, unsigned int as)
{
	/* Never publish the physical page-table base or kernel pointers. */
	dev_info(pfdev->base.dev,
		 "MT6797_GPU_DIAG event=%s as=%u status=%08x fault=%08x tt_flags=%03x memattr=%016llx transcfg=%016llx\n",
		 event, as, gpu_read(pfdev, AS_STATUS(as)),
		 gpu_read(pfdev, AS_FAULTSTATUS(as)),
		 gpu_read(pfdev, AS_TRANSTAB_LO(as)) & 0xfff,
		 panfrost_mt6797_diag_read64(pfdev, AS_MEMATTR_LO(as)),
		 panfrost_mt6797_diag_read64(pfdev, AS_TRANSCFG_LO(as)));
}

void panfrost_gpu_mt6797_submit_diag(struct panfrost_device *pfdev,
				  struct panfrost_mmu *mmu, int js,
				  u64 head, u32 config)
{
	u64 transtab;

	if (!panfrost_mt6797_diag_claim(pfdev, MT6797_DIAG_SUBMIT))
		return;

	transtab = panfrost_mt6797_diag_read64(pfdev, AS_TRANSTAB_LO(mmu->as));
	dev_info(pfdev->base.dev,
		 "MT6797_GPU_DIAG event=submit fmt=%u quirks=%lx coherent=%u selected=%u as=%d tt_match=%u expected_tt_flags=%03llx expected_memattr=%016llx expected_transcfg=%016llx\n",
		 io_pgtable_ops_to_pgtable(mmu->pgtbl_ops)->fmt,
		 mmu->pgtbl_cfg.quirks, pfdev->coherent,
		 pfdev->features.selected_coherency, mmu->as,
		 transtab == mmu->cfg.transtab, mmu->cfg.transtab & 0xfff,
		 mmu->cfg.memattr, mmu->cfg.transcfg);
	dev_info(pfdev->base.dev,
		 "MT6797_GPU_DIAG event=submit js=%d expected_head=%016llx head_next=%016llx expected_config=%08x config_next=%08x status=%08x\n",
		 js, head, panfrost_mt6797_diag_read64(pfdev, JS_HEAD_NEXT_LO(js)),
		 config, gpu_read(pfdev, JS_CONFIG_NEXT(js)), gpu_read(pfdev, JS_STATUS(js)));
	panfrost_mt6797_diag_as(pfdev, "submit", mmu->as);
	panfrost_mt6797_diag_status(pfdev, "submit");
}

static void panfrost_mt6797_fault_diag(struct panfrost_device *pfdev,
				      u32 status, u32 irq)
{
	unsigned int i;

	if (!panfrost_mt6797_diag_claim(pfdev, MT6797_DIAG_FAULT))
		return;

	dev_info(pfdev->base.dev,
		 "MT6797_GPU_DIAG event=fault status=%08x irq=%08x\n", status, irq);
	panfrost_mt6797_diag_status(pfdev, "fault");
	/* T880 has eight address spaces and three job slots. No absent reads. */
	for (i = 0; i < 8; i++)
		if (pfdev->features.as_present & BIT(i))
			panfrost_mt6797_diag_as(pfdev, "fault", i);
	for (i = 0; i < NUM_JOB_SLOTS; i++)
		if (pfdev->features.js_present & BIT(i))
			dev_info(pfdev->base.dev,
				 "MT6797_GPU_DIAG event=fault js=%u status=%08x config=%08x head=%016llx next_config=%08x next_head=%016llx\n",
				 i, gpu_read(pfdev, JS_STATUS(i)), gpu_read(pfdev, JS_CONFIG(i)),
				 panfrost_mt6797_diag_read64(pfdev, JS_HEAD_LO(i)),
				 gpu_read(pfdev, JS_CONFIG_NEXT(i)),
				 panfrost_mt6797_diag_read64(pfdev, JS_HEAD_NEXT_LO(i)));
}

void panfrost_gpu_mt6797_timeout_diag(struct panfrost_device *pfdev,
				   u32 as, u32 command, bool before, u32 status)
{
	if (!panfrost_mt6797_diag_claim(pfdev, MT6797_DIAG_TIMEOUT))
		return;

	dev_info(pfdev->base.dev,
		 "MT6797_GPU_DIAG event=timeout as=%u command=%u before=%u polled_status=%08x\n",
		 as, command, before, status);
	panfrost_mt6797_diag_as(pfdev, "timeout", as);
	panfrost_mt6797_diag_status(pfdev, "timeout");
}
/* MT6797_DIAG_END */

static irqreturn_t panfrost_gpu_irq_handler(int irq, void *data)
{
	struct panfrost_device *pfdev = data;
	u32 fault_status, state;

	if (test_bit(PANFROST_COMP_BIT_GPU, pfdev->is_suspended))
		return IRQ_NONE;

	fault_status = gpu_read(pfdev, GPU_FAULT_STATUS);
	state = gpu_read(pfdev, GPU_INT_STAT);
	if (!state)
		return IRQ_NONE;

	if (state & GPU_IRQ_MASK_ERROR) {
		u64 address = (u64) gpu_read(pfdev, GPU_FAULT_ADDRESS_HI) << 32;

		address |= gpu_read(pfdev, GPU_FAULT_ADDRESS_LO);
		panfrost_mt6797_fault_diag(pfdev, fault_status, state);

		dev_warn(pfdev->base.dev, "GPU Fault 0x%08x (%s) at 0x%016llx\n",
			 fault_status, panfrost_exception_name(fault_status & 0xFF),
			 address);

		if (state & GPU_IRQ_MULTIPLE_FAULT)
			dev_warn(pfdev->base.dev, "There were multiple GPU faults - some have not been reported\n");

		gpu_write(pfdev, GPU_INT_MASK, 0);
	}

	if (state & GPU_IRQ_PERFCNT_SAMPLE_COMPLETED)
		panfrost_perfcnt_sample_done(pfdev);

	if (state & GPU_IRQ_CLEAN_CACHES_COMPLETED)
		panfrost_perfcnt_clean_cache_done(pfdev);

	gpu_write(pfdev, GPU_INT_CLEAR, state);

	return IRQ_HANDLED;
}

int panfrost_gpu_soft_reset(struct panfrost_device *pfdev)
{
	int ret;
	u32 val;

	gpu_write(pfdev, GPU_INT_MASK, 0);
	gpu_write(pfdev, GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);

	clear_bit(PANFROST_COMP_BIT_GPU, pfdev->is_suspended);

	gpu_write(pfdev, GPU_CMD, GPU_CMD_SOFT_RESET);
	ret = readl_relaxed_poll_timeout(pfdev->iomem + GPU_INT_RAWSTAT,
		val, val & GPU_IRQ_RESET_COMPLETED, 10, 10000);

	if (ret) {
		dev_err(pfdev->base.dev, "gpu soft reset timed out, attempting hard reset\n");

		gpu_write(pfdev, GPU_CMD, GPU_CMD_HARD_RESET);
		ret = readl_relaxed_poll_timeout(pfdev->iomem + GPU_INT_RAWSTAT, val,
						 val & GPU_IRQ_RESET_COMPLETED, 100, 10000);
		if (ret) {
			dev_err(pfdev->base.dev, "gpu hard reset timed out\n");
			return ret;
		}
	}

	gpu_write(pfdev, GPU_INT_CLEAR, GPU_IRQ_MASK_ALL);

	/* Only enable the interrupts we care about */
	gpu_write(pfdev, GPU_INT_MASK,
		  GPU_IRQ_MASK_ERROR |
		  GPU_IRQ_PERFCNT_SAMPLE_COMPLETED |
		  GPU_IRQ_CLEAN_CACHES_COMPLETED);

	/*
	 * All in-flight jobs should have released their cycle
	 * counter references upon reset, but let us make sure
	 */
	if (drm_WARN_ON(&pfdev->base, atomic_read(&pfdev->cycle_counter.use_count) != 0))
		atomic_set(&pfdev->cycle_counter.use_count, 0);

	return 0;
}

void panfrost_gpu_amlogic_quirk(struct panfrost_device *pfdev)
{
	/*
	 * The Amlogic integrated Mali-T820, Mali-G31 & Mali-G52 needs
	 * these undocumented bits in GPU_PWR_OVERRIDE1 to be set in order
	 * to operate correctly.
	 */
	gpu_write(pfdev, GPU_PWR_KEY, GPU_PWR_KEY_UNLOCK);
	gpu_write(pfdev, GPU_PWR_OVERRIDE1, 0xfff | (0x20 << 16));
}

#define MT6797_MFG_ASYNC_TIMING		0x1c
#define MT6797_MFG_ASYNC_TIMING_LOW	0x5
#define MT6797_MFG_ASYNC_TIMING_HIGH	0xa
/* The vendor platform selects the timing from the fastest enabled OPP. */
#define MT6797_MFG_ASYNC_TIMING_THRESHOLD	780000000UL
#define MT6797_MFG_PMU_ENABLE_FIRST	0x3e0
#define MT6797_MFG_PMU_ENABLE_LAST	0x3f0
#define MT6797_MFG_DEBUG_SEL		0x180
#define MT6797_MFG_DEBUG_A		0x184
#define MT6797_MFG_DEBUG_IDLE		BIT(2)
#define MT6797_GPU_FUNCTION_CODE_MASK	GENMASK(3, 0)
#define MT6797_GPU_PTP_VERSION_MASK	GENMASK(7, 4)
#define MT6797_GPU_OPP_UNKNOWN		BIT(31)

static unsigned int panfrost_mt6797_gpu_table_type(u32 function_word,
						   u32 date_word)
{
	unsigned int function_code;
	unsigned int ptp_version;
	unsigned int type;

	function_code = FIELD_GET(MT6797_GPU_FUNCTION_CODE_MASK,
				  function_word);
	ptp_version = FIELD_GET(MT6797_GPU_PTP_VERSION_MASK, date_word);

	switch (function_code) {
	case 0:
	case 3:
		type = 0;
		break;
	case 1:
	case 6:
		type = 1;
		break;
	case 2:
	case 7:
		type = 2;
		break;
	default:
		type = 0;
		break;
	}

	if (ptp_version <= 5)
		type += 4;
	else if (ptp_version > 6)
		type += 8;

	if (function_code == 0xf)
		type = 11;
	else if (function_code == 4)
		type = 12;

	return type;
}

static bool panfrost_mt6797_gpu_table_type_valid(unsigned int type)
{
	switch (type) {
	case 0:
	case 1:
	case 2:
	case 4:
	case 5:
	case 6:
	case 8:
	case 9:
	case 10:
	case 11:
	case 12:
		return true;
	default:
		return false;
	}
}

static int panfrost_mt6797_set_supported_hw(struct panfrost_device *pfdev)
{
	struct device *dev = pfdev->base.dev;
	struct nvmem_cell *cell;
	unsigned int table_type = 0;
	u32 supported_hw = MT6797_GPU_OPP_UNKNOWN;
	u32 function_word;
	u32 date_word;
	bool table_valid = false;
	size_t len;
	void *buf;
	int ret;

	cell = devm_nvmem_cell_get(dev, "gpu-efuse");
	if (IS_ERR(cell)) {
		ret = PTR_ERR(cell);
		if (ret == -EPROBE_DEFER)
			return ret;

		dev_warn(dev, "GPU efuse unavailable, using safe OPP profile: %d\n",
			 ret);
		goto set_supported_hw;
	}

	buf = nvmem_cell_read(cell, &len);
	if (IS_ERR(buf)) {
		dev_warn(dev, "failed to read GPU efuse, using safe OPP profile: %ld\n",
			 PTR_ERR(buf));
		goto set_supported_hw;
	}

	if (len != sizeof(struct mt6797_dvfsp_efuse_identity) ||
	    get_unaligned_le32(buf) != MT6797_DVFSP_EFUSE_IDENTITY_ABI) {
		dev_warn(dev, "invalid GPU efuse identity, using safe OPP profile\n");
	} else {
		function_word = get_unaligned_le32(buf + sizeof(u32));
		date_word = get_unaligned_le32(buf + 2 * sizeof(u32));
		table_type = panfrost_mt6797_gpu_table_type(function_word,
							    date_word);
		if (panfrost_mt6797_gpu_table_type_valid(table_type)) {
			supported_hw = BIT(table_type);
			table_valid = true;
		}
	}

	kfree(buf);

set_supported_hw:
	if (table_valid)
		dev_info(dev, "GPU DVFS table type %u, supported-hw %#x\n",
			 table_type, supported_hw);
	else
		dev_info(dev, "GPU DVFS table unknown, supported-hw %#x\n",
			 supported_hw);

	ret = devm_pm_opp_set_supported_hw(dev, &supported_hw, 1);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to select GPU OPP profile\n");

	return 0;
}

int panfrost_gpu_mt6797_init(struct panfrost_device *pfdev)
{
	panfrost_mt6797_gpuldo_prepare(pfdev);
	pfdev->mfgcfg = syscon_regmap_lookup_by_phandle(pfdev->base.dev->of_node,
							"mediatek,mfgcfg");
	if (IS_ERR(pfdev->mfgcfg))
		return dev_err_probe(pfdev->base.dev, PTR_ERR(pfdev->mfgcfg),
				     "failed to get MFG configuration\n");

	return panfrost_mt6797_set_supported_hw(pfdev);
}

int panfrost_gpu_mt6797_power_on(struct panfrost_device *pfdev)
{
	u32 async_timing;
	unsigned int reg;
	int ret;

	async_timing = pfdev->pfdevfreq.fast_rate >=
			MT6797_MFG_ASYNC_TIMING_THRESHOLD ?
			MT6797_MFG_ASYNC_TIMING_HIGH :
			MT6797_MFG_ASYNC_TIMING_LOW;

	ret = regmap_set_bits(pfdev->mfgcfg, MT6797_MFG_ASYNC_TIMING,
			      async_timing);
	if (ret)
		return dev_err_probe(pfdev->base.dev, ret,
				     "failed to configure MFG asynchronous timing\n");

	for (reg = MT6797_MFG_PMU_ENABLE_FIRST;
	     reg <= MT6797_MFG_PMU_ENABLE_LAST; reg += sizeof(u32)) {
		ret = regmap_write(pfdev->mfgcfg, reg, U32_MAX);
		if (ret)
			return dev_err_probe(pfdev->base.dev, ret,
					     "failed to enable MFG PMU register %#x\n",
					     reg);
	}

	return 0;
}

int panfrost_gpu_mt6797_power_off(struct panfrost_device *pfdev)
{
	unsigned int saved_sel, val;
	int ret, restore_ret;

	/*
	 * MT6797's vendor driver keeps shaders and their L2/tiler dependencies
	 * powered until whole-MFG shutdown. Individual PWROFF requests can
	 * stall. Establish whole-MFG idle before genpd removes power instead.
	 * Preserve the debug selector, including on a failed idle poll.
	 */
	ret = regmap_read(pfdev->mfgcfg, MT6797_MFG_DEBUG_SEL, &saved_sel);
	if (ret)
		goto out;

	ret = regmap_write(pfdev->mfgcfg, MT6797_MFG_DEBUG_SEL, 3);
	if (!ret)
		ret = regmap_read_poll_timeout(pfdev->mfgcfg, MT6797_MFG_DEBUG_A,
					      val, val & MT6797_MFG_DEBUG_IDLE,
					      1, 100000);

	restore_ret = regmap_write(pfdev->mfgcfg, MT6797_MFG_DEBUG_SEL, saved_sel);
	if (!ret)
		ret = restore_ret;
out:
	if (ret)
		dev_err(pfdev->base.dev, "MFG idle check failed: %d\n", ret);

	return ret;
}

static void panfrost_gpu_init_quirks(struct panfrost_device *pfdev)
{
	u32 quirks = 0;

	if (panfrost_has_hw_issue(pfdev, HW_ISSUE_8443) ||
	    panfrost_has_hw_issue(pfdev, HW_ISSUE_11035))
		quirks |= SC_LS_PAUSEBUFFER_DISABLE;

	if (panfrost_has_hw_issue(pfdev, HW_ISSUE_10327))
		quirks |= SC_SDC_DISABLE_OQ_DISCARD;

	if (panfrost_has_hw_issue(pfdev, HW_ISSUE_10797))
		quirks |= SC_ENABLE_TEXGRD_FLAGS;

	if (!panfrost_has_hw_issue(pfdev, GPUCORE_1619)) {
		if (panfrost_model_cmp(pfdev, 0x750) < 0) /* T60x, T62x, T72x */
			quirks |= SC_LS_ATTR_CHECK_DISABLE;
		else if (panfrost_model_cmp(pfdev, 0x880) <= 0) /* T76x, T8xx */
			quirks |= SC_LS_ALLOW_ATTR_TYPES;
	}

	if (panfrost_has_hw_issue(pfdev, HW_ISSUE_TTRX_2968_TTRX_3162))
		quirks |= SC_VAR_ALGORITHM;

	if (panfrost_has_hw_feature(pfdev, HW_FEATURE_TLS_HASHING))
		quirks |= SC_TLS_HASH_ENABLE;

	if (quirks)
		gpu_write(pfdev, GPU_SHADER_CONFIG, quirks);


	quirks = gpu_read(pfdev, GPU_TILER_CONFIG);

	/* Set tiler clock gate override if required */
	if (panfrost_has_hw_issue(pfdev, HW_ISSUE_T76X_3953))
		quirks |= TC_CLOCK_GATE_OVERRIDE;

	gpu_write(pfdev, GPU_TILER_CONFIG, quirks);


	quirks = 0;
	if ((panfrost_model_eq(pfdev, 0x860) || panfrost_model_eq(pfdev, 0x880)) &&
	    pfdev->features.revision >= 0x2000)
		quirks |= JM_MAX_JOB_THROTTLE_LIMIT << JM_JOB_THROTTLE_LIMIT_SHIFT;
	else if (panfrost_model_eq(pfdev, 0x6000) &&
		 pfdev->features.coherency_features == BIT(COHERENCY_ACE))
		quirks |= (BIT(COHERENCY_ACE_LITE) | BIT(COHERENCY_ACE)) <<
			   JM_FORCE_COHERENCY_FEATURES_SHIFT;

	if (panfrost_has_hw_feature(pfdev, HW_FEATURE_IDVS_GROUP_SIZE))
		quirks |= JM_DEFAULT_IDVS_GROUP_SIZE << JM_IDVS_GROUP_SIZE_SHIFT;

	if (quirks)
		gpu_write(pfdev, GPU_JM_CONFIG, quirks);

	/* Here goes platform specific quirks */
	if (pfdev->comp->vendor_quirk)
		pfdev->comp->vendor_quirk(pfdev);
}

#define MAX_HW_REVS 6

struct panfrost_model {
	const char *name;
	u32 id;
	u64 features;
	u64 issues;
	struct {
		u32 revision;
		u64 issues;
	} revs[MAX_HW_REVS];
};

#define GPU_MODEL(_name, _id, ...) \
{\
	.name = __stringify(_name),				\
	.id = _id,						\
	.features = hw_features_##_name,			\
	.issues = hw_issues_##_name,				\
	.revs = { __VA_ARGS__ },				\
}

#define GPU_REV_EXT(name, _rev, _p, _s, stat) \
{\
	.revision = (_rev) << 12 | (_p) << 4 | (_s),		\
	.issues = hw_issues_##name##_r##_rev##p##_p##stat,	\
}
#define GPU_REV(name, r, p) GPU_REV_EXT(name, r, p, 0, )

static const struct panfrost_model gpu_models[] = {
	/* T60x has an oddball version */
	GPU_MODEL(t600, 0x600,
		GPU_REV_EXT(t600, 0, 0, 1, _15dev0)),
	GPU_MODEL(t620, 0x620,
		GPU_REV(t620, 0, 1), GPU_REV(t620, 1, 0)),
	GPU_MODEL(t720, 0x720),
	GPU_MODEL(t760, 0x750,
		GPU_REV(t760, 0, 0), GPU_REV(t760, 0, 1),
		GPU_REV_EXT(t760, 0, 1, 0, _50rel0),
		GPU_REV(t760, 0, 2), GPU_REV(t760, 0, 3)),
	GPU_MODEL(t820, 0x820),
	GPU_MODEL(t830, 0x830),
	GPU_MODEL(t860, 0x860),
	GPU_MODEL(t880, 0x880),

	GPU_MODEL(g71, 0x6000,
		GPU_REV_EXT(g71, 0, 0, 1, _05dev0)),
	GPU_MODEL(g72, 0x6001),
	GPU_MODEL(g51, 0x7000),
	GPU_MODEL(g76, 0x7001),
	GPU_MODEL(g52, 0x7002),
	GPU_MODEL(g31, 0x7003,
		GPU_REV(g31, 1, 0)),

	GPU_MODEL(g57, 0x9001,
		GPU_REV(g57, 0, 0)),

	/* MediaTek MT8192 has a Mali-G57 with a different GPU ID from the
	 * standard. Arm's driver does not appear to handle this model.
	 * ChromeOS has a hack downstream for it. Treat it as equivalent to
	 * standard Mali-G57 for now.
	 */
	GPU_MODEL(g57, 0x9003,
		GPU_REV(g57, 0, 0)),

	/* MediaTek MT8188 Mali-G57 MC3 */
	GPU_MODEL(g57, 0x9093,
		GPU_REV(g57, 0, 0)),
	{0},
};

static int panfrost_gpu_init_features(struct panfrost_device *pfdev)
{
	u32 gpu_id, num_js, major, minor, status, rev;
	const char *name = "unknown";
	u64 hw_feat = 0;
	u64 hw_issues = hw_issues_all;
	const struct panfrost_model *model;
	int i;

	pfdev->features.l2_features = gpu_read(pfdev, GPU_L2_FEATURES);
	pfdev->features.core_features = gpu_read(pfdev, GPU_CORE_FEATURES);
	pfdev->features.tiler_features = gpu_read(pfdev, GPU_TILER_FEATURES);
	pfdev->features.mem_features = gpu_read(pfdev, GPU_MEM_FEATURES);
	pfdev->features.mmu_features = gpu_read(pfdev, GPU_MMU_FEATURES);
	pfdev->features.thread_features = gpu_read(pfdev, GPU_THREAD_FEATURES);
	pfdev->features.max_threads = gpu_read(pfdev, GPU_THREAD_MAX_THREADS);
	pfdev->features.thread_max_workgroup_sz = gpu_read(pfdev, GPU_THREAD_MAX_WORKGROUP_SIZE);
	pfdev->features.thread_max_barrier_sz = gpu_read(pfdev, GPU_THREAD_MAX_BARRIER_SIZE);

	if (panfrost_has_hw_feature(pfdev, HW_FEATURE_COHERENCY_REG))
		pfdev->features.coherency_features = gpu_read(pfdev, GPU_COHERENCY_FEATURES);
	else
		pfdev->features.coherency_features = BIT(COHERENCY_ACE_LITE);

	BUILD_BUG_ON(COHERENCY_ACE_LITE != DRM_PANFROST_GPU_COHERENCY_ACE_LITE);
	BUILD_BUG_ON(COHERENCY_ACE != DRM_PANFROST_GPU_COHERENCY_ACE);
	BUILD_BUG_ON(COHERENCY_NONE != DRM_PANFROST_GPU_COHERENCY_NONE);

	if (!pfdev->coherent) {
		pfdev->features.selected_coherency = COHERENCY_NONE;
	} else if (pfdev->features.coherency_features & BIT(COHERENCY_ACE)) {
		pfdev->features.selected_coherency = COHERENCY_ACE;
	} else if (pfdev->features.coherency_features & BIT(COHERENCY_ACE_LITE)) {
		pfdev->features.selected_coherency = COHERENCY_ACE_LITE;
	} else {
		drm_WARN(&pfdev->base, true, "No known coherency protocol supported");
		pfdev->features.selected_coherency = COHERENCY_NONE;
	}

	pfdev->features.afbc_features = gpu_read(pfdev, GPU_AFBC_FEATURES);
	for (i = 0; i < 4; i++)
		pfdev->features.texture_features[i] = gpu_read(pfdev, GPU_TEXTURE_FEATURES(i));

	pfdev->features.as_present = gpu_read(pfdev, GPU_AS_PRESENT);

	pfdev->features.js_present = gpu_read(pfdev, GPU_JS_PRESENT);
	num_js = hweight32(pfdev->features.js_present);
	for (i = 0; i < num_js; i++)
		pfdev->features.js_features[i] = gpu_read(pfdev, GPU_JS_FEATURES(i));

	pfdev->features.shader_present = gpu_read(pfdev, GPU_SHADER_PRESENT_LO);
	pfdev->features.shader_present |= (u64)gpu_read(pfdev, GPU_SHADER_PRESENT_HI) << 32;

	pfdev->features.tiler_present = gpu_read(pfdev, GPU_TILER_PRESENT_LO);
	pfdev->features.tiler_present |= (u64)gpu_read(pfdev, GPU_TILER_PRESENT_HI) << 32;

	pfdev->features.l2_present = gpu_read(pfdev, GPU_L2_PRESENT_LO);
	pfdev->features.l2_present |= (u64)gpu_read(pfdev, GPU_L2_PRESENT_HI) << 32;
	pfdev->features.nr_core_groups = hweight64(pfdev->features.l2_present);

	pfdev->features.stack_present = gpu_read(pfdev, GPU_STACK_PRESENT_LO);
	pfdev->features.stack_present |= (u64)gpu_read(pfdev, GPU_STACK_PRESENT_HI) << 32;

	pfdev->features.thread_tls_alloc = gpu_read(pfdev, GPU_THREAD_TLS_ALLOC);

	gpu_id = gpu_read(pfdev, GPU_ID);
	pfdev->features.revision = gpu_id & 0xffff;
	pfdev->features.id = gpu_id >> 16;

	/* The T60x has an oddball ID value. Fix it up to the standard Midgard
	 * format so we (and userspace) don't have to special case it.
	 */
	if (pfdev->features.id == 0x6956)
		pfdev->features.id = 0x0600;

	major = (pfdev->features.revision >> 12) & 0xf;
	minor = (pfdev->features.revision >> 4) & 0xff;
	status = pfdev->features.revision & 0xf;
	rev = pfdev->features.revision;

	gpu_id = pfdev->features.id;

	for (model = gpu_models; model->name; model++) {
		int best = -1;

		if (!panfrost_model_eq(pfdev, model->id))
			continue;

		name = model->name;
		hw_feat = model->features;
		hw_issues |= model->issues;
		for (i = 0; i < MAX_HW_REVS; i++) {
			if (model->revs[i].revision == rev) {
				best = i;
				break;
			} else if (model->revs[i].revision == (rev & ~0xf))
				best = i;
		}

		if (best >= 0)
			hw_issues |= model->revs[best].issues;

		break;
	}

	if (!model->name) {
		dev_err(pfdev->base.dev, "GPU model not found: mali-%s id rev %#x %#x\n",
			name, gpu_id, rev);
		return -ENODEV;
	}

	bitmap_from_u64(pfdev->features.hw_features, hw_feat);
	bitmap_from_u64(pfdev->features.hw_issues, hw_issues);

	dev_info(pfdev->base.dev, "mali-%s id 0x%x major 0x%x minor 0x%x status 0x%x",
		 name, gpu_id, major, minor, status);
	dev_info(pfdev->base.dev, "features: %64pb, issues: %64pb",
		 pfdev->features.hw_features,
		 pfdev->features.hw_issues);

	dev_info(pfdev->base.dev, "Features: L2:0x%08x Shader:0x%08x Tiler:0x%08x Mem:0x%0x MMU:0x%08x AS:0x%x JS:0x%x",
		 pfdev->features.l2_features,
		 pfdev->features.core_features,
		 pfdev->features.tiler_features,
		 pfdev->features.mem_features,
		 pfdev->features.mmu_features,
		 pfdev->features.as_present,
		 pfdev->features.js_present);

	dev_info(pfdev->base.dev, "shader_present=0x%0llx l2_present=0x%0llx",
		 pfdev->features.shader_present, pfdev->features.l2_present);

	return 0;
}

void panfrost_cycle_counter_get(struct panfrost_device *pfdev)
{
	if (atomic_inc_not_zero(&pfdev->cycle_counter.use_count))
		return;

	spin_lock(&pfdev->cycle_counter.lock);
	if (atomic_inc_return(&pfdev->cycle_counter.use_count) == 1)
		gpu_write(pfdev, GPU_CMD, GPU_CMD_CYCLE_COUNT_START);
	spin_unlock(&pfdev->cycle_counter.lock);
}

void panfrost_cycle_counter_put(struct panfrost_device *pfdev)
{
	if (atomic_add_unless(&pfdev->cycle_counter.use_count, -1, 1))
		return;

	spin_lock(&pfdev->cycle_counter.lock);
	if (atomic_dec_return(&pfdev->cycle_counter.use_count) == 0)
		gpu_write(pfdev, GPU_CMD, GPU_CMD_CYCLE_COUNT_STOP);
	spin_unlock(&pfdev->cycle_counter.lock);
}

unsigned long long panfrost_cycle_counter_read(struct panfrost_device *pfdev)
{
	u32 hi, lo;

	do {
		hi = gpu_read(pfdev, GPU_CYCLE_COUNT_HI);
		lo = gpu_read(pfdev, GPU_CYCLE_COUNT_LO);
	} while (hi != gpu_read(pfdev, GPU_CYCLE_COUNT_HI));

	return ((u64)hi << 32) | lo;
}

unsigned long long panfrost_timestamp_read(struct panfrost_device *pfdev)
{
	u32 hi, lo;

	do {
		hi = gpu_read(pfdev, GPU_TIMESTAMP_HI);
		lo = gpu_read(pfdev, GPU_TIMESTAMP_LO);
	} while (hi != gpu_read(pfdev, GPU_TIMESTAMP_HI));

	return ((u64)hi << 32) | lo;
}

static u64 panfrost_get_core_mask(struct panfrost_device *pfdev)
{
	u64 core_mask;

	if (pfdev->features.l2_present == 1)
		return U64_MAX;

	/*
	 * Only support one core group now.
	 * ~(l2_present - 1) unsets all bits in l2_present except
	 * the bottom bit. (l2_present - 2) has all the bits in
	 * the first core group set. AND them together to generate
	 * a mask of cores in the first core group.
	 */
	core_mask = ~(pfdev->features.l2_present - 1) &
		     (pfdev->features.l2_present - 2);
	dev_info_once(pfdev->base.dev, "using only 1st core group (%lu cores from %lu)\n",
		      hweight64(core_mask),
		      hweight64(pfdev->features.shader_present));

	return core_mask;
}

int panfrost_gpu_prepare_power_on(struct panfrost_device *pfdev)
{
	if (pfdev->comp->vendor_power_on)
		return pfdev->comp->vendor_power_on(pfdev);

	return 0;
}

int panfrost_gpu_power_on(struct panfrost_device *pfdev)
{
	int ret;
	u32 expected, val;
	u64 core_mask;

	panfrost_gpu_init_quirks(pfdev);
	core_mask = panfrost_get_core_mask(pfdev);

	expected = lower_32_bits(pfdev->features.l2_present & core_mask);
	gpu_write(pfdev, L2_PWRON_LO, expected);
	ret = readl_relaxed_poll_timeout(pfdev->iomem + L2_READY_LO,
		val, val == expected, 10, 20000);
	if (ret) {
		dev_err(pfdev->base.dev,
			"L2 power-on timeout: ready=%#x transition=%#x expected=%#x\n",
			val, gpu_read(pfdev, L2_PWRTRANS_LO), expected);
		return ret;
	}

	expected = lower_32_bits(pfdev->features.shader_present & core_mask);
	gpu_write(pfdev, SHADER_PWRON_LO, expected);
	ret = readl_relaxed_poll_timeout(pfdev->iomem + SHADER_READY_LO,
		val, val == expected, 10, 20000);
	if (ret) {
		dev_err(pfdev->base.dev,
			"shader power-on timeout: ready=%#x transition=%#x expected=%#x\n",
			val, gpu_read(pfdev, SHADER_PWRTRANS_LO), expected);
		return ret;
	}

	expected = lower_32_bits(pfdev->features.tiler_present);
	gpu_write(pfdev, TILER_PWRON_LO, expected);
	ret = readl_relaxed_poll_timeout(pfdev->iomem + TILER_READY_LO,
		val, val == expected, 10, 1000);
	if (ret) {
		dev_err(pfdev->base.dev,
			"tiler power-on timeout: ready=%#x transition=%#x expected=%#x\n",
			val, gpu_read(pfdev, TILER_PWRTRANS_LO), expected);
		return ret;
	}

	return 0;
}

void panfrost_gpu_power_off(struct panfrost_device *pfdev)
{
	int ret;
	u32 val;

	/* Teardown cannot veto removal; the callback still reports failures. */
	if (pfdev->comp->vendor_power_off) {
		pfdev->comp->vendor_power_off(pfdev);
		return;
	}

	gpu_write(pfdev, SHADER_PWROFF_LO, pfdev->features.shader_present);
	ret = readl_relaxed_poll_timeout(pfdev->iomem + SHADER_PWRTRANS_LO,
					 val, !val, 1, 2000);
	if (ret)
		dev_err(pfdev->base.dev,
			"shader power transition timeout: ready=%#x transition=%#x present=%#x\n",
			gpu_read(pfdev, SHADER_READY_LO), val,
			lower_32_bits(pfdev->features.shader_present));

	gpu_write(pfdev, TILER_PWROFF_LO, pfdev->features.tiler_present);
	ret = readl_relaxed_poll_timeout(pfdev->iomem + TILER_PWRTRANS_LO,
					 val, !val, 1, 2000);
	if (ret)
		dev_err(pfdev->base.dev,
			"tiler power transition timeout: ready=%#x transition=%#x present=%#x\n",
			gpu_read(pfdev, TILER_READY_LO), val,
			lower_32_bits(pfdev->features.tiler_present));

	gpu_write(pfdev, L2_PWROFF_LO, pfdev->features.l2_present);
	ret = readl_poll_timeout(pfdev->iomem + L2_PWRTRANS_LO,
				 val, !val, 0, 2000);
	if (ret)
		dev_err(pfdev->base.dev,
			"l2 power transition timeout: ready=%#x transition=%#x present=%#x\n",
			gpu_read(pfdev, L2_READY_LO), val,
			lower_32_bits(pfdev->features.l2_present));
}

void panfrost_gpu_suspend_irq(struct panfrost_device *pfdev)
{
	set_bit(PANFROST_COMP_BIT_GPU, pfdev->is_suspended);

	gpu_write(pfdev, GPU_INT_MASK, 0);
	synchronize_irq(pfdev->gpu_irq);
}

int panfrost_gpu_init(struct panfrost_device *pfdev)
{
	int err;

	err = panfrost_gpu_prepare_power_on(pfdev);
	if (err)
		return err;

	err = panfrost_gpu_soft_reset(pfdev);
	if (err)
		return err;

	err = panfrost_gpu_init_features(pfdev);
	if (err)
		return err;

	err = dma_set_mask_and_coherent(pfdev->base.dev,
					DMA_BIT_MASK(FIELD_GET(0xff00,
							       pfdev->features.mmu_features)));
	if (err)
		return err;

	dma_set_max_seg_size(pfdev->base.dev, UINT_MAX);

	pfdev->gpu_irq = platform_get_irq_byname(to_platform_device(pfdev->base.dev), "gpu");
	if (pfdev->gpu_irq < 0)
		return pfdev->gpu_irq;

	err = devm_request_irq(pfdev->base.dev, pfdev->gpu_irq, panfrost_gpu_irq_handler,
			       IRQF_SHARED, KBUILD_MODNAME "-gpu", pfdev);
	if (err) {
		dev_err(pfdev->base.dev, "failed to request gpu irq");
		return err;
	}

	return panfrost_gpu_power_on(pfdev);
}

void panfrost_gpu_fini(struct panfrost_device *pfdev)
{
	panfrost_gpu_power_off(pfdev);
}

u32 panfrost_gpu_get_latest_flush_id(struct panfrost_device *pfdev)
{
	u32 flush_id;

	if (panfrost_has_hw_feature(pfdev, HW_FEATURE_FLUSH_REDUCTION)) {
		/* Flush reduction only makes sense when the GPU is kept powered on between jobs */
		if (pm_runtime_get_if_in_use(pfdev->base.dev)) {
			flush_id = gpu_read(pfdev, GPU_LATEST_FLUSH_ID);
			pm_runtime_put(pfdev->base.dev);
			return flush_id;
		}
	}

	return 0;
}
