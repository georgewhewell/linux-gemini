// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dormant MT6797 A72 P30E MMU-off-visible wire object.
 *
 * This file deliberately has no production caller. It supplies the reviewed
 * controller-side storage and cache protocol for a later P17/P18/P24 owner.
 */

#include <linux/bitops.h>
#include <linux/byteorder/little_endian.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/memory.h>
#include <asm/mt6797_a72_p30e.h>
#include <asm/smp.h>

static_assert(sizeof(struct arm64_mt6797_a72_p30e_wire) ==
	      ARM64_MT6797_A72_P30E_WIRE_BYTES);
static_assert(sizeof(struct arm64_mt6797_a72_p30e_slot) ==
	      ARM64_MT6797_A72_P30E_SLOT_BYTES);

struct arm64_mt6797_a72_p30e_slot arm64_mt6797_a72_p30e_cpu8_slot
	__used __section(".mmuoff.data.bidirectional")
	__aligned(ARM64_MT6797_A72_P30E_SLOT_BYTES) = {};
struct arm64_mt6797_a72_p30e_slot arm64_mt6797_a72_p30e_cpu9_slot
	__used __section(".mmuoff.data.bidirectional")
	__aligned(ARM64_MT6797_A72_P30E_SLOT_BYTES) = {};

static DEFINE_RAW_SPINLOCK(p30e_lock);

extern char __mmuoff_data_bidirectional_start[];
extern char __mmuoff_data_bidirectional_end[];

static struct arm64_mt6797_a72_p30e_slot *p30e_slot(unsigned int cpu)
{
	switch (cpu) {
	case ARM64_MT6797_A72_P30E_CPU8:
		return &arm64_mt6797_a72_p30e_cpu8_slot;
	case ARM64_MT6797_A72_P30E_CPU9:
		return &arm64_mt6797_a72_p30e_cpu9_slot;
	default:
		return NULL;
	}
}

static u64 p30e_mpidr(unsigned int cpu)
{
	return cpu == ARM64_MT6797_A72_P30E_CPU8 ?
		ARM64_MT6797_A72_P30E_MPIDR_CPU8 :
		ARM64_MT6797_A72_P30E_MPIDR_CPU9;
}

static bool p30e_slot_range_valid(phys_addr_t slot_pa)
{
	phys_addr_t start = __pa_symbol(__mmuoff_data_bidirectional_start);
	phys_addr_t end = __pa_symbol(__mmuoff_data_bidirectional_end);

	if (end < start || end - start < ARM64_MT6797_A72_P30E_SLOT_BYTES ||
	    slot_pa < start || slot_pa > end ||
	    ARM64_MT6797_A72_P30E_SLOT_BYTES > end - slot_pa)
		return false;

	return true;
}

phys_addr_t arm64_mt6797_a72_p30e_slot_pa(unsigned int cpu)
{
	phys_addr_t slot_pa;

	switch (cpu) {
	case ARM64_MT6797_A72_P30E_CPU8:
		slot_pa = __pa_symbol(&arm64_mt6797_a72_p30e_cpu8_slot);
		break;
	case ARM64_MT6797_A72_P30E_CPU9:
		slot_pa = __pa_symbol(&arm64_mt6797_a72_p30e_cpu9_slot);
		break;
	default:
		return 0;
	}

	return p30e_slot_range_valid(slot_pa) ? slot_pa : 0;
}

static u64 p30e_word(const struct arm64_mt6797_a72_p30e_wire *wire,
			     unsigned int index)
{
	return le64_to_cpu(READ_ONCE(wire->word[index]));
}

static void p30e_put(struct arm64_mt6797_a72_p30e_wire *wire,
			 unsigned int index, u64 value)
{
	WRITE_ONCE(wire->word[index], cpu_to_le64(value));
}

static u64 p30e_crc64(const struct arm64_mt6797_a72_p30e_wire *wire)
{
	const u8 *bytes = (const u8 *)wire;
	u64 crc = 0;
	unsigned int i, bit;

	for (i = 0; i < ARM64_MT6797_A72_P30E_CRC64_OFF; i++) {
		crc ^= (u64)bytes[i] << 56;
		for (bit = 0; bit < 8; bit++)
			crc = (crc << 1) ^
				((crc & BIT_ULL(63)) ?
				 ARM64_MT6797_A72_P30E_CRC64_POLY : 0);
	}

	return crc;
}

static void p30e_clean_slot(struct arm64_mt6797_a72_p30e_slot *slot)
{
	dcache_clean_poc((unsigned long)slot,
			 (unsigned long)slot + ARM64_MT6797_A72_P30E_SLOT_BYTES);
	dsb(sy);
}

static void p30e_invalidate_slot(struct arm64_mt6797_a72_p30e_slot *slot)
{
	dcache_inval_poc((unsigned long)slot,
			  (unsigned long)slot + ARM64_MT6797_A72_P30E_SLOT_BYTES);
}

static bool p30e_request_matches(unsigned int cpu,
					const struct arm64_mt6797_a72_p30e_request *request,
					const struct arm64_mt6797_a72_p30e_wire *wire)
{
	unsigned int i;

	if (p30e_word(wire, ARM64_MT6797_A72_P30E_TARGET_CPU_WORD) != cpu ||
	    p30e_word(wire, ARM64_MT6797_A72_P30E_TARGET_MPIDR_WORD) !=
	    p30e_mpidr(cpu) ||
	    p30e_word(wire, ARM64_MT6797_A72_P30E_OPERATION_WORD) !=
	    request->operation ||
	    p30e_word(wire, ARM64_MT6797_A72_P30E_GENERATION_WORD) !=
	    request->generation ||
	    p30e_word(wire, ARM64_MT6797_A72_P30E_COOKIE_WORD) != request->cookie)
		return false;

	for (i = 0; i < ARRAY_SIZE(request->boot_identity); i++)
		if (p30e_word(wire, ARM64_MT6797_A72_P30E_BOOT_ID0_WORD + i) !=
		    request->boot_identity[i])
			return false;

	return true;
}

int arm64_mt6797_a72_p30e_arm(unsigned int cpu,
				      const struct arm64_mt6797_a72_p30e_request *request)
{
	struct arm64_mt6797_a72_p30e_slot *slot;
	unsigned long flags;
	unsigned int i;

	if (!request || !p30e_slot(cpu) ||
	    (request->operation != ARM64_MT6797_A72_P30E_OPERATION_CPU8_UP &&
	     request->operation != ARM64_MT6797_A72_P30E_OPERATION_CPU9_UP) ||
	    !request->slot_pa ||
	    request->slot_pa != arm64_mt6797_a72_p30e_slot_pa(cpu) ||
	    request->slot_pa & (ARM64_MT6797_A72_P30E_SLOT_BYTES - 1) ||
	    (cpu == ARM64_MT6797_A72_P30E_CPU8 &&
	     request->operation != ARM64_MT6797_A72_P30E_OPERATION_CPU8_UP) ||
	    (cpu == ARM64_MT6797_A72_P30E_CPU9 &&
	     request->operation != ARM64_MT6797_A72_P30E_OPERATION_CPU9_UP) ||
	    !request->generation || !request->cookie)
		return -EINVAL;

	if (!request->entry_pa || request->entry_pa != __pa_symbol(secondary_entry))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(request->target_boot_identity); i++)
		if (!request->target_boot_identity[i])
			return -EINVAL;

	slot = p30e_slot(cpu);
	raw_spin_lock_irqsave(&p30e_lock, flags);
	if (p30e_word(&slot->wire,
		       ARM64_MT6797_A72_P30E_CONTROLLER_STATE_WORD) !=
	    ARM64_MT6797_A72_P30E_EMPTY ||
	    p30e_word(&slot->wire,
		       ARM64_MT6797_A72_P30E_TARGET_STATE_WORD) !=
	    ARM64_MT6797_A72_P30E_EMPTY) {
		raw_spin_unlock_irqrestore(&p30e_lock, flags);
		return -EBUSY;
	}

	memset(&slot->wire, 0, sizeof(slot->wire));
	p30e_put(&slot->wire, ARM64_MT6797_A72_P30E_MAGIC_WORD,
		 ARM64_MT6797_A72_P30E_MAGIC);
	p30e_put(&slot->wire, ARM64_MT6797_A72_P30E_ABI_WORD,
		 ARM64_MT6797_A72_P30E_ABI_AND_SIZE);
	for (i = 0; i < ARRAY_SIZE(request->boot_identity); i++)
		p30e_put(&slot->wire,
			 ARM64_MT6797_A72_P30E_BOOT_ID0_WORD + i,
			 request->boot_identity[i]);
	for (i = 0; i < ARRAY_SIZE(request->target_boot_identity); i++)
		WRITE_ONCE(slot->target_boot_identity[i],
			   cpu_to_le64(request->target_boot_identity[i]));
	p30e_put(&slot->wire, ARM64_MT6797_A72_P30E_OPERATION_WORD,
		 request->operation);
	p30e_put(&slot->wire, ARM64_MT6797_A72_P30E_TARGET_CPU_WORD, cpu);
	p30e_put(&slot->wire, ARM64_MT6797_A72_P30E_TARGET_MPIDR_WORD,
		 p30e_mpidr(cpu));
	p30e_put(&slot->wire, ARM64_MT6797_A72_P30E_GENERATION_WORD,
		 request->generation);
	p30e_put(&slot->wire, ARM64_MT6797_A72_P30E_COOKIE_WORD,
		 request->cookie);
	p30e_put(&slot->wire, ARM64_MT6797_A72_P30E_CONTROLLER_STATE_WORD,
		 ARM64_MT6797_A72_P30E_ARMED);
	p30e_put(&slot->wire, ARM64_MT6797_A72_P30E_CONTROLLER_SEQUENCE_WORD, 1);
	p30e_put(&slot->wire, ARM64_MT6797_A72_P30E_CRC64_WORD,
		 p30e_crc64(&slot->wire));

	p30e_clean_slot(slot);
	dsb(sy);
	smp_store_release((u64 *)&slot->wire.word[
			 ARM64_MT6797_A72_P30E_CONTROLLER_STATE_WORD],
			 cpu_to_le64(ARM64_MT6797_A72_P30E_ARMED));
	p30e_clean_slot(slot);
	raw_spin_unlock_irqrestore(&p30e_lock, flags);
	return 0;
}

int arm64_mt6797_a72_p30e_readback(
	unsigned int cpu,
	const struct arm64_mt6797_a72_p30e_request *request,
	struct arm64_mt6797_a72_p30e_wire *copy)
{
	struct arm64_mt6797_a72_p30e_slot *slot;
	unsigned long flags;
	u64 state, sequence;
	int ret = 0;

	if (!request || !copy || !p30e_slot(cpu))
		return -EINVAL;
	slot = p30e_slot(cpu);
	raw_spin_lock_irqsave(&p30e_lock, flags);
	dsb(sy);
	p30e_invalidate_slot(slot);
	memcpy(copy, &slot->wire, sizeof(*copy));
	raw_spin_unlock_irqrestore(&p30e_lock, flags);

	state = p30e_word(copy, ARM64_MT6797_A72_P30E_TARGET_STATE_WORD);
	sequence = p30e_word(copy,
				     ARM64_MT6797_A72_P30E_TARGET_SEQUENCE_WORD);
	if (state < ARM64_MT6797_A72_P30E_TARGET_PUBLISHED ||
	    state > ARM64_MT6797_A72_P30E_PANICKED || sequence != 1 ||
	    !p30e_request_matches(cpu, request, copy))
		ret = -EAGAIN;

	return ret;
}
