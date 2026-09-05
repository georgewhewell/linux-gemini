/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_FS_H
#define _MTK_MT6797_CCCI_FS_H

#define MT6797_FS_RX 14U
#define MT6797_FS_TX 15U
#define MT6797_FS_GETDISKINFO 0x100eU
#define MT6797_FS_REPLY_SIZE 120U
#define MT6797_FS_GETATTRIBUTES 0x1010U
#define MT6797_FS_ATTR_REPLY_SIZE 32U
#define MT6797_FS_NAME_MAX 255U

struct mt6797_fs_attr_request {
	u32 index;
	char name[MT6797_FS_NAME_MAX + 1];
};

/* Only a direct child of the retained Z root. Unsupported paths stay captured
 * rather than pretending a broader filesystem operation was performed.
 */
static inline int mt6797_fs_attr_parse(const u8 *data, size_t len,
			       struct mt6797_fs_attr_request *request)
{
	u32 bytes, index;
	size_t chars, i;

	if (!data || !request || len < 28 || len > 3584)
		return -EMSGSIZE;
	if (mt6797_rpc_read(data) || mt6797_rpc_read(data + 4) != len ||
	    (mt6797_rpc_read(data + 8) & 0xffff) != MT6797_FS_RX)
		return -EPROTO;
	if (mt6797_rpc_read(data + 16) != MT6797_FS_GETATTRIBUTES)
		return -EOPNOTSUPP;
	index = mt6797_rpc_read(data + 12);
	if (index >= 5 || mt6797_rpc_read(data + 20) != 1)
		return -EPROTO;
	bytes = mt6797_rpc_read(data + 24);
	if (bytes < 10 || bytes > 2 * (MT6797_FS_NAME_MAX + 4) ||
	    (bytes & 1) || ((bytes + 3) & ~3U) != len - 28)
		return -EMSGSIZE;
	data += 28;
	if (data[0] != 'Z' || data[1] || data[2] != ':' || data[3] ||
	    data[4] != '\\' || data[5] || data[bytes - 2] || data[bytes - 1])
		return -EOPNOTSUPP;
	chars = bytes / 2 - 4;
	for (i = 0; i < chars; i++) {
		u8 c = data[6 + i * 2];

		if (data[7 + i * 2] || c < 32 || c > 126 ||
		    c == '/' || c == '\\' || c == ':')
			return -EOPNOTSUPP;
		request->name[i] = c;
	}
	request->name[chars] = 0;
	if (!strcmp(request->name, ".") || !strcmp(request->name, ".."))
		return -EOPNOTSUPP;
	request->index = index;
	return 0;
}

/* Installed Android25 helper11980: EMFILE/EACCES/ENOENT, otherwise general
 * failure. A nonnegative result is the actual attribute mask, not status+data.
 */
static inline s32 mt6797_fs_attr_result(int error, unsigned int mode)
{
	if (error) {
		switch (error) {
		case -EMFILE: return -5;
		case -EACCES: return -16;
		case -ENOENT: return -9;
		default: return -18;
		}
	}
	return (!(mode & 0333) && (mode & 0444) ? 1 : 0) |
		((mode & 0170000) == 0040000 ? 0x10 : 0);
}

static inline void mt6797_fs_attr_reply(u32 index, s32 result, u8 *reply)
{
	memset(reply, 0, MT6797_FS_ATTR_REPLY_SIZE);
	mt6797_rpc_write(reply + 4, MT6797_FS_ATTR_REPLY_SIZE);
	mt6797_rpc_write(reply + 8, MT6797_FS_TX);
	mt6797_rpc_write(reply + 12, index);
	mt6797_rpc_write(reply + 16, 0xffff0000U | MT6797_FS_GETATTRIBUTES);
	mt6797_rpc_write(reply + 20, 1);
	mt6797_rpc_write(reply + 24, 4);
	mt6797_rpc_write(reply + 28, (u32)result);
}

/* Complete, unfragmented GETDISKINFO only. No modem path is ever opened. */
static int mt6797_fs_disk_reply(const u8 *data, size_t len,
		const struct mt6797_md1_fs_stats *stats, u8 *reply, size_t capacity,
		s32 *result)
{
	static const u8 z_root[] = { 'Z', 0, ':', 0, '\\', 0, 0, 0 };
	const u8 *arg[2];
	u32 lengths[2], argc, flags, index;
	size_t cursor = 24, padded;
	unsigned int i;
	s32 status = -2; /* FS_PARAM_ERROR */
	u8 *info;

	if (!data || !reply || !result || len < 24 || len > 3584 ||
	    capacity < MT6797_FS_REPLY_SIZE)
		return -EMSGSIZE;
	if (mt6797_rpc_read(data) || mt6797_rpc_read(data + 4) != len ||
	    (mt6797_rpc_read(data + 8) & 0xffff) != MT6797_FS_RX)
		return -EPROTO;
	/* FS has five slots; unlike RPC there is no two-slot rule. */
	index = mt6797_rpc_read(data + 12);
	if (index >= 5)
		return -EPROTO;
	if (mt6797_rpc_read(data + 16) != MT6797_FS_GETDISKINFO)
		return -EOPNOTSUPP;
	argc = mt6797_rpc_read(data + 20);
	if (argc != 2)
		return -EPROTO;
	for (i = 0; i < 2; i++) {
		if (len - cursor < 4)
			return -EMSGSIZE;
		lengths[i] = mt6797_rpc_read(data + cursor);
		cursor += 4;
		if (lengths[i] > 3584)
			return -EMSGSIZE;
		padded = (lengths[i] + 3) & ~3U;
		if (padded > len - cursor)
			return -EMSGSIZE;
		arg[i] = data + cursor;
		cursor += padded;
	}
	if (cursor != len)
		return -EPROTO;
	memset(reply, 0, MT6797_FS_REPLY_SIZE);
	if (lengths[0] != sizeof(z_root) || lengths[1] != 4)
		goto respond;
	status = -4; /* FS_DRIVE_NOT_FOUND: only the registered Z root is owned. */
	if (memcmp(arg[0], z_root, sizeof(z_root)))
		goto respond;
	flags = mt6797_rpc_read(arg[1]);
	status = -12; /* FS_UNSUPPORTED_DRIVER_FUNCTION: no FAT statistics. */
	if (!flags || (flags & ~3U))
		goto respond;
	status = -1; /* Actual Gemini statfs failure response: FS_ERROR_RESERVED. */
	if (!stats)
		goto respond;
	info = reply + 36;
	if (flags & 1) {
		info[24] = 'Z';
		info[25] = 1; /* This provider requires a read-only mount and superblock. */
		mt6797_rpc_write(info + 48, 512);
		mt6797_rpc_write(info + 52, stats->block_size / 512);
		mt6797_rpc_write(info + 56, stats->blocks);
	}
	if (flags & 2)
		mt6797_rpc_write(info + 64, stats->free_blocks);
	status = 0;
respond:
	mt6797_rpc_write(reply + 4, MT6797_FS_REPLY_SIZE);
	mt6797_rpc_write(reply + 8, MT6797_FS_TX);
	mt6797_rpc_write(reply + 12, index);
	mt6797_rpc_write(reply + 16, 0xffff0000U | MT6797_FS_GETDISKINFO);
	mt6797_rpc_write(reply + 20, 2);
	mt6797_rpc_write(reply + 24, 4);
	mt6797_rpc_write(reply + 28, (u32)status);
	mt6797_rpc_write(reply + 32, 84);
	*result = status;
	return 0;
}
#endif
