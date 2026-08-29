// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fail-closed arm64 profile for CPUs expected after capability finalization.
 *
 * A profile callback may collect fallible evidence, but only the architecture
 * core can normalize and publish an immutable plan. A separate receipt tracks
 * commit and verification without modifying that plan. This lifecycle does
 * not admit a CPU_ON request.
 */

#define pr_fmt(fmt) "arm64-late-cpu-profile: " fmt

#include <crypto/sha2.h>
#include <linux/buildid.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/panic.h>
#include <linux/printk.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <asm/cpufeature.h>
#include <asm/late_cpu_profile.h>

static struct arm64_late_cpu_profile late_profile __initdata;
static bool late_profile_active __initdata;
static cpumask_t late_profile_targets __initdata;
static bool late_profile_registration_fault __initdata;

/*
 * Runtime observations have a separate owner from profile expectations.
 * A future producer may populate this private record before the seal point;
 * profiles never receive a writable reference to it.
 */
enum late_runtime_evidence_state {
	LATE_RUNTIME_EVIDENCE_OPEN,
	LATE_RUNTIME_EVIDENCE_SEALED_EMPTY,
	LATE_RUNTIME_EVIDENCE_SEALED_IDENTITY,
	LATE_RUNTIME_EVIDENCE_FAULT,
};

enum late_runtime_identity_state {
	LATE_RUNTIME_IDENTITY_UNCOLLECTED,
	LATE_RUNTIME_IDENTITY_VERIFIED,
	LATE_RUNTIME_IDENTITY_FAILED,
};

struct late_runtime_identity {
	struct arm64_late_cpu_runtime_binding binding;
	char profile_id[ARM64_LATE_CPU_PROFILE_ID_LEN];
	u32 target_cpu[ARM64_LATE_CPU_MAX_TARGETS];
	u64 target_mpidr[ARM64_LATE_CPU_MAX_TARGETS];
	u64 config_input_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 record_identity[ARM64_LATE_CPU_ID_WORDS];
};

static struct arm64_late_cpu_evidence late_runtime_evidence __initdata = {
	.abi = ARM64_LATE_CPU_PLAN_ABI,
};

static u32 late_runtime_evidence_state __initdata;
static struct late_runtime_identity late_runtime_identity __initdata;
static u32 late_runtime_identity_state __initdata;

static struct arm64_late_cpu_plan late_plan __ro_after_init;
static struct arm64_late_cpu_receipt late_receipt __ro_after_init = {
	.abi = ARM64_LATE_CPU_PLAN_ABI,
	.state = ARM64_LATE_CPU_PROFILE_NONE,
};

static struct arm64_late_cpu_ready_token late_ready_token __ro_after_init;

static void __init late_profile_block(u64 blockers, const char *reason)
{
	if (!late_receipt.profile_id[0])
		strscpy(late_receipt.profile_id, "invalid-registration",
			sizeof(late_receipt.profile_id));
	late_receipt.blocker_mask |= blockers;
	/* Pairs with state loads before any blocker receipt is consumed. */
	smp_store_release(&late_receipt.state,
			  ARM64_LATE_CPU_PROFILE_BLOCKED);
	pr_warn("%.*s blocked: %s (proof mask %#llx)\n",
		ARM64_LATE_CPU_PROFILE_ID_LEN, late_receipt.profile_id, reason,
		late_receipt.blocker_mask);
}

static bool __init late_profile_plan_has_identity(const struct arm64_late_cpu_plan *plan)
{
	unsigned int i;

	for (i = 0; i < ARM64_LATE_CPU_ID_WORDS; i++)
		if (plan->identity[i])
			return true;

	return false;
}

static bool __init
late_profile_identity_empty(const u64 identity[ARM64_LATE_CPU_ID_WORDS])
{
	unsigned int i;

	for (i = 0; i < ARM64_LATE_CPU_ID_WORDS; i++)
		if (identity[i])
			return false;

	return true;
}

static bool __init
late_profile_runtime_binding_complete(const struct arm64_late_cpu_runtime_binding *binding)
{
	if (binding->origin != ARM64_LATE_CPU_BINDING_RUNTIME ||
	    binding->valid != ARM64_LATE_CPU_BIND_VALID_MASK ||
	    late_profile_identity_empty(binding->expected_config_identity) ||
	    late_profile_identity_empty(binding->running_config_identity) ||
	    late_profile_identity_empty(binding->expected_build_id_identity) ||
	    late_profile_identity_empty(binding->running_build_id_identity) ||
	    late_profile_identity_empty(binding->expected_cmdline_identity) ||
	    late_profile_identity_empty(binding->running_cmdline_identity))
		return false;

	return !memcmp(binding->expected_config_identity,
		       binding->running_config_identity,
		       sizeof(binding->expected_config_identity)) &&
	       !memcmp(binding->expected_build_id_identity,
		       binding->running_build_id_identity,
		       sizeof(binding->expected_build_id_identity)) &&
	       !memcmp(binding->expected_cmdline_identity,
		       binding->running_cmdline_identity,
		       sizeof(binding->expected_cmdline_identity));
}

#ifdef CONFIG_ARM64_LATE_CPU_RUNTIME_IDENTITY
#define LATE_RUNTIME_IKCONFIG_MAX	SZ_4M
#define LATE_RUNTIME_NOTES_MAX		SZ_64K

static const char late_runtime_node_name[] __initconst =
	"gemini-late-cpu-provenance";
static const char late_runtime_compatible[] __initconst =
	"planet,gemini-a72-runtime-binding-v1";
static const char late_runtime_profile_id[] __initconst =
	"mt6797-a53-a72-a41-v7";
static const u8 late_runtime_domain[] __initconst =
	"gemini-a41-runtime-binding-v1";
static const u8 late_runtime_record_tag[] __initconst = "record";
static const u8 late_runtime_ikconfig_tag[] __initconst = "ikconfig";
static const u8 late_runtime_build_id_tag[] __initconst = "gnu-build-id";
static const u8 late_runtime_cmdline_tag[] __initconst = "cmdline";

enum late_runtime_record_digest {
	LATE_RECORD_EXPECTED_IKCONFIG,
	LATE_RECORD_EXPECTED_BUILD_ID,
	LATE_RECORD_EXPECTED_CMDLINE,
	LATE_RECORD_UPSTREAM_SOURCE,
	LATE_RECORD_PATCH_SERIES,
	LATE_RECORD_CONFIG_INPUTS,
	LATE_RECORD_RESOLVED_CONFIG,
	LATE_RECORD_PACKAGE_IMAGE,
	LATE_RECORD_BUILD_PROVENANCE,
	LATE_RECORD_DIGEST_COUNT,
};

enum late_runtime_record_property {
	LATE_PROP_NAME,
	LATE_PROP_COMPATIBLE,
	LATE_PROP_SCHEMA_VERSION,
	LATE_PROP_PROFILE_ID,
	LATE_PROP_TARGET_CPUS,
	LATE_PROP_TARGET_MPIDRS,
	LATE_PROP_EXPECTED_IKCONFIG,
	LATE_PROP_EXPECTED_BUILD_ID,
	LATE_PROP_EXPECTED_CMDLINE,
	LATE_PROP_UPSTREAM_SOURCE,
	LATE_PROP_PATCH_SERIES,
	LATE_PROP_CONFIG_INPUTS,
	LATE_PROP_RESOLVED_CONFIG,
	LATE_PROP_PACKAGE_IMAGE,
	LATE_PROP_BUILD_PROVENANCE,
	LATE_PROP_RECORD_IDENTITY,
	LATE_PROP_COUNT,
};

static const char * const late_runtime_property_name[LATE_PROP_COUNT] __initconst = {
	[LATE_PROP_NAME] = "name",
	[LATE_PROP_COMPATIBLE] = "compatible",
	[LATE_PROP_SCHEMA_VERSION] = "schema-version",
	[LATE_PROP_PROFILE_ID] = "profile-id",
	[LATE_PROP_TARGET_CPUS] = "target-cpus",
	[LATE_PROP_TARGET_MPIDRS] = "target-mpidrs",
	[LATE_PROP_EXPECTED_IKCONFIG] = "expected-ikconfig-identity",
	[LATE_PROP_EXPECTED_BUILD_ID] = "expected-gnu-build-id-identity",
	[LATE_PROP_EXPECTED_CMDLINE] = "expected-cmdline-identity",
	[LATE_PROP_UPSTREAM_SOURCE] = "upstream-source-sha256",
	[LATE_PROP_PATCH_SERIES] = "patch-series-sha256",
	[LATE_PROP_CONFIG_INPUTS] = "config-inputs-sha256",
	[LATE_PROP_RESOLVED_CONFIG] = "resolved-config-sha256",
	[LATE_PROP_PACKAGE_IMAGE] = "package-image-sha256",
	[LATE_PROP_BUILD_PROVENANCE] = "build-provenance-sha256",
	[LATE_PROP_RECORD_IDENTITY] = "record-identity",
};

struct late_runtime_expected_record {
	char profile_id[ARM64_LATE_CPU_PROFILE_ID_LEN];
	u32 target_cpu[ARM64_LATE_CPU_MAX_TARGETS];
	u64 target_mpidr[ARM64_LATE_CPU_MAX_TARGETS];
	u8 digest[LATE_RECORD_DIGEST_COUNT][SHA256_DIGEST_SIZE];
	u8 record_identity[SHA256_DIGEST_SIZE];
};

extern const char kernel_config_data;
extern const char kernel_config_data_end;
extern const char __start_notes;
extern const char __stop_notes;

static void __init
late_runtime_digest_to_identity(const u8 digest[SHA256_DIGEST_SIZE],
				u64 identity[ARM64_LATE_CPU_ID_WORDS])
{
	unsigned int i;

	for (i = 0; i < ARM64_LATE_CPU_ID_WORDS; i++)
		identity[i] = get_unaligned_be64(digest + i * sizeof(u64));
}

static void __init
late_runtime_sha256_update_u16(struct sha256_ctx *ctx, u16 value)
{
	__be16 encoded = cpu_to_be16(value);

	sha256_update(ctx, (const u8 *)&encoded, sizeof(encoded));
}

static void __init
late_runtime_sha256_update_u32(struct sha256_ctx *ctx, u32 value)
{
	__be32 encoded = cpu_to_be32(value);

	sha256_update(ctx, (const u8 *)&encoded, sizeof(encoded));
}

static void __init
late_runtime_sha256_update_u64(struct sha256_ctx *ctx, u64 value)
{
	__be64 encoded = cpu_to_be64(value);

	sha256_update(ctx, (const u8 *)&encoded, sizeof(encoded));
}

static void __init
late_runtime_hash_record(const struct late_runtime_expected_record *record,
			 u8 digest[SHA256_DIGEST_SIZE])
{
	struct sha256_ctx ctx;
	unsigned int i;

	sha256_init(&ctx);
	sha256_update(&ctx, late_runtime_domain, sizeof(late_runtime_domain));
	sha256_update(&ctx, late_runtime_record_tag,
		      sizeof(late_runtime_record_tag));
	late_runtime_sha256_update_u32(&ctx, 1);
	late_runtime_sha256_update_u16(&ctx,
				       sizeof(late_runtime_profile_id) - 1);
	sha256_update(&ctx, (const u8 *)record->profile_id,
		      sizeof(late_runtime_profile_id) - 1);
	late_runtime_sha256_update_u32(&ctx, ARM64_LATE_CPU_MAX_TARGETS);
	for (i = 0; i < ARM64_LATE_CPU_MAX_TARGETS; i++)
		late_runtime_sha256_update_u32(&ctx, record->target_cpu[i]);
	late_runtime_sha256_update_u32(&ctx, ARM64_LATE_CPU_MAX_TARGETS);
	for (i = 0; i < ARM64_LATE_CPU_MAX_TARGETS; i++)
		late_runtime_sha256_update_u64(&ctx, record->target_mpidr[i]);
	for (i = 0; i < LATE_RECORD_DIGEST_COUNT; i++)
		sha256_update(&ctx, record->digest[i], SHA256_DIGEST_SIZE);
	sha256_final(&ctx, digest);
}

static int __init
late_runtime_parse_property(struct late_runtime_expected_record *record,
			    enum late_runtime_record_property index,
			    const struct property *property)
{
	const u8 *value = property->value;
	unsigned int digest;

	if (!value || property->length < 0)
		return -EINVAL;

	if (index >= LATE_PROP_EXPECTED_IKCONFIG &&
	    index <= LATE_PROP_BUILD_PROVENANCE) {
		digest = index - LATE_PROP_EXPECTED_IKCONFIG;
		if (property->length != SHA256_DIGEST_SIZE ||
		    !memchr_inv(value, 0, SHA256_DIGEST_SIZE))
			return -EINVAL;
		memcpy(record->digest[digest], value,
		       SHA256_DIGEST_SIZE);
		return 0;
	}

	switch (index) {
	case LATE_PROP_NAME:
		return property->length == sizeof(late_runtime_node_name) &&
		       !memcmp(value, late_runtime_node_name,
			       sizeof(late_runtime_node_name)) ? 0 : -EINVAL;
	case LATE_PROP_COMPATIBLE:
		return property->length == sizeof(late_runtime_compatible) &&
		       !memcmp(value, late_runtime_compatible,
			       sizeof(late_runtime_compatible)) ? 0 : -EINVAL;
	case LATE_PROP_SCHEMA_VERSION:
		return property->length == sizeof(__be32) &&
		       get_unaligned_be32(value) == 1 ? 0 : -EINVAL;
	case LATE_PROP_PROFILE_ID:
		if (property->length != sizeof(late_runtime_profile_id) ||
		    memcmp(value, late_runtime_profile_id,
			   sizeof(late_runtime_profile_id)))
			return -EINVAL;
		memcpy(record->profile_id, value,
		       sizeof(late_runtime_profile_id));
		return 0;
	case LATE_PROP_TARGET_CPUS:
		if (property->length != 2 * sizeof(__be32))
			return -EINVAL;
		record->target_cpu[0] = get_unaligned_be32(value);
		record->target_cpu[1] =
			get_unaligned_be32(value + sizeof(__be32));
		return record->target_cpu[0] == 8 &&
		       record->target_cpu[1] == 9 ? 0 : -EINVAL;
	case LATE_PROP_TARGET_MPIDRS:
		if (property->length != 2 * sizeof(__be64))
			return -EINVAL;
		record->target_mpidr[0] = get_unaligned_be64(value);
		record->target_mpidr[1] =
			get_unaligned_be64(value + sizeof(__be64));
		return record->target_mpidr[0] == 0x200 &&
		       record->target_mpidr[1] == 0x201 ? 0 : -EINVAL;
	case LATE_PROP_RECORD_IDENTITY:
		if (property->length != SHA256_DIGEST_SIZE ||
		    !memchr_inv(value, 0, SHA256_DIGEST_SIZE))
			return -EINVAL;
		memcpy(record->record_identity, value,
		       SHA256_DIGEST_SIZE);
		return 0;
	default:
		return -EINVAL;
	}
}

static int __init
late_runtime_parse_expected_record(struct late_runtime_identity *identity)
{
	struct late_runtime_expected_record record = {};
	struct device_node *root = NULL;
	struct device_node *chosen = NULL;
	struct device_node *node = NULL;
	const struct property *property;
	u8 record_digest[SHA256_DIGEST_SIZE];
	unsigned long seen = 0;
	unsigned int matches;
	unsigned int index;
	int ret = -EINVAL;

	root = of_find_node_by_path("/");
	chosen = of_find_node_by_path("/chosen");
	node = of_find_node_by_path("/chosen/gemini-late-cpu-provenance");
	if (!root || !chosen || !node || chosen != of_chosen ||
	    node->parent != chosen ||
	    of_node_check_flag(chosen, OF_DYNAMIC) ||
	    of_node_check_flag(chosen, OF_DETACHED) ||
	    of_node_check_flag(chosen, OF_OVERLAY) ||
	    of_node_check_flag(node, OF_DYNAMIC) ||
	    of_node_check_flag(node, OF_DETACHED) ||
	    of_node_check_flag(node, OF_OVERLAY) || node->deadprops)
		goto out;

	matches = 0;
	for_each_child_of_node_scoped(root, child) {
		if (!of_node_name_eq(child, "chosen"))
			continue;
		if (strcmp(kbasename(child->full_name), "chosen") ||
		    child != chosen || ++matches != 1)
			goto out;
	}
	if (matches != 1)
		goto out;

	matches = 0;
	for_each_child_of_node_scoped(chosen, child) {
		if (!of_node_name_eq(child, late_runtime_node_name))
			continue;
		if (strcmp(kbasename(child->full_name), late_runtime_node_name) ||
		    child != node || ++matches != 1)
			goto out;
	}
	if (matches != 1)
		goto out;

	for_each_child_of_node_scoped(node, child)
		goto out;

	matches = 0;
	for_each_compatible_node_scoped(match, NULL, late_runtime_compatible) {
		if (match != node || ++matches != 1)
			goto out;
	}
	if (matches != 1)
		goto out;

	for_each_property_of_node(node, property) {
#ifdef CONFIG_OF_DYNAMIC
		if (of_property_check_flag(property, OF_DYNAMIC))
			goto out;
#endif
		if (!property->name ||
		    !strncmp(property->name, "running-", sizeof("running-") - 1))
			goto out;
		for (index = 0; index < LATE_PROP_COUNT; index++)
			if (!strcmp(property->name,
				    late_runtime_property_name[index]))
				break;
		if (index == LATE_PROP_COUNT || (seen & BIT(index)) ||
		    late_runtime_parse_property(&record, index, property))
			goto out;
		seen |= BIT(index);
	}
	if (seen != GENMASK(LATE_PROP_COUNT - 1, 0))
		goto out;

	late_runtime_hash_record(&record, record_digest);
	if (memcmp(record_digest, record.record_identity,
		   sizeof(record_digest)))
		goto out;

	strscpy(identity->profile_id, record.profile_id,
		sizeof(identity->profile_id));
	memcpy(identity->target_cpu, record.target_cpu,
	       sizeof(identity->target_cpu));
	memcpy(identity->target_mpidr, record.target_mpidr,
	       sizeof(identity->target_mpidr));
	late_runtime_digest_to_identity(record.digest[LATE_RECORD_CONFIG_INPUTS],
					identity->config_input_identity);
	late_runtime_digest_to_identity(record.record_identity,
					identity->record_identity);
	late_runtime_digest_to_identity(record.digest[LATE_RECORD_EXPECTED_IKCONFIG],
					identity->binding.expected_config_identity);
	late_runtime_digest_to_identity(record.digest[LATE_RECORD_EXPECTED_BUILD_ID],
					identity->binding.expected_build_id_identity);
	late_runtime_digest_to_identity(record.digest[LATE_RECORD_EXPECTED_CMDLINE],
					identity->binding.expected_cmdline_identity);
	ret = 0;
out:
	of_node_put(node);
	of_node_put(chosen);
	of_node_put(root);
	return ret;
}

static void __init
late_runtime_hash_blob64(const u8 *tag, size_t tag_size, const u8 *data,
			 size_t data_size, u8 digest[SHA256_DIGEST_SIZE])
{
	struct sha256_ctx ctx;

	sha256_init(&ctx);
	sha256_update(&ctx, late_runtime_domain, sizeof(late_runtime_domain));
	sha256_update(&ctx, tag, tag_size);
	late_runtime_sha256_update_u64(&ctx, data_size);
	sha256_update(&ctx, data, data_size);
	sha256_final(&ctx, digest);
}

static void __init
late_runtime_hash_blob32(const u8 *tag, size_t tag_size, const u8 *data,
			 u32 data_size, u8 digest[SHA256_DIGEST_SIZE])
{
	struct sha256_ctx ctx;

	sha256_init(&ctx);
	sha256_update(&ctx, late_runtime_domain, sizeof(late_runtime_domain));
	sha256_update(&ctx, tag, tag_size);
	late_runtime_sha256_update_u32(&ctx, data_size);
	sha256_update(&ctx, data, data_size);
	sha256_final(&ctx, digest);
}

static int __init
late_runtime_produce_running_identity(struct late_runtime_identity *identity)
{
	const u8 *config = (const u8 *)&kernel_config_data;
	const u8 *config_end = (const u8 *)&kernel_config_data_end;
	const u8 *notes = (const u8 *)&__start_notes;
	const u8 *notes_end = (const u8 *)&__stop_notes;
	u8 build_id[BUILD_ID_SIZE_MAX];
	u8 digest[SHA256_DIGEST_SIZE];
	size_t config_size;
	size_t notes_size;

	if (config_end <= config)
		return -EINVAL;
	config_size = config_end - config;
	if (!config_size || config_size > LATE_RUNTIME_IKCONFIG_MAX)
		return -EINVAL;
	late_runtime_hash_blob64(late_runtime_ikconfig_tag,
				 sizeof(late_runtime_ikconfig_tag), config,
				 config_size, digest);
	late_runtime_digest_to_identity(digest,
					identity->binding.running_config_identity);

	if (notes_end <= notes)
		return -EINVAL;
	notes_size = notes_end - notes;
	if (!notes_size || notes_size > LATE_RUNTIME_NOTES_MAX ||
	    build_id_parse_buf_exact(notes, build_id, notes_size,
				     BUILD_ID_SIZE_MAX))
		return -EINVAL;
	late_runtime_hash_blob32(late_runtime_build_id_tag,
				 sizeof(late_runtime_build_id_tag), build_id,
				 BUILD_ID_SIZE_MAX, digest);
	late_runtime_digest_to_identity(digest,
					identity->binding.running_build_id_identity);
	memzero_explicit(build_id, sizeof(build_id));

	if (!saved_command_line || !saved_command_line_len ||
	    saved_command_line_len != sizeof(CONFIG_CMDLINE) - 1 ||
	    memcmp(saved_command_line, CONFIG_CMDLINE, sizeof(CONFIG_CMDLINE)))
		return -EINVAL;
	late_runtime_hash_blob64(late_runtime_cmdline_tag,
				 sizeof(late_runtime_cmdline_tag),
				 (const u8 *)saved_command_line,
				 saved_command_line_len, digest);
	late_runtime_digest_to_identity(digest,
					identity->binding.running_cmdline_identity);

	identity->binding.valid = ARM64_LATE_CPU_BIND_VALID_MASK;
	identity->binding.origin = ARM64_LATE_CPU_BINDING_RUNTIME;
	return late_profile_runtime_binding_complete(&identity->binding) ?
		0 : -EINVAL;
}

static bool __init
late_runtime_identity_complete(const struct late_runtime_identity *identity)
{
	return !strcmp(identity->profile_id, late_runtime_profile_id) &&
	       identity->target_cpu[0] == 8 && identity->target_cpu[1] == 9 &&
	       identity->target_mpidr[0] == 0x200 &&
	       identity->target_mpidr[1] == 0x201 &&
	       !late_profile_identity_empty(identity->config_input_identity) &&
	       !late_profile_identity_empty(identity->record_identity) &&
	       late_profile_runtime_binding_complete(&identity->binding);
}
#else
static bool __init
late_runtime_identity_complete(const struct late_runtime_identity *identity)
{
	(void)identity;
	return false;
}
#endif

void __init arm64_collect_late_cpu_runtime_identity(void)
{
	struct late_runtime_identity staged = {};

	if (READ_ONCE(late_runtime_evidence_state) !=
		    LATE_RUNTIME_EVIDENCE_OPEN ||
	    READ_ONCE(late_runtime_identity_state) !=
		    LATE_RUNTIME_IDENTITY_UNCOLLECTED ||
	    system_capabilities_finalized() ||
	    cpus_have_cap(ARM64_ALWAYS_SYSTEM)) {
		WRITE_ONCE(late_runtime_identity_state,
			   LATE_RUNTIME_IDENTITY_FAILED);
		WRITE_ONCE(late_runtime_evidence_state,
			   LATE_RUNTIME_EVIDENCE_FAULT);
		return;
	}

	/* A failure leaves every private identity field at its zero value. */
	WRITE_ONCE(late_runtime_identity_state, LATE_RUNTIME_IDENTITY_FAILED);
#ifdef CONFIG_ARM64_LATE_CPU_RUNTIME_IDENTITY
	if (late_runtime_parse_expected_record(&staged)) {
		pr_warn("static runtime identity record is unavailable or invalid\n");
		return;
	}
	if (late_runtime_produce_running_identity(&staged)) {
		pr_warn("running config, build-ID, or command-line identity did not match\n");
		return;
	}

	late_runtime_identity = staged;
	WRITE_ONCE(late_runtime_identity_state, LATE_RUNTIME_IDENTITY_VERIFIED);
	pr_info("verified the pre-finalization runtime identity binding\n");
#else
	pr_warn("runtime identity producer is not configured\n");
#endif
}

static bool __init
late_profile_binding_empty(const struct arm64_late_cpu_runtime_binding *binding)
{
	return !binding->valid &&
	       binding->origin == ARM64_LATE_CPU_BINDING_NONE &&
	       late_profile_identity_empty(binding->expected_config_identity) &&
	       late_profile_identity_empty(binding->running_config_identity) &&
	       late_profile_identity_empty(binding->expected_build_id_identity) &&
	       late_profile_identity_empty(binding->running_build_id_identity) &&
	       late_profile_identity_empty(binding->expected_cmdline_identity) &&
	       late_profile_identity_empty(binding->running_cmdline_identity);
}

/* Profile-owned production input may describe expectations, never observations. */
static bool __init
late_profile_runtime_fields_empty(const struct arm64_late_cpu_evidence *evidence)
{
	unsigned int target;

	if (!late_profile_binding_empty(&evidence->binding) ||
	    !late_profile_identity_empty(evidence->evidence_identity))
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (evidence->observed_target_mpidr[target] ||
		    evidence->observed_target_midr[target] ||
		    evidence->observed_target_revidr[target] ||
		    memchr_inv(&evidence->target_cap[target], 0,
			       sizeof(evidence->target_cap[target])) ||
		    memchr_inv(&evidence->target_policy[target], 0,
			       sizeof(evidence->target_policy[target])))
			return false;

	return !memchr_inv(&evidence->system_cap, 0,
			   sizeof(evidence->system_cap));
}

static bool __init
late_runtime_evidence_storage_empty(void)
{
	unsigned int target;

	if (late_runtime_evidence.abi != ARM64_LATE_CPU_PLAN_ABI ||
	    !late_profile_identity_empty(late_runtime_evidence.source_parent_identity) ||
	    !late_profile_identity_empty(late_runtime_evidence.config_input_identity) ||
	    !late_profile_binding_empty(&late_runtime_evidence.binding) ||
	    !late_profile_identity_empty(late_runtime_evidence.evidence_identity) ||
	    late_runtime_evidence.blocker_mask ||
	    memchr_inv(&late_runtime_evidence.system_cap, 0,
		       sizeof(late_runtime_evidence.system_cap)))
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		if (late_runtime_evidence.target_cpu[target] ||
		    late_runtime_evidence.expected_target_mpidr[target] ||
		    late_runtime_evidence.observed_target_mpidr[target] ||
		    late_runtime_evidence.expected_target_midr[target] ||
		    late_runtime_evidence.observed_target_midr[target] ||
		    late_runtime_evidence.observed_target_revidr[target] ||
		    memchr_inv(&late_runtime_evidence.target_cap[target], 0,
			       sizeof(late_runtime_evidence.target_cap[target])) ||
		    memchr_inv(&late_runtime_evidence.target_policy[target], 0,
			       sizeof(late_runtime_evidence.target_policy[target])))
			return false;
	}

	return true;
}

static bool __init
late_profile_identity_cross_bound(const struct arm64_late_cpu_evidence *evidence,
				  const char *profile_id,
				  const struct cpumask *registered_targets)
{
	unsigned int target;

	if (strcmp(late_runtime_identity.profile_id, profile_id) ||
	    memcmp(late_runtime_identity.config_input_identity,
		   evidence->config_input_identity,
		   sizeof(evidence->config_input_identity)) ||
	    late_profile_identity_empty(late_runtime_identity.record_identity) ||
	    !late_profile_runtime_binding_complete(&late_runtime_evidence.binding))
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (late_runtime_identity.target_cpu[target] !=
			    evidence->target_cpu[target] ||
		    late_runtime_identity.target_mpidr[target] !=
			    evidence->expected_target_mpidr[target] ||
		    !cpumask_test_cpu(late_runtime_identity.target_cpu[target],
				      registered_targets))
			return false;

	return true;
}

void __init arm64_seal_late_cpu_runtime_evidence(void)
{
	u32 state = READ_ONCE(late_runtime_evidence_state);
	u32 identity_state = READ_ONCE(late_runtime_identity_state);

	if (state != LATE_RUNTIME_EVIDENCE_OPEN ||
	    system_capabilities_finalized() ||
	    cpus_have_cap(ARM64_ALWAYS_SYSTEM)) {
		/* Publish the fault before profile preparation examines state. */
		smp_store_release(&late_runtime_evidence_state,
				  LATE_RUNTIME_EVIDENCE_FAULT);
		return;
	}

	if (!late_runtime_evidence_storage_empty() ||
	    identity_state == LATE_RUNTIME_IDENTITY_UNCOLLECTED) {
		/* Publish the fault before profile preparation examines state. */
		smp_store_release(&late_runtime_evidence_state,
				  LATE_RUNTIME_EVIDENCE_FAULT);
		return;
	}

	if (identity_state == LATE_RUNTIME_IDENTITY_FAILED &&
	    !memchr_inv(&late_runtime_identity, 0,
		       sizeof(late_runtime_identity))) {
		late_runtime_evidence.blocker_mask |=
			ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING;
		state = LATE_RUNTIME_EVIDENCE_SEALED_EMPTY;
	} else if (identity_state == LATE_RUNTIME_IDENTITY_VERIFIED &&
		   late_runtime_identity_complete(&late_runtime_identity)) {
		late_runtime_evidence.binding = late_runtime_identity.binding;
		state = LATE_RUNTIME_EVIDENCE_SEALED_IDENTITY;
	} else {
		/* Publish the fault before profile preparation examines state. */
		smp_store_release(&late_runtime_evidence_state,
				  LATE_RUNTIME_EVIDENCE_FAULT);
		return;
	}

	/* Publish every core-owned field before profile planning may consume it. */
	smp_store_release(&late_runtime_evidence_state, state);
}

static bool __init
late_profile_target_effects_match(
	const struct arm64_late_cpu_target_effect_plan *left,
	const struct arm64_late_cpu_target_effect_plan *right)
{
	return left->valid == right->valid &&
	       left->spectre_v2_state == right->spectre_v2_state &&
	       left->spectre_v2_conduit == right->spectre_v2_conduit &&
	       left->spectre_v2_callback == right->spectre_v2_callback &&
	       left->spectre_v2_hyp_vector == right->spectre_v2_hyp_vector &&
	       left->spectre_v4_state == right->spectre_v4_state &&
	       left->spectre_v4_method == right->spectre_v4_method &&
	       left->spectre_v4_conduit == right->spectre_v4_conduit &&
	       left->spectre_v4_policy == right->spectre_v4_policy &&
	       left->spectre_v4_callback_required ==
		       right->spectre_v4_callback_required &&
	       left->bhb_method == right->bhb_method &&
	       left->bhb_loop_count == right->bhb_loop_count &&
	       left->bhb_matcher_loop_count == right->bhb_matcher_loop_count &&
	       left->bhb_conduit == right->bhb_conduit &&
	       left->bhb_mitigation_state == right->bhb_mitigation_state &&
	       left->bhb_vector_template == right->bhb_vector_template &&
	       left->bhb_hyp_vector == right->bhb_hyp_vector &&
	       left->bhb_v2_non_vulnerable == right->bhb_v2_non_vulnerable;
}

static bool __init
late_profile_effects_match(const struct arm64_late_cpu_effect_plan *left,
			   const struct arm64_late_cpu_effect_plan *right)
{
	unsigned int target;

	if (left->ctr_mismatch.required != right->ctr_mismatch.required ||
	    left->ctr_mismatch.target_mask != right->ctr_mismatch.target_mask ||
	    left->ctr_mismatch.trap_ctr_el0 !=
		    right->ctr_mismatch.trap_ctr_el0 ||
	    left->ctr_mismatch.alternative != right->ctr_mismatch.alternative ||
	    left->spectre_v2.required != right->spectre_v2.required ||
	    left->spectre_v2.target_mask != right->spectre_v2.target_mask ||
	    left->spectre_v2.mitigation_state !=
		    right->spectre_v2.mitigation_state ||
	    left->spectre_v2.conduit != right->spectre_v2.conduit ||
	    left->spectre_v2.callback != right->spectre_v2.callback ||
	    left->spectre_v2.hyp_vector != right->spectre_v2.hyp_vector ||
	    left->spectre_v2.alternative != right->spectre_v2.alternative ||
	    left->spectre_v4.required != right->spectre_v4.required ||
	    left->spectre_v4.target_mask != right->spectre_v4.target_mask ||
	    left->spectre_v4.mitigation_state !=
		    right->spectre_v4.mitigation_state ||
	    left->spectre_v4.method != right->spectre_v4.method ||
	    left->spectre_v4.conduit != right->spectre_v4.conduit ||
	    left->spectre_v4.policy != right->spectre_v4.policy ||
	    left->spectre_v4.callback_required_mask !=
		    right->spectre_v4.callback_required_mask ||
	    left->spectre_v4.firmware_alternative !=
		    right->spectre_v4.firmware_alternative ||
	    left->bhb.required != right->bhb.required ||
	    left->bhb.target_mask != right->bhb.target_mask ||
	    left->bhb.method != right->bhb.method ||
	    left->bhb.loop_count != right->bhb.loop_count ||
	    left->bhb.matcher_loop_count != right->bhb.matcher_loop_count ||
	    left->bhb.conduit != right->bhb.conduit ||
	    left->bhb.system_method != right->bhb.system_method ||
	    left->bhb.mitigation_state != right->bhb.mitigation_state ||
	    left->bhb.vector_template != right->bhb.vector_template ||
	    left->bhb.hyp_vector != right->bhb.hyp_vector ||
	    left->bhb.alternative != right->bhb.alternative ||
	    left->bhb.v2_non_vulnerable != right->bhb.v2_non_vulnerable ||
	    left->compat_aes_clear != right->compat_aes_clear ||
	    left->speculative_at_finalization !=
		    right->speculative_at_finalization)
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (!late_profile_target_effects_match(
			    &left->target[target], &right->target[target]))
			return false;

	return true;
}

static bool __init
late_profile_receipt_identity_matches(const struct arm64_late_cpu_plan *plan,
				      const struct arm64_late_cpu_receipt *receipt)
{
	return receipt->abi == plan->abi &&
	       !memcmp(receipt->profile_id, plan->profile_id,
		       sizeof(receipt->profile_id)) &&
	       !memcmp(receipt->plan_identity, plan->identity,
		       sizeof(receipt->plan_identity));
}

static bool __init
late_profile_descriptor_matches(const struct arm64_late_cpu_profile *profile)
{
	return profile && profile->name && profile->prepare &&
	       profile->derive_effects &&
	       strnlen(profile->name, ARM64_LATE_CPU_PROFILE_ID_LEN) <
		       ARM64_LATE_CPU_PROFILE_ID_LEN &&
	       late_profile_active &&
	       !strcmp(late_profile.name, profile->name) &&
	       late_profile.target_count == profile->target_count &&
	       late_profile.classify_local_cap == profile->classify_local_cap &&
	       late_profile.validate_plan == profile->validate_plan &&
	       late_profile.derive_effects == profile->derive_effects &&
	       late_profile.prepare == profile->prepare &&
	       late_profile.verify_system == profile->verify_system &&
	       late_profile.finalize_user == profile->finalize_user;
}

void __init
arm64_activate_late_cpu_profile(const struct arm64_late_cpu_profile *profile)
{
	if (!profile || !profile->name || !profile->prepare ||
	    !profile->classify_local_cap || !profile->validate_plan ||
	    !profile->derive_effects ||
	    !profile->target_count ||
	    profile->target_count > ARM64_LATE_CPU_MAX_TARGETS) {
		late_profile_registration_fault = true;
		return;
	}

	if (!late_profile_active) {
		if (strscpy(late_receipt.profile_id, profile->name,
			    sizeof(late_receipt.profile_id)) < 0) {
			late_profile_registration_fault = true;
			return;
		}
		late_profile = *profile;
		late_profile.name = late_receipt.profile_id;
		late_profile_active = true;
		WRITE_ONCE(late_receipt.state,
			   ARM64_LATE_CPU_PROFILE_REGISTERED);
	} else if (!late_profile_descriptor_matches(profile)) {
		late_profile_registration_fault = true;
	}
}

void __init
arm64_register_late_cpu_target(const struct arm64_late_cpu_profile *profile,
			       unsigned int cpu)
{
	if (!late_profile_descriptor_matches(profile) || cpu >= nr_cpu_ids) {
		late_profile_registration_fault = true;
		return;
	}

	if (cpumask_test_and_set_cpu(cpu, &late_profile_targets))
		late_profile_registration_fault = true;
}

void __init arm64_prepare_late_cpu_profile(void)
{
	struct arm64_late_cpu_evidence profile_evidence = {
		.abi = ARM64_LATE_CPU_PLAN_ABI,
	};
	struct arm64_late_cpu_plan draft = {
		.abi = ARM64_LATE_CPU_PLAN_ABI,
	};
	u32 runtime_state;
	int validate_ret;
	int effect_ret;
	int plan_ret;
	int ret;

	if (!late_profile_active && !late_profile_registration_fault)
		return;

	if (late_profile_registration_fault) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_REGISTRATION,
				   "registration identity changed");
		return;
	}
	/* Pairs with the release stores at the architecture seal point. */
	runtime_state = smp_load_acquire(&late_runtime_evidence_state);
	if (runtime_state != LATE_RUNTIME_EVIDENCE_SEALED_EMPTY &&
	    runtime_state != LATE_RUNTIME_EVIDENCE_SEALED_IDENTITY) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING,
				   "runtime evidence was not sealed");
		return;
	}
	if (cpumask_weight(&late_profile_targets) != late_profile.target_count) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_REGISTRATION,
				   "target registrations are incomplete");
		return;
	}

	if (READ_ONCE(late_receipt.state) !=
	    ARM64_LATE_CPU_PROFILE_REGISTERED) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_REGISTRATION,
				   "invalid prepare state");
		return;
	}

	strscpy(draft.profile_id, late_receipt.profile_id,
		sizeof(draft.profile_id));
	cpumask_copy(&draft.target_cpus, &late_profile_targets);
	ret = late_profile.prepare(&profile_evidence, &late_profile_targets);
	if (profile_evidence.abi != ARM64_LATE_CPU_PLAN_ABI) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_CAP_INVENTORY,
				   "prepare altered evidence ABI");
		return;
	}
	if (profile_evidence.binding.origin ==
	    ARM64_LATE_CPU_BINDING_RUNTIME) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING,
				   "profile declared runtime evidence");
		return;
	}
	if (profile_evidence.binding.origin != ARM64_LATE_CPU_BINDING_NONE &&
	    profile_evidence.binding.origin != ARM64_LATE_CPU_BINDING_FIXTURE) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING,
				   "profile declared an invalid evidence origin");
		return;
	}

	draft.evidence = profile_evidence;
	if (profile_evidence.binding.origin == ARM64_LATE_CPU_BINDING_NONE) {
		if (!late_profile_runtime_fields_empty(&profile_evidence)) {
			late_profile_block(ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING,
					   "profile supplied runtime observations");
			return;
		}
		if (runtime_state == LATE_RUNTIME_EVIDENCE_SEALED_IDENTITY) {
			if (!late_profile_identity_cross_bound(&profile_evidence,
							       draft.profile_id,
							       &late_profile_targets)) {
				late_profile_block(ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING,
						   "runtime identity did not match profile inputs");
				return;
			}
			draft.evidence.binding = late_runtime_evidence.binding;
			draft.evidence.blocker_mask &=
				~ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING;
		}
	}
	if (!late_profile_runtime_binding_complete(&draft.evidence.binding))
		draft.evidence.blocker_mask |=
			ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING;
	/* ABI 7 has no architecture-owned mutation implementation. */
	draft.evidence.blocker_mask |= ARM64_LATE_CPU_BLOCK_COMMIT_PATH;
	plan_ret = arm64_plan_late_cpu_capabilities(&draft, &late_profile);
	effect_ret = plan_ret ? -EAGAIN :
		arm64_plan_late_cpu_effects(&draft, &late_profile);
	validate_ret = late_profile.validate_plan(&draft);
	if (plan_ret)
		draft.evidence.blocker_mask |=
			ARM64_LATE_CPU_BLOCK_CAP_INVENTORY;
	if (effect_ret)
		draft.evidence.blocker_mask |= ARM64_LATE_CPU_BLOCK_EFFECT_PLAN;
	if (validate_ret)
		draft.evidence.blocker_mask |=
			ARM64_LATE_CPU_BLOCK_PLAN_VALIDATION;

	if (ret || plan_ret || effect_ret || validate_ret ||
	    draft.evidence.blocker_mask) {
		u64 blockers = draft.evidence.blocker_mask ?:
			       ARM64_LATE_CPU_BLOCK_CAP_INVENTORY;

		late_profile_block(blockers,
				   "required evidence is incomplete");
		return;
	}
	if (!draft.local_caps_planned || !draft.effects_planned ||
	    !late_profile_plan_has_identity(&draft)) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_SOURCE_IDENTITY,
				   "canonical plan identity is unavailable");
		return;
	}

	late_plan = draft;
	memcpy(late_receipt.plan_identity, late_plan.identity,
	       sizeof(late_receipt.plan_identity));
	/* Publish the immutable plan before exposing PLAN_FROZEN. */
	smp_store_release(&late_receipt.state,
			  ARM64_LATE_CPU_PROFILE_PLAN_FROZEN);
	pr_info("%s froze an immutable capability plan\n",
		late_receipt.profile_id);
}

void __init arm64_commit_late_cpu_profile(void)
{
	u32 state = READ_ONCE(late_receipt.state);

	if (state == ARM64_LATE_CPU_PROFILE_NONE ||
	    state == ARM64_LATE_CPU_PROFILE_BLOCKED)
		return;
	if (!late_profile_active)
		return;
	if (state != ARM64_LATE_CPU_PROFILE_PLAN_FROZEN ||
	    !late_plan.local_caps_planned || !late_plan.effects_planned ||
	    !late_profile_plan_has_identity(&late_plan) ||
	    memcmp(late_receipt.plan_identity, late_plan.identity,
		   sizeof(late_receipt.plan_identity)))
		panic("late CPU profile reached capability commit out of order");

	/*
	 * ABI 7 deliberately publishes no mutation path. The complete canonical
	 * evaluator must add one architecture-owned, callback-free commit before
	 * it can make PLAN_FROZEN reachable for a production profile.
	 */
	panic("late CPU profile commit implementation is unavailable");
}

void __init arm64_verify_late_cpu_profile_system(void)
{
	int ret;

	if (READ_ONCE(late_receipt.state) == ARM64_LATE_CPU_PROFILE_BLOCKED)
		return;
	if (!late_profile_active)
		return;

	if (READ_ONCE(late_receipt.state) !=
	    ARM64_LATE_CPU_PROFILE_COMMITTED || !late_profile.verify_system)
		panic("late CPU profile reached system finalization out of order");
	if (!late_profile_receipt_identity_matches(&late_plan, &late_receipt))
		panic("late CPU profile receipt identity changed before verification");

	ret = late_profile.verify_system(&late_plan, &late_receipt);
	if (ret || late_receipt.blocker_mask ||
	    !late_profile_receipt_identity_matches(&late_plan, &late_receipt) ||
	    !late_receipt.commit_complete ||
	    !late_profile_effects_match(&late_receipt.committed,
					&late_plan.effects) ||
	    late_receipt.strict_caps_verified ||
	    late_receipt.alternatives_finalized ||
	    late_receipt.user_hwcaps_finalized)
		panic("late CPU profile system verification failed: %d", ret);

	late_receipt.strict_caps_verified = 1;
	late_receipt.alternatives_finalized = 1;
	/* Pairs with the state load before user verification consumes them. */
	smp_store_release(&late_receipt.state,
			  ARM64_LATE_CPU_PROFILE_SYSTEM_VERIFIED);
}

void __init arm64_finalize_late_cpu_profile_user(void)
{
	int ret;

	if (READ_ONCE(late_receipt.state) == ARM64_LATE_CPU_PROFILE_BLOCKED)
		return;
	if (!late_profile_active)
		return;

	if (READ_ONCE(late_receipt.state) !=
	    ARM64_LATE_CPU_PROFILE_SYSTEM_VERIFIED ||
	    !late_profile.finalize_user)
		panic("late CPU profile reached user finalization out of order");
	if (!late_profile_receipt_identity_matches(&late_plan, &late_receipt) ||
	    !late_profile_effects_match(&late_receipt.committed,
					&late_plan.effects))
		panic("late CPU profile receipt changed before user finalization");

	ret = late_profile.finalize_user(&late_plan, &late_receipt);
	if (ret || late_receipt.blocker_mask ||
	    !late_profile_receipt_identity_matches(&late_plan, &late_receipt) ||
	    !late_profile_effects_match(&late_receipt.committed,
					&late_plan.effects) ||
	    !late_receipt.commit_complete ||
	    !late_receipt.strict_caps_verified ||
	    !late_receipt.alternatives_finalized ||
	    late_receipt.user_hwcaps_finalized)
		panic("late CPU profile user verification failed: %d", ret);

	late_ready_token.abi = ARM64_LATE_CPU_PLAN_ABI;
	strscpy(late_ready_token.profile_id, late_plan.profile_id,
		sizeof(late_ready_token.profile_id));
	cpumask_copy(&late_ready_token.target_cpus, &late_plan.target_cpus);
	memcpy(late_ready_token.plan_identity, late_plan.identity,
	       sizeof(late_ready_token.plan_identity));
	memcpy(late_ready_token.source_parent_identity,
	       late_plan.evidence.source_parent_identity,
	       sizeof(late_ready_token.source_parent_identity));
	memcpy(late_ready_token.config_input_identity,
	       late_plan.evidence.config_input_identity,
	       sizeof(late_ready_token.config_input_identity));
	late_ready_token.binding = late_plan.evidence.binding;
	memcpy(late_ready_token.evidence_identity,
	       late_plan.evidence.evidence_identity,
	       sizeof(late_ready_token.evidence_identity));
	memcpy(late_ready_token.target_cpu, late_plan.evidence.target_cpu,
	       sizeof(late_ready_token.target_cpu));
	memcpy(late_ready_token.expected_target_mpidr,
	       late_plan.evidence.expected_target_mpidr,
	       sizeof(late_ready_token.expected_target_mpidr));
	memcpy(late_ready_token.observed_target_mpidr,
	       late_plan.evidence.observed_target_mpidr,
	       sizeof(late_ready_token.observed_target_mpidr));
	memcpy(late_ready_token.expected_target_midr,
	       late_plan.evidence.expected_target_midr,
	       sizeof(late_ready_token.expected_target_midr));
	memcpy(late_ready_token.observed_target_midr,
	       late_plan.evidence.observed_target_midr,
	       sizeof(late_ready_token.observed_target_midr));
	memcpy(late_ready_token.observed_target_revidr,
	       late_plan.evidence.observed_target_revidr,
	       sizeof(late_ready_token.observed_target_revidr));
	late_ready_token.committed = late_receipt.committed;
	if (!late_profile_receipt_identity_matches(&late_plan, &late_receipt) ||
	    !late_profile_effects_match(&late_receipt.committed,
					&late_plan.effects))
		panic("late CPU profile receipt changed before READY publication");
	late_receipt.user_hwcaps_finalized = 1;
	/* Pairs with smp_load_acquire() in the READY-token accessor. */
	smp_store_release(&late_receipt.state,
			  ARM64_LATE_CPU_PROFILE_READY);
	pr_info("%s ready\n", late_receipt.profile_id);
}

const struct arm64_late_cpu_ready_token *
arm64_get_late_cpu_ready_token(void)
{
	/* A36 consumers may observe only the immutable READY token. */
	if (smp_load_acquire(&late_receipt.state) !=
	    ARM64_LATE_CPU_PROFILE_READY)
		return NULL;

	return &late_ready_token;
}
