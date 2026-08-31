// SPDX-License-Identifier: GPL-2.0-only
/* Experiment-only retained ledger around the first protected readback. */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pstore_ram.h>
#include <linux/string.h>

#include "ram_internal.h"

#define GEMINI_PRB_RESERVE_BASE		0x44410000ULL
#define GEMINI_PRB_RESERVE_SIZE		0x000e0000ULL
#define GEMINI_PRB_SLOT_SIZE		0x00001000UL
#if defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_FIRST_DMESG_WRITE_QUALIFICATION) || \
	defined(CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_FIRST_DMESG_ENTRY_QUALIFICATION) || \
	defined(CONFIG_PSTORE_GEMINI_PROTECTED_CLOCK_FIRST_DMESG_CALL_QUALIFICATION) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_PROVIDER_CLOCK_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_PROVIDER_SNAPSHOT_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_SNAPSHOT_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PHYSICAL_SOURCE_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PHYSICAL_SOURCE_PRECAPTURE_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_GLOBAL_INITCALL_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_EARLY_INITCALL_LEDGER)
#define GEMINI_PRB_LEDGER_BASE		GEMINI_PRB_RESERVE_BASE
#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_FIRST_DMESG_WRITE_QUALIFICATION
#define GEMINI_PRB_SLOT_COUNT		1
#else
#define GEMINI_PRB_SLOT_COUNT		2
#endif
#define GEMINI_PRB_FIRST_OWNED_SLOT	0
#else
#define GEMINI_PRB_LEDGER_BASE		0x444bb000ULL
#define GEMINI_PRB_SLOT_COUNT		4
#define GEMINI_PRB_FIRST_OWNED_SLOT	2
#endif
#define GEMINI_PRB_HEADER_SIZE		12
#define GEMINI_PRB_SIGNATURE		0x43474244

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_STAGE_CONTROL
static const char *gemini_prb_stage = "not-run";
#define GEMINI_PRB_SET_STAGE(stage) (gemini_prb_stage = (stage))
#else
#define GEMINI_PRB_SET_STAGE(stage) ((void)(stage))
#endif

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_PREFIX_CONTROL
static unsigned int gemini_prb_prefix_checkpoint = ~0U;
static unsigned int gemini_prb_prefix_slot_index = ~0U;
static const char *gemini_prb_prefix_reason = "not-run";
static u32 gemini_prb_prefix_signature;
static u32 gemini_prb_prefix_start;
static u32 gemini_prb_prefix_size;

static void gemini_prb_capture_prefix(unsigned int checkpoint,
				      unsigned int slot_index,
				      bool expected_empty,
				      void __iomem *slot)
{
	u32 signature = readl(slot);
	u32 start = readl((u8 __iomem *)slot + 4);
	u32 size = readl((u8 __iomem *)slot + 8);

	gemini_prb_prefix_checkpoint = checkpoint;
	gemini_prb_prefix_slot_index = slot_index;
	gemini_prb_prefix_signature = signature;
	gemini_prb_prefix_start = start;
	gemini_prb_prefix_size = size;
	if (signature != GEMINI_PRB_SIGNATURE)
		gemini_prb_prefix_reason = "bad-signature";
	else if (expected_empty && start)
		gemini_prb_prefix_reason = "nonzero-start";
	else if (expected_empty && size)
		gemini_prb_prefix_reason = "nonzero-size";
	else if (expected_empty)
		gemini_prb_prefix_reason = "unstable-or-other";
	else
		gemini_prb_prefix_reason = "exact-record-refused";
}
#else
static inline void gemini_prb_capture_prefix(unsigned int checkpoint,
					     unsigned int slot_index,
					     bool expected_empty,
					     void __iomem *slot)
{
	(void)checkpoint;
	(void)slot_index;
	(void)expected_empty;
	(void)slot;
}
#endif

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_MAP_CONTROL
static const char *gemini_prb_map_reason = "not-run";
static u32 gemini_prb_map_parallel_signature;
static u32 gemini_prb_map_parallel_start;
static u32 gemini_prb_map_parallel_size;
static u32 gemini_prb_map_ramoops_signature;
static u32 gemini_prb_map_ramoops_start;
static u32 gemini_prb_map_ramoops_size;
static unsigned int gemini_prb_map_ramoops_reads;

static bool gemini_prb_header_empty(u32 signature, u32 start, u32 size)
{
	return signature == GEMINI_PRB_SIGNATURE && !start && !size;
}

static void gemini_prb_capture_map_control(void __iomem *ledger)
{
	bool mapped;

	gemini_prb_map_parallel_signature = readl(ledger);
	gemini_prb_map_parallel_start = readl((u8 __iomem *)ledger + 4);
	gemini_prb_map_parallel_size = readl((u8 __iomem *)ledger + 8);
	mapped = persistent_ram_gemini_snapshot_header(GEMINI_PRB_LEDGER_BASE,
						       &gemini_prb_map_ramoops_signature,
						       &gemini_prb_map_ramoops_start,
						       &gemini_prb_map_ramoops_size);
	if (!mapped) {
		gemini_prb_map_reason = "ramoops-map-unavailable";
		return;
	}

	gemini_prb_map_ramoops_reads = 3;
	if (gemini_prb_header_empty(gemini_prb_map_ramoops_signature,
				    gemini_prb_map_ramoops_start,
				    gemini_prb_map_ramoops_size) &&
	    gemini_prb_map_parallel_signature == ~0U &&
	    gemini_prb_map_parallel_start == ~0U &&
	    gemini_prb_map_parallel_size == ~0U)
		gemini_prb_map_reason = "ramoops-empty-parallel-all-ones";
	else if (gemini_prb_header_empty(gemini_prb_map_ramoops_signature,
					 gemini_prb_map_ramoops_start,
					 gemini_prb_map_ramoops_size) &&
		 gemini_prb_header_empty(gemini_prb_map_parallel_signature,
					 gemini_prb_map_parallel_start,
					 gemini_prb_map_parallel_size))
		gemini_prb_map_reason = "both-empty";
	else if (gemini_prb_map_ramoops_signature ==
		 gemini_prb_map_parallel_signature &&
		 gemini_prb_map_ramoops_start == gemini_prb_map_parallel_start &&
		 gemini_prb_map_ramoops_size == gemini_prb_map_parallel_size)
		gemini_prb_map_reason = "views-match-other";
	else
		gemini_prb_map_reason = "views-differ";
}
#endif

static bool gemini_prb_armed;

#ifdef CONFIG_PSTORE_GEMINI_A72_PLATFORM_PROVIDER_CLOCK_LEDGER
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_A72_PLATFORM_PROVIDER_CLOCK_SNAPSHOT_V1 token=GAPC-20260825-A "
	"checkpoint=before-clock slot=1 crc32=7a63713c\n",
	"====0.000000-D\n"
	"GEMINI_A72_PLATFORM_PROVIDER_CLOCK_SNAPSHOT_V1 token=GAPC-20260825-A "
	"checkpoint=after-clock slot=2 crc32=5773d4f6\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_PROVIDER_SNAPSHOT_LEDGER)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_A72_PLATFORM_PROVIDER_SNAPSHOT_V1 token=GAPP-20260825-A "
	"checkpoint=before-provider slot=1 crc32=0150f9c7\n",
	"====0.000000-D\n"
	"GEMINI_A72_PLATFORM_PROVIDER_SNAPSHOT_V1 token=GAPP-20260825-A "
	"checkpoint=after-provider slot=2 crc32=4fffb31e\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_SNAPSHOT_LEDGER)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_A72_PLATFORM_SNAPSHOT_V1 token=GAPS-20260824-A "
	"checkpoint=before-platform slot=1 crc32=a8bf2262\n",
	"====0.000000-D\n"
	"GEMINI_A72_PLATFORM_SNAPSHOT_V1 token=GAPS-20260824-A "
	"checkpoint=after-platform slot=2 crc32=ca566ccf\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_A72_EARLY_INITCALL_LEDGER)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_A72_EARLY_INIT_V1 token=GAEI-20260824-A "
	"checkpoint=pure-init outcome=commit slot=1 crc32=03d9627f\n",
	"====0.000000-D\n"
	"GEMINI_A72_EARLY_INIT_V1 token=GAEI-20260824-A "
	"checkpoint=core-init outcome=commit slot=2 crc32=57dd63b5\n",
};

static const char gemini_prb_refusal_record[] =
	"====0.000000-D\n"
	"GEMINI_A72_EARLY_INIT_V1 token=GAEI-20260824-A "
	"checkpoint=pure-init outcome=primary-refused slot=2 "
	"crc32=5767e326\n";
#elif defined(CONFIG_PSTORE_GEMINI_A72_GLOBAL_INITCALL_LEDGER)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_A72_INITCALL_V1 token=GAIC-20260824-A "
	"checkpoint=subsys-init slot=1 crc32=cf2a6946\n",
	"====0.000000-D\n"
	"GEMINI_A72_INITCALL_V1 token=GAIC-20260824-A "
	"checkpoint=fs-init slot=2 crc32=91ac2a49\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_A72_PHYSICAL_SOURCE_PRECAPTURE_LEDGER)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_A72_INIT_PROBE_V1 token=GAIP-20260824-A "
	"checkpoint=driver-init slot=1 crc32=85e5f336\n",
	"====0.000000-D\n"
	"GEMINI_A72_INIT_PROBE_V1 token=GAIP-20260824-A "
	"checkpoint=probe-enter slot=2 crc32=85116721\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_A72_PHYSICAL_SOURCE_LEDGER)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_A72_PHYSICAL_SOURCE_V1 token=GPSQ-20260824-A "
	"checkpoint=before-bigidvfs slot=1 crc32=47eaad49\n",
	"====0.000000-D\n"
	"GEMINI_A72_PHYSICAL_SOURCE_V1 token=GPSQ-20260824-A "
	"checkpoint=after-bigidvfs slot=2 crc32=d03ca6dc\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_PROTECTED_CLOCK_FIRST_DMESG_CALL_QUALIFICATION)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_PROTECTED_CLOCK_FIRST_DMESG_V1 token=GPCF-20260823-A "
	"checkpoint=before-clock slot=1 crc32=183854b2\n",
	"====0.000000-D\n"
	"GEMINI_PROTECTED_CLOCK_FIRST_DMESG_V1 token=GPCF-20260823-A "
	"checkpoint=after-clock slot=2 crc32=d14b85aa\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_FIRST_DMESG_ENTRY_QUALIFICATION)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_CLOCK_BACKEND_FIRST_DMESG_V1 token=GCBF-20260823-A "
	"checkpoint=driver-init slot=1 crc32=6197fd57\n",
	"====0.000000-D\n"
	"GEMINI_CLOCK_BACKEND_FIRST_DMESG_V1 token=GCBF-20260823-A "
	"checkpoint=probe-enter slot=2 crc32=61636940\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_FIRST_DMESG_WRITE_QUALIFICATION)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_MANUAL_CHECKPOINT_CONTROL_V1 token=GMCP-20260822-B "
	"checkpoint=manual-first slot=1 crc32=7785e4ce\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_CONTROL)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_MANUAL_CHECKPOINT_CONTROL_V1 token=GMCP-20260821-A "
	"checkpoint=manual-first slot=173 crc32=9576f05d\n",
	"====0.000000-D\n"
	"GEMINI_MANUAL_CHECKPOINT_CONTROL_V1 token=GMCP-20260821-A "
	"checkpoint=manual-second slot=174 crc32=c90b9e18\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_ENTRY_LEDGER)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_CLOCK_BACKEND_ENTRY_LEDGER_V1 token=GCBE-20260821-A "
	"checkpoint=driver-init slot=173 crc32=cda5d04d\n",
	"====0.000000-D\n"
	"GEMINI_CLOCK_BACKEND_ENTRY_LEDGER_V1 token=GCBE-20260821-A "
	"checkpoint=probe-enter slot=174 crc32=a3662888\n",
};
#elif defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_PROBE_GATE_LEDGER)
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_PROTECTED_READBACK_LEDGER_V2 token=GPRB-20260821-B "
	"checkpoint=probe-enter slot=173 crc32=06a9b43b\n",
	"====0.000000-D\n"
	"GEMINI_PROTECTED_READBACK_LEDGER_V2 token=GPRB-20260821-B "
	"checkpoint=gate-passed slot=174 crc32=41e86ca4\n",
};
#else
static const char * const gemini_prb_records[] = {
	"====0.000000-D\n"
	"GEMINI_PROTECTED_READBACK_LEDGER_V1 token=GPRB-20260821-A "
	"checkpoint=before-clock slot=173 crc32=08f2fe56\n",
	"====0.000000-D\n"
	"GEMINI_PROTECTED_READBACK_LEDGER_V1 token=GPRB-20260821-A "
	"checkpoint=after-clock slot=174 crc32=e477a18e\n",
};
#endif

static bool gemini_prb_slot_empty(void __iomem *slot)
{
	return readl(slot) == GEMINI_PRB_SIGNATURE &&
	       readl((u8 __iomem *)slot + 4) == 0 &&
	       readl((u8 __iomem *)slot + 8) == 0;
}

static bool gemini_prb_slot_available(void __iomem *slot)
{
#if defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_PROVIDER_CLOCK_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_PROVIDER_SNAPSHOT_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_SNAPSHOT_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_EARLY_INITCALL_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_GLOBAL_INITCALL_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PHYSICAL_SOURCE_PRECAPTURE_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PHYSICAL_SOURCE_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_RAW_ENTRY_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_RAW_WRITE_QUALIFICATION) || \
	defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_FIRST_DMESG_WRITE_QUALIFICATION) || \
	defined(CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_FIRST_DMESG_ENTRY_QUALIFICATION)
	return readl(slot) == ~0U &&
	       readl((u8 __iomem *)slot + 4) == ~0U &&
	       readl((u8 __iomem *)slot + 8) == ~0U;
#else
	return gemini_prb_slot_empty(slot);
#endif
}

static bool gemini_prb_slot_exact(void __iomem *slot, const char *record)
{
	size_t len = strlen(record);
	size_t i;

	if (readl(slot) != GEMINI_PRB_SIGNATURE ||
	    readl((u8 __iomem *)slot + 4) != len ||
	    readl((u8 __iomem *)slot + 8) != len)
		return false;
	for (i = 0; i < len; i++)
		if (readb((u8 __iomem *)slot + GEMINI_PRB_HEADER_SIZE + i) !=
		    record[i])
			return false;

	return true;
}

static bool gemini_prb_write(void __iomem *slot, const char *record)
{
	size_t len = strlen(record);
	size_t i;

	if (!gemini_prb_slot_available(slot) ||
	    len > GEMINI_PRB_SLOT_SIZE - GEMINI_PRB_HEADER_SIZE) {
		GEMINI_PRB_SET_STAGE("write-precondition-refused");
		return false;
	}

	memcpy_toio((u8 __iomem *)slot + GEMINI_PRB_HEADER_SIZE, record, len);
	wmb(); /* Commit payload before producer metadata. */
	writel(len, (u8 __iomem *)slot + 4);
	wmb(); /* Commit start before size. */
	writel(len, (u8 __iomem *)slot + 8);
#if defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_PROVIDER_CLOCK_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_PROVIDER_SNAPSHOT_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PLATFORM_SNAPSHOT_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_EARLY_INITCALL_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_GLOBAL_INITCALL_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PHYSICAL_SOURCE_PRECAPTURE_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_A72_PHYSICAL_SOURCE_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_RAW_ENTRY_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_RAW_WRITE_QUALIFICATION) || \
	defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_FIRST_DMESG_WRITE_QUALIFICATION) || \
	defined(CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_FIRST_DMESG_ENTRY_QUALIFICATION)
	wmb(); /* Make a raw record valid only after payload and metadata. */
	writel(GEMINI_PRB_SIGNATURE, slot);
#endif
	mb(); /* Order the complete local readback after the commit. */

	if (readl(slot) != GEMINI_PRB_SIGNATURE ||
	    readl((u8 __iomem *)slot + 4) != len ||
	    readl((u8 __iomem *)slot + 8) != len) {
		GEMINI_PRB_SET_STAGE("metadata-readback-refused");
		return false;
	}
	for (i = 0; i < len; i++) {
		if (readb((u8 __iomem *)slot + GEMINI_PRB_HEADER_SIZE + i) !=
		    record[i]) {
			GEMINI_PRB_SET_STAGE("payload-readback-refused");
			return false;
		}
	}

	GEMINI_PRB_SET_STAGE("success");
	return true;
}

#if defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_PROBE_GATE_LEDGER) || \
	defined(CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_ENTRY_LEDGER)
static bool gemini_prb_minimal_dt(void)
{
	struct device_node *node;
	struct resource resource;
	bool exact = false;

	if (!of_machine_is_compatible("planet,gemini-pda"))
		return false;

	node = of_find_node_by_path("/reserved-memory/ramoops@44410000");
	if (!node)
		return false;
	if (of_device_is_compatible(node, "ramoops") &&
	    !of_address_to_resource(node, 0, &resource) &&
	    resource.start == GEMINI_PRB_RESERVE_BASE &&
	    resource_size(&resource) == GEMINI_PRB_RESERVE_SIZE &&
	    of_property_read_bool(node, "no-map"))
		exact = true;
	of_node_put(node);

	return exact;
}
#endif

static bool gemini_prb_exact_dt(void)
{
	struct device_node *node;
	struct resource resource;
	const char *model;
	u32 value;
	bool exact = false;

	if (!of_machine_is_compatible("planet,gemini-pda") ||
	    of_property_read_string(of_root, "model", &model) ||
	    strcmp(model, "MT6797X"))
		return false;

	node = of_find_node_by_path("/reserved-memory/ramoops@44410000");
	if (!node)
		return false;
	if (!of_device_is_compatible(node, "ramoops") ||
	    of_address_to_resource(node, 0, &resource) ||
	    resource.start != GEMINI_PRB_RESERVE_BASE ||
	    resource_size(&resource) != GEMINI_PRB_RESERVE_SIZE ||
	    !of_property_read_bool(node, "no-map"))
		goto out;

	if (of_property_read_u32(node, "record-size", &value) ||
	    value != 0x1000)
		goto out;
	if (of_property_read_u32(node, "console-size", &value) ||
	    value != 0x10000)
		goto out;
	if (of_property_read_u32(node, "ftrace-size", &value) ||
	    value != 0x1000)
		goto out;
	if (of_property_read_u32(node, "pmsg-size", &value) ||
	    value != 0x20000)
		goto out;
	if (of_property_read_u32(node, "mem-type", &value) || value)
		goto out;
	exact = true;
out:
	of_node_put(node);
	return exact;
}

static bool gemini_prb_prefix_valid(void __iomem *ledger,
				    unsigned int checkpoint)
{
	unsigned int i;

	for (i = 0; i < GEMINI_PRB_SLOT_COUNT; i++) {
		void __iomem *slot = (u8 __iomem *)ledger +
				       i * GEMINI_PRB_SLOT_SIZE;

		if (checkpoint == 1 && i == GEMINI_PRB_FIRST_OWNED_SLOT) {
			if (!gemini_prb_slot_exact(slot, gemini_prb_records[0])) {
				gemini_prb_capture_prefix(checkpoint, i, false,
							  slot);
				return false;
			}
		} else if (!gemini_prb_slot_available(slot)) {
			gemini_prb_capture_prefix(checkpoint, i, true, slot);
			return false;
		}
	}

	return true;
}

bool gemini_protected_readback_ledger_checkpoint(unsigned int checkpoint)
{
	void __iomem *ledger;
	void __iomem *slot;
	bool written = false;

	GEMINI_PRB_SET_STAGE("call-entry");
#if defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_RAW_WRITE_QUALIFICATION) || \
	defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_FIRST_DMESG_WRITE_QUALIFICATION)
	if (checkpoint != 0 || gemini_prb_armed) {
#else
	if (checkpoint > 1 || (checkpoint == 0 && gemini_prb_armed) ||
	    (checkpoint == 1 && !gemini_prb_armed)) {
#endif
		GEMINI_PRB_SET_STAGE("sequence-refused");
		return false;
	}
#ifdef CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_ENTRY_LEDGER
	if (!gemini_prb_minimal_dt())
		return false;
#elif defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_PROBE_GATE_LEDGER)
	if (checkpoint == 0) {
		if (!gemini_prb_minimal_dt())
			return false;
	} else if (!gemini_prb_exact_dt()) {
		return false;
	}
#else
	if (!gemini_prb_exact_dt()) {
		GEMINI_PRB_SET_STAGE("dt-refused");
		return false;
	}
#endif

	ledger = ioremap_wc(GEMINI_PRB_LEDGER_BASE,
			    GEMINI_PRB_SLOT_COUNT * GEMINI_PRB_SLOT_SIZE);
	if (!ledger) {
		GEMINI_PRB_SET_STAGE("map-refused");
		return false;
	}
#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_MAP_CONTROL
	if (checkpoint == 0) {
		gemini_prb_capture_map_control(ledger);
		GEMINI_PRB_SET_STAGE("map-control-observed");
		goto out;
	}
#endif
	if (!gemini_prb_prefix_valid(ledger, checkpoint)) {
		GEMINI_PRB_SET_STAGE("prefix-refused");
		goto out;
	}

	slot = (u8 __iomem *)ledger +
	       (GEMINI_PRB_FIRST_OWNED_SLOT + checkpoint) *
	       GEMINI_PRB_SLOT_SIZE;
	written = gemini_prb_write(slot, gemini_prb_records[checkpoint]);
out:
	iounmap(ledger);
	if (checkpoint == 0 && written)
		gemini_prb_armed = true;
	else if (checkpoint == 1)
		gemini_prb_armed = false;

	return written;
}

#ifdef CONFIG_PSTORE_GEMINI_A72_EARLY_INITCALL_LEDGER
static bool gemini_a72_pure_refusal_checkpoint(void)
{
	void __iomem *ledger;
	void __iomem *slot;
	bool written = false;

	if (!gemini_prb_exact_dt())
		return false;
	ledger = ioremap_wc(GEMINI_PRB_LEDGER_BASE,
			    GEMINI_PRB_SLOT_COUNT * GEMINI_PRB_SLOT_SIZE);
	if (!ledger)
		return false;
	slot = (u8 __iomem *)ledger + GEMINI_PRB_SLOT_SIZE;
	if (gemini_prb_slot_available(slot))
		written = gemini_prb_write(slot, gemini_prb_refusal_record);
	iounmap(ledger);

	return written;
}

static int __init gemini_a72_pure_initcall_checkpoint(void)
{
	if (gemini_protected_readback_ledger_checkpoint(0))
		return 0;
	return gemini_a72_pure_refusal_checkpoint() ? -EAGAIN : -EIO;
}
pure_initcall(gemini_a72_pure_initcall_checkpoint);

static int __init gemini_a72_core_initcall_checkpoint(void)
{
	return gemini_protected_readback_ledger_checkpoint(1) ? 0 : -EIO;
}
core_initcall(gemini_a72_core_initcall_checkpoint);
#endif

#ifdef CONFIG_PSTORE_GEMINI_A72_GLOBAL_INITCALL_LEDGER
static int __init gemini_a72_subsys_initcall_checkpoint(void)
{
	return gemini_protected_readback_ledger_checkpoint(0) ? 0 : -EIO;
}
subsys_initcall(gemini_a72_subsys_initcall_checkpoint);

static int __init gemini_a72_fs_initcall_checkpoint(void)
{
	return gemini_protected_readback_ledger_checkpoint(1) ? 0 : -EIO;
}
fs_initcall(gemini_a72_fs_initcall_checkpoint);
#endif

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_CONTROL
static int __init gemini_protected_readback_manual_control_init(void)
{
	bool first;
	bool second;

	first = gemini_protected_readback_ledger_checkpoint(0);
#if defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_RAW_WRITE_QUALIFICATION) || \
	defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_FIRST_DMESG_WRITE_QUALIFICATION)
	second = false;
#else
	second = first && gemini_protected_readback_ledger_checkpoint(1);
#endif
	pr_info("GEMINI_MANUAL_CHECKPOINT_CONTROL_LIVE_V1 "
		"first=%u second=%u retained_writes=%u protected_calls=0 "
		"cpu_requests=0\n",
		first, second, first + second);

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_STAGE_CONTROL
	pr_info("%s first=%u second=%u stage=%s writes=%u protected=%u cpu=%u\n",
		"GEMINI_MANUAL_CHECKPOINT_STAGE_V1",
		first, second, gemini_prb_stage, first + second, 0, 0);
#endif

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_RAW_WRITE_QUALIFICATION
	pr_info("%s commit=%u stage=%s writes=%u protected=%u clock=%u "
		"bigidvfs=%u cpu=%u\n",
		"GEMINI_MANUAL_RAW_WRITE_QUALIFICATION_LIVE_V1",
		first, gemini_prb_stage, first, 0, 0, 0, 0);
#endif

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_FIRST_DMESG_WRITE_QUALIFICATION
	pr_info("%s commit=%u stage=%s writes=%u record=%u address=%llx "
		"protected=%u clock=%u bigidvfs=%u cpu=%u\n",
		"GEMINI_FIRST_DMESG_RAW_WRITE_QUALIFICATION_LIVE_V1",
		first, gemini_prb_stage, first, 1, GEMINI_PRB_LEDGER_BASE,
		0, 0, 0, 0);
#endif

#if defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_PREFIX_CONTROL) && \
	!defined(CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_MAP_CONTROL)
	pr_info("%s cp=%u slot=%u why=%s hdr=%08x/%u/%u reads=3\n",
		"GEMINI_MANUAL_CHECKPOINT_PREFIX_V1",
		gemini_prb_prefix_checkpoint, gemini_prb_prefix_slot_index,
		gemini_prb_prefix_reason, gemini_prb_prefix_signature,
		gemini_prb_prefix_start, gemini_prb_prefix_size);
#endif

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_MANUAL_MAP_CONTROL
	pr_info("%s r=171 p=%llx why=%s rh=%08x/%u/%u ph=%08x/%u/%u rr=%u pr=3 w=0\n",
		"GEMINI_MANUAL_CHECKPOINT_MAP_CONTROL_V1", GEMINI_PRB_LEDGER_BASE,
		gemini_prb_map_reason, gemini_prb_map_ramoops_signature,
		gemini_prb_map_ramoops_start, gemini_prb_map_ramoops_size,
		gemini_prb_map_parallel_signature, gemini_prb_map_parallel_start,
		gemini_prb_map_parallel_size, gemini_prb_map_ramoops_reads);
#endif

	return 0;
}
late_initcall(gemini_protected_readback_manual_control_init);
#endif
