/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __GEMINI_ADMISSION_TRACE_INTERNAL_H
#define __GEMINI_ADMISSION_TRACE_INTERNAL_H

#include <linux/gemini_admission_trace.h>
#include <linux/types.h>

#define GEMINI_ADMISSION_TRACE_SIGNATURE 0x31544147U /* "GAT1" */
#define GEMINI_ADMISSION_TRACE_SLOT_SIZE 0x800U
#define GEMINI_ADMISSION_TRACE_HEADER_SIZE 12U
#define GEMINI_ADMISSION_TRACE_SLOT_COUNT 2U

struct gemini_admission_trace_ops {
	u32 (*read_word)(void *context, unsigned int slot, unsigned int word);
	void (*write_word)(void *context, unsigned int slot, unsigned int word,
			   u32 value);
	u8 (*read_byte)(void *context, unsigned int slot, unsigned int offset);
	void (*write_byte)(void *context, unsigned int slot, unsigned int offset,
			   u8 value);
	void (*sync)(void *context);
};

struct gemini_admission_trace_owner {
	unsigned int commits;
	bool entry_committed;
	bool terminal_committed;
	bool failed;
};

int
gemini_admission_trace_owner_entry(struct gemini_admission_trace_owner *owner,
				   const struct gemini_admission_trace_ops *ops,
				   void *context);
int
gemini_admission_trace_owner_zero_request(struct gemini_admission_trace_owner *owner,
					  const struct gemini_admission_trace_ops *ops,
					  void *context,
				  enum gemini_admission_trace_zero_result result);
int
gemini_admission_trace_initialize(const struct gemini_admission_trace_ops *ops,
				  void *context);

#endif /* __GEMINI_ADMISSION_TRACE_INTERNAL_H */
