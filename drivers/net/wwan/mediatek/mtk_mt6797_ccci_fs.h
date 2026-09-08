/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_FS_H
#define _MTK_MT6797_CCCI_FS_H

/* MD1 NVRAM filesystem service over CCCI channels 14/15. The wire contract
 * mirrors the installed Android25 ccci_fsd daemon (SHA256 688e9f4d...), see
 * tools/gemini/README.cellular-fs-complete.md for the per-op evidence.
 */
#define MT6797_FS_RX 14U
#define MT6797_FS_TX 15U
#define MT6797_FS_INDEX_COUNT 5U
#define MT6797_FS_MAX_ARGS 5U
#define MT6797_FS_HEADER 16U
#define MT6797_FS_PREFIX 20U /* header plus operation word, repeated per fragment */
#define MT6797_FS_FRAGMENT 0xd80U /* vendor CCCI_MTU: payload bytes per frame */
#define MT6797_FS_FRAME_MAX 3584U /* vendor SKB_4K, also the local RX buffer */
#define MT6797_FS_REQUEST_MAX (MT6797_FS_PREFIX + 0x4000U)
#define MT6797_FS_MORE 0x80000000U
#define MT6797_FS_READ_MAX 16356U /* vendor 0x4000 buffer minus its 0x1c prefix */
#define MT6797_FS_REPLY_MAX 16448U
#define MT6797_FS_HANDLES 129U /* vendor FS_FILE_MAX; slot 0 is never issued */
#define MT6797_FS_NAME_MAX 255U
#define MT6797_FS_DIRENT_SIZE 52U
#define MT6797_FS_DISKINFO_SIZE 84U
#define MT6797_FS_REPLY_SIZE 120U /* GETDISKINFO */
#define MT6797_FS_ATTR_REPLY_SIZE 32U /* single s32 result */
#define MT6797_FS_REPLY_OP 0xffff0000U

enum mt6797_fs_op {
	MT6797_FS_OPEN = 0x1001,
	MT6797_FS_SEEK = 0x1002,
	MT6797_FS_READ = 0x1003,
	MT6797_FS_WRITE = 0x1004,
	MT6797_FS_CLOSE = 0x1005,
	MT6797_FS_CLOSEALL = 0x1006,
	MT6797_FS_CREATEDIR = 0x1007,
	MT6797_FS_REMOVEDIR = 0x1008,
	MT6797_FS_GETFILESIZE = 0x1009,
	MT6797_FS_GETFOLDERSIZE = 0x100a,
	MT6797_FS_RENAME = 0x100b,
	MT6797_FS_MOVE = 0x100c,
	MT6797_FS_COUNT = 0x100d,
	MT6797_FS_GETDISKINFO = 0x100e,
	MT6797_FS_DELETE = 0x100f,
	MT6797_FS_GETATTRIBUTES = 0x1010,
	MT6797_FS_OPENHINT = 0x1011,
	MT6797_FS_FINDFIRST = 0x1012,
	MT6797_FS_FINDNEXT = 0x1013,
	MT6797_FS_FINDCLOSE = 0x1014,
	MT6797_FS_LOCKFAT = 0x1015,
	MT6797_FS_UNLOCKALL = 0x1016,
	MT6797_FS_SHUTDOWN = 0x1017,
	MT6797_FS_XDELETE = 0x1018,
	MT6797_FS_CLEARDISKFLAG = 0x1019,
	MT6797_FS_GETDRIVE = 0x101a,
	MT6797_FS_GETCLUSTERSIZE = 0x101b,
	MT6797_FS_SETDISKFLAG = 0x101c,
	MT6797_FS_RESTORE = 0x1021,
	MT6797_FS_COMPACT_READ = 0x1022,
};

/* Vendor FS result codes actually produced by the installed daemon. */
enum mt6797_fs_status {
	MT6797_FS_NO_ERROR = 0,
	MT6797_FS_ERROR_RESERVED = -1,
	MT6797_FS_PARAM_ERROR = -2,
	MT6797_FS_INVALID_FILENAME = -3,
	MT6797_FS_DRIVE_NOT_FOUND = -4,
	MT6797_FS_TOO_MANY_FILES = -5,
	MT6797_FS_NO_MORE_FILES = -6,
	MT6797_FS_FILE_NOT_FOUND = -9,
	MT6797_FS_INVALID_FILE_HANDLE = -10,
	MT6797_FS_UNSUPPORTED_DRIVER_FUNCTION = -12,
	MT6797_FS_ACCESS_DENIED = -16,
	MT6797_FS_GENERAL_FAILURE = -18,
	MT6797_FS_PATH_NOT_FOUND = -19,
	MT6797_FS_READ_ONLY_ERROR = -45,
	MT6797_FS_PATH_OVER_LEN_ERROR = -49,
};

#define MT6797_FS_ATTR_READ_ONLY 0x01U
#define MT6797_FS_ATTR_DIR 0x10U
#define MT6797_FS_OPEN_READ_ONLY 0x100U
#define MT6797_FS_OPEN_CREATE 0x10000U
#define MT6797_FS_OPEN_CREATE_ALWAYS 0x20000U

struct mt6797_fs_request {
	u32 index;
	u32 op;
	u32 argc;
	const u8 *arg[MT6797_FS_MAX_ARGS];
	u32 len[MT6797_FS_MAX_ARGS];
};

/* Argument stream of one reply: [argc][len,data,pad]... */
struct mt6797_fs_reply {
	u32 index;
	u32 op;
	u32 argc;
	size_t len;
	size_t capacity;
	u8 *payload;
};

struct mt6797_fs_handle {
	struct file *file;
	char *pattern;
	bool search;
	u8 attr;
	u8 attr_mask;
};

/* Owned by one ordered worker; no internal locking. */
struct mt6797_fs_service {
	struct mt6797_fs_handle handle[MT6797_FS_HANDLES];
	unsigned int open_count;
};

static inline u32 mt6797_fs_read32(const u8 *p)
{
	return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline void mt6797_fs_write32(u8 *p, u32 value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

/* Complete, unfragmented channel-14 request with 4-byte padded arguments.
 * Zero-length arguments are refused like the vendor GetPackInfo does.
 */
static inline int mt6797_fs_request_parse(const u8 *data, size_t len,
					  struct mt6797_fs_request *request)
{
	size_t cursor = 24, padded;
	unsigned int i;

	if (!data || !request || len < 24 || len > MT6797_FS_REQUEST_MAX)
		return -EMSGSIZE;
	if (mt6797_fs_read32(data) || mt6797_fs_read32(data + 4) != len ||
	    (mt6797_fs_read32(data + 8) & 0xffff) != MT6797_FS_RX)
		return -EPROTO;
	request->index = mt6797_fs_read32(data + 12);
	request->op = mt6797_fs_read32(data + 16);
	request->argc = mt6797_fs_read32(data + 20);
	if (request->index >= MT6797_FS_INDEX_COUNT ||
	    request->argc > MT6797_FS_MAX_ARGS)
		return -EPROTO;
	for (i = 0; i < request->argc; i++) {
		if (len - cursor < 4)
			return -EMSGSIZE;
		request->len[i] = mt6797_fs_read32(data + cursor);
		cursor += 4;
		if (!request->len[i] || request->len[i] > len - cursor)
			return -EMSGSIZE;
		padded = (request->len[i] + 3) & ~3UL;
		if (padded > len - cursor)
			return -EMSGSIZE;
		request->arg[i] = data + cursor;
		cursor += padded;
	}
	return cursor == len ? 0 : -EPROTO;
}

/* NAPI-owned assembly buffer. Each continuation repeats only the 20-byte
 * header/op prefix; argc and argument bytes belong to the combined stream.
 * No service effects occur here. A negative return invalidates the assembly.
 * Return 0 for more fragments, 1 for a complete, strictly parsed request.
 */
static inline int mt6797_fs_request_append(u8 *buffer, size_t *used,
					   const u8 *frame, size_t len)
{
	struct mt6797_fs_request request;
	size_t skip = *used ? MT6797_FS_PREFIX : 0;
	u32 data0;
	int ret;

	if (len < (skip ? MT6797_FS_PREFIX + 1 : 24) ||
	    len > MT6797_FS_PREFIX + MT6797_FS_FRAGMENT)
		return -EMSGSIZE;
	data0 = mt6797_fs_read32(frame);
	if ((data0 & ~MT6797_FS_MORE) ||
	    mt6797_fs_read32(frame + 4) != len ||
	    (mt6797_fs_read32(frame + 8) & 0xffff) != MT6797_FS_RX ||
	    mt6797_fs_read32(frame + 12) >= MT6797_FS_INDEX_COUNT)
		return -EPROTO;
	if (*used && (mt6797_fs_read32(buffer + 12) !=
		     mt6797_fs_read32(frame + 12) ||
		     mt6797_fs_read32(buffer + 16) !=
		     mt6797_fs_read32(frame + 16)))
		return -EPROTO;
	if (*used > MT6797_FS_REQUEST_MAX ||
	    len - skip > MT6797_FS_REQUEST_MAX - *used)
		return -EMSGSIZE;
	memcpy(buffer + *used, frame + skip, len - skip);
	*used += len - skip;
	if (data0 & MT6797_FS_MORE)
		return 0;
	/* Internal canonical header describes the complete argument stream. */
	mt6797_fs_write32(buffer, 0);
	mt6797_fs_write32(buffer + 4, *used);
	ret = mt6797_fs_request_parse(buffer, *used, &request);
	return ret ?: 1;
}

static inline void mt6797_fs_reply_init(struct mt6797_fs_reply *reply,
					u8 *payload, size_t capacity,
					u32 index, u32 op)
{
	reply->index = index;
	reply->op = op;
	reply->argc = 0;
	reply->len = 4;
	reply->capacity = capacity;
	reply->payload = payload;
	memset(payload, 0, 4);
}

static inline int mt6797_fs_reply_add(struct mt6797_fs_reply *reply,
				      const void *data, u32 len)
{
	size_t padded = (len + 3) & ~3UL;

	if (reply->argc >= MT6797_FS_MAX_ARGS ||
	    reply->capacity - reply->len < 4 + padded)
		return -EMSGSIZE;
	mt6797_fs_write32(reply->payload + reply->len, len);
	reply->len += 4;
	if (len) {
		memcpy(reply->payload + reply->len, data, len);
		memset(reply->payload + reply->len + len, 0, padded - len);
	}
	reply->len += padded;
	reply->argc++;
	mt6797_fs_write32(reply->payload, reply->argc);
	return 0;
}

static inline int mt6797_fs_reply_add_u32(struct mt6797_fs_reply *reply,
					  u32 value)
{
	u8 bytes[4];

	mt6797_fs_write32(bytes, value);
	return mt6797_fs_reply_add(reply, bytes, 4);
}

/* Append an argument of up to @max bytes to be filled in place through
 * *@data, then settle its actual length with mt6797_fs_reply_settle().
 */
static inline int mt6797_fs_reply_reserve(struct mt6797_fs_reply *reply,
					  u32 max, u8 **data)
{
	if (reply->argc >= MT6797_FS_MAX_ARGS ||
	    reply->capacity - reply->len < 4 + ((max + 3) & ~3UL))
		return -EMSGSIZE;
	*data = reply->payload + reply->len + 4;
	return 0;
}

static inline void mt6797_fs_reply_settle(struct mt6797_fs_reply *reply,
					  u32 len)
{
	size_t padded = (len + 3) & ~3UL;

	mt6797_fs_write32(reply->payload + reply->len, len);
	reply->len += 4;
	memset(reply->payload + reply->len + len, 0, padded - len);
	reply->len += padded;
	reply->argc++;
	mt6797_fs_write32(reply->payload, reply->argc);
}

/* Number of channel-15 frames the vendor daemon would emit for this reply. */
static inline unsigned int mt6797_fs_reply_frames(const struct mt6797_fs_reply *reply)
{
	return reply->len <= MT6797_FS_FRAGMENT ? 1 :
		(reply->len + MT6797_FS_FRAGMENT - 1) / MT6797_FS_FRAGMENT;
}

/* Frame @n (0-based). Non-final fragments carry data0 bit 31; TX assigns the
 * channel sequence. Returns the frame length or -EMSGSIZE.
 */
static inline int mt6797_fs_reply_frame(const struct mt6797_fs_reply *reply,
					unsigned int n, u8 *frame,
					size_t capacity)
{
	size_t offset = (size_t)n * MT6797_FS_FRAGMENT, chunk;
	bool last;

	if (n >= mt6797_fs_reply_frames(reply))
		return -EINVAL;
	chunk = min_t(size_t, reply->len - offset, MT6797_FS_FRAGMENT);
	last = offset + chunk == reply->len;
	if (capacity < MT6797_FS_PREFIX + chunk)
		return -EMSGSIZE;
	mt6797_fs_write32(frame, last ? 0 : 0x80000000U);
	mt6797_fs_write32(frame + 4, MT6797_FS_PREFIX + chunk);
	mt6797_fs_write32(frame + 8, MT6797_FS_TX);
	mt6797_fs_write32(frame + 12, reply->index);
	mt6797_fs_write32(frame + 16, MT6797_FS_REPLY_OP | reply->op);
	memcpy(frame + MT6797_FS_PREFIX, reply->payload + offset, chunk);
	return MT6797_FS_PREFIX + chunk;
}

/* Installed daemon helper 0x11980: EMFILE/EACCES/ENOENT, else general failure. */
static inline s32 mt6797_fs_status(int error)
{
	switch (error) {
	case 0: return MT6797_FS_NO_ERROR;
	case -EMFILE: return MT6797_FS_TOO_MANY_FILES;
	case -EACCES: return MT6797_FS_ACCESS_DENIED;
	case -ENOENT: return MT6797_FS_FILE_NOT_FOUND;
	default: return MT6797_FS_GENERAL_FAILURE;
	}
}

/* Bit 0 iff no write bit and some read bit; 0x10 for directories. */
static inline u32 mt6797_fs_attributes(unsigned int mode)
{
	u32 attributes = 0;

	if (!(mode & 0333) && (mode & 0444))
		attributes |= MT6797_FS_ATTR_READ_ONLY;
	if ((mode & 0170000) == 0040000)
		attributes |= MT6797_FS_ATTR_DIR;
	return attributes;
}

/* Strict UTF-16LE decode of one NUL-terminated argument into UTF-8. Unpaired
 * surrogates, embedded NUL and overflow are refused; the vendor daemon
 * instead narrows each code unit to its low byte.
 */
static inline int mt6797_fs_utf16_decode(const u8 *in, u32 bytes, char *out,
					 size_t outsize)
{
	size_t o = 0;
	u32 i, c, low;

	if (bytes < 2 || (bytes & 1) || in[bytes - 2] || in[bytes - 1])
		return -EILSEQ;
	for (i = 0; i + 2 < bytes; i += 2) {
		c = in[i] | ((u32)in[i + 1] << 8);
		if (!c)
			return -EILSEQ;
		if (c >= 0xd800 && c <= 0xdbff) {
			if (i + 4 >= bytes)
				return -EILSEQ;
			low = in[i + 2] | ((u32)in[i + 3] << 8);
			if (low < 0xdc00 || low > 0xdfff)
				return -EILSEQ;
			c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
			i += 2;
		} else if (c >= 0xdc00 && c <= 0xdfff) {
			return -EILSEQ;
		}
		if (c < 0x80) {
			if (o + 1 >= outsize)
				return -ENAMETOOLONG;
			out[o++] = c;
		} else if (c < 0x800) {
			if (o + 2 >= outsize)
				return -ENAMETOOLONG;
			out[o++] = 0xc0 | (c >> 6);
			out[o++] = 0x80 | (c & 0x3f);
		} else if (c < 0x10000) {
			if (o + 3 >= outsize)
				return -ENAMETOOLONG;
			out[o++] = 0xe0 | (c >> 12);
			out[o++] = 0x80 | ((c >> 6) & 0x3f);
			out[o++] = 0x80 | (c & 0x3f);
		} else {
			if (o + 4 >= outsize)
				return -ENAMETOOLONG;
			out[o++] = 0xf0 | (c >> 18);
			out[o++] = 0x80 | ((c >> 12) & 0x3f);
			out[o++] = 0x80 | ((c >> 6) & 0x3f);
			out[o++] = 0x80 | (c & 0x3f);
		}
	}
	out[o] = 0;
	return 0;
}

/* One UTF-8 sequence to a code point; malformed bytes map to themselves as
 * Latin-1 so no directory entry is ever hidden. Returns bytes consumed.
 */
static inline unsigned int mt6797_fs_utf8_next(const char *s, u32 *cp)
{
	const u8 *p = (const u8 *)s;
	unsigned int n, i;
	u32 c = p[0];

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if ((c & 0xe0) == 0xc0 && c >= 0xc2) {
		n = 2;
		c &= 0x1f;
	} else if ((c & 0xf0) == 0xe0) {
		n = 3;
		c &= 0x0f;
	} else if ((c & 0xf8) == 0xf0 && c <= 0xf4) {
		n = 4;
		c &= 0x07;
	} else {
		*cp = p[0];
		return 1;
	}
	for (i = 1; i < n; i++) {
		if ((p[i] & 0xc0) != 0x80) {
			*cp = p[0];
			return 1;
		}
		c = (c << 6) | (p[i] & 0x3f);
	}
	if ((n == 3 && (c < 0x800 || (c >= 0xd800 && c <= 0xdfff))) ||
	    (n == 4 && (c < 0x10000 || c > 0x10ffff))) {
		*cp = p[0];
		return 1;
	}
	*cp = c;
	return n;
}

/* Vendor FindFirst/FindNext name copy (0x916c-0x9214): at most @max_chars - 1
 * UTF-16 code units, '/' becomes '\', then a NUL unit. Returns the argument
 * length in bytes (0 when no unit fits) and whether the whole name fitted.
 */
static inline u32 mt6797_fs_name_encode(const char *name, u32 max_chars,
					u8 *out, size_t outsize, bool *fitted)
{
	u32 units = 0, cp, unit;
	size_t i = 0, total = 0;

	while (name[i]) {
		i += mt6797_fs_utf8_next(name + i, &cp);
		total += cp >= 0x10000 ? 2 : 1;
	}
	*fitted = total < max_chars;
	i = 0;
	while (name[i] && max_chars && units < max_chars - 1) {
		i += mt6797_fs_utf8_next(name + i, &cp);
		if (cp == '/')
			cp = '\\';
		if (cp >= 0x10000) {
			if (units + 2 > max_chars - 1 ||
			    (units + 3) * 2 > outsize)
				break;
			cp -= 0x10000;
			unit = 0xd800 | (cp >> 10);
			out[units * 2] = unit;
			out[units * 2 + 1] = unit >> 8;
			units++;
			unit = 0xdc00 | (cp & 0x3ff);
		} else {
			if ((units + 2) * 2 > outsize)
				break;
			unit = cp;
		}
		out[units * 2] = unit;
		out[units * 2 + 1] = unit >> 8;
		units++;
	}
	if (!units)
		return 0;
	out[units * 2] = 0;
	out[units * 2 + 1] = 0;
	return units * 2 + 2;
}

/* Vendor pattern matcher 0x1330: anchored, case-sensitive '*' and '?'. */
static inline bool mt6797_fs_match(const char *pattern, const char *name)
{
	const char *star = NULL, *backtrack = NULL;

	while (*name) {
		if (*pattern == '*') {
			star = pattern++;
			backtrack = name;
		} else if (*pattern == '?' || *pattern == *name) {
			pattern++;
			name++;
		} else if (star) {
			pattern = star + 1;
			name = ++backtrack;
		} else {
			return false;
		}
	}
	while (*pattern == '*')
		pattern++;
	return !*pattern;
}

/* Z:\a\b -> "a/b" (Z:\ -> ""). Both separators are accepted like the
 * vendor's byte copy; empty and "." components are dropped; ".." is refused.
 */
static inline int mt6797_fs_path_decode_drive(const u8 *arg, u32 len, char *out,
					      size_t outsize, u8 *drive)
{
	size_t i = 0, o = 0, start;
	int ret;

	ret = mt6797_fs_utf16_decode(arg, len, out, outsize);
	if (ret)
		return ret;
	if ((out[0] != 'Z' && out[0] != 'X' && out[0] != 'Y') || out[1] != ':')
		return -ENXIO;
	*drive = out[0];
	i = 2;
	while (out[i]) {
		while (out[i] == '\\' || out[i] == '/')
			i++;
		start = i;
		while (out[i] && out[i] != '\\' && out[i] != '/')
			i++;
		if (i == start)
			break;
		if (i - start > MT6797_FS_NAME_MAX)
			return -ENAMETOOLONG;
		if (i - start == 1 && out[start] == '.')
			continue;
		if (i - start == 2 && out[start] == '.' && out[start + 1] == '.')
			return -EINVAL;
		if (o)
			out[o++] = '/';
		memmove(out + o, out + start, i - start);
		o += i - start;
	}
	out[o] = 0;
	return 0;
}

/* Legacy codec callers retain single-Z interpretation. */
static inline int mt6797_fs_path_decode(const u8 *arg, u32 len, char *out,
					size_t outsize)
{
	u8 drive;
	int ret = mt6797_fs_path_decode_drive(arg, len, out, outsize, &drive);

	return ret ? ret : drive == 'Z' ? 0 : -ENXIO;
}

static inline s32 mt6797_fs_path_status(int error)
{
	switch (error) {
	case -ENXIO: return MT6797_FS_PATH_NOT_FOUND; /* unknown root (0xeaf4) */
	case -EINVAL: return MT6797_FS_ACCESS_DENIED; /* ".." (0xe370, 0xe3f4) */
	case -ENAMETOOLONG: return MT6797_FS_PATH_OVER_LEN_ERROR; /* 0xe738 */
	default: return MT6797_FS_INVALID_FILENAME; /* malformed UTF-16 */
	}
}

/* Vendor EntryLinuxToDos packing (0x11dc0-0x11e04), month copied from mday. */
static inline u32 mt6797_fs_dos_time(const struct tm *tm)
{
	return (tm->tm_sec & 0x1f) | ((u32)tm->tm_min << 5) |
		(((u32)tm->tm_hour << 11) & 0xffff) |
		(((u32)tm->tm_mday << 16) & 0x1f0000) |
		(((u32)tm->tm_mday << 21) & 0x1e00000) |
		((u32)(tm->tm_year - 80) << 25);
}

static inline void mt6797_fs_dirent(u32 attributes, bool fitted, u32 atime,
				    u32 mtime, u32 size, u8 *entry)
{
	memset(entry, 0, MT6797_FS_DIRENT_SIZE);
	entry[11] = attributes;
	entry[12] = fitted;
	mt6797_fs_write32(entry + 14, atime);
	mt6797_fs_write32(entry + 22, mtime);
	mt6797_fs_write32(entry + 28, size);
}

int mt6797_fs_service_request(struct mt6797_fs_service *service,
			      const u8 *data, size_t len,
			      struct mt6797_fs_reply *reply, s32 *status);
bool mt6797_fs_service_supports(u32 op);
void mt6797_fs_service_release(struct mt6797_fs_service *service);

/* Vendor GETDISKINFO (0x621c): BASIC_INFO and FREE_SPACE from the retained
 * root's statfs; FAT statistics are not a Linux capability. Fills the 84-byte
 * FS_DiskInfo and returns the vendor status.
 */
static inline s32 mt6797_fs_diskinfo_drive(const struct mt6797_fs_request *request,
				     const struct mt6797_md1_fs_stats *stats,
				     u8 *info, u8 drive)
{
	u8 z_root[] = { 'Z', 0, ':', 0, '\\', 0, 0, 0 };
	u32 flags;

	memset(info, 0, MT6797_FS_DISKINFO_SIZE);
	if (request->argc != 2 || request->len[0] != sizeof(z_root) ||
	    request->len[1] != 4)
		return MT6797_FS_PARAM_ERROR;
	if (drive != 'Z' && drive != 'X' && drive != 'Y')
		return MT6797_FS_DRIVE_NOT_FOUND;
	z_root[0] = drive;
	if (memcmp(request->arg[0], z_root, sizeof(z_root)))
		return MT6797_FS_DRIVE_NOT_FOUND;
	flags = mt6797_fs_read32(request->arg[1]);
	if (!flags || (flags & ~3U))
		return MT6797_FS_UNSUPPORTED_DRIVER_FUNCTION;
	/* Actual Gemini statfs failure response. */
	if (!stats)
		return MT6797_FS_ERROR_RESERVED;
	if (flags & 1) {
		info[24] = drive;
		info[25] = 1; /* Stock daemon volume field. */
		mt6797_fs_write32(info + 48, 512);
		mt6797_fs_write32(info + 52, stats->block_size / 512);
		mt6797_fs_write32(info + 56, stats->blocks);
	}
	if (flags & 2)
		mt6797_fs_write32(info + 64, stats->free_blocks);
	return MT6797_FS_NO_ERROR;
}
static inline s32 mt6797_fs_diskinfo(const struct mt6797_fs_request *request,
				     const struct mt6797_md1_fs_stats *stats, u8 *info)
{
	return mt6797_fs_diskinfo_drive(request, stats, info, 'Z');
}
#endif
