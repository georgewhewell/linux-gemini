// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/ksysfs.c - sysfs attributes in /sys/kernel, which
 * 		     are not related to any other subsystem
 *
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
 */

#include <asm/byteorder.h>
#include <linux/kobject.h>
#include <linux/ksysfs.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/vmcore_info.h>
#include <linux/profile.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/capability.h>
#include <linux/compiler.h>

#include <linux/rcupdate.h>	/* rcu_expedited and rcu_normal */

#if defined(__LITTLE_ENDIAN)
#define CPU_BYTEORDER_STRING	"little"
#elif defined(__BIG_ENDIAN)
#define CPU_BYTEORDER_STRING	"big"
#else
#error Unknown byteorder
#endif

#define KERNEL_ATTR_RO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

#define KERNEL_ATTR_RW(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RW(_name)

/* current uevent sequence number */
static ssize_t uevent_seqnum_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)atomic64_read(&uevent_seqnum));
}
KERNEL_ATTR_RO(uevent_seqnum);

#ifdef CONFIG_I2C_GEMINI_DA921X_DUAL_MODALIAS_STATE_DIAGNOSTIC
static ssize_t
gemini_da921x_dual_modalias_state_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
		atomic_read(&gemini_da921x_dual_modalias_state) ?
		"validated" : "pending");
}
KERNEL_ATTR_RO(gemini_da921x_dual_modalias_state);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC
static ssize_t
gemini_da921x_dual_modalias_stage_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n",
			  atomic_read(&gemini_da921x_dual_modalias_stage));
}
KERNEL_ATTR_RO(gemini_da921x_dual_modalias_stage);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_ENVELOPE_STATE_DIAGNOSTIC
static ssize_t
gemini_da921x_dual_modalias_envelope_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"envp_idx=%d envp_capacity=%d terminator_null=%d buflen=%d buf_capacity=%d\n",
		atomic_read(&gemini_da921x_dual_modalias_envp_idx),
		atomic_read(&gemini_da921x_dual_modalias_envp_capacity),
		atomic_read(&gemini_da921x_dual_modalias_terminator_null),
		atomic_read(&gemini_da921x_dual_modalias_buflen),
		atomic_read(&gemini_da921x_dual_modalias_buf_capacity));
}
KERNEL_ATTR_RO(gemini_da921x_dual_modalias_envelope);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_ENTRY_CLASSIFICATION_DIAGNOSTIC
static ssize_t
gemini_da921x_dual_modalias_entry_classification_show(struct kobject *kobj,
						      struct kobj_attribute *attr,
						      char *buf)
{
	return sysfs_emit(buf,
		"present_mask=0x%x duplicate_mask=0x%x ordered_prefix=%d seqnum_count=%d seqnum_index=%d unexpected_count=%d\n",
		atomic_read(&gemini_da921x_dual_modalias_present_mask),
		atomic_read(&gemini_da921x_dual_modalias_duplicate_mask),
		atomic_read(&gemini_da921x_dual_modalias_ordered_prefix),
		atomic_read(&gemini_da921x_dual_modalias_seqnum_count),
		atomic_read(&gemini_da921x_dual_modalias_seqnum_index),
		atomic_read(&gemini_da921x_dual_modalias_unexpected_count));
}
KERNEL_ATTR_RO(gemini_da921x_dual_modalias_entry_classification);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_LISTENER_DISCOVERY_DIAGNOSTIC
static ssize_t
gemini_da921x_uevent_listener_state_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "sockets=%d listeners=%d\n",
		atomic_read(&gemini_da921x_uevent_socket_count),
		atomic_read(&gemini_da921x_uevent_listener_count));
}
KERNEL_ATTR_RO(gemini_da921x_uevent_listener_state);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NO_LISTENER_DELIVERY_DIAGNOSTIC
static ssize_t
gemini_da921x_uevent_no_listener_state_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf,
		"sockets=%d listeners=%d allocations=%d broadcasts=%d retval=%d\n",
		atomic_read(&gemini_da921x_no_listener_socket_count),
		atomic_read(&gemini_da921x_no_listener_listener_count),
		atomic_read(&gemini_da921x_no_listener_allocation_count),
		atomic_read(&gemini_da921x_no_listener_broadcast_count),
		atomic_read(&gemini_da921x_no_listener_retval));
}
KERNEL_ATTR_RO(gemini_da921x_uevent_no_listener_state);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_BOUNDED_LISTENER_DIAGNOSTIC
static ssize_t
gemini_da921x_bounded_listener_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"attempts=%d baseline_sockets=%d sockets=%d listeners=%d broadcasts=%d\n",
		atomic_read(&gemini_da921x_bounded_listener_attempts),
		atomic_read(&gemini_da921x_bounded_listener_baseline_socket_count),
		atomic_read(&gemini_da921x_bounded_listener_socket_count),
		atomic_read(&gemini_da921x_bounded_listener_listener_count),
		atomic_read(&gemini_da921x_bounded_listener_broadcast_count));
}

static ssize_t
gemini_da921x_bounded_listener_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;

	if (count != sizeof("probe\n") - 1 || memcmp(buf, "probe\n", count))
		return -EINVAL;
	ret = gemini_da921x_trigger_bounded_listener();
	return ret ?: count;
}

static struct kobj_attribute gemini_da921x_uevent_bounded_listener_attr =
	__ATTR(gemini_da921x_uevent_bounded_listener, 0644,
	       gemini_da921x_bounded_listener_show,
	       gemini_da921x_bounded_listener_store);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_SINGLE_MULTICAST_DIAGNOSTIC
static ssize_t
gemini_da921x_single_multicast_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"attempts=%d baseline_sockets=%d sockets=%d listeners=%d allocations=%d broadcasts=%d retval=%d\n",
		atomic_read(&gemini_da921x_single_multicast_attempts),
		atomic_read(&gemini_da921x_single_multicast_baseline_socket_count),
		atomic_read(&gemini_da921x_single_multicast_socket_count),
		atomic_read(&gemini_da921x_single_multicast_listener_count),
		atomic_read(&gemini_da921x_single_multicast_allocation_count),
		atomic_read(&gemini_da921x_single_multicast_broadcast_count),
		atomic_read(&gemini_da921x_single_multicast_retval));
}

static ssize_t
gemini_da921x_single_multicast_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;

	if (count != sizeof("probe\n") - 1 || memcmp(buf, "probe\n", count))
		return -EINVAL;
	ret = gemini_da921x_trigger_single_multicast();
	return ret ?: count;
}

static struct kobj_attribute gemini_da921x_uevent_single_multicast_attr =
	__ATTR(gemini_da921x_uevent_single_multicast, 0644,
	       gemini_da921x_single_multicast_show,
	       gemini_da921x_single_multicast_store);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_UNTAGGED_DISPATCH_DIAGNOSTIC
static ssize_t
gemini_da921x_untagged_dispatch_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"attempts=%d entries=%d returns=%d baseline_sockets=%d sockets=%d listeners=%d allocations=%d broadcasts=%d retval=%d\n",
		atomic_read(&gemini_da921x_untagged_dispatch_attempts),
		atomic_read(&gemini_da921x_untagged_dispatch_entries),
		atomic_read(&gemini_da921x_untagged_dispatch_returns),
		atomic_read(&gemini_da921x_untagged_dispatch_baseline_socket_count),
		atomic_read(&gemini_da921x_untagged_dispatch_socket_count),
		atomic_read(&gemini_da921x_untagged_dispatch_listener_count),
		atomic_read(&gemini_da921x_untagged_dispatch_allocation_count),
		atomic_read(&gemini_da921x_untagged_dispatch_broadcast_count),
		atomic_read(&gemini_da921x_untagged_dispatch_retval));
}

static ssize_t
gemini_da921x_untagged_dispatch_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	int ret;

	if (count != sizeof("probe\n") - 1 || memcmp(buf, "probe\n", count))
		return -EINVAL;
	ret = gemini_da921x_trigger_untagged_dispatch();
	return ret ?: count;
}

static struct kobj_attribute gemini_da921x_uevent_untagged_dispatch_attr =
	__ATTR(gemini_da921x_uevent_untagged_dispatch, 0644,
	       gemini_da921x_untagged_dispatch_show,
	       gemini_da921x_untagged_dispatch_store);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC
static ssize_t
gemini_da921x_net_broadcast_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"attempts=%d entries=%d returns=%d namespace_checks=%d untagged_routes=%d tagged_routes=%d baseline_sockets=%d sockets=%d listeners=%d allocations=%d broadcasts=%d retval=%d\n",
		atomic_read(&gemini_da921x_net_broadcast_attempts),
		atomic_read(&gemini_da921x_net_broadcast_entries),
		atomic_read(&gemini_da921x_net_broadcast_returns),
		atomic_read(&gemini_da921x_net_broadcast_namespace_checks),
		atomic_read(&gemini_da921x_net_broadcast_untagged_routes),
		atomic_read(&gemini_da921x_net_broadcast_tagged_routes),
		atomic_read(&gemini_da921x_net_broadcast_baseline_socket_count),
		atomic_read(&gemini_da921x_net_broadcast_socket_count),
		atomic_read(&gemini_da921x_net_broadcast_listener_count),
		atomic_read(&gemini_da921x_net_broadcast_allocation_count),
		atomic_read(&gemini_da921x_net_broadcast_broadcast_count),
		atomic_read(&gemini_da921x_net_broadcast_retval));
}

static ssize_t
gemini_da921x_net_broadcast_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	int ret;

	if (count != sizeof("probe\n") - 1 || memcmp(buf, "probe\n", count))
		return -EINVAL;
	ret = gemini_da921x_trigger_net_broadcast();
	return ret ?: count;
}

static struct kobj_attribute gemini_da921x_uevent_net_broadcast_attr =
	__ATTR(gemini_da921x_uevent_net_broadcast, 0644,
	       gemini_da921x_net_broadcast_show,
	       gemini_da921x_net_broadcast_store);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC
static ssize_t
gemini_da921x_normal_fallthrough_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"attempts=%d callsite_entries=%d callsite_returns=%d public_returns=%d retval=%d\n",
		atomic_read(&gemini_da921x_normal_fallthrough_attempts),
		atomic_read(&gemini_da921x_normal_fallthrough_callsite_entries),
		atomic_read(&gemini_da921x_normal_fallthrough_callsite_returns),
		atomic_read(&gemini_da921x_normal_fallthrough_public_returns),
		atomic_read(&gemini_da921x_normal_fallthrough_retval));
}

static ssize_t
gemini_da921x_normal_fallthrough_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	int ret;

	if (count != sizeof("probe\n") - 1 || memcmp(buf, "probe\n", count))
		return -EINVAL;
	ret = gemini_da921x_trigger_normal_fallthrough();
	return ret ?: count;
}

static struct kobj_attribute gemini_da921x_uevent_normal_fallthrough_attr =
	__ATTR(gemini_da921x_uevent_normal_fallthrough, 0644,
	       gemini_da921x_normal_fallthrough_show,
	       gemini_da921x_normal_fallthrough_store);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC
static ssize_t
gemini_da921x_natural_device_add_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"attempts=%d register_entries=%d register_returns=%d register_retval=%d callsite_entries=%d callsite_returns=%d public_returns=%d wrapper_entries=%d wrapper_returns=%d namespace_checks=%d untagged_routes=%d tagged_routes=%d sockets=%d listeners=%d allocations=%d broadcasts=%d uevent_retval=%d\n",
		atomic_read(&gemini_da921x_natural_device_add_attempts),
		atomic_read(&gemini_da921x_natural_device_add_register_entries),
		atomic_read(&gemini_da921x_natural_device_add_register_returns),
		atomic_read(&gemini_da921x_natural_device_add_register_retval),
		atomic_read(&gemini_da921x_natural_device_add_callsite_entries),
		atomic_read(&gemini_da921x_natural_device_add_callsite_returns),
		atomic_read(&gemini_da921x_natural_device_add_public_returns),
		atomic_read(&gemini_da921x_natural_device_add_wrapper_entries),
		atomic_read(&gemini_da921x_natural_device_add_wrapper_returns),
		atomic_read(&gemini_da921x_natural_device_add_namespace_checks),
		atomic_read(&gemini_da921x_natural_device_add_untagged_routes),
		atomic_read(&gemini_da921x_natural_device_add_tagged_routes),
		atomic_read(&gemini_da921x_natural_device_add_socket_count),
		atomic_read(&gemini_da921x_natural_device_add_listener_count),
		atomic_read(&gemini_da921x_natural_device_add_allocation_count),
		atomic_read(&gemini_da921x_natural_device_add_broadcast_count),
		atomic_read(&gemini_da921x_natural_device_add_uevent_retval));
}
KERNEL_ATTR_RO(gemini_da921x_natural_device_add);
#endif

/* cpu byteorder */
static ssize_t cpu_byteorder_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", CPU_BYTEORDER_STRING);
}
KERNEL_ATTR_RO(cpu_byteorder);

/* address bits */
static ssize_t address_bits_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%zu\n", sizeof(void *) * 8 /* CHAR_BIT */);
}
KERNEL_ATTR_RO(address_bits);

#ifdef CONFIG_UEVENT_HELPER
/* uevent helper program, used during early boot */
static ssize_t uevent_helper_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", uevent_helper);
}
static ssize_t uevent_helper_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	if (count+1 > UEVENT_HELPER_PATH_LEN)
		return -ENOENT;
	memcpy(uevent_helper, buf, count);
	uevent_helper[count] = '\0';
	if (count && uevent_helper[count-1] == '\n')
		uevent_helper[count-1] = '\0';
	return count;
}
KERNEL_ATTR_RW(uevent_helper);
#endif

#ifdef CONFIG_PROFILING
static ssize_t profiling_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", prof_on);
}
static ssize_t profiling_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int ret;
	static DEFINE_MUTEX(lock);

	/*
	 * We need serialization, for profile_setup() initializes prof_on
	 * value and profile_init() must not reallocate prof_buffer after
	 * once allocated.
	 */
	guard(mutex)(&lock);
	if (prof_on)
		return -EEXIST;
	/*
	 * This eventually calls into get_option() which
	 * has a ton of callers and is not const.  It is
	 * easiest to cast it away here.
	 */
	profile_setup((char *)buf);
	ret = profile_init();
	if (ret)
		return ret;
	ret = create_proc_profile();
	if (ret)
		return ret;
	return count;
}
KERNEL_ATTR_RW(profiling);
#endif

#ifdef CONFIG_VMCORE_INFO

static ssize_t vmcoreinfo_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	phys_addr_t vmcore_base = paddr_vmcoreinfo_note();
	return sysfs_emit(buf, "%pa %x\n", &vmcore_base,
			  (unsigned int)VMCOREINFO_NOTE_SIZE);
}
KERNEL_ATTR_RO(vmcoreinfo);

#endif /* CONFIG_VMCORE_INFO */

/* whether file capabilities are enabled */
static ssize_t fscaps_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", file_caps_enabled);
}
KERNEL_ATTR_RO(fscaps);

#ifndef CONFIG_TINY_RCU
int rcu_expedited;
static ssize_t rcu_expedited_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(rcu_expedited));
}
static ssize_t rcu_expedited_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	if (kstrtoint(buf, 0, &rcu_expedited))
		return -EINVAL;

	return count;
}
KERNEL_ATTR_RW(rcu_expedited);

int rcu_normal;
static ssize_t rcu_normal_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(rcu_normal));
}
static ssize_t rcu_normal_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	if (kstrtoint(buf, 0, &rcu_normal))
		return -EINVAL;

	return count;
}
KERNEL_ATTR_RW(rcu_normal);
#endif /* #ifndef CONFIG_TINY_RCU */

/*
 * Make /sys/kernel/notes give the raw contents of our kernel .notes section.
 */
extern const void __start_notes;
extern const void __stop_notes;
#define	notes_size (&__stop_notes - &__start_notes)

static __ro_after_init BIN_ATTR_SIMPLE_RO(notes);

struct kobject *kernel_kobj;
EXPORT_SYMBOL_GPL(kernel_kobj);

static struct attribute * kernel_attrs[] = {
	&fscaps_attr.attr,
	&uevent_seqnum_attr.attr,
#ifdef CONFIG_I2C_GEMINI_DA921X_DUAL_MODALIAS_STATE_DIAGNOSTIC
	&gemini_da921x_dual_modalias_state_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC
	&gemini_da921x_dual_modalias_stage_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_ENVELOPE_STATE_DIAGNOSTIC
	&gemini_da921x_dual_modalias_envelope_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_ENTRY_CLASSIFICATION_DIAGNOSTIC
	&gemini_da921x_dual_modalias_entry_classification_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_LISTENER_DISCOVERY_DIAGNOSTIC
	&gemini_da921x_uevent_listener_state_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NO_LISTENER_DELIVERY_DIAGNOSTIC
	&gemini_da921x_uevent_no_listener_state_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_BOUNDED_LISTENER_DIAGNOSTIC
	&gemini_da921x_uevent_bounded_listener_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_SINGLE_MULTICAST_DIAGNOSTIC
	&gemini_da921x_uevent_single_multicast_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_UNTAGGED_DISPATCH_DIAGNOSTIC
	&gemini_da921x_uevent_untagged_dispatch_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC
	&gemini_da921x_uevent_net_broadcast_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC
	&gemini_da921x_uevent_normal_fallthrough_attr.attr,
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC
	&gemini_da921x_natural_device_add_attr.attr,
#endif
	&cpu_byteorder_attr.attr,
	&address_bits_attr.attr,
#ifdef CONFIG_UEVENT_HELPER
	&uevent_helper_attr.attr,
#endif
#ifdef CONFIG_PROFILING
	&profiling_attr.attr,
#endif
#ifdef CONFIG_VMCORE_INFO
	&vmcoreinfo_attr.attr,
#endif
#ifndef CONFIG_TINY_RCU
	&rcu_expedited_attr.attr,
	&rcu_normal_attr.attr,
#endif
	NULL
};

static const struct attribute_group kernel_attr_group = {
	.attrs = kernel_attrs,
};

void __init ksysfs_init(void)
{
	int error;

	kernel_kobj = kobject_create_and_add("kernel", NULL);
	if (!kernel_kobj) {
		error = -ENOMEM;
		goto exit;
	}
	error = sysfs_create_group(kernel_kobj, &kernel_attr_group);
	if (error)
		goto kset_exit;

	if (notes_size > 0) {
		bin_attr_notes.private = (void *)&__start_notes;
		bin_attr_notes.size = notes_size;
		error = sysfs_create_bin_file(kernel_kobj, &bin_attr_notes);
		if (error)
			goto group_exit;
	}

	return;

group_exit:
	sysfs_remove_group(kernel_kobj, &kernel_attr_group);
kset_exit:
	kobject_put(kernel_kobj);
exit:
	pr_err("failed to initialize the kernel kobject: %d\n", error);
}
