// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 SPM PCM firmware image parser.
 *
 * The MediaTek 3.18 kernel loads the SPM scenario programs with
 * request_firmware() and parses them in spm_load_pcm_firmware()
 * (spm_v2/mt_spm.c).  An image is:
 *
 *	le16	words			instruction count
 *	le32	code[words]		PCM instructions
 *	struct pcm_desc from .size:	le16 size, u8 sess, u8 replace,
 *					le16 addr_2nd, le16 reserved,
 *					le32 vec[16]
 *	char	version[]		NUL-terminated, ends the file
 *
 * The vendor parser trusts all of this.  Here every field is bounded by the
 * file size and by the 8 KiB carveout slot the vendor copies each image into,
 * the version must be terminated, every enabled event vector must enter the
 * image and nothing may follow the version.
 */

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "mt6797-spm-fw.h"

/* spm_v2/mt_spm.c dyna_load_pcm_path[] for CONFIG_ARCH_MT6797 */
const char *const mt6797_spm_fw_name[MT6797_SPM_FW_COUNT] = {
	[MT6797_SPM_FW_SUSPEND] = "mediatek/mt6797/pcm_suspend.bin",
	[MT6797_SPM_FW_SUSPEND_BY_MP1] = "mediatek/mt6797/pcm_suspend_by_mp1.bin",
	[MT6797_SPM_FW_VCOREFS_LPM] = "mediatek/mt6797/pcm_vcorefs_lpm.bin",
	[MT6797_SPM_FW_VCOREFS_HPM] = "mediatek/mt6797/pcm_vcorefs_hpm.bin",
	[MT6797_SPM_FW_VCOREFS_ULTRA] = "mediatek/mt6797/pcm_vcorefs_ultra.bin",
	[MT6797_SPM_FW_SODI] = "mediatek/mt6797/pcm_sodi.bin",
	[MT6797_SPM_FW_SODI_BY_MP1] = "mediatek/mt6797/pcm_sodi_by_mp1.bin",
	[MT6797_SPM_FW_DEEPIDLE] = "mediatek/mt6797/pcm_deepidle.bin",
	[MT6797_SPM_FW_DEEPIDLE_BY_MP1] = "mediatek/mt6797/pcm_deepidle_by_mp1.bin",
};

int mt6797_spm_pcm_parse(const u8 *data, size_t len,
			 struct mt6797_spm_pcm *pcm)
{
	size_t code_len, off, ver_len;
	const u8 *nul;
	unsigned int i;

	memset(pcm, 0, sizeof(*pcm));

	if (len < 2)
		return -EINVAL;

	pcm->words = get_unaligned_le16(data);
	if (!pcm->words || pcm->words > MT6797_SPM_PCM_MAX_WORDS)
		return -EFBIG;

	code_len = (size_t)pcm->words * 4;
	off = 2 + code_len;
	if (len < off + MT6797_SPM_PCM_DESC_SIZE + 1)
		return -EINVAL;
	pcm->code = data + 2;

	pcm->size = get_unaligned_le16(data + off);
	pcm->sess = data[off + 2];
	pcm->replace = data[off + 3];
	pcm->addr_2nd = get_unaligned_le16(data + off + 4);
	for (i = 0; i < MT6797_SPM_PCM_VECTORS; i++)
		pcm->vec[i] = get_unaligned_le32(data + off + 8 + 4 * i);
	off += MT6797_SPM_PCM_DESC_SIZE;

	/* __spm_kick_im_to_fetch() programs PCM_IM_LEN = size - 1 */
	if (!pcm->size || pcm->size > pcm->words)
		return -EINVAL;
	if (pcm->addr_2nd > pcm->size)
		return -EINVAL;
	for (i = 0; i < MT6797_SPM_PCM_VECTORS; i++)
		if (pcm->vec[i] &&
		    MT6797_SPM_PCM_VEC_PC(pcm->vec[i]) >= pcm->size)
			return -EINVAL;

	nul = memchr(data + off, '\0', len - off);
	if (!nul)
		return -EINVAL;
	ver_len = nul - (data + off);
	if (!ver_len || ver_len >= sizeof(pcm->version) ||
	    nul + 1 != data + len)
		return -EINVAL;
	memcpy(pcm->version, data + off, ver_len);

	return 0;
}

/*
 * The vendor picks the image by the CPU that enters the scenario:
 * DYNA_LOAD_PCM_<SCENARIO> + cpu / 4 (mt_spm_sleep.c spm_go_to_sleep(),
 * mt_spm_sodi.c spm_go_to_sodi()).  The *_by_mp1 images differ only in
 * addressing MP1_CPUSYS_PWR_CON/MP1_CPU0_PWR_CON instead of the MP0 pair,
 * so they serve only the LITTLE clusters.
 */
int mt6797_spm_fw_select(enum mt6797_spm_scenario scenario, unsigned int cpu)
{
	int base;

	switch (scenario) {
	case MT6797_SPM_SCENARIO_SUSPEND:
		base = MT6797_SPM_FW_SUSPEND;
		break;
	case MT6797_SPM_SCENARIO_SODI:
		base = MT6797_SPM_FW_SODI;
		break;
	case MT6797_SPM_SCENARIO_DEEPIDLE:
		base = MT6797_SPM_FW_DEEPIDLE;
		break;
	default:
		return -EINVAL;
	}

	if (cpu >= 8)
		return -EINVAL;

	return base + cpu / 4;
}
