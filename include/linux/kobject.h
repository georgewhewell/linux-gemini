// SPDX-License-Identifier: GPL-2.0
/*
 * kobject.h - generic kernel object infrastructure.
 *
 * Copyright (c) 2002-2003 Patrick Mochel
 * Copyright (c) 2002-2003 Open Source Development Labs
 * Copyright (c) 2006-2008 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2006-2008 Novell Inc.
 *
 * Please read Documentation/core-api/kobject.rst before using the kobject
 * interface, ESPECIALLY the parts about reference counts and object
 * destructors.
 */

#ifndef _KOBJECT_H_
#define _KOBJECT_H_

#include <linux/types.h>
#include <linux/list.h>
#include <linux/sysfs.h>
#include <linux/compiler.h>
#include <linux/container_of.h>
#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/kobject_ns.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/uidgid.h>

#define UEVENT_HELPER_PATH_LEN		256
#define UEVENT_NUM_ENVP			64	/* number of env pointers */
#define UEVENT_BUFFER_SIZE		2048	/* buffer for the variables */

#ifdef CONFIG_UEVENT_HELPER
/* path to the userspace helper executed on an event */
extern char uevent_helper[];
#endif

/* counter to tag the uevent, read only except for the kobject core */
extern atomic64_t uevent_seqnum;

#ifdef CONFIG_I2C_GEMINI_DA921X_DUAL_MODALIAS_STATE_DIAGNOSTIC
/* experiment-only exact-event validation state */
extern atomic_t gemini_da921x_dual_modalias_state;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC
/* experiment-only last successful exact-event validation stage */
extern atomic_t gemini_da921x_dual_modalias_stage;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_ENVELOPE_STATE_DIAGNOSTIC
extern atomic_t gemini_da921x_dual_modalias_envp_idx;
extern atomic_t gemini_da921x_dual_modalias_envp_capacity;
extern atomic_t gemini_da921x_dual_modalias_terminator_null;
extern atomic_t gemini_da921x_dual_modalias_buflen;
extern atomic_t gemini_da921x_dual_modalias_buf_capacity;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_ENTRY_CLASSIFICATION_DIAGNOSTIC
extern atomic_t gemini_da921x_dual_modalias_present_mask;
extern atomic_t gemini_da921x_dual_modalias_duplicate_mask;
extern atomic_t gemini_da921x_dual_modalias_ordered_prefix;
extern atomic_t gemini_da921x_dual_modalias_seqnum_count;
extern atomic_t gemini_da921x_dual_modalias_seqnum_index;
extern atomic_t gemini_da921x_dual_modalias_unexpected_count;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_LISTENER_DISCOVERY_DIAGNOSTIC
/* experiment-only bounded uevent socket and listener counts */
extern atomic_t gemini_da921x_uevent_socket_count;
extern atomic_t gemini_da921x_uevent_listener_count;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NO_LISTENER_DELIVERY_DIAGNOSTIC
extern atomic_t gemini_da921x_no_listener_socket_count;
extern atomic_t gemini_da921x_no_listener_listener_count;
extern atomic_t gemini_da921x_no_listener_allocation_count;
extern atomic_t gemini_da921x_no_listener_broadcast_count;
extern atomic_t gemini_da921x_no_listener_retval;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_BOUNDED_LISTENER_DIAGNOSTIC
extern atomic_t gemini_da921x_bounded_listener_attempts;
extern atomic_t gemini_da921x_bounded_listener_baseline_socket_count;
extern atomic_t gemini_da921x_bounded_listener_socket_count;
extern atomic_t gemini_da921x_bounded_listener_listener_count;
extern atomic_t gemini_da921x_bounded_listener_broadcast_count;
int gemini_da921x_trigger_bounded_listener(void);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_SINGLE_MULTICAST_DIAGNOSTIC
extern atomic_t gemini_da921x_single_multicast_attempts;
extern atomic_t gemini_da921x_single_multicast_baseline_socket_count;
extern atomic_t gemini_da921x_single_multicast_socket_count;
extern atomic_t gemini_da921x_single_multicast_listener_count;
extern atomic_t gemini_da921x_single_multicast_allocation_count;
extern atomic_t gemini_da921x_single_multicast_broadcast_count;
extern atomic_t gemini_da921x_single_multicast_retval;
int gemini_da921x_trigger_single_multicast(void);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_UNTAGGED_DISPATCH_DIAGNOSTIC
extern atomic_t gemini_da921x_untagged_dispatch_attempts;
extern atomic_t gemini_da921x_untagged_dispatch_entries;
extern atomic_t gemini_da921x_untagged_dispatch_returns;
extern atomic_t gemini_da921x_untagged_dispatch_baseline_socket_count;
extern atomic_t gemini_da921x_untagged_dispatch_socket_count;
extern atomic_t gemini_da921x_untagged_dispatch_listener_count;
extern atomic_t gemini_da921x_untagged_dispatch_allocation_count;
extern atomic_t gemini_da921x_untagged_dispatch_broadcast_count;
extern atomic_t gemini_da921x_untagged_dispatch_retval;
int gemini_da921x_trigger_untagged_dispatch(void);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC
extern atomic_t gemini_da921x_net_broadcast_attempts;
extern atomic_t gemini_da921x_net_broadcast_entries;
extern atomic_t gemini_da921x_net_broadcast_returns;
extern atomic_t gemini_da921x_net_broadcast_namespace_checks;
extern atomic_t gemini_da921x_net_broadcast_untagged_routes;
extern atomic_t gemini_da921x_net_broadcast_tagged_routes;
extern atomic_t gemini_da921x_net_broadcast_baseline_socket_count;
extern atomic_t gemini_da921x_net_broadcast_socket_count;
extern atomic_t gemini_da921x_net_broadcast_listener_count;
extern atomic_t gemini_da921x_net_broadcast_allocation_count;
extern atomic_t gemini_da921x_net_broadcast_broadcast_count;
extern atomic_t gemini_da921x_net_broadcast_retval;
int gemini_da921x_trigger_net_broadcast(void);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC
extern atomic_t gemini_da921x_normal_fallthrough_attempts;
extern atomic_t gemini_da921x_normal_fallthrough_callsite_entries;
extern atomic_t gemini_da921x_normal_fallthrough_callsite_returns;
extern atomic_t gemini_da921x_normal_fallthrough_public_returns;
extern atomic_t gemini_da921x_normal_fallthrough_retval;
int gemini_da921x_trigger_normal_fallthrough(void);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC
extern atomic_t gemini_da921x_natural_device_add_attempts;
extern atomic_t gemini_da921x_natural_device_add_register_entries;
extern atomic_t gemini_da921x_natural_device_add_register_returns;
extern atomic_t gemini_da921x_natural_device_add_register_retval;
extern atomic_t gemini_da921x_natural_device_add_callsite_entries;
extern atomic_t gemini_da921x_natural_device_add_callsite_returns;
extern atomic_t gemini_da921x_natural_device_add_public_returns;
extern atomic_t gemini_da921x_natural_device_add_wrapper_entries;
extern atomic_t gemini_da921x_natural_device_add_wrapper_returns;
extern atomic_t gemini_da921x_natural_device_add_namespace_checks;
extern atomic_t gemini_da921x_natural_device_add_untagged_routes;
extern atomic_t gemini_da921x_natural_device_add_tagged_routes;
extern atomic_t gemini_da921x_natural_device_add_socket_count;
extern atomic_t gemini_da921x_natural_device_add_listener_count;
extern atomic_t gemini_da921x_natural_device_add_allocation_count;
extern atomic_t gemini_da921x_natural_device_add_broadcast_count;
extern atomic_t gemini_da921x_natural_device_add_uevent_retval;
bool gemini_da921x_natural_device_add_begin(struct kobject *kobj);
void gemini_da921x_natural_device_add_end(struct kobject *kobj, int retval);
#endif

/*
 * The actions here must match the index to the string array
 * in lib/kobject_uevent.c
 *
 * Do not add new actions here without checking with the driver-core
 * maintainers. Action strings are not meant to express subsystem
 * or device specific properties. In most cases you want to send a
 * kobject_uevent_env(kobj, KOBJ_CHANGE, env) with additional event
 * specific variables added to the event environment.
 */
enum kobject_action {
	KOBJ_ADD,
	KOBJ_REMOVE,
	KOBJ_CHANGE,
	KOBJ_MOVE,
	KOBJ_ONLINE,
	KOBJ_OFFLINE,
	KOBJ_BIND,
	KOBJ_UNBIND,
};

struct kobject {
	const char		*name;
	struct list_head	entry;
	struct kobject		*parent;
	struct kset		*kset;
	const struct kobj_type	*ktype;
	struct kernfs_node	*sd; /* sysfs directory entry */
	struct kref		kref;

	unsigned int state_initialized:1;
	unsigned int state_in_sysfs:1;
	unsigned int state_add_uevent_sent:1;
	unsigned int state_remove_uevent_sent:1;
	unsigned int uevent_suppress:1;

#ifdef CONFIG_DEBUG_KOBJECT_RELEASE
	struct delayed_work	release;
#endif
};

__printf(2, 3) int kobject_set_name(struct kobject *kobj, const char *name, ...);
__printf(2, 0) int kobject_set_name_vargs(struct kobject *kobj, const char *fmt, va_list vargs);

static inline const char *kobject_name(const struct kobject *kobj)
{
	return kobj->name;
}

void kobject_init(struct kobject *kobj, const struct kobj_type *ktype);
__printf(3, 4) __must_check int kobject_add(struct kobject *kobj,
					    struct kobject *parent,
					    const char *fmt, ...);
__printf(4, 5) __must_check int kobject_init_and_add(struct kobject *kobj,
						     const struct kobj_type *ktype,
						     struct kobject *parent,
						     const char *fmt, ...);

void kobject_del(struct kobject *kobj);

struct kobject * __must_check kobject_create_and_add(const char *name, struct kobject *parent);

int __must_check kobject_rename(struct kobject *, const char *new_name);
int __must_check kobject_move(struct kobject *, struct kobject *);

struct kobject *kobject_get(struct kobject *kobj);
struct kobject * __must_check kobject_get_unless_zero(struct kobject *kobj);
void kobject_put(struct kobject *kobj);

const struct ns_common *kobject_namespace(const struct kobject *kobj);
void kobject_get_ownership(const struct kobject *kobj, kuid_t *uid, kgid_t *gid);
char *kobject_get_path(const struct kobject *kobj, gfp_t flag);

struct kobj_type {
	void (*release)(struct kobject *kobj);
	const struct sysfs_ops *sysfs_ops;
	const struct attribute_group **default_groups;
	const struct kobj_ns_type_operations *(*child_ns_type)(const struct kobject *kobj);
	const struct ns_common *(*namespace)(const struct kobject *kobj);
	void (*get_ownership)(const struct kobject *kobj, kuid_t *uid, kgid_t *gid);
};

struct kobj_uevent_env {
	char *argv[3];
	char *envp[UEVENT_NUM_ENVP];
	int envp_idx;
	char buf[UEVENT_BUFFER_SIZE];
	int buflen;
};

struct kset_uevent_ops {
	int (* const filter)(const struct kobject *kobj);
	const char *(* const name)(const struct kobject *kobj);
	int (* const uevent)(const struct kobject *kobj, struct kobj_uevent_env *env);
};

struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count);
};

extern const struct sysfs_ops kobj_sysfs_ops;

struct sock;

/**
 * struct kset - a set of kobjects of a specific type, belonging to a specific subsystem.
 *
 * A kset defines a group of kobjects.  They can be individually
 * different "types" but overall these kobjects all want to be grouped
 * together and operated on in the same manner.  ksets are used to
 * define the attribute callbacks and other common events that happen to
 * a kobject.
 *
 * @list: the list of all kobjects for this kset
 * @list_lock: a lock for iterating over the kobjects
 * @kobj: the embedded kobject for this kset (recursion, isn't it fun...)
 * @uevent_ops: the set of uevent operations for this kset.  These are
 * called whenever a kobject has something happen to it so that the kset
 * can add new environment variables, or filter out the uevents if so
 * desired.
 */
struct kset {
	struct list_head list;
	spinlock_t list_lock;
	struct kobject kobj;
	const struct kset_uevent_ops *uevent_ops;
} __randomize_layout;

void kset_init(struct kset *kset);
int __must_check kset_register(struct kset *kset);
void kset_unregister(struct kset *kset);
struct kset * __must_check kset_create_and_add(const char *name, const struct kset_uevent_ops *u,
					       struct kobject *parent_kobj);

static inline struct kset *to_kset(struct kobject *kobj)
{
	return kobj ? container_of(kobj, struct kset, kobj) : NULL;
}

static inline struct kset *kset_get(struct kset *k)
{
	return k ? to_kset(kobject_get(&k->kobj)) : NULL;
}

static inline void kset_put(struct kset *k)
{
	kobject_put(&k->kobj);
}

static inline const struct kobj_type *get_ktype(const struct kobject *kobj)
{
	return kobj->ktype;
}

struct kobject *kset_find_obj(struct kset *, const char *);

/* The global /sys/kernel/ kobject for people to chain off of */
extern struct kobject *kernel_kobj;
/* The global /sys/kernel/mm/ kobject for people to chain off of */
extern struct kobject *mm_kobj;
/* The global /sys/hypervisor/ kobject for people to chain off of */
extern struct kobject *hypervisor_kobj;
/* The global /sys/power/ kobject for people to chain off of */
extern struct kobject *power_kobj;
/* The global /sys/firmware/ kobject for people to chain off of */
extern struct kobject *firmware_kobj;

int kobject_uevent(struct kobject *kobj, enum kobject_action action);
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
			char *envp[]);
int kobject_synth_uevent(struct kobject *kobj, const char *buf, size_t count);

__printf(2, 3)
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...);

#endif /* _KOBJECT_H_ */
