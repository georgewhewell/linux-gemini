/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_HANDOFF_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_HANDOFF_H

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct mt6797_dvfsp_handoff;

/*
 * The CSPM base is valid only for the duration of this callback. Consumers
 * must not retain it. The handoff owner serializes the callback with its
 * validation, PM lifecycle, and every other admitted CSPM transaction.
 */
typedef int (*mt6797_dvfsp_cspm_transaction_fn)(void __iomem *cspm,
						void *context);

struct mt6797_dvfsp_i2c6_lease {
	u64 generation;
	u64 cookie;
};

static inline int
mt6797_dvfsp_i2c6_merge_transfer_result(int transfer_result, int lease_result)
{
	/* Lease teardown is a 0-or-negative status, never a transfer count. */
	if (transfer_result < 0 || lease_result >= 0)
		return transfer_result;

	return lease_result;
}

/*
 * This private contract describes an authoritative state owner. It is not
 * an OPP, clock, regulator, or userspace ABI. No owner is registered by
 * default. The owner callbacks must hold their own transition lock while
 * sampling or validating hardware state and must not recursively call this
 * registry.
 */
#define MT6797_DVFSP_STATE_OWNER_ABI		1
#define MT6797_DVFSP_STATE_CLUSTER_MASK	GENMASK(3, 0)
#define MT6797_DVFSP_STATE_CLUSTER_COUNT	4

enum mt6797_dvfsp_state_cluster_id {
	MT6797_DVFSP_STATE_CLUSTER_LL,
	MT6797_DVFSP_STATE_CLUSTER_L,
	MT6797_DVFSP_STATE_CLUSTER_B,
	MT6797_DVFSP_STATE_CLUSTER_CCI,
};

enum mt6797_dvfsp_state_field {
	MT6797_DVFSP_STATE_FIELD_FLAGS = 0,
	MT6797_DVFSP_STATE_FIELD_OPP,
	MT6797_DVFSP_STATE_FIELD_FREQUENCY,
	MT6797_DVFSP_STATE_FIELD_VOLTAGE,
	MT6797_DVFSP_STATE_FIELD_VSRAM,
	MT6797_DVFSP_STATE_FIELD_CEILING,
	MT6797_DVFSP_STATE_FIELD_FLOOR,
	MT6797_DVFSP_STATE_FIELD_CLOCK,
	MT6797_DVFSP_STATE_FIELD_RAIL,
};

#define MT6797_DVFSP_STATE_FIELD_ALL	GENMASK(8, 0)
#define MT6797_DVFSP_STATE_FLAG_PRESENT	BIT(0)
#define MT6797_DVFSP_STATE_FLAG_ON		BIT(1)

struct mt6797_dvfsp_state_cluster {
	u32 valid_fields;
	u32 state_flags;
	u32 opp_id;
	u32 frequency_khz;
	u32 voltage_uv;
	u32 vsram_uv;
	u32 ceiling_khz;
	u32 floor_khz;
	u32 clock_state;
	u32 rail_state;
};
#define MT6797_DVFSP_STATE_PROVENANCE_ABI		2
#define MT6797_DVFSP_STATE_PROVENANCE_EFUSE_VARIANT	BIT(0)
#define MT6797_DVFSP_STATE_PROVENANCE_EEM_PTP		BIT(1)
#define MT6797_DVFSP_STATE_PROVENANCE_PPM_LIMIT	BIT(2)
#define MT6797_DVFSP_STATE_PROVENANCE_LIVE_VPROC	BIT(3)
#define MT6797_DVFSP_STATE_PROVENANCE_LIVE_VSRAM	BIT(4)
#define MT6797_DVFSP_STATE_PROVENANCE_CLOCK_OWNER	BIT(5)
#define MT6797_DVFSP_STATE_PROVENANCE_RAIL_OWNER	BIT(6)
#define MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL	GENMASK(6, 0)

struct mt6797_dvfsp_state_provenance {
	u32 abi;
	u32 source_mask;
	u32 variant_id;
	u64 table_epoch;
	u64 calibration_handle;
};

#define MT6797_DVFSP_CALIBRATION_STATE_ABI		2
#define MT6797_DVFSP_CALIBRATION_STATE_PHASE_INIT01	1
#define MT6797_DVFSP_CALIBRATION_STATE_PHASE_INIT02	2
#define MT6797_DVFSP_CALIBRATION_STATE_PHASE_MON		3
#define MT6797_DVFSP_CALIBRATION_STATE_BANK_BIG		BIT(0)
#define MT6797_DVFSP_CALIBRATION_STATE_BANK_L		BIT(3)
#define MT6797_DVFSP_CALIBRATION_STATE_BANK_2L		BIT(4)
#define MT6797_DVFSP_CALIBRATION_STATE_BANK_CCI		BIT(5)
#define MT6797_DVFSP_CALIBRATION_STATE_BANK_ALL		\
	(MT6797_DVFSP_CALIBRATION_STATE_BANK_BIG | \
	 MT6797_DVFSP_CALIBRATION_STATE_BANK_L | \
	 MT6797_DVFSP_CALIBRATION_STATE_BANK_2L | \
	 MT6797_DVFSP_CALIBRATION_STATE_BANK_CCI)
#define MT6797_DVFSP_CALIBRATION_TABLE_MAX		16

struct mt6797_dvfsp_calibration_table_entry {
	u32 frequency_khz;
	u32 vproc_uv;
	u32 vsram_uv;
	u32 ppm_limit_khz;
};

struct mt6797_dvfsp_calibration_cluster {
	u32 table_count;
	struct mt6797_dvfsp_calibration_table_entry
		table[MT6797_DVFSP_CALIBRATION_TABLE_MAX];
};

struct mt6797_dvfsp_calibration_state {
	u32 abi;
	u32 phase;
	u32 bank_mask;
	s32 temperature_millic;
	u64 thermal_generation;
	u64 clock_owner_generation;
	u64 rail_owner_generation;
	/* Common provider epoch shared by calibration and live state. */
	u64 source_generation;
	struct mt6797_dvfsp_state_provenance provenance;
	struct mt6797_dvfsp_calibration_cluster
		cluster[MT6797_DVFSP_STATE_CLUSTER_COUNT];
};

struct mt6797_dvfsp_state_snapshot {
	u32 abi;
	u32 cluster_mask;
	u64 generation;
	u64 owner_handle;
	u64 transition_handle;
	struct mt6797_dvfsp_state_provenance provenance;
	struct mt6797_dvfsp_state_cluster
		cluster[MT6797_DVFSP_STATE_CLUSTER_COUNT];
};

struct mt6797_dvfsp_state_hold {
	u64 generation;
	u32 cluster_mask;
	u32 reserved;
	u64 owner_handle;
	u64 transition_handle;
	struct mt6797_dvfsp_state_provenance provenance;
};

enum mt6797_dvfsp_state_invalidation_reason {
	MT6797_DVFSP_STATE_INVALID_OWNER_REMOVED,
	MT6797_DVFSP_STATE_INVALID_CLOCK_TRANSITION,
	MT6797_DVFSP_STATE_INVALID_RAIL_TRANSITION,
	MT6797_DVFSP_STATE_INVALID_SUSPEND_RESUME,
	MT6797_DVFSP_STATE_INVALID_PCM_FAULT,
};

struct mt6797_dvfsp_state_owner_identity;
struct mt6797_dvfsp_state_owner_ops {
	int (*identify)(void *context,
					       struct mt6797_dvfsp_state_owner_identity *identity);
	int (*snapshot)(void *context,
			       struct mt6797_dvfsp_state_snapshot *snapshot);
	int (*validate)(void *context,
			       const struct mt6797_dvfsp_state_snapshot *snapshot);
	int (*hold)(void *context,
		    const struct mt6797_dvfsp_state_snapshot *snapshot,
		    struct mt6797_dvfsp_state_hold *hold);
	int (*release)(void *context,
		      const struct mt6797_dvfsp_state_hold *hold);
	void (*invalidate)(void *context,
			   enum mt6797_dvfsp_state_invalidation_reason reason);
};
#define MT6797_DVFSP_STATE_OWNER_IDENTITY_ABI	1
#define MT6797_DVFSP_STATE_OWNER_RESOURCE_CPU_PLL	BIT(0)
#define MT6797_DVFSP_STATE_OWNER_RESOURCE_BIG_CLUSTER	BIT(1)
#define MT6797_DVFSP_STATE_OWNER_RESOURCE_ALL	GENMASK(1, 0)
#define MT6797_DVFSP_STATE_OWNER_BACKEND_MCUMIXED_DVFSP	1
#define MT6797_DVFSP_STATE_OWNER_BACKEND_BIGIDVFS_SMCCC	2
struct mt6797_dvfsp_state_owner_identity {
	u32 abi;
	u32 resource_mask;
	u32 cpu_pll_backend;
	u32 big_cluster_backend;
	u32 reserved;
	u64 owner_handle;
	u64 transition_handle;
	struct mt6797_dvfsp_state_provenance provenance;
};
#define MT6797_DVFSP_STATE_BACKEND_ABI		1
#define MT6797_DVFSP_STATE_BACKEND_CPU_PLL_CLUSTERS \
	(BIT(MT6797_DVFSP_STATE_CLUSTER_LL) | \
	 BIT(MT6797_DVFSP_STATE_CLUSTER_L) | \
	 BIT(MT6797_DVFSP_STATE_CLUSTER_CCI))
#define MT6797_DVFSP_STATE_BACKEND_BIG_CLUSTER_CLUSTERS \
	BIT(MT6797_DVFSP_STATE_CLUSTER_B)

struct mt6797_dvfsp_state_backend_snapshot {
	u32 abi;
	u32 backend;
	u32 resource_mask;
	u32 cluster_mask;
	u64 generation;
	u64 owner_handle;
	u64 transition_handle;
	struct mt6797_dvfsp_state_provenance provenance;
	struct mt6797_dvfsp_state_cluster
		cluster[MT6797_DVFSP_STATE_CLUSTER_COUNT];
};

struct mt6797_dvfsp_state_backend_ops {
	u32 abi;
	u32 backend;
	u32 resource_mask;
	u32 cluster_mask;
	int (*snapshot)(void *context,
			struct mt6797_dvfsp_state_backend_snapshot *snapshot);
	int (*validate)(void *context,
			const struct mt6797_dvfsp_state_backend_snapshot *snapshot);
	int (*hold)(void *context,
			const struct mt6797_dvfsp_state_backend_snapshot *snapshot,
			struct mt6797_dvfsp_state_hold *hold);
	int (*release)(void *context,
			const struct mt6797_dvfsp_state_hold *hold);
	void (*invalidate)(void *context,
			enum mt6797_dvfsp_state_invalidation_reason reason);
};

#define MT6797_DVFSP_STATE_CALIBRATION_ABI	1

struct mt6797_dvfsp_state_calibration_hold {
	u64 generation;
	u64 transition_handle;
	u64 owner_handle;
	struct mt6797_dvfsp_state_provenance provenance;
};

struct mt6797_dvfsp_state_calibration_ops {
	u32 abi;
	int (*snapshot)(void *context,
			struct mt6797_dvfsp_state_provenance *provenance);
	int (*snapshot_state)(void *context,
			struct mt6797_dvfsp_calibration_state *state);
	int (*validate)(void *context,
			const struct mt6797_dvfsp_state_provenance *provenance);
	int (*hold)(void *context,
			const struct mt6797_dvfsp_state_snapshot *snapshot,
			struct mt6797_dvfsp_state_calibration_hold *hold);
	int (*release)(void *context,
			const struct mt6797_dvfsp_state_calibration_hold *hold);
	void (*invalidate)(void *context,
			enum mt6797_dvfsp_state_invalidation_reason reason);
};

#define MT6797_DVFSP_STATE_TRANSITION_LOCK_ABI	1

struct mt6797_dvfsp_state_transition_lock_ops {
	u32 abi;
	int (*lock)(void *context);
	void (*unlock)(void *context);
};

/* Keep the backend descriptors and callbacks immutable while registered. */
struct mt6797_dvfsp_protected_state_owner {
	const struct mt6797_dvfsp_state_backend_ops *cpu_pll_ops;
	void *cpu_pll_context;
	const struct mt6797_dvfsp_state_backend_ops *big_cluster_ops;
	void *big_cluster_context;
	u64 owner_handle;
	u64 transition_handle;
	const struct mt6797_dvfsp_state_calibration_ops *calibration_ops;
	void *calibration_context;
	const struct mt6797_dvfsp_state_transition_lock_ops *transition_lock_ops;
	void *transition_lock_context;
	struct mt6797_dvfsp_state_provenance provenance;
};
#define MT6797_DVFSP_PCM_ADAPTER_ABI		1
#define MT6797_DVFSP_PCM_IMAGE_HASH_BYTES	32
#define MT6797_DVFSP_PCM_RESOURCE_CSPM		BIT(0)
#define MT6797_DVFSP_PCM_RESOURCE_CSRAM		BIT(1)
#define MT6797_DVFSP_PCM_RESOURCE_I2C_APPM	BIT(2)
#define MT6797_DVFSP_PCM_RESOURCE_SEMAPHORE	BIT(3)
#define MT6797_DVFSP_PCM_RESOURCE_ALL		GENMASK(3, 0)
#define MT6797_DVFSP_PCM_CSPM_BASE		0x11015000ULL
#define MT6797_DVFSP_PCM_CSPM_LENGTH		0x1000
#define MT6797_DVFSP_PCM_CSRAM_BASE		0x0012a000ULL
#define MT6797_DVFSP_PCM_CSRAM_LENGTH		0x3000

enum mt6797_dvfsp_pcm_adapter_phase {
	MT6797_DVFSP_PCM_PHASE_UNAVAILABLE,
	MT6797_DVFSP_PCM_PHASE_SNAPSHOTTED,
	MT6797_DVFSP_PCM_PHASE_STATE_HELD,
	MT6797_DVFSP_PCM_PHASE_RESOURCES_HELD,
	MT6797_DVFSP_PCM_PHASE_IMAGE_READY,
	MT6797_DVFSP_PCM_PHASE_RESET_INITIALIZED,
	MT6797_DVFSP_PCM_PHASE_IMAGE_ACKED,
	MT6797_DVFSP_PCM_PHASE_CONTROL_INITIALIZED,
	MT6797_DVFSP_PCM_PHASE_RUNNING,
	MT6797_DVFSP_PCM_PHASE_LEASE_REGISTERED,
	MT6797_DVFSP_PCM_PHASE_INVALIDATED,
	MT6797_DVFSP_PCM_PHASE_FAULTED,
};

enum mt6797_dvfsp_pcm_adapter_fault {
	MT6797_DVFSP_PCM_FAULT_OWNER_REMOVED,
	MT6797_DVFSP_PCM_FAULT_STATE_CHANGED,
	MT6797_DVFSP_PCM_FAULT_RESOURCE_IDENTITY,
	MT6797_DVFSP_PCM_FAULT_IMAGE_IDENTITY,
	MT6797_DVFSP_PCM_FAULT_CHECKPOINT,
	MT6797_DVFSP_PCM_FAULT_TEARDOWN,
};

struct mt6797_dvfsp_pcm_image_identity {
	u32 abi;
	u32 target_revision;
	u32 loader_domain;
	u32 reserved;
	u64 physical_base;
	u32 length;
	u32 alignment;
	u8 sha256[MT6797_DVFSP_PCM_IMAGE_HASH_BYTES];
};

struct mt6797_dvfsp_pcm_resource_identity {
	u32 abi;
	u32 resource_mask;
	u64 cspm_base;
	u32 cspm_length;
	u32 reserved;
	u64 csram_base;
	u32 csram_length;
	u32 reserved2;
	u64 owner_handle;
};

struct mt6797_dvfsp_pcm_adapter_identity {
	u32 abi;
	u32 cluster_mask;
	u64 state_generation;
	u64 state_owner_handle;
	struct mt6797_dvfsp_pcm_image_identity image;
	struct mt6797_dvfsp_pcm_resource_identity resources;
};

struct mt6797_dvfsp_pcm_adapter_ops {
	int (*resources_acquire)(void *context,
				 const struct mt6797_dvfsp_pcm_adapter_identity *identity,
				 struct mt6797_dvfsp_pcm_resource_identity *resources);
	int (*image_prepare)(void *context,
				const struct mt6797_dvfsp_pcm_adapter_identity *identity,
				struct mt6797_dvfsp_pcm_image_identity *image);
	int (*reset_initialize)(void *context,
				       const struct mt6797_dvfsp_pcm_adapter_identity *identity);
	int (*image_acknowledge)(void *context,
					const struct mt6797_dvfsp_pcm_adapter_identity *identity);
	int (*control_initialize)(void *context,
					  const struct mt6797_dvfsp_pcm_adapter_identity *identity);
	int (*run)(void *context,
		   const struct mt6797_dvfsp_pcm_adapter_identity *identity);
	int (*lease_register)(void *context,
				      const struct mt6797_dvfsp_pcm_adapter_identity *identity);
	int (*lease_unregister)(void *context,
					const struct mt6797_dvfsp_pcm_adapter_identity *identity);
	int (*teardown)(void *context,
				const struct mt6797_dvfsp_pcm_adapter_identity *identity,
				enum mt6797_dvfsp_pcm_adapter_phase phase);
	void (*invalidate)(void *context,
			  enum mt6797_dvfsp_pcm_adapter_fault fault);
};

/*
 * This is a private platform callback, not a userspace or regulator ABI.
 * The callback owner is external to Linux and is not present by default.
 */
#define MT6797_DVFSP_I2C6_FW_ABI		1
#define MT6797_DVFSP_I2C6_FW_USER		1
#define MT6797_DVFSP_I2C6_FW_PAUSE_SOURCE	0x2
#define MT6797_DVFSP_I2C6_FW_SW_PAUSE_BIT	BIT(13)
#define MT6797_DVFSP_I2C6_FW_DONE_BIT		BIT(15)
#define MT6797_DVFSP_I2C6_FW_WORDS		GENMASK(2, 0)
#define MT6797_DVFSP_I2C6_FW_TIMEOUT_US	2000

struct mt6797_dvfsp_i2c6_fw_lease {
	u64 generation;
	u64 cookie;
	u64 owner_handle;
};

struct mt6797_dvfsp_i2c6_fw_request {
	u32 abi;
	u32 user;
	u32 pause_source;
	u32 sw_pause_bit;
	u32 fw_done_bit;
	u32 timeout_us;
	u64 generation;
	u64 cookie;
	u64 owner_handle;
};

struct mt6797_dvfsp_i2c6_fw_response {
	u32 abi;
	u32 user;
	s32 status;
	u32 returned;
	u32 pause_source_map;
	u32 sw_pause_words;
	u32 fw_done_words;
	u32 reserved;
	u64 generation;
	u64 cookie;
	u64 owner_handle;
};

struct mt6797_dvfsp_i2c6_fw_ops {
	int (*acquire)(void *context,
			      const struct mt6797_dvfsp_i2c6_fw_request *request,
			      struct mt6797_dvfsp_i2c6_fw_response *response);
	int (*release)(void *context,
		       const struct mt6797_dvfsp_i2c6_fw_request *request,
		       struct mt6797_dvfsp_i2c6_fw_response *response);
};

#if IS_REACHABLE(CONFIG_MTK_MT6797_DVFSP_HANDOFF)
struct mt6797_dvfsp_handoff *mt6797_dvfsp_handoff_get(struct device *consumer);
int mt6797_dvfsp_handoff_require_ready(struct mt6797_dvfsp_handoff *handoff);
int mt6797_dvfsp_cspm_transaction(struct mt6797_dvfsp_handoff *handoff,
				  mt6797_dvfsp_cspm_transaction_fn operation,
				  void *context);
int mt6797_dvfsp_handoff_begin_i2c6_transfer(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_i2c6_lease *lease);
int mt6797_dvfsp_handoff_end_i2c6_transfer(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_lease *lease, int transfer_result);
int mt6797_dvfsp_handoff_i2c6_fw_register(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_fw_ops *ops, void *context);
int mt6797_dvfsp_handoff_i2c6_fw_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_fw_ops *ops, void *context);
int mt6797_dvfsp_handoff_i2c6_fw_acquire(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_lease *lease,
	struct mt6797_dvfsp_i2c6_fw_lease *fw_lease,
	struct mt6797_dvfsp_i2c6_fw_response *response);
int mt6797_dvfsp_handoff_i2c6_fw_release(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_lease *lease,
	const struct mt6797_dvfsp_i2c6_fw_lease *fw_lease,
	struct mt6797_dvfsp_i2c6_fw_response *response);
int mt6797_dvfsp_handoff_state_owner_register(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_owner_ops *ops, void *context);
int mt6797_dvfsp_handoff_state_owner_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_owner_ops *ops, void *context);
int mt6797_dvfsp_handoff_state_snapshot(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_state_snapshot *snapshot);
int mt6797_dvfsp_handoff_state_validate(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_snapshot *snapshot);
int mt6797_dvfsp_handoff_state_hold(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_snapshot *snapshot,
	struct mt6797_dvfsp_state_hold *hold);
int mt6797_dvfsp_handoff_state_release(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_hold *hold);
void mt6797_dvfsp_handoff_state_invalidate(
	struct mt6797_dvfsp_handoff *handoff,
	enum mt6797_dvfsp_state_invalidation_reason reason);
int mt6797_dvfsp_handoff_state_owner_identity(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_state_owner_identity *identity);
int mt6797_dvfsp_protected_state_owner_register(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_protected_state_owner *owner);
int mt6797_dvfsp_protected_state_owner_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_protected_state_owner *owner);
int mt6797_dvfsp_handoff_pcm_adapter_register(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_pcm_adapter_ops *ops, void *context);
int mt6797_dvfsp_handoff_pcm_adapter_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_pcm_adapter_ops *ops, void *context);
int mt6797_dvfsp_handoff_pcm_adapter_start(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_pcm_image_identity *image);
int mt6797_dvfsp_handoff_pcm_adapter_stop(
	struct mt6797_dvfsp_handoff *handoff);
int mt6797_dvfsp_handoff_pcm_adapter_invalidate(
	struct mt6797_dvfsp_handoff *handoff,
	enum mt6797_dvfsp_pcm_adapter_fault fault);
enum mt6797_dvfsp_pcm_adapter_phase
mt6797_dvfsp_handoff_pcm_adapter_phase(
	struct mt6797_dvfsp_handoff *handoff);
/* The readiness test is atomic; clock validation is sleepable. */
bool
mt6797_dvfsp_handoff_is_ready_atomic(struct mt6797_dvfsp_handoff *handoff);
int mt6797_dvfsp_handoff_validate_clock(
	struct mt6797_dvfsp_handoff *handoff, bool gated);
int mt6797_dvfsp_handoff_validate_clock_pm(
	struct mt6797_dvfsp_handoff *handoff, bool gated);
#else
static inline struct mt6797_dvfsp_handoff *
mt6797_dvfsp_handoff_get(struct device *consumer)
{
	return ERR_PTR(-ENODEV);
}

static inline int
mt6797_dvfsp_handoff_require_ready(struct mt6797_dvfsp_handoff *handoff)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_cspm_transaction(struct mt6797_dvfsp_handoff *handoff,
			      mt6797_dvfsp_cspm_transaction_fn operation,
			      void *context)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_begin_i2c6_transfer(struct mt6797_dvfsp_handoff *handoff,
					 struct mt6797_dvfsp_i2c6_lease *lease)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_end_i2c6_transfer(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_lease *lease,
	int transfer_result)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_i2c6_fw_register(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_fw_ops *ops, void *context)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_i2c6_fw_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_fw_ops *ops, void *context)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_i2c6_fw_acquire(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_lease *lease,
	struct mt6797_dvfsp_i2c6_fw_lease *fw_lease,
	struct mt6797_dvfsp_i2c6_fw_response *response)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_i2c6_fw_release(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_i2c6_lease *lease,
	const struct mt6797_dvfsp_i2c6_fw_lease *fw_lease,
	struct mt6797_dvfsp_i2c6_fw_response *response)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_state_owner_register(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_owner_ops *ops, void *context)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_state_owner_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_owner_ops *ops, void *context)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_state_snapshot(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_state_snapshot *snapshot)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_state_validate(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_snapshot *snapshot)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_state_hold(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_snapshot *snapshot,
	struct mt6797_dvfsp_state_hold *hold)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_state_release(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_state_hold *hold)
{
	return -ENODEV;
}

static inline void
mt6797_dvfsp_handoff_state_invalidate(
	struct mt6797_dvfsp_handoff *handoff,
	enum mt6797_dvfsp_state_invalidation_reason reason)
{
}
static inline int
mt6797_dvfsp_handoff_state_owner_identity(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_state_owner_identity *identity)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_protected_state_owner_register(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_protected_state_owner *owner)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_protected_state_owner_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_protected_state_owner *owner)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_pcm_adapter_register(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_pcm_adapter_ops *ops, void *context)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_pcm_adapter_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_pcm_adapter_ops *ops, void *context)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_pcm_adapter_start(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_pcm_image_identity *image)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_pcm_adapter_stop(
	struct mt6797_dvfsp_handoff *handoff)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_pcm_adapter_invalidate(
	struct mt6797_dvfsp_handoff *handoff,
	enum mt6797_dvfsp_pcm_adapter_fault fault)
{
	return -ENODEV;
}

static inline enum mt6797_dvfsp_pcm_adapter_phase
mt6797_dvfsp_handoff_pcm_adapter_phase(
	struct mt6797_dvfsp_handoff *handoff)
{
	return MT6797_DVFSP_PCM_PHASE_UNAVAILABLE;
}

static inline bool
mt6797_dvfsp_handoff_is_ready_atomic(struct mt6797_dvfsp_handoff *handoff)
{
	return false;
}

static inline int
mt6797_dvfsp_handoff_validate_clock(
	struct mt6797_dvfsp_handoff *handoff, bool gated)
{
	return -ENODEV;
}

static inline int
mt6797_dvfsp_handoff_validate_clock_pm(
	struct mt6797_dvfsp_handoff *handoff, bool gated)
{
	return -ENODEV;
}
#endif

#endif
