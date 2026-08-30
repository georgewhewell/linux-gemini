/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_H

#include <linux/ioport.h>
#include <linux/mutex.h>
#include <linux/types.h>

/*
 * c5b0be85017ad0c599725e8273842efdbecdd88a,
 * drivers/misc/mediatek/connectivity/common/common_main/mt6797/
 * mtk_wcn_consys_hw.c:301-309 enables VCN18 then waits 240us. Lines 311-326
 * enable VCN28 when co_clock_type is zero; Gemini's WMT_SOC.cfg:6 specifies
 * co_clock_flag=0. Lines 341-354 prepare PWRON_CONFG_EN then enable
 * SCP_SYS_CONN through CCF, and lines 401-402 wait 30us before lines 415-420
 * read the chip ID.
 */
#define MT6797_CONSYS_LIFECYCLE_ABI	1
#define MT6797_CONSYS_VCN18_SETTLE_US	240
#define MT6797_CONSYS_DOMAIN_SETTLE_US	30

/*
 * The vendor DT declares this as a no-map 2 MiB reservation allocated in
 * 0x40000000..0xc0000000.  It is an AP/CONNSYS shared debug/EMI window, not a
 * firmware-patch load address.  Future runtime owners must validate this
 * contract before they alter CONSYS state, but must not infer a patch address
 * from it.
 */
#define MT6797_CONSYS_SHARED_MEM_SIZE	0x200000ULL
#define MT6797_CONSYS_SHARED_MEM_MIN	0x40000000ULL
#define MT6797_CONSYS_SHARED_MEM_END	0xc0000000ULL

/*
 * ERROR is sticky.  A backend cleanup error means the core cannot establish
 * that the subsystem is off, so callers must not issue another identity vote.
 */
enum mt6797_consys_state {
	MT6797_CONSYS_OFF,
	MT6797_CONSYS_IDENTITY_HELD,
	MT6797_CONSYS_ERROR,
};

enum mt6797_consys_poll {
	MT6797_CONSYS_POLL_CHIP_READY,
};

struct mt6797_consys_timeouts {
	u32 poll_interval_us;
	u32 chip_ready_timeout_us;
};

struct mt6797_consys_lifecycle;

/*
 * Caller-owned, opaque-in-practice capability for one long-lived CONSYS
 * owner.  Zero-initialize it before acquire(). Its generation makes a lease
 * from an earlier release fail closed after a later acquisition.
 */
struct mt6797_consys_runtime_lease {
	struct mt6797_consys_lifecycle *lifecycle;
	u64 generation;
};

/*
 * The backend owns all hardware access.  Each cleanup operation must be
 * idempotent and tolerate a failed matching setup operation: a setup write
 * can have taken effect before it reports an error.  poll() returns 0 when
 * the condition is true, -EAGAIN while it is pending, or another errno.
 *
 * No MT6797 register offsets or protection masks are represented here.  The
 * Vendor source at c5b0be85017ad0c599725e8273842efdbecdd88a,
 * drivers/misc/mediatek/connectivity/common/common_main/mt6797/
 * mtk_wcn_consys_hw.c, holds reset while it enables the SCP_SYS_CONN domain
 * before it observes the chip ID. This core stops at that observation; it
 * never releases reset, configures AFE, or loads firmware. The two configure
 * operations are persistent, idempotent writes rather than votes: vendor
 * hw.c:328-330 and :341-342 set them, while its off path at :611-750 does not
 * restore either. The PWRON operation explicitly covers SPM PWRON_CONFG_EN;
 * successful enable_scp_sys_conn() means its CCF/SCPSYS provider has completed
 * domain power-on. Therefore this core does not read the raw power ACK
 * registers; their acknowledgment boundary belongs to that provider. The
 * same source's include/mtk_wcn_consys_hw.h:42 sets
 * CONSYS_AHB_CLK_MAGEMENT to 0, so an AHB bus-clock operation is deliberately
 * absent. The bus-protection sequence belongs to the MT6797 SCPSYS provider,
 * which follows the configured vendor CCF path rather than this core.
 */
struct mt6797_consys_ops {
	u32 abi;
	int (*enable_vcn18)(void *context);
	int (*disable_vcn18)(void *context);
	int (*enable_vcn28)(void *context);
	int (*disable_vcn28)(void *context);
	/* Persistent topckgen sleep-mask configuration before reset/domain. */
	int (*configure_sleep_mask)(void *context);
	int (*assert_reset)(void *context);
	/* Persistent SPM PWRON_CONFG_EN configuration before the domain vote. */
	int (*configure_pwron)(void *context);
	/* CCF/SCPSYS SCP_SYS_CONN vote; its success owns the power ACK boundary. */
	int (*enable_scp_sys_conn)(void *context);
	int (*disable_scp_sys_conn)(void *context);
	int (*poll)(void *context, enum mt6797_consys_poll poll);
	void (*delay_us)(void *context, u32 min, u32 max);
};

struct mt6797_consys_lifecycle {
	const struct mt6797_consys_ops *ops;
	void *context;
	struct mt6797_consys_timeouts timeouts;
	/* Serializes state and users, including the identity transaction. */
	struct mutex lock;
	enum mt6797_consys_state state;
	u32 users;
	const struct mt6797_consys_runtime_lease *runtime_lease;
	u32 runtime_pins;
	u64 runtime_generation;
};

int mt6797_consys_lifecycle_init(struct mt6797_consys_lifecycle *lifecycle,
				 const struct mt6797_consys_ops *ops, void *context,
				 const struct mt6797_consys_timeouts *timeouts);
int mt6797_consys_identity_get(struct mt6797_consys_lifecycle *lifecycle);
int mt6797_consys_identity_put(struct mt6797_consys_lifecycle *lifecycle);

/*
 * Acquire one exclusive lifecycle capability.  Unlike identity_get(), this
 * keeps the reviewed bring-up prefix held for a controller lifetime. Hardware
 * children pin it before BTIF setup and release it only after quiescing.
 */
int mt6797_consys_runtime_lease_acquire(
		struct mt6797_consys_lifecycle *lifecycle,
		struct mt6797_consys_runtime_lease *lease);
int mt6797_consys_runtime_lease_release(
		struct mt6797_consys_runtime_lease *lease);
bool mt6797_consys_runtime_lease_active(
		const struct mt6797_consys_runtime_lease *lease);
int mt6797_consys_runtime_lease_pin(
		const struct mt6797_consys_runtime_lease *lease);
void mt6797_consys_runtime_lease_unpin(
		const struct mt6797_consys_runtime_lease *lease);
enum mt6797_consys_state
mt6797_consys_lifecycle_state(struct mt6797_consys_lifecycle *lifecycle);
u32 mt6797_consys_lifecycle_users(struct mt6797_consys_lifecycle *lifecycle);

/* Validate only the documented shared-memory geometry; this does not map it. */
bool mt6797_consys_shared_mem_valid(phys_addr_t start, resource_size_t size);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_H */
