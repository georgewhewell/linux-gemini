// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 MD1 read-only NVRAM filesystem service.
 *
 * Serves the modem's boot-time walk of drive Z: from the retained read-only
 * nvdata "/md" authority. Every operation follows the installed Android25
 * ccci_fsd daemon's wire shapes and result codes; writes are answered with
 * the code the daemon produces on a read-only backing, never faked.
 */
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/soc/mediatek/mt6797-md1-fs-provider.h>
#include "mtk_mt6797_ccci_fs.h"

bool mt6797_fs_service_supports(u32 op)
{
	switch (op) {
	case MT6797_FS_OPEN:
	case MT6797_FS_SEEK:
	case MT6797_FS_READ:
	case MT6797_FS_WRITE:
	case MT6797_FS_CLOSE:
	case MT6797_FS_CLOSEALL:
	case MT6797_FS_CREATEDIR:
	case MT6797_FS_REMOVEDIR:
	case MT6797_FS_GETFILESIZE:
	case MT6797_FS_RENAME:
	case MT6797_FS_MOVE:
	case MT6797_FS_GETDISKINFO:
	case MT6797_FS_DELETE:
	case MT6797_FS_GETATTRIBUTES:
	case MT6797_FS_OPENHINT:
	case MT6797_FS_FINDFIRST:
	case MT6797_FS_FINDNEXT:
	case MT6797_FS_FINDCLOSE:
	case MT6797_FS_LOCKFAT:
	case MT6797_FS_UNLOCKALL:
	case MT6797_FS_SHUTDOWN:
	case MT6797_FS_XDELETE:
	case MT6797_FS_CLEARDISKFLAG:
	case MT6797_FS_GETCLUSTERSIZE:
	case MT6797_FS_SETDISKFLAG:
	case MT6797_FS_RESTORE:
	case MT6797_FS_COMPACT_READ:
		return true;
	default:
		return false;
	}
}

static void mt6797_fs_handle_release(struct mt6797_fs_service *service,
				     unsigned int index)
{
	struct mt6797_fs_handle *handle = &service->handle[index];

	if (!handle->file)
		return;
	mt6797_md1_fs_provider_close(handle->file);
	kfree(handle->pattern);
	memset(handle, 0, sizeof(*handle));
	service->open_count--;
}

void mt6797_fs_service_release(struct mt6797_fs_service *service)
{
	unsigned int i;

	for (i = 1; i < MT6797_FS_HANDLES; i++)
		mt6797_fs_handle_release(service, i);
}

/* Vendor GetFreeHandle (0x10fc4): lowest free slot from 1, never 0. */
static int mt6797_fs_handle_alloc(struct mt6797_fs_service *service)
{
	unsigned int i;

	for (i = 1; i < MT6797_FS_HANDLES; i++)
		if (!service->handle[i].file)
			return i;
	return -EMFILE;
}

static struct mt6797_fs_handle *mt6797_fs_handle_get(struct mt6797_fs_service *service,
						     u32 index, bool search)
{
	if (index >= MT6797_FS_HANDLES || !service->handle[index].file ||
	    service->handle[index].search != search)
		return NULL;
	return &service->handle[index];
}

static bool mt6797_fs_arg_u32(const struct mt6797_fs_request *request,
			      unsigned int n, u32 *value)
{
	if (n >= request->argc || request->len[n] != 4)
		return false;
	*value = mt6797_fs_read32(request->arg[n]);
	return true;
}

static s32 mt6797_fs_arg_path(const struct mt6797_fs_request *request,
			      unsigned int n, char *path, size_t size)
{
	int ret;

	if (n >= request->argc)
		return MT6797_FS_PARAM_ERROR;
	ret = mt6797_fs_path_decode(request->arg[n], request->len[n], path, size);
	return ret ? mt6797_fs_path_status(ret) : MT6797_FS_NO_ERROR;
}

static u32 mt6797_fs_dos_stamp(s64 seconds)
{
	struct tm tm;

	time64_to_tm(seconds, -sys_tz.tz_minuteswest * 60, &tm);
	return mt6797_fs_dos_time(&tm);
}

/* OPEN/OPENHINT: FS_READ_ONLY selects O_RDONLY, otherwise O_RDWR.
 * Creation/truncation/write access requires the separately registered RAM
 * backend. The legacy read-only backend still refuses these with EROFS.
 */
static s32 mt6797_fs_open(struct mt6797_fs_service *service,
			  const struct mt6797_fs_request *request, char *path)
{
	struct mt6797_md1_fs_attr attr;
	struct file *file;
	u32 flags;
	s32 status;
	int slot, ret;

	if (!mt6797_fs_arg_u32(request, 1, &flags))
		return MT6797_FS_PARAM_ERROR;
	status = mt6797_fs_arg_path(request, 0, path, MT6797_MD1_FS_PATH_MAX + 1);
	if (status)
		return status;
	if (service->open_count >= MT6797_FS_HANDLES - 1)
		return MT6797_FS_TOO_MANY_FILES;
	if (!(flags & MT6797_FS_OPEN_READ_ONLY) ||
	    (flags & (MT6797_FS_OPEN_CREATE | MT6797_FS_OPEN_CREATE_ALWAYS))) {
		file = mt6797_md1_fs_provider_open_ram(path,
			!(flags & MT6797_FS_OPEN_READ_ONLY),
			flags & (MT6797_FS_OPEN_CREATE | MT6797_FS_OPEN_CREATE_ALWAYS),
			flags & MT6797_FS_OPEN_CREATE_ALWAYS);
	} else {
		ret = mt6797_md1_fs_provider_getattr(path, &attr);
		if (ret)
			return mt6797_fs_status(ret);
		file = mt6797_md1_fs_provider_open(path, S_ISDIR(attr.mode));
	}
	if (IS_ERR(file))
		return mt6797_fs_status(PTR_ERR(file));
	slot = mt6797_fs_handle_alloc(service);
	if (slot < 0) {
		mt6797_md1_fs_provider_close(file);
		return MT6797_FS_TOO_MANY_FILES;
	}
	service->handle[slot].file = file;
	service->open_count++;
	return slot;
}

static s32 mt6797_fs_seek(struct mt6797_fs_service *service,
			  const struct mt6797_fs_request *request)
{
	struct mt6797_fs_handle *handle;
	u32 index, offset, whence;
	loff_t pos;

	if (!mt6797_fs_arg_u32(request, 0, &index) ||
	    !mt6797_fs_arg_u32(request, 1, &offset) ||
	    !mt6797_fs_arg_u32(request, 2, &whence))
		return MT6797_FS_PARAM_ERROR;
	handle = mt6797_fs_handle_get(service, index, false);
	if (!handle)
		return MT6797_FS_INVALID_FILE_HANDLE;
	if (whence > SEEK_END)
		return MT6797_FS_PARAM_ERROR;
	pos = mt6797_md1_fs_provider_llseek(handle->file, (s32)offset, whence);
	if (pos < 0)
		return mt6797_fs_status(pos);
	return pos > S32_MAX ? MT6797_FS_GENERAL_FAILURE : (s32)pos;
}

static int mt6797_fs_read(struct mt6797_fs_service *service,
			  const struct mt6797_fs_request *request,
			  struct mt6797_fs_reply *reply, s32 *status)
{
	struct mt6797_fs_handle *handle = NULL;
	u32 index, length = 0, count = 0;
	ssize_t got;
	u8 *data;
	int ret;

	*status = MT6797_FS_PARAM_ERROR;
	if (mt6797_fs_arg_u32(request, 0, &index) &&
	    mt6797_fs_arg_u32(request, 1, &length) &&
	    length <= MT6797_FS_READ_MAX) {
		handle = mt6797_fs_handle_get(service, index, false);
		*status = handle ? MT6797_FS_NO_ERROR : MT6797_FS_INVALID_FILE_HANDLE;
	}
	if (!handle)
		length = 0;
	/* Status and count precede the data; all three are settled below. */
	reply->len += 16;
	ret = mt6797_fs_reply_reserve(reply, length, &data);
	reply->len -= 16;
	if (ret)
		return ret;
	if (handle) {
		got = mt6797_md1_fs_provider_read(handle->file, data, length,
						  &handle->file->f_pos);
		if (got < 0)
			*status = mt6797_fs_status(got);
		else
			count = got;
	}
	mt6797_fs_reply_add_u32(reply, (u32)*status);
	mt6797_fs_reply_add_u32(reply, count);
	mt6797_fs_reply_settle(reply, count);
	return 0;
}

static s32 mt6797_fs_close(struct mt6797_fs_service *service,
			   const struct mt6797_fs_request *request)
{
	u32 index;

	if (!mt6797_fs_arg_u32(request, 0, &index))
		return MT6797_FS_PARAM_ERROR;
	if (index >= MT6797_FS_HANDLES || !service->handle[index].file)
		return MT6797_FS_INVALID_FILE_HANDLE;
	mt6797_fs_handle_release(service, index);
	return MT6797_FS_NO_ERROR;
}

/* Android25 cmpt_Read (0x6ab4): a 40-byte control block combines
 * OPEN/SIZE/SEEK/READ/CLOSE bits 1/2/4/8/16. Its embedded pointers are
 * modem addresses, never AP pointers. Reply args are {completed, error},
 * file size, count, data; the internal stage result is not a wire argument.
 */
static int mt6797_fs_compact_read(struct mt6797_fs_service *service,
				const struct mt6797_fs_request *request,
				struct mt6797_fs_reply *reply,
				char *path, s32 *status)
{
	struct mt6797_fs_request open_request = *request;
	struct mt6797_md1_fs_attr attr;
	struct mt6797_fs_handle *handle;
	const u8 *control = request->arg[1];
	u32 operations = mt6797_fs_read32(control);
	u32 length = operations & 8 ? mt6797_fs_read32(control + 32) : 0;
	u32 completed = 0, size = 0, count = 0;
	u8 result[8], *data;
	s32 error = MT6797_FS_GENERAL_FAILURE, slot;
	loff_t pos;
	ssize_t got;
	int ret;

	/* Reserve the final data argument after the three fixed arguments.
	 * This also checks capacity before acquiring a file handle.
	 */
	reply->len += 28;
	ret = mt6797_fs_reply_reserve(reply, length, &data);
	reply->len -= 28;
	if (ret)
		return ret;
	*status = -7; /* vendor: missing OPEN */
	if (!(operations & 1))
		goto answer;
	open_request.argc = 2;
	open_request.arg[1] = control + 12;
	open_request.len[1] = 4;
	slot = mt6797_fs_open(service, &open_request, path);
	*status = -1;
	if (slot < 0) {
		error = slot;
		goto answer;
	}
	handle = &service->handle[slot];
	completed = 1;
	if (operations & 2) {
		ret = mt6797_md1_fs_provider_fstat(handle->file, &attr);
		if (ret) {
			error = mt6797_fs_status(ret);
			*status = -2;
			goto close;
		}
		size = attr.size;
		completed |= 2;
	}
	if (operations & 4) {
		u32 whence = mt6797_fs_read32(control + 24);

		pos = whence > SEEK_END ? -EINVAL :
			mt6797_md1_fs_provider_llseek(handle->file,
				(s32)mt6797_fs_read32(control + 20), whence);
		if (pos < 0 || pos > S32_MAX) {
			error = whence > SEEK_END ? MT6797_FS_PARAM_ERROR :
				pos < 0 ? mt6797_fs_status(pos) : MT6797_FS_GENERAL_FAILURE;
			*status = -3;
			goto close;
		}
		completed |= 4;
	}
	if (operations & 8) {
		got = mt6797_md1_fs_provider_read(handle->file, data, length,
						&handle->file->f_pos);
		if (got < 0) {
			error = mt6797_fs_status(got);
			*status = -4;
			goto close;
		}
		count = got;
		completed |= 8;
	}
	*status = -6; /* missing CLOSE still releases the temporary handle */
	if (operations & 16) {
		completed |= 16;
		error = MT6797_FS_NO_ERROR;
		*status = 0;
	}
close:
	mt6797_fs_handle_release(service, slot);
answer:
	mt6797_fs_write32(result, completed);
	mt6797_fs_write32(result + 4, (u32)error);
	mt6797_fs_reply_add(reply, result, sizeof(result));
	mt6797_fs_reply_add_u32(reply, size);
	mt6797_fs_reply_add_u32(reply, count);
	mt6797_fs_reply_settle(reply, count);
	return 0;
}

static int mt6797_fs_getfilesize(struct mt6797_fs_service *service,
				 const struct mt6797_fs_request *request,
				 struct mt6797_fs_reply *reply, s32 *status)
{
	struct mt6797_fs_handle *handle;
	struct mt6797_md1_fs_attr attr;
	u32 index, size = 0;
	int ret;

	*status = MT6797_FS_PARAM_ERROR;
	if (mt6797_fs_arg_u32(request, 0, &index)) {
		handle = mt6797_fs_handle_get(service, index, false);
		if (!handle) {
			*status = MT6797_FS_INVALID_FILE_HANDLE;
		} else {
			ret = mt6797_md1_fs_provider_fstat(handle->file, &attr);
			*status = mt6797_fs_status(ret);
			if (!ret)
				size = attr.size; /* vendor: st_size low 32 bits */
		}
	}
	ret = mt6797_fs_reply_add_u32(reply, (u32)*status);
	return ret ? ret : mt6797_fs_reply_add_u32(reply, size);
}

static s32 mt6797_fs_getattributes(const struct mt6797_fs_request *request,
				   char *path)
{
	struct mt6797_md1_fs_attr attr;
	s32 status;
	int ret;

	status = mt6797_fs_arg_path(request, 0, path, MT6797_MD1_FS_PATH_MAX + 1);
	if (status)
		return status;
	ret = mt6797_md1_fs_provider_getattr(path, &attr);
	return ret ? mt6797_fs_status(ret) : (s32)mt6797_fs_attributes(attr.mode);
}

/* Next entry matching the stored pattern and attribute filter:
 * (attributes & attr) == attr && (attributes & attr_mask) == 0.
 */
static int mt6797_fs_find_entry(struct mt6797_fs_handle *handle,
				struct mt6797_md1_fs_entry *entry, u32 *attributes)
{
	int ret;

	for (;;) {
		ret = mt6797_md1_fs_provider_next_entry(handle->file, entry);
		if (ret)
			return ret;
		*attributes = mt6797_fs_attributes(entry->attr.mode);
		if (mt6797_fs_match(handle->pattern, entry->name) &&
		    (*attributes & handle->attr) == handle->attr &&
		    !(*attributes & handle->attr_mask))
			return 0;
		cond_resched();
	}
}

static int mt6797_fs_find_reply(struct mt6797_fs_reply *reply, s32 status,
				const struct mt6797_md1_fs_entry *entry,
				u32 attributes, u32 max_chars)
{
	u8 dirent[MT6797_FS_DIRENT_SIZE], name[(MT6797_FS_NAME_MAX + 1) * 2];
	bool fitted = false;
	u32 bytes = 0;
	int ret;

	memset(dirent, 0, sizeof(dirent));
	if (status >= 0) {
		bytes = mt6797_fs_name_encode(entry->name, max_chars, name,
					      sizeof(name), &fitted);
		mt6797_fs_dirent(attributes, fitted,
				 mt6797_fs_dos_stamp(entry->attr.atime),
				 mt6797_fs_dos_stamp(entry->attr.mtime),
				 entry->attr.size, dirent);
	}
	ret = mt6797_fs_reply_add_u32(reply, (u32)status);
	if (!ret)
		ret = mt6797_fs_reply_add(reply, dirent, sizeof(dirent));
	return ret ? ret : mt6797_fs_reply_add(reply, name, bytes);
}

/* FINDFIRST: directory part and pattern split at the last separator; the
 * search handle keeps the open directory, pattern and attribute filter.
 */
static int mt6797_fs_findfirst(struct mt6797_fs_service *service,
			       const struct mt6797_fs_request *request,
			       struct mt6797_fs_reply *reply, s32 *status,
			       char *path)
{
	struct mt6797_md1_fs_entry *entry = NULL;
	struct mt6797_fs_handle *handle;
	struct file *dir;
	char *separator, *pattern;
	u32 attributes = 0, max_chars = 0;
	int slot, ret;

	*status = MT6797_FS_PARAM_ERROR;
	if (request->argc != 4 || !mt6797_fs_arg_u32(request, 3, &max_chars))
		goto reply;
	*status = mt6797_fs_arg_path(request, 0, path, MT6797_MD1_FS_PATH_MAX + 1);
	if (*status)
		goto reply;
	separator = strrchr(path, '/');
	if (separator) {
		*separator = 0;
		pattern = separator + 1;
	} else {
		/* Pattern in the root: keep it behind the now empty directory. */
		pattern = path + MT6797_MD1_FS_PATH_MAX + 1;
		memcpy(pattern, path, strlen(path) + 1);
		path[0] = 0;
	}
	*status = MT6797_FS_TOO_MANY_FILES;
	if (service->open_count >= MT6797_FS_HANDLES - 1)
		goto reply;
	slot = mt6797_fs_handle_alloc(service);
	if (slot < 0)
		goto reply;
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	dir = mt6797_md1_fs_provider_open(path, true);
	if (IS_ERR(dir)) {
		*status = mt6797_fs_status(PTR_ERR(dir));
		goto reply;
	}
	handle = &service->handle[slot];
	handle->pattern = kstrdup(pattern, GFP_KERNEL);
	if (!handle->pattern) {
		mt6797_md1_fs_provider_close(dir);
		ret = -ENOMEM;
		goto free;
	}
	handle->file = dir;
	handle->search = true;
	handle->attr = request->arg[1][0];
	handle->attr_mask = request->arg[2][0];
	service->open_count++;
	ret = mt6797_fs_find_entry(handle, entry, &attributes);
	if (ret) {
		mt6797_fs_handle_release(service, slot);
		*status = ret == -ENOENT ? MT6797_FS_NO_MORE_FILES :
			mt6797_fs_status(ret);
		goto reply;
	}
	*status = slot;
reply:
	ret = mt6797_fs_find_reply(reply, *status, entry, attributes, max_chars);
free:
	kfree(entry);
	return ret;
}

static int mt6797_fs_findnext(struct mt6797_fs_service *service,
			      const struct mt6797_fs_request *request,
			      struct mt6797_fs_reply *reply, s32 *status)
{
	struct mt6797_md1_fs_entry *entry;
	struct mt6797_fs_handle *handle;
	u32 index, attributes = 0, max_chars = 0;
	int ret;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	*status = MT6797_FS_PARAM_ERROR;
	if (mt6797_fs_arg_u32(request, 0, &index) &&
	    mt6797_fs_arg_u32(request, 1, &max_chars)) {
		handle = mt6797_fs_handle_get(service, index, true);
		if (!handle) {
			*status = MT6797_FS_INVALID_FILE_HANDLE;
		} else {
			ret = mt6797_fs_find_entry(handle, entry, &attributes);
			*status = !ret ? MT6797_FS_NO_ERROR :
				ret == -ENOENT ? MT6797_FS_NO_MORE_FILES :
				mt6797_fs_status(ret);
		}
	}
	ret = mt6797_fs_find_reply(reply, *status, entry, attributes, max_chars);
	kfree(entry);
	return ret;
}

static s32 mt6797_fs_getclustersize(const struct mt6797_fs_request *request)
{
	struct mt6797_md1_fs_stats stats;
	u32 drive;
	int ret;

	if (!mt6797_fs_arg_u32(request, 0, &drive))
		return MT6797_FS_PARAM_ERROR;
	if (drive != 'Z')
		return MT6797_FS_DRIVE_NOT_FOUND;
	ret = mt6797_md1_fs_provider_stats(&stats);
	if (ret)
		return mt6797_fs_status(ret);
	return stats.block_size > S32_MAX ? MT6797_FS_GENERAL_FAILURE :
		(s32)stats.block_size;
}

/* Write-class operations: after the same path checks, the daemon's
 * mkdir/rmdir/rename/unlink on a read-only backing fails with EROFS, which
 * its inline errno mapping reports as FS_GENERAL_FAILURE.
 */
static s32 mt6797_fs_refuse_write(const struct mt6797_fs_request *request,
				  unsigned int paths, char *path)
{
	unsigned int i;
	s32 status;

	if (request->argc < paths)
		return MT6797_FS_PARAM_ERROR;
	for (i = 0; i < paths; i++) {
		status = mt6797_fs_arg_path(request, i, path,
					    MT6797_MD1_FS_PATH_MAX + 1);
		if (status)
			return status;
	}
	return MT6797_FS_GENERAL_FAILURE;
}


static int mt6797_fs_write(struct mt6797_fs_service *service,
			 const struct mt6797_fs_request *request,
			 struct mt6797_fs_reply *reply, s32 *status)
{
	struct mt6797_fs_handle *handle;
	u32 index, length, count = 0;
	ssize_t written;
	int ret;

	*status = MT6797_FS_PARAM_ERROR;
	if (request->argc != 3 || !mt6797_fs_arg_u32(request, 0, &index) ||
	    !mt6797_fs_arg_u32(request, 2, &length) || length > request->len[1])
		goto reply;
	handle = mt6797_fs_handle_get(service, index, false);
	*status = MT6797_FS_INVALID_FILE_HANDLE;
	if (!handle)
		goto reply;
	written = mt6797_md1_fs_provider_write(handle->file, request->arg[1],
					     length, &handle->file->f_pos);
	if (written < 0)
		*status = mt6797_fs_status(written);
	else {
		*status = MT6797_FS_NO_ERROR;
		count = written;
	}
reply:
	ret = mt6797_fs_reply_add_u32(reply, (u32)*status);
	return ret ? ret : mt6797_fs_reply_add_u32(reply, count);
}

static int mt6797_fs_diskinfo_reply(const struct mt6797_fs_request *request,
				    struct mt6797_fs_reply *reply, s32 *status)
{
	struct mt6797_md1_fs_stats stats;
	u8 info[MT6797_FS_DISKINFO_SIZE];
	int ret;

	ret = mt6797_md1_fs_provider_stats(&stats);
	*status = mt6797_fs_diskinfo(request, ret ? NULL : &stats, info);
	ret = mt6797_fs_reply_add_u32(reply, (u32)*status);
	return ret ? ret : mt6797_fs_reply_add(reply, info, sizeof(info));
}

/* Process context. @reply must offer MT6797_FS_REPLY_MAX bytes. Returns a
 * negative errno only for malformed frames or local resource failure, in
 * which case nothing is answered; otherwise *status is the vendor result.
 */
int mt6797_fs_service_request(struct mt6797_fs_service *service,
			      const u8 *data, size_t len,
			      struct mt6797_fs_reply *reply, s32 *status)
{
	struct mt6797_fs_request request;
	char *path;
	int ret;

	ret = mt6797_fs_request_parse(data, len, &request);
	if (ret)
		return ret;
	if (!mt6797_fs_service_supports(request.op))
		return -EOPNOTSUPP;
	/* OPENHINT echoes eight hint bytes. Reject malformed shapes before
	 * accessing arg[2], which the parser only initializes for present args.
	 */
	if (request.op == MT6797_FS_OPENHINT &&
	    (request.argc != 3 || request.len[2] != 8))
		return -EINVAL;
	if (request.op == MT6797_FS_COMPACT_READ &&
	    (request.argc != 2 || request.len[1] != 40 ||
	     ((mt6797_fs_read32(request.arg[1]) & 8) &&
	      mt6797_fs_read32(request.arg[1] + 32) > MT6797_FS_READ_MAX)))
		return -EINVAL;
	if (reply->capacity < MT6797_FS_REPLY_MAX)
		return -EMSGSIZE;
	mt6797_fs_reply_init(reply, reply->payload, reply->capacity,
			     request.index, request.op);
	/* One decoded path plus a FINDFIRST pattern copy. */
	path = kmalloc(2 * (MT6797_MD1_FS_PATH_MAX + 1), GFP_KERNEL);
	if (!path)
		return -ENOMEM;
	switch (request.op) {
	case MT6797_FS_OPEN:
		*status = mt6797_fs_open(service, &request, path);
		break;
	case MT6797_FS_OPENHINT:
		*status = mt6797_fs_open(service, &request, path);
		break;
	case MT6797_FS_SEEK:
		*status = mt6797_fs_seek(service, &request);
		break;
	case MT6797_FS_READ:
		ret = mt6797_fs_read(service, &request, reply, status);
		goto out;
	case MT6797_FS_COMPACT_READ:
		ret = mt6797_fs_compact_read(service, &request, reply, path, status);
		goto out;
	case MT6797_FS_WRITE:
		ret = mt6797_fs_write(service, &request, reply, status);
		goto out;
	case MT6797_FS_CLOSE:
	case MT6797_FS_FINDCLOSE:
		*status = mt6797_fs_close(service, &request);
		break;
	case MT6797_FS_CLOSEALL:
		mt6797_fs_service_release(service);
		*status = MT6797_FS_NO_ERROR;
		break;
	case MT6797_FS_SHUTDOWN:
		mt6797_fs_service_release(service);
		*status = MT6797_FS_NO_ERROR;
		ret = 0; /* vendor 0x6754: reply carries no argument */
		goto out;
	case MT6797_FS_GETFILESIZE:
		ret = mt6797_fs_getfilesize(service, &request, reply, status);
		goto out;
	case MT6797_FS_GETDISKINFO:
		ret = mt6797_fs_diskinfo_reply(&request, reply, status);
		goto out;
	case MT6797_FS_GETATTRIBUTES:
		*status = mt6797_fs_getattributes(&request, path);
		break;
	case MT6797_FS_FINDFIRST:
		ret = mt6797_fs_findfirst(service, &request, reply, status, path);
		goto out;
	case MT6797_FS_FINDNEXT:
		ret = mt6797_fs_findnext(service, &request, reply, status);
		goto out;
	case MT6797_FS_LOCKFAT:
	case MT6797_FS_CLEARDISKFLAG:
	case MT6797_FS_SETDISKFLAG:
		*status = MT6797_FS_NO_ERROR; /* vendor 0xd5d8 */
		break;
	case MT6797_FS_UNLOCKALL:
		*status = 1; /* vendor 0x6738 literally answers 1 */
		break;
	case MT6797_FS_GETCLUSTERSIZE:
		*status = mt6797_fs_getclustersize(&request);
		break;
	case MT6797_FS_CREATEDIR:
	case MT6797_FS_REMOVEDIR:
	case MT6797_FS_DELETE:
	case MT6797_FS_RESTORE:
	case MT6797_FS_XDELETE:
		*status = mt6797_fs_refuse_write(&request, 1, path);
		break;
	case MT6797_FS_RENAME:
	case MT6797_FS_MOVE:
		*status = mt6797_fs_refuse_write(&request, 2, path);
		break;
	default:
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = mt6797_fs_reply_add_u32(reply, (u32)*status);
	if (request.op == MT6797_FS_OPENHINT && !ret)
		ret = mt6797_fs_reply_add(reply, request.arg[2], 8);
out:
	kfree(path);
	return ret;
}
