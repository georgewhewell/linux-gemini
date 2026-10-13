/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT6797_MD3_FS_TRACE_H
#define MT6797_MD3_FS_TRACE_H
/* Optional owner diagnostics: no path bytes, subscriber data or file contents.
 * Drive is the syntactic request selector, not a granted authority claim.
 */
static bool md3_fs_open_trace(const u8 *data, size_t len, u32 *drive, u32 *flags)
{
	struct mt6797_fs_request r;
	*drive = 0;
	*flags = 0;
	if (mt6797_fs_request_parse_md3(data, len, &r) ||
	    (r.op != MT6797_FS_OPEN && r.op != MT6797_FS_OPENHINT) ||
	    r.argc < 2 || r.len[0] < 4 || r.len[1] != 4 ||
	    r.arg[0][1] || r.arg[0][2] != ':' || r.arg[0][3] ||
	    r.arg[0][0] < 'A' || r.arg[0][0] > 'Z')
		return false;
	*drive = r.arg[0][0];
	*flags = mt6797_fs_read32(r.arg[1]);
	return true;
}
#endif
