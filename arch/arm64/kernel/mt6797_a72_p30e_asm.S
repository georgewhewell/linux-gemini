/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Dormant MT6797 A72 P30E MMU-off claim/publication primitives.
 *
 * This file is not connected to secondary_entry. Its symbols are a reviewed
 * implementation seam for a later owner that can prove the complete P17/P18
 * and P24 lifecycle.
 */

#include <linux/linkage.h>

#include <asm/assembler.h>
#include <asm/cputype.h>
#include <asm/errno.h>
#include <asm/mt6797_a72_p30e.h>

	.section ".idmap.text", "a"

SYM_FUNC_START_LOCAL(p30e_select_slot)
	mrs	x0, mpidr_el1
	mov_q	x2, MPIDR_HWID_BITMASK
	and	x0, x0, x2
	adr_l	x1, arm64_mt6797_a72_p30e_cpu8_slot
	cmp	x0, #ARM64_MT6797_A72_P30E_MPIDR_CPU8
	b.eq	.Lp30e_cpu8
	adr_l	x1, arm64_mt6797_a72_p30e_cpu9_slot
	cmp	x0, #ARM64_MT6797_A72_P30E_MPIDR_CPU9
	b.eq	.Lp30e_cpu9
	mov	x0, #-ENODEV
	ret
.Lp30e_cpu8:
	mov	x2, #ARM64_MT6797_A72_P30E_CPU8
	ret
.Lp30e_cpu9:
	mov	x2, #ARM64_MT6797_A72_P30E_CPU9
	ret
SYM_FUNC_END(p30e_select_slot)

/* x1 = selected slot; the cache routine corrupts x0-x3 only. */
SYM_FUNC_START_LOCAL(p30e_clean_slot)
	mov	x0, x1
	add	x1, x1, #ARM64_MT6797_A72_P30E_SLOT_BYTES
	adr_l	x2, dcache_clean_inval_poc
	blr	x2
	dsb	sy
	ret
SYM_FUNC_END(p30e_clean_slot)

/* Return x0 = 0 for a matching CRC64-ECMA value, 1 otherwise. */
SYM_FUNC_START_LOCAL(p30e_crc_bad)
	mov	x5, xzr
	mov	x6, #0
	mov_q	x11, ARM64_MT6797_A72_P30E_CRC64_POLY
.Lp30e_crc_word:
	ldr	x7, [x1, x6, lsl #3]
	mov	x8, #8
.Lp30e_crc_byte:
	and	x9, x7, #0xff
	lsl	x9, x9, #56
	eor	x5, x5, x9
	mov	x10, #8
.Lp30e_crc_bit:
	tbz	x5, #63, .Lp30e_crc_zero
	lsl	x5, x5, #1
	eor	x5, x5, x11
	b	.Lp30e_crc_next
.Lp30e_crc_zero:
	lsl	x5, x5, #1
.Lp30e_crc_next:
	subs	x10, x10, #1
	b.ne	.Lp30e_crc_bit
	lsr	x7, x7, #8
	subs	x8, x8, #1
	b.ne	.Lp30e_crc_byte
	add	x6, x6, #1
	cmp	x6, #ARM64_MT6797_A72_P30E_CRC64_WORD
	b.lo	.Lp30e_crc_word
	ldr	x7, [x1, #ARM64_MT6797_A72_P30E_CRC64_OFF]
	cmp	x5, x7
	cset	x0, ne
	ret
SYM_FUNC_END(p30e_crc_bad)

/* x1 = selected slot, x5 = failure reason. */
SYM_FUNC_START_LOCAL(p30e_publish_failure)
	/* Called by direct branches; x15 holds the public caller's link. */
	mov	x4, #ARM64_MT6797_A72_P30E_FAILED
	str	x4, [x1, #ARM64_MT6797_A72_P30E_TARGET_STATE_OFF]
	mov	x4, #1
	str	x4, [x1, #ARM64_MT6797_A72_P30E_TARGET_SEQUENCE_OFF]
	str	x5, [x1, #ARM64_MT6797_A72_P30E_TARGET_REASON_OFF]
	str	xzr, [x1, #ARM64_MT6797_A72_P30E_TARGET_EFFECTS_OFF]
	str	xzr, [x1, #ARM64_MT6797_A72_P30E_TARGET_ENTRY_PC_OFF]
	str	xzr, [x1, #ARM64_MT6797_A72_P30E_TARGET_ENTRY_SP_OFF]
	bl	p30e_clean_slot
	bl	p30e_select_slot
	mov	x4, #ARM64_MT6797_A72_P30E_FAILED
	add	x3, x1, #ARM64_MT6797_A72_P30E_TARGET_STATE_OFF
	stlr	x4, [x3]
	mov	x0, #-EPROTO
	mov	x30, x15
	ret
SYM_FUNC_END(p30e_publish_failure)

SYM_FUNC_START(arm64_mt6797_a72_p30e_target_claim)
	mov	x15, x30
	bl	p30e_select_slot
	cmp	x0, #0
	b.lt	.Lp30e_claim_no_target

	ldr	x4, [x1, #ARM64_MT6797_A72_P30E_MAGIC_OFF]
	mov_q	x6, ARM64_MT6797_A72_P30E_MAGIC
	cmp	x4, x6
	mov	x5, #1
	b.ne	p30e_publish_failure
	ldr	x4, [x1, #ARM64_MT6797_A72_P30E_ABI_OFF]
	mov_q	x6, ARM64_MT6797_A72_P30E_ABI_AND_SIZE
	cmp	x4, x6
	mov	x5, #2
	b.ne	p30e_publish_failure
	ldr	x4, [x1, #ARM64_MT6797_A72_P30E_TARGET_MPIDR_OFF]
	cmp	x4, x0
	mov	x5, #3
	b.ne	p30e_publish_failure
	ldr	x4, [x1, #ARM64_MT6797_A72_P30E_TARGET_CPU_OFF]
	cmp	x4, x2
	mov	x5, #3
	b.ne	p30e_publish_failure
	mov	x6, #0
.Lp30e_boot_identity:
	add	x3, x1, #ARM64_MT6797_A72_P30E_BOOT_ID0_OFF
	ldr	x4, [x3, x6, lsl #3]
	add	x3, x1, #ARM64_MT6797_A72_P30E_TARGET_BOOT_ID_OFF
	ldr	x7, [x3, x6, lsl #3]
	cmp	x4, x7
	mov	x5, #9
	b.ne	p30e_publish_failure
	add	x6, x6, #1
	cmp	x6, #ARM64_MT6797_A72_P30E_TARGET_BOOT_ID_WORDS
	b.lo	.Lp30e_boot_identity
	ldr	x4, [x1, #ARM64_MT6797_A72_P30E_OPERATION_OFF]
	cmp	x2, #ARM64_MT6797_A72_P30E_CPU8
	b.ne	.Lp30e_operation_cpu9
	cmp	x4, #ARM64_MT6797_A72_P30E_OPERATION_CPU8_UP
	b.eq	.Lp30e_operation_ok
	b	.Lp30e_bad_operation
.Lp30e_operation_cpu9:
	cmp	x2, #ARM64_MT6797_A72_P30E_CPU9
	b.ne	.Lp30e_bad_operation
	cmp	x4, #ARM64_MT6797_A72_P30E_OPERATION_CPU9_UP
	b.eq	.Lp30e_operation_ok
.Lp30e_bad_operation:
	mov	x5, #7
	b	p30e_publish_failure
.Lp30e_operation_ok:
	add	x3, x1, #ARM64_MT6797_A72_P30E_CONTROLLER_STATE_OFF
	ldar	x4, [x3]
	cmp	x4, #ARM64_MT6797_A72_P30E_EMPTY
	b.eq	.Lp30e_claim_not_armed
	cmp	x4, #ARM64_MT6797_A72_P30E_ARMED
	mov	x5, #5
	b.ne	p30e_publish_failure
	add	x3, x1, #ARM64_MT6797_A72_P30E_TARGET_STATE_OFF
	ldar	x4, [x3]
	cmp	x4, #ARM64_MT6797_A72_P30E_EMPTY
	mov	x5, #5
	b.ne	p30e_publish_failure
	ldr	x4, [x1, #ARM64_MT6797_A72_P30E_TARGET_SEQUENCE_OFF]
	cbnz	x4, .Lp30e_claim_bad_sequence
	ldr	x4, [x1, #ARM64_MT6797_A72_P30E_CONTROLLER_SEQUENCE_OFF]
	cbz	x4, .Lp30e_claim_bad_sequence
	ldr	x4, [x1, #ARM64_MT6797_A72_P30E_GENERATION_OFF]
	cbz	x4, .Lp30e_claim_bad_token
	ldr	x4, [x1, #ARM64_MT6797_A72_P30E_COOKIE_OFF]
	cbz	x4, .Lp30e_claim_bad_token
	bl	p30e_crc_bad
	cbnz	x0, .Lp30e_claim_bad_crc
	mov	x4, #ARM64_MT6797_A72_P30E_TARGET_CLAIMED
	str	x4, [x1, #ARM64_MT6797_A72_P30E_TARGET_STATE_OFF]
	bl	p30e_clean_slot
	bl	p30e_select_slot
	mov	x4, #ARM64_MT6797_A72_P30E_TARGET_CLAIMED
	add	x3, x1, #ARM64_MT6797_A72_P30E_TARGET_STATE_OFF
	stlr	x4, [x3]
	mov	x0, #0
	mov	x30, x15
	ret
.Lp30e_claim_not_armed:
	mov	x0, #-EAGAIN
	mov	x30, x15
	ret
.Lp30e_claim_bad_sequence:
	mov	x5, #6
	b	p30e_publish_failure
.Lp30e_claim_bad_token:
	mov	x5, #8
	b	p30e_publish_failure
.Lp30e_claim_bad_crc:
	mov	x5, #4
	b	p30e_publish_failure
.Lp30e_claim_no_target:
	mov	x30, x15
	ret
SYM_FUNC_END(arm64_mt6797_a72_p30e_target_claim)

SYM_FUNC_START(arm64_mt6797_a72_p30e_target_publish)
	/* Preserve the terminal tuple across slot selection/cache maintenance. */
	mov	x15, x30
	mov	x5, x0
	mov	x6, x1
	mov	x7, x2
	mov	x8, x3
	mov	x9, x4
	cmp	x5, #ARM64_MT6797_A72_P30E_TARGET_PUBLISHED
	b.lo	.Lp30e_publish_bad_state
	cmp	x5, #ARM64_MT6797_A72_P30E_PANICKED
	b.hi	.Lp30e_publish_bad_state
	bl	p30e_select_slot
	cmp	x0, #0
	b.lt	.Lp30e_publish_no_target
	add	x3, x1, #ARM64_MT6797_A72_P30E_TARGET_STATE_OFF
	ldar	x4, [x3]
	cmp	x4, #ARM64_MT6797_A72_P30E_TARGET_CLAIMED
	b.ne	.Lp30e_publish_not_claimed
	ldr	x10, [x1, #ARM64_MT6797_A72_P30E_TARGET_SEQUENCE_OFF]
	add	x10, x10, #1
	str	x10, [x1, #ARM64_MT6797_A72_P30E_TARGET_SEQUENCE_OFF]
	str	x6, [x1, #ARM64_MT6797_A72_P30E_TARGET_REASON_OFF]
	str	x7, [x1, #ARM64_MT6797_A72_P30E_TARGET_EFFECTS_OFF]
	str	x8, [x1, #ARM64_MT6797_A72_P30E_TARGET_ENTRY_PC_OFF]
	str	x9, [x1, #ARM64_MT6797_A72_P30E_TARGET_ENTRY_SP_OFF]
	bl	p30e_clean_slot
	bl	p30e_select_slot
	add	x3, x1, #ARM64_MT6797_A72_P30E_TARGET_STATE_OFF
	stlr	x5, [x3]
	mov	x0, #0
	mov	x30, x15
	ret
.Lp30e_publish_bad_state:
	mov	x0, #-EINVAL
	mov	x30, x15
	ret
.Lp30e_publish_no_target:
	mov	x0, #-ENODEV
	mov	x30, x15
	ret
.Lp30e_publish_not_claimed:
	mov	x0, #-EALREADY
	mov	x30, x15
	ret
SYM_FUNC_END(arm64_mt6797_a72_p30e_target_publish)

	.text
