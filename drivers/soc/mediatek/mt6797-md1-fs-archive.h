/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6797_MD1_FS_ARCHIVE_H
#define _MT6797_MD1_FS_ARCHIVE_H

#define MT6797_FS_ARCHIVE_MAX (16U * 1024 * 1024)
#define MT6797_FS_ARCHIVE_HEADER 16U
#define MT6797_FS_ARCHIVE_ENTRY 140U
#define MT6797_FS_ARCHIVE_FILES 4096U

/* Caller owns an immutable buffer for the entire view lifetime. Format
 * validation does not establish file seals, source identity or authority.
 */
struct mt6797_fs_archive {
	const u8 *data;
	u32 count;
	u32 size;
};

static inline u32 mt6797_fs_archive_u32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline bool mt6797_fs_archive_path(const u8 *p)
{
	unsigned int i, length = 0, component = 0, start = 0;

	while (length < 128 && p[length])
		length++;
	if (!length || length == 128)
		return false;
	for (i = length; i < 128; i++)
		if (p[i])
			return false;
	for (i = 0; i <= length; i++) {
		if (i == length || p[i] == '/') {
			if (!component || (component == 1 && p[start] == '.') ||
			    (component == 2 && p[start] == '.' && p[start + 1] == '.'))
				return false;
			start = i + 1;
			component = 0;
		} else {
			if (p[i] < 32 || p[i] == 127 || p[i] == '\\' || p[i] == ':')
				return false;
			component++;
		}
	}
	/* Strict UTF-8: no overlong scalars, surrogates or >U+10FFFF. */
	for (i = 0; i < length;) {
		u32 c = p[i++], minimum;
		unsigned int extra;

		if (c < 0x80)
			continue;
		if (c >= 0xc2 && c <= 0xdf) {
			c &= 0x1f; extra = 1; minimum = 0x80;
		} else if (c >= 0xe0 && c <= 0xef) {
			c &= 0x0f; extra = 2; minimum = 0x800;
		} else if (c >= 0xf0 && c <= 0xf4) {
			c &= 7; extra = 3; minimum = 0x10000;
		} else {
			return false;
		}
		if (extra > length - i)
			return false;
		while (extra--) {
			if ((p[i] & 0xc0) != 0x80)
				return false;
			c = (c << 6) | (p[i++] & 0x3f);
		}
		if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
			return false;
	}
	return true;
}

/* Process context, no provider lock held. No partial view on failure. */
static inline int mt6797_fs_archive_validate(const u8 *data, size_t size,
					    struct mt6797_fs_archive *view)
{
	u32 count, next, i, j;

	if (!data || !view || size < MT6797_FS_ARCHIVE_HEADER ||
	    size > MT6797_FS_ARCHIVE_MAX || memcmp(data, "MD1BKP1\0", 8))
		return -EINVAL;
	count = mt6797_fs_archive_u32(data + 8);
	if (!count || count > MT6797_FS_ARCHIVE_FILES ||
	    mt6797_fs_archive_u32(data + 12) != size ||
	    count > (size - MT6797_FS_ARCHIVE_HEADER) / MT6797_FS_ARCHIVE_ENTRY)
		return -EINVAL;
	next = MT6797_FS_ARCHIVE_HEADER + count * MT6797_FS_ARCHIVE_ENTRY;
	for (i = 0; i < count; i++) {
		const u8 *entry = data + MT6797_FS_ARCHIVE_HEADER +
				  i * MT6797_FS_ARCHIVE_ENTRY;
		u32 drive = mt6797_fs_archive_u32(entry);
		u32 offset = mt6797_fs_archive_u32(entry + 4);
		u32 length = mt6797_fs_archive_u32(entry + 8);

		if ((drive != 'Z' && drive != 'X' && drive != 'Y') ||
		    !mt6797_fs_archive_path(entry + 12) || offset != next ||
		    length > size - next)
			return -EINVAL;
		for (j = 0; j < i; j++) {
			const u8 *prior = data + MT6797_FS_ARCHIVE_HEADER +
					  j * MT6797_FS_ARCHIVE_ENTRY;

			if (drive == mt6797_fs_archive_u32(prior) &&
			    !memcmp(entry + 12, prior + 12, 128))
				return -EINVAL;
		}
		next += length;
		cond_resched();
	}
	if (next != size)
		return -EINVAL;
	*view = (struct mt6797_fs_archive){ data, count, size };
	return 0;
}

#endif
