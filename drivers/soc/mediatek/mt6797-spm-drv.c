// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 System Power Manager (SPM): PCM firmware and CPU idle
 * status.
 *
 * MT6797 has no SPM program behind per-core CPU_SUSPEND power-down: the
 * vendor kernel does not build its MCDI driver for this SoC, and in the
 * vendor BL31 a power-down suspend only programs the warm-boot mailbox
 * before parking the core in WFI for an SPM program to power off.  Per-core
 * idle is WFI with hardware SPARK2 retention, which BL31 enables for every
 * little core it powers on.  Deeper states (SODI, deep idle, suspend) are
 * SPM programs that the vendor kernel loads from files, run with a single
 * CPU online, and enter with a cluster-level PSCI CPU_SUSPEND.
 *
 * This driver loads and validates those programs and gives a read-only view
 * of the SPM power, idle and retention status.  It never writes SPM.
 */

#include <linux/bits.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/seq_file.h>

#include "mt6797-spm-fw.h"

/* Offsets from the vendor mt_spm_reg_mt6797.h */
#define MT6797_SPM_CPU_IDLE_STA		0x174
#define MT6797_SPM_CPU_RET_STATUS	0x1e4

/* CPU_IDLE_STA: MP0/MP1 CPUn STANDBYWFI at bits 10..17, MCUSYS_IDLE at 23 */
#define MT6797_SPM_IDLE_STA_WFI(cpu)	BIT(10 + (cpu))
#define MT6797_SPM_IDLE_STA_MCUSYS	BIT(23)
#define MT6797_SPM_LITTLE_CPUS		8

/* One read busy-waits the reading CPU for at most ~10 s. */
#define MT6797_SPM_SAMPLES_DEFAULT	10000
#define MT6797_SPM_SAMPLES_MAX		100000
#define MT6797_SPM_SAMPLE_US_DEFAULT	10
#define MT6797_SPM_SAMPLE_US_MAX	100

struct mt6797_spm_reg {
	const char *name;
	u32 offset;
};

#define SPM_REG(_name, _offset) { .name = #_name, .offset = _offset }

static const struct mt6797_spm_reg mt6797_spm_status_regs[] = {
	SPM_REG(POWERON_CONFIG_EN, 0x000),
	SPM_REG(SPM_POWER_ON_VAL0, 0x004),
	SPM_REG(SPM_POWER_ON_VAL1, 0x008),
	SPM_REG(PCM_CON0, 0x018),
	SPM_REG(PCM_CON1, 0x01c),
	SPM_REG(PCM_IM_PTR, 0x020),
	SPM_REG(PCM_IM_LEN, 0x024),
	SPM_REG(PCM_REG_DATA_INI, 0x028),
	SPM_REG(PCM_PWR_IO_EN, 0x02c),
	SPM_REG(PCM_TIMER_VAL, 0x030),
	SPM_REG(PCM_WDT_VAL, 0x034),
	SPM_REG(PCM_EVENT_VECTOR_EN, 0x080),
	SPM_REG(SPM_SCP_MAILBOX, 0x098),
	SPM_REG(SPM_CPU_WAKEUP_EVENT, 0x0b0),
	SPM_REG(SPM_IRQ_MASK, 0x0b4),
	SPM_REG(SPM_SRC_REQ, 0x0b8),
	SPM_REG(SPM_SRC_MASK, 0x0bc),
	SPM_REG(SPM_SRC2_MASK, 0x0c0),
	SPM_REG(SPM_WAKEUP_EVENT_MASK, 0x0c4),
	SPM_REG(PCM_REG0_DATA, 0x100),
	SPM_REG(PCM_REG1_DATA, 0x104),
	SPM_REG(PCM_REG6_DATA, 0x118),
	SPM_REG(PCM_REG13_DATA, 0x134),
	SPM_REG(PCM_REG15_DATA, 0x13c),
	SPM_REG(SPM_IRQ_STA, 0x158),
	SPM_REG(SPM_WAKEUP_STA, 0x15c),
	SPM_REG(SPM_WAKEUP_EXT_STA, 0x160),
	SPM_REG(SPM_WAKEUP_MISC, 0x164),
	SPM_REG(SUBSYS_IDLE_STA, 0x170),
	SPM_REG(CPU_IDLE_STA, 0x174),
	SPM_REG(CPU_IDLE_STA2, 0x178),
	SPM_REG(PCM_FSM_STA, 0x17c),
	SPM_REG(PWR_STATUS, 0x180),
	SPM_REG(PWR_STATUS_2ND, 0x184),
	SPM_REG(CPU_PWR_STATUS, 0x188),
	SPM_REG(CPU_PWR_STATUS_2ND, 0x18c),
	SPM_REG(CPU_RET_STATUS, 0x1e4),
	SPM_REG(MCUSYS_PWR_CON, 0x200),
	SPM_REG(MP_TOP_PWR_CON, 0x204),
	SPM_REG(MP0_CPUSYS_PWR_CON, 0x210),
	SPM_REG(MP1_CPUSYS_PWR_CON, 0x214),
	SPM_REG(MP2_CPUSYS_PWR_CON, 0x218),
	SPM_REG(MP0_CPU0_PWR_CON, 0x220),
	SPM_REG(MP0_CPU1_PWR_CON, 0x224),
	SPM_REG(MP0_CPU2_PWR_CON, 0x228),
	SPM_REG(MP0_CPU3_PWR_CON, 0x22c),
	SPM_REG(MP1_CPU0_PWR_CON, 0x230),
	SPM_REG(MP1_CPU1_PWR_CON, 0x234),
	SPM_REG(MP1_CPU2_PWR_CON, 0x238),
	SPM_REG(MP1_CPU3_PWR_CON, 0x23c),
	SPM_REG(SPM_SW_FLAG, 0x600),
	SPM_REG(SPM_SW_DEBUG, 0x604),
	SPM_REG(SPM_SW_RSV_0, 0x608),
	SPM_REG(SPM_SW_RSV_1, 0x60c),
	SPM_REG(SPM_SW_RSV_2, 0x610),
	SPM_REG(SPM_SW_RSV_3, 0x614),
	SPM_REG(SPM_SW_RSV_4, 0x618),
	SPM_REG(SPM_SW_RSV_5, 0x61c),
};

struct mt6797_spm {
	struct device *dev;
	struct regmap *regmap;
	struct dentry *debugfs;
	const struct firmware *fw[MT6797_SPM_FW_COUNT];
	struct mt6797_spm_pcm pcm[MT6797_SPM_FW_COUNT];
	int fw_status[MT6797_SPM_FW_COUNT];
	u32 samples;
	u32 sample_us;
};

static int mt6797_spm_firmware_show(struct seq_file *s, void *unused)
{
	struct mt6797_spm *spm = s->private;
	unsigned int i;

	for (i = 0; i < MT6797_SPM_FW_COUNT; i++) {
		const struct mt6797_spm_pcm *pcm = &spm->pcm[i];

		if (spm->fw_status[i]) {
			seq_printf(s, "%s status=%d\n", mt6797_spm_fw_name[i],
				   spm->fw_status[i]);
			continue;
		}

		seq_printf(s,
			   "%s status=0 bytes=%zu words=%u size=%u sess=%u replace=%u addr_2nd=%u version=%s\n",
			   mt6797_spm_fw_name[i], spm->fw[i]->size, pcm->words,
			   pcm->size, pcm->sess, pcm->replace, pcm->addr_2nd,
			   pcm->version);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6797_spm_firmware);

static int mt6797_spm_status_show(struct seq_file *s, void *unused)
{
	struct mt6797_spm *spm = s->private;
	unsigned int i, val;
	int ret;

	for (i = 0; i < ARRAY_SIZE(mt6797_spm_status_regs); i++) {
		const struct mt6797_spm_reg *reg = &mt6797_spm_status_regs[i];

		ret = regmap_read(spm->regmap, reg->offset, &val);
		if (ret)
			return ret;
		seq_printf(s, "%-22s 0x%03x 0x%08x\n", reg->name, reg->offset,
			   val);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6797_spm_status);

/*
 * Sample which LITTLE cores sit in WFI and which of those are in SPARK2
 * retention.  The reading CPU is busy and never counts as idle.
 */
static int mt6797_spm_idle_show(struct seq_file *s, void *unused)
{
	u32 wfi[MT6797_SPM_LITTLE_CPUS] = { }, ret[MT6797_SPM_LITTLE_CPUS] = { };
	struct mt6797_spm *spm = s->private;
	u32 samples = clamp(spm->samples, 1U, MT6797_SPM_SAMPLES_MAX);
	u32 sample_us = min(spm->sample_us, MT6797_SPM_SAMPLE_US_MAX);
	unsigned int idle, retention, cpu, n;
	u32 mcusys = 0;
	int err;

	for (n = 0; n < samples; n++) {
		err = regmap_read(spm->regmap, MT6797_SPM_CPU_IDLE_STA, &idle);
		if (err)
			return err;
		err = regmap_read(spm->regmap, MT6797_SPM_CPU_RET_STATUS,
				  &retention);
		if (err)
			return err;

		for (cpu = 0; cpu < MT6797_SPM_LITTLE_CPUS; cpu++) {
			if (idle & MT6797_SPM_IDLE_STA_WFI(cpu))
				wfi[cpu]++;
			if (retention & BIT(cpu))
				ret[cpu]++;
		}
		if (idle & MT6797_SPM_IDLE_STA_MCUSYS)
			mcusys++;

		if (sample_us)
			udelay(sample_us);
		if (!(n % 1000))
			cond_resched();
	}

	seq_printf(s, "samples=%u sample_us=%u mcusys_idle=%u\n", samples,
		   sample_us, mcusys);
	for (cpu = 0; cpu < MT6797_SPM_LITTLE_CPUS; cpu++)
		seq_printf(s, "cpu%u wfi=%u retention=%u\n", cpu, wfi[cpu],
			   ret[cpu]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6797_spm_idle);

static void mt6797_spm_release_firmware(void *data)
{
	struct mt6797_spm *spm = data;
	unsigned int i;

	for (i = 0; i < MT6797_SPM_FW_COUNT; i++)
		release_firmware(spm->fw[i]);
}

static void mt6797_spm_load_firmware(struct mt6797_spm *spm)
{
	unsigned int i, loaded = 0;
	int ret;

	for (i = 0; i < MT6797_SPM_FW_COUNT; i++) {
		ret = request_firmware(&spm->fw[i], mt6797_spm_fw_name[i],
				       spm->dev);
		if (ret) {
			spm->fw[i] = NULL;
			spm->fw_status[i] = ret;
			continue;
		}

		ret = mt6797_spm_pcm_parse(spm->fw[i]->data, spm->fw[i]->size,
					   &spm->pcm[i]);
		if (ret) {
			dev_err(spm->dev, "%s: invalid PCM image: %d\n",
				mt6797_spm_fw_name[i], ret);
			release_firmware(spm->fw[i]);
			spm->fw[i] = NULL;
			spm->fw_status[i] = ret;
			continue;
		}

		dev_dbg(spm->dev, "%s: %s, %u words\n", mt6797_spm_fw_name[i],
			spm->pcm[i].version, spm->pcm[i].words);
		loaded++;
	}

	dev_info(spm->dev, "%u of %u PCM images valid\n", loaded,
		 MT6797_SPM_FW_COUNT);
}

static int mt6797_spm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_spm *spm;
	int ret;

	spm = devm_kzalloc(dev, sizeof(*spm), GFP_KERNEL);
	if (!spm)
		return -ENOMEM;

	spm->dev = dev;
	spm->samples = MT6797_SPM_SAMPLES_DEFAULT;
	spm->sample_us = MT6797_SPM_SAMPLE_US_DEFAULT;

	spm->regmap = syscon_regmap_lookup_by_phandle(dev->of_node,
						      "mediatek,spm");
	if (IS_ERR(spm->regmap))
		return dev_err_probe(dev, PTR_ERR(spm->regmap),
				     "failed to get SPM syscon\n");

	ret = devm_add_action(dev, mt6797_spm_release_firmware, spm);
	if (ret)
		return ret;
	mt6797_spm_load_firmware(spm);

	spm->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("firmware", 0400, spm->debugfs, spm,
			    &mt6797_spm_firmware_fops);
	debugfs_create_file("status", 0400, spm->debugfs, spm,
			    &mt6797_spm_status_fops);
	debugfs_create_file("idle", 0400, spm->debugfs, spm,
			    &mt6797_spm_idle_fops);
	debugfs_create_u32("idle_samples", 0600, spm->debugfs, &spm->samples);
	debugfs_create_u32("idle_sample_us", 0600, spm->debugfs,
			   &spm->sample_us);

	platform_set_drvdata(pdev, spm);

	return 0;
}

static void mt6797_spm_remove(struct platform_device *pdev)
{
	struct mt6797_spm *spm = platform_get_drvdata(pdev);

	debugfs_remove_recursive(spm->debugfs);
}

static const struct of_device_id mt6797_spm_of_match[] = {
	{ .compatible = "mediatek,mt6797-spm" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_spm_of_match);

static struct platform_driver mt6797_spm_driver = {
	.probe = mt6797_spm_probe,
	.remove = mt6797_spm_remove,
	.driver = {
		.name = "mt6797-spm",
		.of_match_table = mt6797_spm_of_match,
	},
};
module_platform_driver(mt6797_spm_driver);

MODULE_FIRMWARE("mediatek/mt6797/pcm_suspend.bin");
MODULE_FIRMWARE("mediatek/mt6797/pcm_suspend_by_mp1.bin");
MODULE_FIRMWARE("mediatek/mt6797/pcm_vcorefs_lpm.bin");
MODULE_FIRMWARE("mediatek/mt6797/pcm_vcorefs_hpm.bin");
MODULE_FIRMWARE("mediatek/mt6797/pcm_vcorefs_ultra.bin");
MODULE_FIRMWARE("mediatek/mt6797/pcm_sodi.bin");
MODULE_FIRMWARE("mediatek/mt6797/pcm_sodi_by_mp1.bin");
MODULE_FIRMWARE("mediatek/mt6797/pcm_deepidle.bin");
MODULE_FIRMWARE("mediatek/mt6797/pcm_deepidle_by_mp1.bin");
MODULE_DESCRIPTION("MediaTek MT6797 SPM firmware and idle status");
MODULE_LICENSE("GPL");
