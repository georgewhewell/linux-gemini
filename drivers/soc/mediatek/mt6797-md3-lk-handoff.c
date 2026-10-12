// SPDX-License-Identifier: GPL-2.0-only
/* Optional MD3 image metadata, acquired before MD1 clears the LK tag table.
 * Wire definitions: public MT6797 ccci_util_lib_fo.c and mt_ccci_common.h.
 * No MMIO, memory mapping, power, reset, firmware loading or modem response.
 */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/soc/mediatek/mt6797-md3-lk-handoff.h>
#include <linux/string.h>
#include <linux/unaligned.h>

enum md3_tag {
	MD3_COUNT, MD3_TABLE, MD3_SMEM, MD3_CHECK, MD3_IMAGE,
	MD3_GATE, MD3_SUPPORT, MD3_C2K_MODE, MD3_MPU, MD3_TAGS,
};

static const char * const mt6797_md3_tag_names[MD3_TAGS] = {
	"hdr_count", "hdr_tbl_inf", "smem_layout", "md3_chk", "md3img",
	"opt_using_lk_val", "opt_md3_support", "opt_c2k_lte_mode",
	"md_mpu_inf",
};

/* Exact f4988d74 MT6797 LK defaults. ROM/RW are secure-locked, shared
 * regions are not. These are LK handoff values, not the later kernel's
 * alternative D3 permissions for AP3. Re-SET of locked ROM/RW is forbidden.
 */
#define MD3_LK_FORBIDDEN_COMMON ((5U << 21) | (5U << 18) | (5U << 12) | \
			       (5U << 9) | (5U << 6))
#define MD3_LK_ROM_RW (BIT(26) | MD3_LK_FORBIDDEN_COMMON | (5U << 3) | 4U)
#define MD3_LK_AP3 (MD3_LK_FORBIDDEN_COMMON | (5U << 3))
#define MD3_LK_INTER MD3_LK_FORBIDDEN_COMMON

static int mt6797_md3_mpu_expected(const struct mt6797_md3_lk_snapshot *s,
				 struct mt6797_md3_lk_mpu_entry expected[4])
{
	u64 base = s->modem.base_phys;
	u64 ap3 = s->smem.base_phys + s->smem.ap_md3_offset;
	u64 inter = s->smem.base_phys + s->smem.md1_md3_offset;
	u32 rom = s->header_image_size, ram = s->header_mem_size;

	/* The retained v1 LK assistant uses these header sizes for MPU, even
	 * though the kernel v1 firmware loader ignores them for image copying.
	 * Admit the exact aligned, whole reservation case only.
	 */
	if (s->modem.base_phys > U32_MAX || s->smem.base_phys > U32_MAX ||
	    !rom || rom >= ram || ram != s->modem.size ||
	    ((base | rom | ram | ap3 | inter | s->smem.ap_md3_size |
	      s->smem.md1_md3_size) & 0xffff) ||
	    !s->smem.ap_md3_size || !s->smem.md1_md3_size ||
	    base + ram > U32_MAX || ap3 + s->smem.ap_md3_size > U32_MAX ||
	    inter + s->smem.md1_md3_size != ap3)
		return -ERANGE;
	expected[0] = (struct mt6797_md3_lk_mpu_entry) {
		ap3, ap3 + s->smem.ap_md3_size - 1, 8, MD3_LK_AP3, 0 };
	expected[1] = (struct mt6797_md3_lk_mpu_entry) {
		inter, inter + s->smem.md1_md3_size - 1, 9, MD3_LK_INTER, 0 };
	expected[2] = (struct mt6797_md3_lk_mpu_entry) {
		base, base + rom - 1, 16, MD3_LK_ROM_RW, 0 };
	expected[3] = (struct mt6797_md3_lk_mpu_entry) {
		base + rom, base + ram - 1, 17, MD3_LK_ROM_RW, 0 };
	return 0;
}

static int mt6797_md3_mpu_parse(const u8 *data, u32 bytes,
			      struct mt6797_md3_lk_snapshot *out)
{
	static const u32 regions[] = { 8, 9, 16, 17 };
	struct mt6797_md3_lk_mpu_entry expected[4], entry, records[4] = {};
	u32 seen = 0, selected = 0, i, j;
	int ret;

	if (!bytes)
		return -ENOENT;
	/* LK caps the complete MD1+MD3 table at sixteen 20-byte records. The
	 * md_mpu_num tag has a pointer/value bug there; use this tag's length.
	 */
	if (bytes % 20 || bytes > 16 * 20)
		return -EMSGSIZE;
	for (i = 0; i < bytes; i += 20) {
		entry.start = get_unaligned_le32(data + i);
		entry.end = get_unaligned_le32(data + i + 4);
		entry.region = get_unaligned_le32(data + i + 8);
		entry.permission = get_unaligned_le32(data + i + 12);
		entry.relate_region = get_unaligned_le32(data + i + 16);
		if (entry.region > 31)
			return -ERANGE;
		if (seen & BIT(entry.region))
			return -EEXIST;
		seen |= BIT(entry.region);
		for (j = 0; j < ARRAY_SIZE(regions); j++) {
			if (entry.region != regions[j])
				continue;
			records[j] = entry;
			selected |= BIT(j);
		}
	}
	/* Preserve the actual unambiguous records even when the expected LK
	 * policy does not match. No hardware ownership follows from this copy.
	 * Structural errors above publish neither partial nor duplicate data.
	 */
	out->mpu_record_count = bytes / 20;
	out->mpu_present_mask = selected;
	memcpy(out->mpu, records, sizeof(records));
	if (selected != 15)
		return -ENODATA;
	ret = mt6797_md3_mpu_expected(out, expected);
	if (ret)
		return ret;
	if (memcmp(records, expected, sizeof(records)))
		return -EUCLEAN;
	return 0;
}

int mt6797_md3_lk_memory_attest(const struct mt6797_md3_lk_snapshot *s,
			      const u32 banks[4], u32 lock)
{
	struct mt6797_md3_lk_mpu_entry expected[4];
	u64 bases[2];
	u32 mode, bank, word, slot, value;
	bool match;
	int ret;

	if (!s || !banks)
		return -EINVAL;
	if (s->mpu_error)
		return s->mpu_error;
	if (!(lock & BIT(17)))
		return -EUCLEAN;
	ret = mt6797_md3_mpu_expected(s, expected);
	if (ret)
		return ret;
	if (memcmp(expected, s->mpu, sizeof(expected)))
		return -EUCLEAN;
	bases[0] = s->modem.base_phys;
	bases[1] = s->smem.base_phys + s->smem.md1_md3_offset;
	/* Exact LK's two DDR branches, both banks must choose the same one.
	 * Kernel fallback's invalid slots differ and are not adopted as LK.
	 */
	for (mode = 0; mode < 2; mode++) {
		match = true;
		for (bank = 0; bank < 2; bank++) {
			u64 base = bases[bank];

			if (mode) {
				if (base < 0x40000000ULL)
					return -ERANGE;
				base -= 0x40000000ULL;
			}
			if (base + 0xe000000ULL > U32_MAX)
				return -ERANGE;
			for (word = 0; word < 2; word++) {
				value = 0;
				for (slot = 0; slot < 4; slot++)
					value |= ((((base + (word * 4 + slot) *
						     0x2000000ULL) >> 24) | 1) & 0xff) <<
						 (slot * 8);
				if (banks[bank * 2 + word] != value)
					match = false;
			}
		}
		if (match)
			return 0;
	}
	return -EUCLEAN;
}
EXPORT_SYMBOL_GPL(mt6797_md3_lk_memory_attest);

static bool mt6797_md3_range(u32 offset, u32 size, size_t limit)
{
	u32 end;

	return !check_add_overflow(offset, size, &end) && end <= limit;
}

static bool mt6797_md3_overlap(u32 a, u32 as, u32 b, u32 bs)
{
	return (u64)a < (u64)b + bs && (u64)b < (u64)a + as;
}

/* Full bounded walk: reject duplicate relevant tags, repeated/overlapping
 * link headers and payload aliases. Unknown tags remain supported. Walking
 * prior links avoids a large stack bitmap; LK caps the entire table at 64 KiB.
 */
static int mt6797_md3_tags(const u8 *table, size_t size, u32 count,
			 struct mt6797_md1_lk_tag tags[MD3_TAGS])
{
	u32 i, j, offset = 0, prior, payload, bytes;
	unsigned int seen = 0;
	const u8 *tag;

	for (i = 0; i < count; i++) {
		if (!mt6797_md3_range(offset, MT6797_MD1_LK_TAG_V2_SIZE, size))
			return -ERANGE;
		prior = 0;
		for (j = 0; j < i; j++) {
			if (mt6797_md3_overlap(offset, MT6797_MD1_LK_TAG_V2_SIZE,
					       prior, MT6797_MD1_LK_TAG_V2_SIZE))
				return -ELOOP;
			prior = get_unaligned_le32(table + prior + 72);
		}
		tag = table + offset;
		if (strnlen(tag, MT6797_MD1_LK_TAG_NAME_SIZE) == MT6797_MD1_LK_TAG_NAME_SIZE)
			return -EBADMSG;
		payload = get_unaligned_le32(tag + 64);
		bytes = get_unaligned_le32(tag + 68);
		if (!mt6797_md3_range(payload, bytes, size))
			return -ERANGE;
		for (j = 0; j < MD3_TAGS; j++) {
			if (strcmp(tag, mt6797_md3_tag_names[j]))
				continue;
			if (seen & BIT(j))
				return -EEXIST;
			seen |= BIT(j);
			tags[j].data_offset = payload;
			tags[j].data_size = bytes;
		}
		offset = get_unaligned_le32(tag + 72);
	}
	if ((seen & GENMASK(MD3_IMAGE, MD3_COUNT)) != GENMASK(MD3_IMAGE, MD3_COUNT))
		return -ENOENT;
	for (i = 0; i < MD3_TAGS; i++) {
		if (!(seen & BIT(i)))
			continue;
		if (!tags[i].data_size)
			return -EMSGSIZE;
		for (j = 0; j < i; j++)
			if ((seen & BIT(j)) && mt6797_md3_overlap(tags[i].data_offset,
				tags[i].data_size, tags[j].data_offset, tags[j].data_size))
				return -EINVAL;
		offset = 0;
		for (j = 0; j < count; j++) {
			if (mt6797_md3_overlap(tags[i].data_offset, tags[i].data_size,
					       offset, MT6797_MD1_LK_TAG_V2_SIZE))
				return -EINVAL;
			offset = get_unaligned_le32(table + offset + 72);
		}
	}
	return 0;
}

int mt6797_md3_lk_snapshot_parse(const struct mt6797_md1_lk_info *info,
	const void *table, size_t size,
	const struct mt6797_md3_lk_contract *contract,
	struct mt6797_md3_lk_snapshot *snapshot)
{
	struct mt6797_md1_lk_tag tags[MD3_TAGS] = {};
	struct mt6797_md3_lk_snapshot out = {};
	const u8 *raw = table, *records, *header;
	u64 end, smem_end, ap3_base, inter_end;
	u32 count, ids = 0, i;
	int ret;

	if (!info || !raw || !contract || !snapshot ||
	    !size || size != info->table_size || size > MT6797_MD1_LK_INFO_MAX_SIZE ||
	    info->version != MT6797_MD1_LK_INFO_V2_VERSION || !info->tag_count ||
	    info->tag_count > size / MT6797_MD1_LK_TAG_V2_SIZE)
		return -EINVAL;
	if (info->error || info->md_error[MT6797_MD3_LK_INDEX])
		return -EIO;
	if (!(info->load_flags & BIT(MT6797_MD3_LK_INDEX)))
		return -ENODATA;
	if (!contract->md_base || !contract->smem_base ||
	    !contract->md_size || !contract->smem_size ||
	    check_add_overflow(contract->md_base, (u64)contract->md_size, &end) ||
	    check_add_overflow(contract->smem_base, (u64)contract->smem_size, &smem_end) ||
	    (contract->md_base < smem_end && contract->smem_base < end))
		return -ERANGE;
	ret = mt6797_md3_tags(raw, size, info->tag_count, tags);
	if (ret)
		return ret;
	if (tags[MD3_COUNT].data_size != sizeof(u32) ||
	    tags[MD3_IMAGE].data_size != sizeof(u32) ||
	    tags[MD3_CHECK].data_size != MT6797_MD3_CHECK_HEADER_SIZE)
		return -EMSGSIZE;
	count = get_unaligned_le32(raw + tags[MD3_COUNT].data_offset);
	if (!count || count > MT6797_MD1_LK_INFO_MD_COUNT ||
	    tags[MD3_TABLE].data_size != count * MT6797_MD1_LK_MODEM_INFO_SIZE)
		return -EMSGSIZE;
	records = raw + tags[MD3_TABLE].data_offset;
	for (i = 0; i < count; i++) {
		u8 id = records[i * MT6797_MD1_LK_MODEM_INFO_SIZE + 12];

		if (id >= MT6797_MD1_LK_INFO_MD_COUNT || (ids & BIT(id)))
			return -EEXIST;
		ids |= BIT(id);
	}
	ret = mt6797_md1_lk_modem_info_find(records, tags[MD3_TABLE].data_size,
					 MT6797_MD3_LK_INDEX, &out.modem);
	if (ret)
		return ret;
	if (out.modem.load_error)
		return -EIO;
	if (out.modem.base_phys != contract->md_base || out.modem.size != contract->md_size)
		return -ERANGE;
	/* Exact board firmware: MD_SYS3 binds 3, legacy 3G/C2K load type 2. */
	if (out.modem.md_type != 2)
		return -EOPNOTSUPP;
	ret = mt6797_md1_lk_smem_layout_parse(raw + tags[MD3_SMEM].data_offset,
					   tags[MD3_SMEM].data_size, &out.smem);
	if (ret)
		return ret;
	if (out.smem.base_phys != contract->smem_base ||
	    out.smem.total_size != contract->smem_size ||
	    !out.smem.ap_md3_size || !out.smem.md1_md3_size)
		return -ERANGE;
	ap3_base = out.smem.base_phys + out.smem.ap_md3_offset;
	inter_end = out.smem.base_phys + out.smem.md1_md3_offset + out.smem.md1_md3_size;
	if (ap3_base != inter_end ||
	    ((contract->md_base | contract->md_size | ap3_base | out.smem.ap_md3_size |
	      out.smem.md1_md3_offset | out.smem.md1_md3_size) & 0xffff))
		return -ERANGE;
	header = raw + tags[MD3_CHECK].data_offset;
	if (memcmp(header, "CHECK_HEADER", 12) ||
	    get_unaligned_le32(header + 184) != MT6797_MD3_CHECK_HEADER_SIZE ||
	    memcmp(header + 24, "MT6797", 6) ||
	    strnlen(header + 24, 16) == 16 || header[168] != 3)
		return -EBADMSG;
	out.header_version = get_unaligned_le32(header + 12);
	out.image_type = get_unaligned_le32(header + 20);
	if ((out.header_version != 1 && out.header_version != 2) || out.image_type != 2)
		return -EOPNOTSUPP;
	out.raw_image_size = get_unaligned_le32(raw + tags[MD3_IMAGE].data_offset);
	out.header_mem_size = get_unaligned_le32(header + 172);
	out.header_image_size = get_unaligned_le32(header + 176);
	if (out.raw_image_size < MT6797_MD3_CHECK_HEADER_SIZE ||
	    out.raw_image_size > out.modem.size)
		return -ERANGE;
	out.effective_image_size = out.raw_image_size;
	/* Public v1 loader deliberately ignores the v2-only size fields. */
	if (out.header_version == 2) {
		if (!out.header_mem_size || out.header_mem_size > out.modem.size ||
		    out.header_image_size > out.raw_image_size)
			return -ERANGE;
		if (out.header_image_size)
			out.effective_image_size = out.header_image_size;
	}
	for (i = MD3_GATE; i <= MD3_C2K_MODE; i++)
		if (tags[i].data_size && tags[i].data_size != sizeof(u32))
			return -EMSGSIZE;
	if (tags[MD3_GATE].data_size) {
		out.options.gate_present = true;
		out.options.gate = (s32)get_unaligned_le32(raw + tags[MD3_GATE].data_offset);
	}
	if (tags[MD3_SUPPORT].data_size) {
		out.options.support_present = true;
		out.options.support = (s32)get_unaligned_le32(raw + tags[MD3_SUPPORT].data_offset);
	}
	if (tags[MD3_C2K_MODE].data_size) {
		out.c2k_lte_mode_present = true;
		out.c2k_lte_mode = (s32)get_unaligned_le32(raw + tags[MD3_C2K_MODE].data_offset);
	}
	memcpy(out.check_header, header, sizeof(out.check_header));
	out.mpu_error = mt6797_md3_mpu_parse(raw + tags[MD3_MPU].data_offset,
					 tags[MD3_MPU].data_size, &out);
	*snapshot = out;
	return 0;
}
