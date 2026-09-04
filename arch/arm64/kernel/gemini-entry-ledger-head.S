/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Experiment-only Gemini arm64 entry ledger.
 *
 * These macros execute before ordinary virtual mappings exist.  They use
 * only x9-x15, issue aligned 32-bit or narrower accesses, and never call,
 * touch the stack, or use a literal pool.
 */

	.macro	gemini_entry_ledger_store_word base, offset, low, high
	movz	w10, #\low
	movk	w10, #\high, lsl #16
	str	w10, [\base, #\offset]
	.endm

	.macro	gemini_entry_ledger_verify_word base, offset, low, high, fail
	movz	w10, #\low
	movk	w10, #\high, lsl #16
	ldr	w11, [\base, #\offset]
	cmp	w11, w10
	b.ne	\fail
	.endm

	.macro	gemini_entry_ledger_require_empty_range base, count, fail
	mov	w13, #\count
.Lgemini_empty_range_\@:
	ldr	w10, [\base]
	cmp	w10, w12
	b.ne	\fail
	ldr	w10, [\base, #4]
	cbnz	w10, \fail
	ldr	w10, [\base, #8]
	cbnz	w10, \fail
	add	\base, \base, #1, lsl #12
	subs	w13, w13, #1
	b.ne	.Lgemini_empty_range_\@
	.endm

	.macro	gemini_entry_ledger_primary_store base
	gemini_entry_ledger_store_word \base, 12, 0x3d3d, 0x3d3d
	gemini_entry_ledger_store_word \base, 16, 0x2e30, 0x3030
	gemini_entry_ledger_store_word \base, 20, 0x3030, 0x3030
	gemini_entry_ledger_store_word \base, 24, 0x442d, 0x470a
	gemini_entry_ledger_store_word \base, 28, 0x4541, 0x2d4c
	gemini_entry_ledger_store_word \base, 32, 0x3032, 0x3632
	gemini_entry_ledger_store_word \base, 36, 0x3930, 0x3430
	gemini_entry_ledger_store_word \base, 40, 0x492d, 0x4520
	gemini_entry_ledger_store_word \base, 44, 0x0a30, 0x0000
	.endm

	.macro	gemini_entry_ledger_primary_verify base, fail
	gemini_entry_ledger_verify_word \base, 12, 0x3d3d, 0x3d3d, \fail
	gemini_entry_ledger_verify_word \base, 16, 0x2e30, 0x3030, \fail
	gemini_entry_ledger_verify_word \base, 20, 0x3030, 0x3030, \fail
	gemini_entry_ledger_verify_word \base, 24, 0x442d, 0x470a, \fail
	gemini_entry_ledger_verify_word \base, 28, 0x4541, 0x2d4c, \fail
	gemini_entry_ledger_verify_word \base, 32, 0x3032, 0x3632, \fail
	gemini_entry_ledger_verify_word \base, 36, 0x3930, 0x3430, \fail
	gemini_entry_ledger_verify_word \base, 40, 0x492d, 0x4520, \fail
	gemini_entry_ledger_verify_word \base, 44, 0x0a30, 0x0000, \fail
	.endm

	.macro	gemini_entry_ledger_switch_store base
	gemini_entry_ledger_store_word \base, 12, 0x3d3d, 0x3d3d
	gemini_entry_ledger_store_word \base, 16, 0x2e30, 0x3030
	gemini_entry_ledger_store_word \base, 20, 0x3030, 0x3030
	gemini_entry_ledger_store_word \base, 24, 0x442d, 0x470a
	gemini_entry_ledger_store_word \base, 28, 0x4541, 0x2d4c
	gemini_entry_ledger_store_word \base, 32, 0x3032, 0x3632
	gemini_entry_ledger_store_word \base, 36, 0x3930, 0x3430
	gemini_entry_ledger_store_word \base, 40, 0x492d, 0x4520
	gemini_entry_ledger_store_word \base, 44, 0x0a31, 0x0000
	.endm

	.macro	gemini_entry_ledger_switch_verify base, fail
	gemini_entry_ledger_verify_word \base, 12, 0x3d3d, 0x3d3d, \fail
	gemini_entry_ledger_verify_word \base, 16, 0x2e30, 0x3030, \fail
	gemini_entry_ledger_verify_word \base, 20, 0x3030, 0x3030, \fail
	gemini_entry_ledger_verify_word \base, 24, 0x442d, 0x470a, \fail
	gemini_entry_ledger_verify_word \base, 28, 0x4541, 0x2d4c, \fail
	gemini_entry_ledger_verify_word \base, 32, 0x3032, 0x3632, \fail
	gemini_entry_ledger_verify_word \base, 36, 0x3930, 0x3430, \fail
	gemini_entry_ledger_verify_word \base, 40, 0x492d, 0x4520, \fail
	gemini_entry_ledger_verify_word \base, 44, 0x0a31, 0x0000, \fail
	.endm

	.macro	gemini_entry_ledger_require_primary_or_empty base, fail
	ldr	w10, [\base]
	cmp	w10, w12
	b.ne	\fail
	ldr	w10, [\base, #4]
	cbz	w10, .Lgemini_primary_empty_\@
	cmp	w10, #34
	b.ne	\fail
	ldr	w11, [\base, #8]
	cmp	w11, w10
	b.ne	\fail
	gemini_entry_ledger_primary_verify \base, \fail
	b	.Lgemini_primary_valid_\@
.Lgemini_primary_empty_\@:
	ldr	w10, [\base, #8]
	cbnz	w10, \fail
.Lgemini_primary_valid_\@:
	.endm

	.macro	gemini_arm64_entry_ledger_primary
	mrs	x10, CurrentEL
	cmp	x10, #CurrentEL_EL1
	b.eq	.Lgemini_primary_el1_\@
	cmp	x10, #CurrentEL_EL2
	b.ne	.Lgemini_primary_done_\@
	mrs	x10, sctlr_el2
	b	.Lgemini_primary_state_\@
.Lgemini_primary_el1_\@:
	mrs	x10, sctlr_el1
.Lgemini_primary_state_\@:
	tbnz	x10, #0, .Lgemini_primary_done_\@
	tbnz	x10, #2, .Lgemini_primary_done_\@
	movz	x9, #0xb000
	movk	x9, #0x444b, lsl #16
	movz	w12, #0x4244
	movk	w12, #0x4347, lsl #16
	mov	x14, x9
	gemini_entry_ledger_require_empty_range x14, 4, .Lgemini_primary_done_\@
	gemini_entry_ledger_primary_store x9
	dsb	sy
	movz	w13, #34
	str	w13, [x9, #4]
	dsb	sy
	str	w13, [x9, #8]
	dsb	sy
	ldr	w10, [x9]
	cmp	w10, w12
	b.ne	.Lgemini_primary_done_\@
	ldr	w10, [x9, #4]
	cmp	w10, w13
	b.ne	.Lgemini_primary_done_\@
	ldr	w10, [x9, #8]
	cmp	w10, w13
	b.ne	.Lgemini_primary_done_\@
	gemini_entry_ledger_primary_verify x9, .Lgemini_primary_done_\@
.Lgemini_primary_done_\@:
	.endm

	.macro	gemini_arm64_entry_ledger_pre_switch
	mrs	x10, CurrentEL
	cmp	x10, #CurrentEL_EL1
	b.eq	.Lgemini_switch_el1_\@
	cmp	x10, #CurrentEL_EL2
	b.ne	.Lgemini_switch_done_\@
	mrs	x10, sctlr_el2
	b	.Lgemini_switch_state_\@
.Lgemini_switch_el1_\@:
	mrs	x10, sctlr_el1
.Lgemini_switch_state_\@:
	tbnz	x10, #0, .Lgemini_switch_done_\@
	tbnz	x10, #2, .Lgemini_switch_done_\@
	movz	x9, #0xb000
	movk	x9, #0x444b, lsl #16
	movz	w12, #0x4244
	movk	w12, #0x4347, lsl #16
	mov	x14, x9
	gemini_entry_ledger_require_primary_or_empty x14, .Lgemini_switch_done_\@
	add	x14, x9, #1, lsl #12
	gemini_entry_ledger_require_empty_range x14, 3, .Lgemini_switch_done_\@
	add	x14, x9, #1, lsl #12
	gemini_entry_ledger_switch_store x14
	dsb	sy
	movz	w13, #34
	str	w13, [x14, #4]
	dsb	sy
	str	w13, [x14, #8]
	dsb	sy
	ldr	w10, [x14]
	cmp	w10, w12
	b.ne	.Lgemini_switch_done_\@
	ldr	w10, [x14, #4]
	cmp	w10, w13
	b.ne	.Lgemini_switch_done_\@
	ldr	w10, [x14, #8]
	cmp	w10, w13
	b.ne	.Lgemini_switch_done_\@
	gemini_entry_ledger_switch_verify x14, .Lgemini_switch_done_\@
.Lgemini_switch_done_\@:
	.endm
