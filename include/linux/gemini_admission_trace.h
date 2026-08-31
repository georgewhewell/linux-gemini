/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_GEMINI_ADMISSION_TRACE_H
#define _LINUX_GEMINI_ADMISSION_TRACE_H

#include <linux/errno.h>
#include <linux/types.h>

enum gemini_admission_trace_zero_result {
	GEMINI_ADMISSION_TRACE_ZERO_SOURCE_REGISTER = 1,
	GEMINI_ADMISSION_TRACE_ZERO_DERIVE,
	GEMINI_ADMISSION_TRACE_ZERO_PUBLISH,
};

/* Read-only physical evidence captured before a live admission token. */
struct gemini_admission_trace_snapshot {
	u32 entry_words[7];
	u32 terminal_words[7];
};

#ifdef CONFIG_PSTORE_GEMINI_ADMISSION_TRACE
int gemini_admission_trace_entry(void);
int
gemini_admission_trace_zero_request(enum gemini_admission_trace_zero_result result);
int gemini_admission_trace_prepare(struct gemini_admission_trace_snapshot *snapshot);
#else
static inline int gemini_admission_trace_entry(void)
{
	return -EOPNOTSUPP;
}

static inline int
gemini_admission_trace_zero_request(enum gemini_admission_trace_zero_result result)
{
	return -EOPNOTSUPP;
}

static inline int
gemini_admission_trace_prepare(struct gemini_admission_trace_snapshot *snapshot)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_GEMINI_ADMISSION_TRACE_H */
