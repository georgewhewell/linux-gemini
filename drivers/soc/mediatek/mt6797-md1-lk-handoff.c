// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fail-closed parser for the MT6797 MD1 LK CCCI hand-off.
 *
 * The vendor 3.18 MT6797 implementation reads ccci,modem_info_v2 from
 * /chosen, then maps the explicitly reported tag table. It locates md1_chk
 * and md1img in that table rather than looking for a header in a modem
 * partition:
 *
 * drivers/misc/mediatek/ccci_util/ccci_util_lib_fo.c:74-96, 295-355,
 * 755-786, 845-889
 *
 * This file has no Device Tree, physical-memory, firmware, block I/O, MMIO,
 * DMA, clock, reset, power, interrupt, CCCI, SIM or RF access. It only
 * rejects malformed copies supplied by a separately-gated acquisition owner.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define MT6797_MD1_LK_INFO_TABLE_PHYS_OFFSET	0
#define MT6797_MD1_LK_INFO_TABLE_SIZE_OFFSET	8
#define MT6797_MD1_LK_INFO_ERROR_OFFSET	12
#define MT6797_MD1_LK_INFO_VERSION_OFFSET	16
#define MT6797_MD1_LK_INFO_TAG_COUNT_OFFSET	20
#define MT6797_MD1_LK_INFO_LOAD_FLAGS_OFFSET	24
#define MT6797_MD1_LK_INFO_MD_ERROR_OFFSET	28

#define MT6797_MD1_LK_TAG_DATA_OFFSET		64
#define MT6797_MD1_LK_TAG_DATA_SIZE_OFFSET	68
#define MT6797_MD1_LK_TAG_NEXT_OFFSET		72

#define MT6797_MD1_LK_MODEM_INFO_BASE_OFFSET	0
#define MT6797_MD1_LK_MODEM_INFO_SIZE_OFFSET	8
#define MT6797_MD1_LK_MODEM_INFO_MD_ID_OFFSET	12
#define MT6797_MD1_LK_MODEM_INFO_ERROR_OFFSET	13
#define MT6797_MD1_LK_MODEM_INFO_TYPE_OFFSET	14
#define MT6797_MD1_LK_MODEM_INFO_VERSION_OFFSET	15

#define MT6797_MD1_LK_SMEM_BASE_OFFSET		0
#define MT6797_MD1_LK_SMEM_AP_MD1_OFFSET	8
#define MT6797_MD1_LK_SMEM_AP_MD1_SIZE		12
#define MT6797_MD1_LK_SMEM_AP_MD3_OFFSET	16
#define MT6797_MD1_LK_SMEM_AP_MD3_SIZE		20
#define MT6797_MD1_LK_SMEM_MD1_MD3_OFFSET	24
#define MT6797_MD1_LK_SMEM_MD1_MD3_SIZE		28
#define MT6797_MD1_LK_SMEM_TOTAL_SIZE		32

static bool mt6797_md1_lk_range_valid(u32 offset, u32 length, size_t limit)
{
	u32 end;

	return !check_add_overflow(offset, length, &end) && end <= limit;
}

int mt6797_md1_lk_info_parse_v2(const void *data, size_t size,
				struct mt6797_md1_lk_info *info)
{
	const u8 *raw = data;
	struct mt6797_md1_lk_info candidate;
	u32 i;

	if (!raw || !info)
		return -EINVAL;
	if (size != MT6797_MD1_LK_INFO_V2_SIZE)
		return -EMSGSIZE;

	memset(&candidate, 0, sizeof(candidate));
	candidate.table_phys = get_unaligned_le64(raw +
		MT6797_MD1_LK_INFO_TABLE_PHYS_OFFSET);
	candidate.table_size = get_unaligned_le32(raw +
		MT6797_MD1_LK_INFO_TABLE_SIZE_OFFSET);
	candidate.error = (s32)get_unaligned_le32(raw +
		MT6797_MD1_LK_INFO_ERROR_OFFSET);
	candidate.version = get_unaligned_le32(raw +
		MT6797_MD1_LK_INFO_VERSION_OFFSET);
	candidate.tag_count = get_unaligned_le32(raw +
		MT6797_MD1_LK_INFO_TAG_COUNT_OFFSET);
	candidate.load_flags = get_unaligned_le32(raw +
		MT6797_MD1_LK_INFO_LOAD_FLAGS_OFFSET);
	for (i = 0; i < MT6797_MD1_LK_INFO_MD_COUNT; i++)
		candidate.md_error[i] = (s32)get_unaligned_le32(raw +
			MT6797_MD1_LK_INFO_MD_ERROR_OFFSET + i * sizeof(u32));

	if (!candidate.table_phys || !candidate.table_size ||
	    candidate.table_size > MT6797_MD1_LK_INFO_MAX_SIZE ||
	    candidate.version != MT6797_MD1_LK_INFO_V2_VERSION ||
	    !candidate.tag_count ||
	    candidate.tag_count > MT6797_MD1_LK_TAG_MAX_COUNT ||
	    candidate.tag_count > candidate.table_size / MT6797_MD1_LK_TAG_V2_SIZE)
		return -EINVAL;

	*info = candidate;
	return 0;
}

static int mt6797_md1_lk_tag_at(const u8 *data, size_t size, u32 offset,
				struct mt6797_md1_lk_tag *tag, const char **tag_name)
{
	const u8 *raw;

	if (!mt6797_md1_lk_range_valid(offset, MT6797_MD1_LK_TAG_V2_SIZE,
				       size))
		return -ERANGE;
	raw = data + offset;
	/* The vendor code treats this as a C string; require its terminator. */
	if (strnlen(raw, MT6797_MD1_LK_TAG_NAME_SIZE) ==
	    MT6797_MD1_LK_TAG_NAME_SIZE)
		return -EBADMSG;

	tag->data_offset = get_unaligned_le32(raw + MT6797_MD1_LK_TAG_DATA_OFFSET);
	tag->data_size = get_unaligned_le32(raw + MT6797_MD1_LK_TAG_DATA_SIZE_OFFSET);
	tag->next_tag_offset = get_unaligned_le32(raw + MT6797_MD1_LK_TAG_NEXT_OFFSET);
	if (!mt6797_md1_lk_range_valid(tag->data_offset, tag->data_size, size))
		return -ERANGE;
	*tag_name = raw;
	return 0;
}

int mt6797_md1_lk_tag_find_v2(const void *data, size_t size, u32 tag_count,
			      const char *name, struct mt6797_md1_lk_tag *tag)
{
	const u8 *raw = data;
	const char *tag_name;
	struct mt6797_md1_lk_tag current;
	u32 offset = 0;
	u32 i;
	int ret;

	if (!raw || !name || !tag || !tag_count ||
	    tag_count > MT6797_MD1_LK_TAG_MAX_COUNT ||
	    size > MT6797_MD1_LK_INFO_MAX_SIZE ||
	    strnlen(name, MT6797_MD1_LK_TAG_NAME_SIZE) ==
		MT6797_MD1_LK_TAG_NAME_SIZE)
		return -EINVAL;

	for (i = 0; i < tag_count; i++) {
		ret = mt6797_md1_lk_tag_at(raw, size, offset, &current, &tag_name);
		if (ret)
			return ret;
		if (!strcmp(tag_name, name)) {
			*tag = current;
			return 0;
		}
		if (i + 1 == tag_count)
			break;
		if (!mt6797_md1_lk_range_valid(current.next_tag_offset,
					       MT6797_MD1_LK_TAG_V2_SIZE, size))
			return -ERANGE;
		offset = current.next_tag_offset;
	}

	return -ENOENT;
}

int mt6797_md1_lk_modem_info_find(const void *data, size_t size, u8 md_id,
				   struct mt6797_md1_lk_modem_info *info)
{
	const u8 *raw = data;
	struct mt6797_md1_lk_modem_info candidate;
	u32 i;

	if (!raw || !info || md_id >= MT6797_MD1_LK_INFO_MD_COUNT ||
	    !size || size % MT6797_MD1_LK_MODEM_INFO_SIZE ||
	    size / MT6797_MD1_LK_MODEM_INFO_SIZE > MT6797_MD1_LK_INFO_MD_COUNT)
		return -EINVAL;

	for (i = 0; i < size / MT6797_MD1_LK_MODEM_INFO_SIZE; i++) {
		const u8 *entry = raw + i * MT6797_MD1_LK_MODEM_INFO_SIZE;

		if (entry[MT6797_MD1_LK_MODEM_INFO_MD_ID_OFFSET] != md_id)
			continue;
		memset(&candidate, 0, sizeof(candidate));
		candidate.base_phys = get_unaligned_le64(entry +
			MT6797_MD1_LK_MODEM_INFO_BASE_OFFSET);
		candidate.size = get_unaligned_le32(entry +
			MT6797_MD1_LK_MODEM_INFO_SIZE_OFFSET);
		candidate.md_id = entry[MT6797_MD1_LK_MODEM_INFO_MD_ID_OFFSET];
		candidate.load_error = entry[MT6797_MD1_LK_MODEM_INFO_ERROR_OFFSET];
		candidate.md_type = entry[MT6797_MD1_LK_MODEM_INFO_TYPE_OFFSET];
		candidate.version = entry[MT6797_MD1_LK_MODEM_INFO_VERSION_OFFSET];
		if (!candidate.base_phys || !candidate.size)
			return -EINVAL;
		*info = candidate;
		return 0;
	}

	return -ENOENT;
}

int mt6797_md1_lk_smem_layout_parse(const void *data, size_t size,
					    struct mt6797_md1_lk_smem_layout *layout)
{
	const u8 *raw = data;
	struct mt6797_md1_lk_smem_layout candidate;

	if (!raw || !layout)
		return -EINVAL;
	if (size != MT6797_MD1_LK_SMEM_LAYOUT_SIZE)
		return -EMSGSIZE;

	memset(&candidate, 0, sizeof(candidate));
	candidate.base_phys = get_unaligned_le64(raw + MT6797_MD1_LK_SMEM_BASE_OFFSET);
	candidate.ap_md1_offset = get_unaligned_le32(raw + MT6797_MD1_LK_SMEM_AP_MD1_OFFSET);
	candidate.ap_md1_size = get_unaligned_le32(raw + MT6797_MD1_LK_SMEM_AP_MD1_SIZE);
	candidate.ap_md3_offset = get_unaligned_le32(raw + MT6797_MD1_LK_SMEM_AP_MD3_OFFSET);
	candidate.ap_md3_size = get_unaligned_le32(raw + MT6797_MD1_LK_SMEM_AP_MD3_SIZE);
	candidate.md1_md3_offset = get_unaligned_le32(raw + MT6797_MD1_LK_SMEM_MD1_MD3_OFFSET);
	candidate.md1_md3_size = get_unaligned_le32(raw + MT6797_MD1_LK_SMEM_MD1_MD3_SIZE);
	candidate.total_size = get_unaligned_le32(raw + MT6797_MD1_LK_SMEM_TOTAL_SIZE);
	if (!candidate.base_phys || !candidate.total_size ||
	    !mt6797_md1_lk_range_valid(candidate.ap_md1_offset,
					 candidate.ap_md1_size, candidate.total_size) ||
	    !mt6797_md1_lk_range_valid(candidate.ap_md3_offset,
					 candidate.ap_md3_size, candidate.total_size) ||
	    !mt6797_md1_lk_range_valid(candidate.md1_md3_offset,
					 candidate.md1_md3_size, candidate.total_size))
		return -ERANGE;

	*layout = candidate;
	return 0;
}

int mt6797_md1_lk_md1_load_evidence(
	const struct mt6797_md1_lk_info *lk_info,
	const struct mt6797_md1_lk_modem_info *md1,
	enum mt6797_md1_lk_md1_load_evidence *evidence)
{
	if (!lk_info || !md1 || !evidence ||
	    md1->md_id != MT6797_MD1_LK_MD1_INDEX)
		return -EINVAL;

	/*
	 * LK records the flag after image-load success, but platform configuration
	 * can still fail afterwards.  Any global or MD1 error wins over the flag.
	 */
	if (lk_info->error || lk_info->md_error[MT6797_MD1_LK_MD1_INDEX] ||
	    md1->load_error) {
		*evidence = MT6797_MD1_LK_MD1_LOAD_FAILED;
		return -EIO;
	}

	if (!(lk_info->load_flags & MT6797_MD1_LK_MD1_LOAD_FLAG)) {
		*evidence = MT6797_MD1_LK_MD1_LOAD_NONE;
		return -ENODATA;
	}

	/*
	 * Nothing in this hand-off identifies whether LK later released the boot
	 * vector.  Deliberately preserve that missing state as an explicit result.
	 */
	*evidence = MT6797_MD1_LK_MD1_LOAD_EXECUTION_UNKNOWN;
	return 0;
}
