# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux pstorefs routines.
#

obj-$(CONFIG_PSTORE) += pstore.o

pstore-objs += inode.o platform.o
pstore-$(CONFIG_PSTORE_FTRACE)	+= ftrace.o

pstore-$(CONFIG_PSTORE_PMSG)	+= pmsg.o

ramoops-objs += ram.o ram_core.o
obj-$(CONFIG_PSTORE_RAM)	+= ramoops.o
obj-$(CONFIG_PSTORE_GEMINI_ARM64_ENTRY_LEDGER) += gemini_entry_ledger.o
obj-$(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_LEDGER) += gemini_protected_readback_ledger.o
obj-$(CONFIG_PSTORE_GEMINI_TRANSITION_LEDGER) += gemini_transition_ledger.o
obj-$(CONFIG_PSTORE_GEMINI_ADMISSION_TRACE) += gemini_admission_trace.o
obj-$(CONFIG_PSTORE_GEMINI_ADMISSION_TRACE_KUNIT_TEST) += gemini_admission_trace_test.o
obj-$(CONFIG_PSTORE_GEMINI_TRANSITION_LEDGER_KUNIT_TEST) += gemini_transition_ledger_test.o

pstore_zone-objs += zone.o
obj-$(CONFIG_PSTORE_ZONE)	+= pstore_zone.o

pstore_blk-objs += blk.o
obj-$(CONFIG_PSTORE_BLK)	+= pstore_blk.o
