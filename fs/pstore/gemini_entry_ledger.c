// SPDX-License-Identifier: GPL-2.0-only
/* Experiment-only retained entry ledger for the Planet Gemini PDA. */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/pstore_ram.h>
#include <linux/string.h>

#include <asm/barrier.h>

#define GEMINI_ENTRY_RESERVE_BASE	0x44410000ULL
#define GEMINI_ENTRY_RESERVE_SIZE	0x000e0000ULL
#define GEMINI_ENTRY_BASE		0x444bb000ULL
#define GEMINI_ENTRY_SLOT_SIZE		0x00001000UL
#define GEMINI_ENTRY_SLOT_COUNT		4
#define GEMINI_ENTRY_HEADER_SIZE	12
#define GEMINI_ENTRY_SIGNATURE		0x43474244

static const char * const gemini_entry_records[] __initconst = {
	"====0.000000-D\n"
	"GAEL-20260904-I E0\n",
	"====0.000000-D\n"
	"GAEL-20260904-I E1\n",
	"====0.000000-D\n"
	"GAEL-20260904-I E2\n",
	"====0.000000-D\n"
	"GAEL-20260904-I E3\n",
};

static bool __init gemini_entry_slot_empty(void __iomem *slot)
{
	return readl(slot) == GEMINI_ENTRY_SIGNATURE &&
	       readl((u8 __iomem *)slot + 4) == 0 &&
	       readl((u8 __iomem *)slot + 8) == 0;
}

static bool __init gemini_entry_slot_exact(void __iomem *slot,
					   const char *record)
{
	size_t i;
	size_t len = strlen(record);

	if (readl(slot) != GEMINI_ENTRY_SIGNATURE ||
	    readl((u8 __iomem *)slot + 4) != len ||
	    readl((u8 __iomem *)slot + 8) != len)
		return false;
	for (i = 0; i < len; i++)
		if (readb((u8 __iomem *)slot + GEMINI_ENTRY_HEADER_SIZE + i) !=
		    record[i])
			return false;

	return true;
}

static bool __init gemini_entry_prefix_valid(void __iomem *ledger,
					     unsigned int stage)
{
	unsigned int i;

	for (i = 0; i < GEMINI_ENTRY_SLOT_COUNT; i++) {
		void __iomem *slot = (u8 __iomem *)ledger +
				       i * GEMINI_ENTRY_SLOT_SIZE;

		if (i < stage) {
			if (!gemini_entry_slot_empty(slot) &&
			    !gemini_entry_slot_exact(slot, gemini_entry_records[i]))
				return false;
		} else if (!gemini_entry_slot_empty(slot)) {
			return false;
		}
	}

	return true;
}

static bool __init gemini_entry_write(void __iomem *slot,
				      const char *record)
{
	size_t i;
	size_t len = strlen(record);

	if (!gemini_entry_slot_empty(slot) ||
	    len > GEMINI_ENTRY_SLOT_SIZE - GEMINI_ENTRY_HEADER_SIZE)
		return false;

	memcpy_toio((u8 __iomem *)slot + GEMINI_ENTRY_HEADER_SIZE, record, len);
	dsb(sy);
	writel(len, (u8 __iomem *)slot + 4);
	dsb(sy);
	writel(len, (u8 __iomem *)slot + 8);
	dsb(sy);

	if (readl(slot) != GEMINI_ENTRY_SIGNATURE ||
	    readl((u8 __iomem *)slot + 4) != len ||
	    readl((u8 __iomem *)slot + 8) != len)
		return false;
	for (i = 0; i < len; i++)
		if (readb((u8 __iomem *)slot + GEMINI_ENTRY_HEADER_SIZE + i) !=
		    record[i])
			return false;

	return true;
}

static bool __init gemini_entry_exact_flat_dt(void)
{
	u64 addr;
	u64 size;
	unsigned long root = of_get_flat_dt_root();
	const void *no_map;
	int no_map_len;
	int reserved;
	int ramoops;

	if (!of_flat_dt_is_compatible(root, "planet,gemini-pda"))
		return false;
	reserved = of_get_flat_dt_subnode_by_name(root, "reserved-memory");
	if (reserved < 0)
		return false;
	ramoops = of_get_flat_dt_subnode_by_name(reserved,
						 "ramoops@44410000");
	if (ramoops < 0 ||
	    !of_flat_dt_is_compatible(ramoops, "ramoops") ||
	    !of_flat_dt_get_addr_size(ramoops, "reg", &addr, &size))
		return false;
	no_map = of_get_flat_dt_prop(ramoops, "no-map", &no_map_len);
	if (!no_map || no_map_len != 0 ||
	    addr != GEMINI_ENTRY_RESERVE_BASE ||
	    size != GEMINI_ENTRY_RESERVE_SIZE)
		return false;

	return memblock_is_region_reserved(addr, size);
}

static void __init gemini_entry_checkpoint(unsigned int stage,
					   bool require_reservation)
{
	void __iomem *ledger;
	void __iomem *slot;

	if (stage < 2 || stage >= GEMINI_ENTRY_SLOT_COUNT)
		return;
	if (require_reservation && !gemini_entry_exact_flat_dt())
		return;

	ledger = early_ioremap(GEMINI_ENTRY_BASE,
			       GEMINI_ENTRY_SLOT_COUNT * GEMINI_ENTRY_SLOT_SIZE);
	if (!ledger)
		return;

	if (!gemini_entry_prefix_valid(ledger, stage))
		goto out;
	slot = (u8 __iomem *)ledger + stage * GEMINI_ENTRY_SLOT_SIZE;
	gemini_entry_write(slot, gemini_entry_records[stage]);
out:
	early_iounmap(ledger,
		      GEMINI_ENTRY_SLOT_COUNT * GEMINI_ENTRY_SLOT_SIZE);
}

void __init gemini_arm64_entry_ledger_post_mmu_checkpoint(void)
{
	gemini_entry_checkpoint(2, false);
}

void __init gemini_arm64_entry_ledger_post_reserved_checkpoint(void)
{
	gemini_entry_checkpoint(3, true);
}
