// SPDX-License-Identifier: GPL-2.0
/*
 * kernel userspace event delivery
 *
 * Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
 * Copyright (C) 2004 Novell, Inc.  All rights reserved.
 * Copyright (C) 2004 IBM, Inc. All rights reserved.
 *
 * Authors:
 *	Robert Love		<rml@novell.com>
 *	Kay Sievers		<kay.sievers@vrfy.org>
 *	Arjan van de Ven	<arjanv@redhat.com>
 *	Greg Kroah-Hartman	<greg@kroah.com>
 */

#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/kobject.h>
#include <linux/export.h>
#include <linux/kmod.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <linux/uidgid.h>
#include <linux/uuid.h>
#include <linux/ctype.h>
#include <net/sock.h>
#include <net/netlink.h>
#include <net/net_namespace.h>


atomic64_t uevent_seqnum;
#ifdef CONFIG_I2C_GEMINI_DA921X_DUAL_MODALIAS_STATE_DIAGNOSTIC
atomic_t gemini_da921x_dual_modalias_state = ATOMIC_INIT(0);
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC
atomic_t gemini_da921x_dual_modalias_stage = ATOMIC_INIT(0);
#endif
#ifdef CONFIG_I2C_GEMINI_DA921X_ENVELOPE_STATE_DIAGNOSTIC
atomic_t gemini_da921x_dual_modalias_envp_idx = ATOMIC_INIT(-1);
atomic_t gemini_da921x_dual_modalias_envp_capacity = ATOMIC_INIT(-1);
atomic_t gemini_da921x_dual_modalias_terminator_null = ATOMIC_INIT(-1);
atomic_t gemini_da921x_dual_modalias_buflen = ATOMIC_INIT(-1);
atomic_t gemini_da921x_dual_modalias_buf_capacity = ATOMIC_INIT(-1);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_ENTRY_CLASSIFICATION_DIAGNOSTIC
atomic_t gemini_da921x_dual_modalias_present_mask = ATOMIC_INIT(-1);
atomic_t gemini_da921x_dual_modalias_duplicate_mask = ATOMIC_INIT(-1);
atomic_t gemini_da921x_dual_modalias_ordered_prefix = ATOMIC_INIT(-1);
atomic_t gemini_da921x_dual_modalias_seqnum_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_dual_modalias_seqnum_index = ATOMIC_INIT(-1);
atomic_t gemini_da921x_dual_modalias_unexpected_count = ATOMIC_INIT(-1);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_LISTENER_DISCOVERY_DIAGNOSTIC
atomic_t gemini_da921x_uevent_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_uevent_listener_count = ATOMIC_INIT(-1);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NO_LISTENER_DELIVERY_DIAGNOSTIC
atomic_t gemini_da921x_no_listener_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_no_listener_listener_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_no_listener_allocation_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_no_listener_broadcast_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_no_listener_retval = ATOMIC_INIT(-1);
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_BOUNDED_LISTENER_DIAGNOSTIC
atomic_t gemini_da921x_bounded_listener_attempts = ATOMIC_INIT(0);
atomic_t gemini_da921x_bounded_listener_baseline_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_bounded_listener_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_bounded_listener_listener_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_bounded_listener_broadcast_count = ATOMIC_INIT(-1);

static DEFINE_MUTEX(gemini_da921x_bounded_listener_lock);
static struct kobject *gemini_da921x_bounded_listener_target;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_SINGLE_MULTICAST_DIAGNOSTIC
atomic_t gemini_da921x_single_multicast_attempts = ATOMIC_INIT(0);
atomic_t gemini_da921x_single_multicast_baseline_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_single_multicast_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_single_multicast_listener_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_single_multicast_allocation_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_single_multicast_broadcast_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_single_multicast_retval = ATOMIC_INIT(-1);

static DEFINE_MUTEX(gemini_da921x_single_multicast_lock);
static struct kobject *gemini_da921x_single_multicast_target;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_UNTAGGED_DISPATCH_DIAGNOSTIC
atomic_t gemini_da921x_untagged_dispatch_attempts = ATOMIC_INIT(0);
atomic_t gemini_da921x_untagged_dispatch_entries = ATOMIC_INIT(0);
atomic_t gemini_da921x_untagged_dispatch_returns = ATOMIC_INIT(0);
atomic_t gemini_da921x_untagged_dispatch_baseline_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_untagged_dispatch_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_untagged_dispatch_listener_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_untagged_dispatch_allocation_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_untagged_dispatch_broadcast_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_untagged_dispatch_retval = ATOMIC_INIT(-1);

static DEFINE_MUTEX(gemini_da921x_untagged_dispatch_lock);
static struct kobject *gemini_da921x_untagged_dispatch_target;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC
atomic_t gemini_da921x_net_broadcast_attempts = ATOMIC_INIT(0);
atomic_t gemini_da921x_net_broadcast_entries = ATOMIC_INIT(0);
atomic_t gemini_da921x_net_broadcast_returns = ATOMIC_INIT(0);
atomic_t gemini_da921x_net_broadcast_namespace_checks = ATOMIC_INIT(0);
atomic_t gemini_da921x_net_broadcast_untagged_routes = ATOMIC_INIT(0);
atomic_t gemini_da921x_net_broadcast_tagged_routes = ATOMIC_INIT(0);
atomic_t gemini_da921x_net_broadcast_baseline_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_net_broadcast_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_net_broadcast_listener_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_net_broadcast_allocation_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_net_broadcast_broadcast_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_net_broadcast_retval = ATOMIC_INIT(-1);

static DEFINE_MUTEX(gemini_da921x_net_broadcast_lock);
static struct kobject *gemini_da921x_net_broadcast_target;
static struct kobject *gemini_da921x_net_broadcast_active_target;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC
atomic_t gemini_da921x_normal_fallthrough_attempts = ATOMIC_INIT(0);
atomic_t gemini_da921x_normal_fallthrough_callsite_entries = ATOMIC_INIT(0);
atomic_t gemini_da921x_normal_fallthrough_callsite_returns = ATOMIC_INIT(0);
atomic_t gemini_da921x_normal_fallthrough_public_returns = ATOMIC_INIT(0);
atomic_t gemini_da921x_normal_fallthrough_retval = ATOMIC_INIT(-1);

static DEFINE_MUTEX(gemini_da921x_normal_fallthrough_lock);
static struct kobject *gemini_da921x_normal_fallthrough_target;
static struct kobject *gemini_da921x_normal_fallthrough_active_target;
#endif

#ifdef CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC
atomic_t gemini_da921x_natural_device_add_attempts = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_register_entries = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_register_returns = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_register_retval = ATOMIC_INIT(-1);
atomic_t gemini_da921x_natural_device_add_callsite_entries = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_callsite_returns = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_public_returns = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_wrapper_entries = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_wrapper_returns = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_namespace_checks = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_untagged_routes = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_tagged_routes = ATOMIC_INIT(0);
atomic_t gemini_da921x_natural_device_add_socket_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_natural_device_add_listener_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_natural_device_add_allocation_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_natural_device_add_broadcast_count = ATOMIC_INIT(-1);
atomic_t gemini_da921x_natural_device_add_uevent_retval = ATOMIC_INIT(-1);

static DEFINE_MUTEX(gemini_da921x_natural_device_add_lock);
static struct kobject *gemini_da921x_natural_device_add_active_target;
#endif

#ifdef CONFIG_UEVENT_HELPER
char uevent_helper[UEVENT_HELPER_PATH_LEN] = CONFIG_UEVENT_HELPER_PATH;
#endif

struct uevent_sock {
	struct list_head list;
	struct sock *sk;
};

#ifdef CONFIG_NET
static LIST_HEAD(uevent_sock_list);
/* This lock protects uevent_sock_list */
static DEFINE_MUTEX(uevent_sock_mutex);
#endif

/* the strings here must match the enum in include/linux/kobject.h */
static const char *kobject_actions[] = {
	[KOBJ_ADD] =		"add",
	[KOBJ_REMOVE] =		"remove",
	[KOBJ_CHANGE] =		"change",
	[KOBJ_MOVE] =		"move",
	[KOBJ_ONLINE] =		"online",
	[KOBJ_OFFLINE] =	"offline",
	[KOBJ_BIND] =		"bind",
	[KOBJ_UNBIND] =		"unbind",
};

static int kobject_action_type(const char *buf, size_t count,
			       enum kobject_action *type,
			       const char **args)
{
	enum kobject_action action;
	size_t count_first;
	const char *args_start;
	int ret = -EINVAL;

	if (count && (buf[count-1] == '\n' || buf[count-1] == '\0'))
		count--;

	if (!count)
		goto out;

	args_start = strnchr(buf, count, ' ');
	if (args_start) {
		count_first = args_start - buf;
		args_start = args_start + 1;
	} else
		count_first = count;

	for (action = 0; action < ARRAY_SIZE(kobject_actions); action++) {
		if (strncmp(kobject_actions[action], buf, count_first) != 0)
			continue;
		if (kobject_actions[action][count_first] != '\0')
			continue;
		if (args)
			*args = args_start;
		*type = action;
		ret = 0;
		break;
	}
out:
	return ret;
}

static const char *action_arg_word_end(const char *buf, const char *buf_end,
				       char delim)
{
	const char *next = buf;

	while (next <= buf_end && *next != delim)
		if (!isalnum(*next++))
			return NULL;

	if (next == buf)
		return NULL;

	return next;
}

static int kobject_action_args(const char *buf, size_t count,
			       struct kobj_uevent_env **ret_env)
{
	struct kobj_uevent_env *env = NULL;
	const char *next, *buf_end, *key;
	int key_len;
	int r = -EINVAL;

	if (count && (buf[count - 1] == '\n' || buf[count - 1] == '\0'))
		count--;

	if (!count)
		return -EINVAL;

	env = kzalloc_obj(*env);
	if (!env)
		return -ENOMEM;

	/* first arg is UUID */
	if (count < UUID_STRING_LEN || !uuid_is_valid(buf) ||
	    add_uevent_var(env, "SYNTH_UUID=%.*s", UUID_STRING_LEN, buf))
		goto out;

	/*
	 * the rest are custom environment variables in KEY=VALUE
	 * format with ' ' delimiter between each KEY=VALUE pair
	 */
	next = buf + UUID_STRING_LEN;
	buf_end = buf + count - 1;

	while (next <= buf_end) {
		if (*next != ' ')
			goto out;

		/* skip the ' ', key must follow */
		key = ++next;
		if (key > buf_end)
			goto out;

		buf = next;
		next = action_arg_word_end(buf, buf_end, '=');
		if (!next || next > buf_end || *next != '=')
			goto out;
		key_len = next - buf;

		/* skip the '=', value must follow */
		if (++next > buf_end)
			goto out;

		buf = next;
		next = action_arg_word_end(buf, buf_end, ' ');
		if (!next)
			goto out;

		if (add_uevent_var(env, "SYNTH_ARG_%.*s=%.*s",
				   key_len, key, (int) (next - buf), buf))
			goto out;
	}

	r = 0;
out:
	if (r)
		kfree(env);
	else
		*ret_env = env;
	return r;
}

/**
 * kobject_synth_uevent - send synthetic uevent with arguments
 *
 * @kobj: struct kobject for which synthetic uevent is to be generated
 * @buf: buffer containing action type and action args, newline is ignored
 * @count: length of buffer
 *
 * Returns 0 if kobject_synthetic_uevent() is completed with success or the
 * corresponding error when it fails.
 */
int kobject_synth_uevent(struct kobject *kobj, const char *buf, size_t count)
{
	char *no_uuid_envp[] = { "SYNTH_UUID=0", NULL };
	enum kobject_action action;
	const char *action_args;
	struct kobj_uevent_env *env;
	const char *msg = NULL, *devpath;
	int r;

	r = kobject_action_type(buf, count, &action, &action_args);
	if (r) {
		msg = "unknown uevent action string";
		goto out;
	}

	if (!action_args) {
		r = kobject_uevent_env(kobj, action, no_uuid_envp);
		goto out;
	}

	r = kobject_action_args(action_args,
				count - (action_args - buf), &env);
	if (r == -EINVAL) {
		msg = "incorrect uevent action arguments";
		goto out;
	}

	if (r)
		goto out;

	r = kobject_uevent_env(kobj, action, env->envp);
	kfree(env);
out:
	if (r) {
		devpath = kobject_get_path(kobj, GFP_KERNEL);
		pr_warn("synth uevent: %s: %s\n",
		       devpath ?: "unknown device",
		       msg ?: "failed to send uevent");
		kfree(devpath);
	}
	return r;
}

#ifdef CONFIG_UEVENT_HELPER
static int kobj_usermode_filter(struct kobject *kobj)
{
	const struct kobj_ns_type_operations *ops;

	ops = kobj_ns_ops(kobj);
	if (ops) {
		const struct ns_common *init_ns, *ns;

		ns = kobj->ktype->namespace(kobj);
		init_ns = ops->initial_ns();
		return ns != init_ns;
	}

	return 0;
}

static int init_uevent_argv(struct kobj_uevent_env *env, const char *subsystem)
{
	int buffer_size = sizeof(env->buf) - env->buflen;
	int len;

	len = strscpy(&env->buf[env->buflen], subsystem, buffer_size);
	if (len < 0) {
		pr_warn("%s: insufficient buffer space (%u left) for %s\n",
			__func__, buffer_size, subsystem);
		return -ENOMEM;
	}

	env->argv[0] = uevent_helper;
	env->argv[1] = &env->buf[env->buflen];
	env->argv[2] = NULL;

	env->buflen += len + 1;
	return 0;
}

static void cleanup_uevent_env(struct subprocess_info *info)
{
	kfree(info->data);
}
#endif

#ifdef CONFIG_NET
static struct sk_buff *alloc_uevent_skb(struct kobj_uevent_env *env,
					const char *action_string,
					const char *devpath)
{
	struct netlink_skb_parms *parms;
	struct sk_buff *skb = NULL;
	char *scratch;
	size_t len;

	/* allocate message with maximum possible size */
	len = strlen(action_string) + strlen(devpath) + 2;
	skb = alloc_skb(len + env->buflen, GFP_KERNEL);
	if (!skb)
		return NULL;

	/* add header */
	scratch = skb_put(skb, len);
	sprintf(scratch, "%s@%s", action_string, devpath);

	skb_put_data(skb, env->buf, env->buflen);

	parms = &NETLINK_CB(skb);
	parms->creds.uid = GLOBAL_ROOT_UID;
	parms->creds.gid = GLOBAL_ROOT_GID;
	parms->dst_group = 1;
	parms->portid = 0;

	return skb;
}

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NETLINK_SERIALIZATION_DIAGNOSTIC)
static int gemini_da921x_serialize_uevent(struct kobj_uevent_env *env,
					  const char *action_string,
					  const char *devpath)
{
	struct netlink_skb_parms *parms;
	struct sk_buff *skb;
	size_t action_len = strlen(action_string);
	size_t devpath_len = strlen(devpath);
	size_t header_len = action_len + devpath_len + 2;
	int ret = -EINVAL;

	skb = alloc_uevent_skb(env, action_string, devpath);
	if (!skb)
		return -ENOMEM;

	if (skb->len != header_len + env->buflen)
		goto out;
	if (memcmp(skb->data, action_string, action_len))
		goto out;
	if (skb->data[action_len] != '@')
		goto out;
	if (memcmp(skb->data + action_len + 1, devpath, devpath_len))
		goto out;
	if (skb->data[header_len - 1] != '\0')
		goto out;
	if (memcmp(skb->data + header_len, env->buf, env->buflen))
		goto out;

	parms = &NETLINK_CB(skb);
	if (!uid_eq(parms->creds.uid, GLOBAL_ROOT_UID) ||
	    !gid_eq(parms->creds.gid, GLOBAL_ROOT_GID) ||
	    parms->dst_group != 1 || parms->portid)
		goto out;

	atomic_set(&gemini_da921x_dual_modalias_stage, 18);
	ret = 0;
out:
	consume_skb(skb);
	return ret;
}
#endif

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_LISTENER_DISCOVERY_DIAGNOSTIC)
static int gemini_da921x_discover_uevent_listeners(void)
{
	struct uevent_sock *ue_sk;
	int listener_count = 0;
	int socket_count = 0;
	int ret = 0;

	mutex_lock(&uevent_sock_mutex);
	list_for_each_entry(ue_sk, &uevent_sock_list, list) {
		if (socket_count == 1024) {
			ret = -E2BIG;
			break;
		}
		socket_count++;
		if (netlink_has_listeners(ue_sk->sk, 1))
			listener_count++;
	}
	mutex_unlock(&uevent_sock_mutex);
	if (ret)
		return ret;

	atomic_set(&gemini_da921x_uevent_socket_count, socket_count);
	atomic_set(&gemini_da921x_uevent_listener_count, listener_count);
	atomic_set(&gemini_da921x_dual_modalias_stage, 19);
	return 0;
}
#endif

struct gemini_da921x_broadcast_observation {
	bool reject_listeners;
	int sockets;
	int listeners;
	int allocations;
	int broadcasts;
};

static int uevent_net_broadcast_untagged(struct kobj_uevent_env *env,
					 const char *action_string,
					 const char *devpath,
					 struct gemini_da921x_broadcast_observation *observation)
{
	struct sk_buff *skb = NULL;
	struct uevent_sock *ue_sk;
	int retval = 0;

	/* send netlink message */
	mutex_lock(&uevent_sock_mutex);
	list_for_each_entry(ue_sk, &uevent_sock_list, list) {
		struct sock *uevent_sock = ue_sk->sk;

		if (observation) {
			if (observation->sockets == 1024) {
				retval = -E2BIG;
				break;
			}
			observation->sockets++;
		}
		if (!netlink_has_listeners(uevent_sock, 1))
			continue;

		if (observation) {
			observation->listeners++;
			if (observation->reject_listeners) {
				retval = -EADDRINUSE;
				break;
			}
		}
		if (!skb) {
			retval = -ENOMEM;
			skb = alloc_uevent_skb(env, action_string, devpath);
			if (!skb)
				continue;
			if (observation)
				observation->allocations++;
		}

		if (observation)
			observation->broadcasts++;
		retval = netlink_broadcast(uevent_sock, skb_get(skb), 0, 1,
					   GFP_KERNEL);
		/* ENOBUFS should be handled in userspace */
		if (retval == -ENOBUFS || retval == -ESRCH)
			retval = 0;
	}
	mutex_unlock(&uevent_sock_mutex);
	consume_skb(skb);

	return retval;
}

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NO_LISTENER_DELIVERY_DIAGNOSTIC)
static int
gemini_da921x_no_listener(struct kobj_uevent_env *env,
			  const char *action_string,
			  const char *devpath)
{
	struct gemini_da921x_broadcast_observation observation = {
		.reject_listeners = true,
	};
	int retval;

	retval = uevent_net_broadcast_untagged(env, action_string, devpath,
					       &observation);
	if (retval || observation.listeners || observation.allocations ||
	    observation.broadcasts ||
	    observation.sockets != atomic_read(&gemini_da921x_uevent_socket_count))
		return retval ?: -EINVAL;

	atomic_set(&gemini_da921x_no_listener_socket_count,
		   observation.sockets);
	atomic_set(&gemini_da921x_no_listener_listener_count,
		   observation.listeners);
	atomic_set(&gemini_da921x_no_listener_allocation_count,
		   observation.allocations);
	atomic_set(&gemini_da921x_no_listener_broadcast_count,
		   observation.broadcasts);
	atomic_set(&gemini_da921x_no_listener_retval, retval);
	atomic_set(&gemini_da921x_dual_modalias_stage, 20);
	return 0;
}
#endif

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_BOUNDED_LISTENER_DIAGNOSTIC)
static int gemini_da921x_arm_bounded_listener(struct kobject *kobj)
{
	int ret = 0;

	mutex_lock(&gemini_da921x_bounded_listener_lock);
	if (gemini_da921x_bounded_listener_target ||
	    atomic_read(&gemini_da921x_bounded_listener_attempts))
		ret = -EALREADY;
	else
		gemini_da921x_bounded_listener_target = kobject_get(kobj);
	mutex_unlock(&gemini_da921x_bounded_listener_lock);
	return ret;
}

static int gemini_da921x_validate_bounded_listener(void)
{
	int baseline_sockets =
		atomic_read(&gemini_da921x_no_listener_socket_count);
	int sockets = atomic_read(&gemini_da921x_uevent_socket_count);
	int listeners = atomic_read(&gemini_da921x_uevent_listener_count);

	if (atomic_read(&gemini_da921x_bounded_listener_attempts) != 1 ||
	    baseline_sockets != 1 ||
	    atomic_read(&gemini_da921x_no_listener_listener_count) ||
	    atomic_read(&gemini_da921x_no_listener_allocation_count) ||
	    atomic_read(&gemini_da921x_no_listener_broadcast_count) ||
	    atomic_read(&gemini_da921x_no_listener_retval) ||
	    sockets != baseline_sockets || listeners != 1)
		return -EINVAL;

	atomic_set(&gemini_da921x_bounded_listener_baseline_socket_count,
		   baseline_sockets);
	atomic_set(&gemini_da921x_bounded_listener_socket_count, sockets);
	atomic_set(&gemini_da921x_bounded_listener_listener_count, listeners);
	atomic_set(&gemini_da921x_bounded_listener_broadcast_count, 0);
	atomic_set(&gemini_da921x_dual_modalias_stage, 21);
	return 0;
}

int gemini_da921x_trigger_bounded_listener(void)
{
	struct kobject *kobj;
	int ret;

	if (atomic_read(&gemini_da921x_dual_modalias_stage) != 20)
		return -EAGAIN;
	if (atomic_cmpxchg(&gemini_da921x_bounded_listener_attempts, 0, 1))
		return -EALREADY;

	mutex_lock(&gemini_da921x_bounded_listener_lock);
	kobj = gemini_da921x_bounded_listener_target;
	gemini_da921x_bounded_listener_target = NULL;
	mutex_unlock(&gemini_da921x_bounded_listener_lock);
	if (!kobj)
		return -ENODEV;

	ret = kobject_uevent(kobj, KOBJ_ADD);
	kobject_put(kobj);
	return ret;
}
#endif

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_SINGLE_MULTICAST_DIAGNOSTIC)
static int gemini_da921x_arm_single_multicast(struct kobject *kobj)
{
	int ret = 0;

	mutex_lock(&gemini_da921x_single_multicast_lock);
	if (gemini_da921x_single_multicast_target ||
	    atomic_read(&gemini_da921x_single_multicast_attempts))
		ret = -EALREADY;
	else
		gemini_da921x_single_multicast_target = kobject_get(kobj);
	mutex_unlock(&gemini_da921x_single_multicast_lock);
	return ret;
}

static int gemini_da921x_single_multicast(struct kobj_uevent_env *env,
					  const char *action_string,
					  const char *devpath)
{
	struct gemini_da921x_broadcast_observation observation = { };
	struct sk_buff *skb = NULL;
	struct uevent_sock *ue_sk;
	int baseline_sockets =
		atomic_read(&gemini_da921x_bounded_listener_socket_count);
	int retval = 0;

	mutex_lock(&uevent_sock_mutex);
	list_for_each_entry(ue_sk, &uevent_sock_list, list) {
		if (observation.sockets == 1024) {
			retval = -E2BIG;
			goto out_unlock;
		}
		observation.sockets++;
		if (netlink_has_listeners(ue_sk->sk, 1))
			observation.listeners++;
	}
	if (baseline_sockets != 1 || observation.sockets != baseline_sockets ||
	    observation.listeners != 1) {
		retval = -EINVAL;
		goto out_unlock;
	}

	list_for_each_entry(ue_sk, &uevent_sock_list, list) {
		if (!netlink_has_listeners(ue_sk->sk, 1))
			continue;
		skb = alloc_uevent_skb(env, action_string, devpath);
		if (!skb) {
			retval = -ENOMEM;
			goto out_unlock;
		}
		observation.allocations++;
		observation.broadcasts++;
		retval = netlink_broadcast(ue_sk->sk, skb_get(skb), 0, 1,
					   GFP_KERNEL);
		if (retval == -ENOBUFS || retval == -ESRCH)
			retval = 0;
		break;
	}

out_unlock:
	mutex_unlock(&uevent_sock_mutex);
	consume_skb(skb);
	atomic_set(&gemini_da921x_single_multicast_baseline_socket_count,
		   baseline_sockets);
	atomic_set(&gemini_da921x_single_multicast_socket_count,
		   observation.sockets);
	atomic_set(&gemini_da921x_single_multicast_listener_count,
		   observation.listeners);
	atomic_set(&gemini_da921x_single_multicast_allocation_count,
		   observation.allocations);
	atomic_set(&gemini_da921x_single_multicast_broadcast_count,
		   observation.broadcasts);
	atomic_set(&gemini_da921x_single_multicast_retval, retval);
	if (retval || observation.allocations != 1 ||
	    observation.broadcasts != 1)
		return retval ?: -EINVAL;

	atomic_set(&gemini_da921x_dual_modalias_stage, 22);
	return 0;
}

int gemini_da921x_trigger_single_multicast(void)
{
	struct kobject *kobj;
	int ret;

	if (atomic_read(&gemini_da921x_dual_modalias_stage) != 21)
		return -EAGAIN;
	if (atomic_cmpxchg(&gemini_da921x_single_multicast_attempts, 0, 1))
		return -EALREADY;

	mutex_lock(&gemini_da921x_single_multicast_lock);
	kobj = gemini_da921x_single_multicast_target;
	gemini_da921x_single_multicast_target = NULL;
	mutex_unlock(&gemini_da921x_single_multicast_lock);
	if (!kobj)
		return -ENODEV;

	ret = kobject_uevent(kobj, KOBJ_ADD);
	kobject_put(kobj);
	return ret;
}
#endif

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_UNTAGGED_DISPATCH_DIAGNOSTIC)
static int gemini_da921x_arm_untagged_dispatch(struct kobject *kobj)
{
	int ret = 0;

	mutex_lock(&gemini_da921x_untagged_dispatch_lock);
	if (gemini_da921x_untagged_dispatch_target ||
	    atomic_read(&gemini_da921x_untagged_dispatch_attempts))
		ret = -EALREADY;
	else
		gemini_da921x_untagged_dispatch_target = kobject_get(kobj);
	mutex_unlock(&gemini_da921x_untagged_dispatch_lock);
	return ret;
}

static int gemini_da921x_untagged_dispatch(struct kobj_uevent_env *env,
					   const char *action_string,
					  const char *devpath)
{
	struct gemini_da921x_broadcast_observation observation = { };
	int baseline_sockets =
		atomic_read(&gemini_da921x_single_multicast_socket_count);
	int retval;

	atomic_inc(&gemini_da921x_untagged_dispatch_entries);
	retval = uevent_net_broadcast_untagged(env, action_string, devpath,
					       &observation);
	atomic_inc(&gemini_da921x_untagged_dispatch_returns);
	atomic_set(&gemini_da921x_untagged_dispatch_baseline_socket_count,
		   baseline_sockets);
	atomic_set(&gemini_da921x_untagged_dispatch_socket_count,
		   observation.sockets);
	atomic_set(&gemini_da921x_untagged_dispatch_listener_count,
		   observation.listeners);
	atomic_set(&gemini_da921x_untagged_dispatch_allocation_count,
		   observation.allocations);
	atomic_set(&gemini_da921x_untagged_dispatch_broadcast_count,
		   observation.broadcasts);
	atomic_set(&gemini_da921x_untagged_dispatch_retval, retval);
	if (retval || baseline_sockets != 1 || observation.sockets != 1 ||
	    observation.listeners != 1 || observation.allocations != 1 ||
	    observation.broadcasts != 1 ||
	    atomic_read(&gemini_da921x_untagged_dispatch_entries) != 1 ||
	    atomic_read(&gemini_da921x_untagged_dispatch_returns) != 1)
		return retval ?: -EINVAL;

	atomic_set(&gemini_da921x_dual_modalias_stage, 23);
	return 0;
}

int gemini_da921x_trigger_untagged_dispatch(void)
{
	struct kobject *kobj;
	int ret;

	if (atomic_read(&gemini_da921x_dual_modalias_stage) != 22)
		return -EAGAIN;
	if (atomic_cmpxchg(&gemini_da921x_untagged_dispatch_attempts, 0, 1))
		return -EALREADY;

	mutex_lock(&gemini_da921x_untagged_dispatch_lock);
	kobj = gemini_da921x_untagged_dispatch_target;
	gemini_da921x_untagged_dispatch_target = NULL;
	mutex_unlock(&gemini_da921x_untagged_dispatch_lock);
	if (!kobj)
		return -ENODEV;

	ret = kobject_uevent(kobj, KOBJ_ADD);
	kobject_put(kobj);
	return ret;
}
#endif

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC)
static int gemini_da921x_arm_net_broadcast(struct kobject *kobj)
{
	int ret = 0;

	mutex_lock(&gemini_da921x_net_broadcast_lock);
	if (gemini_da921x_net_broadcast_target ||
	    gemini_da921x_net_broadcast_active_target ||
	    atomic_read(&gemini_da921x_net_broadcast_attempts))
		ret = -EALREADY;
	else
		gemini_da921x_net_broadcast_target = kobject_get(kobj);
	mutex_unlock(&gemini_da921x_net_broadcast_lock);
	return ret;
}

int gemini_da921x_trigger_net_broadcast(void)
{
	struct kobject *kobj;
	int ret;

	if (atomic_read(&gemini_da921x_dual_modalias_stage) != 23)
		return -EAGAIN;
	if (atomic_cmpxchg(&gemini_da921x_net_broadcast_attempts, 0, 1))
		return -EALREADY;

	mutex_lock(&gemini_da921x_net_broadcast_lock);
	kobj = gemini_da921x_net_broadcast_target;
	gemini_da921x_net_broadcast_target = NULL;
	mutex_unlock(&gemini_da921x_net_broadcast_lock);
	if (!kobj)
		return -ENODEV;

	ret = kobject_uevent(kobj, KOBJ_ADD);
	kobject_put(kobj);
	return ret;
}
#endif

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC)
static int gemini_da921x_arm_normal_fallthrough(struct kobject *kobj)
{
	int ret = 0;

	mutex_lock(&gemini_da921x_normal_fallthrough_lock);
	if (gemini_da921x_normal_fallthrough_target ||
	    gemini_da921x_normal_fallthrough_active_target ||
	    atomic_read(&gemini_da921x_normal_fallthrough_attempts))
		ret = -EALREADY;
	else
		gemini_da921x_normal_fallthrough_target = kobject_get(kobj);
	mutex_unlock(&gemini_da921x_normal_fallthrough_lock);
	return ret;
}

static bool gemini_da921x_prepare_normal_fallthrough(struct kobject *kobj)
{
	bool active;

	mutex_lock(&gemini_da921x_normal_fallthrough_lock);
	active = gemini_da921x_normal_fallthrough_active_target == kobj;
	mutex_unlock(&gemini_da921x_normal_fallthrough_lock);
	return active;
}

int gemini_da921x_trigger_normal_fallthrough(void)
{
	struct kobject *kobj;
	int ret;

	if (atomic_read(&gemini_da921x_dual_modalias_stage) != 24)
		return -EAGAIN;
	if (atomic_cmpxchg(&gemini_da921x_normal_fallthrough_attempts, 0, 1))
		return -EALREADY;

	mutex_lock(&gemini_da921x_normal_fallthrough_lock);
	kobj = gemini_da921x_normal_fallthrough_target;
	gemini_da921x_normal_fallthrough_target = NULL;
	gemini_da921x_normal_fallthrough_active_target = kobj;
	mutex_unlock(&gemini_da921x_normal_fallthrough_lock);
	if (!kobj)
		return -ENODEV;

	ret = kobject_uevent(kobj, KOBJ_ADD);
	mutex_lock(&gemini_da921x_normal_fallthrough_lock);
	gemini_da921x_normal_fallthrough_active_target = NULL;
	mutex_unlock(&gemini_da921x_normal_fallthrough_lock);
	atomic_inc(&gemini_da921x_normal_fallthrough_public_returns);
	if (!ret &&
	    (atomic_read(&gemini_da921x_normal_fallthrough_callsite_entries) != 1 ||
	     atomic_read(&gemini_da921x_normal_fallthrough_callsite_returns) != 1 ||
	     atomic_read(&gemini_da921x_dual_modalias_stage) != 25))
		ret = -EINVAL;
	atomic_set(&gemini_da921x_normal_fallthrough_retval, ret);
	kobject_put(kobj);
	return ret;
}
#endif

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
static bool gemini_da921x_natural_device_add_active(struct kobject *kobj)
{
	bool active;

	mutex_lock(&gemini_da921x_natural_device_add_lock);
	active = gemini_da921x_natural_device_add_active_target == kobj;
	mutex_unlock(&gemini_da921x_natural_device_add_lock);
	return active;
}

bool gemini_da921x_natural_device_add_begin(struct kobject *kobj)
{
	bool armed = false;

	mutex_lock(&gemini_da921x_natural_device_add_lock);
	if (!gemini_da921x_natural_device_add_active_target &&
	    !atomic_read(&gemini_da921x_natural_device_add_attempts) &&
	    !atomic_read(&gemini_da921x_dual_modalias_stage)) {
		gemini_da921x_natural_device_add_active_target = kobj;
		atomic_set(&gemini_da921x_natural_device_add_attempts, 1);
		atomic_set(&gemini_da921x_natural_device_add_register_entries, 1);
		armed = true;
	}
	mutex_unlock(&gemini_da921x_natural_device_add_lock);
	return armed;
}

void gemini_da921x_natural_device_add_end(struct kobject *kobj, int retval)
{
	bool active;

	mutex_lock(&gemini_da921x_natural_device_add_lock);
	active = gemini_da921x_natural_device_add_active_target == kobj;
	if (active)
		gemini_da921x_natural_device_add_active_target = NULL;
	mutex_unlock(&gemini_da921x_natural_device_add_lock);
	if (!active)
		return;

	atomic_inc(&gemini_da921x_natural_device_add_register_returns);
	atomic_set(&gemini_da921x_natural_device_add_register_retval, retval);
	if (retval || atomic_read(&gemini_da921x_dual_modalias_stage) != 20 ||
	    atomic_read(&gemini_da921x_no_listener_socket_count) != 1 ||
	    atomic_read(&gemini_da921x_no_listener_listener_count) ||
	    atomic_read(&gemini_da921x_no_listener_allocation_count) ||
	    atomic_read(&gemini_da921x_no_listener_broadcast_count) ||
	    atomic_read(&gemini_da921x_no_listener_retval) ||
	    atomic_read(&gemini_da921x_natural_device_add_attempts) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_register_entries) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_register_returns) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_callsite_entries) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_callsite_returns) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_public_returns) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_wrapper_entries) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_wrapper_returns) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_namespace_checks) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_untagged_routes) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_tagged_routes) ||
	    atomic_read(&gemini_da921x_natural_device_add_socket_count) != 1 ||
	    atomic_read(&gemini_da921x_natural_device_add_listener_count) ||
	    atomic_read(&gemini_da921x_natural_device_add_allocation_count) ||
	    atomic_read(&gemini_da921x_natural_device_add_broadcast_count) ||
	    atomic_read(&gemini_da921x_natural_device_add_uevent_retval))
		return;

	atomic_set(&gemini_da921x_dual_modalias_stage, 26);
}
#endif

static int uevent_net_broadcast_tagged(struct sock *usk,
				       struct kobj_uevent_env *env,
				       const char *action_string,
				       const char *devpath)
{
	struct user_namespace *owning_user_ns = sock_net(usk)->user_ns;
	struct sk_buff *skb = NULL;
	int ret = 0;

	skb = alloc_uevent_skb(env, action_string, devpath);
	if (!skb)
		return -ENOMEM;

	/* fix credentials */
	if (owning_user_ns != &init_user_ns) {
		struct netlink_skb_parms *parms = &NETLINK_CB(skb);
		kuid_t root_uid;
		kgid_t root_gid;

		/* fix uid */
		root_uid = make_kuid(owning_user_ns, 0);
		if (uid_valid(root_uid))
			parms->creds.uid = root_uid;

		/* fix gid */
		root_gid = make_kgid(owning_user_ns, 0);
		if (gid_valid(root_gid))
			parms->creds.gid = root_gid;
	}

	ret = netlink_broadcast(usk, skb, 0, 1, GFP_KERNEL);
	/* ENOBUFS should be handled in userspace */
	if (ret == -ENOBUFS || ret == -ESRCH)
		ret = 0;

	return ret;
}
#endif

static int kobject_uevent_net_broadcast(struct kobject *kobj,
					struct kobj_uevent_env *env,
					const char *action_string,
					const char *devpath)
{
	int ret = 0;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC)
	struct gemini_da921x_broadcast_observation observation = { };
	bool observe;

	mutex_lock(&gemini_da921x_net_broadcast_lock);
	observe = gemini_da921x_net_broadcast_active_target == kobj;
	mutex_unlock(&gemini_da921x_net_broadcast_lock);
	if (observe)
		atomic_inc(&gemini_da921x_net_broadcast_entries);
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
	bool observe_natural =
		gemini_da921x_natural_device_add_active(kobj);

	if (observe_natural)
		atomic_inc(&gemini_da921x_natural_device_add_wrapper_entries);
#endif

#ifdef CONFIG_NET
	const struct kobj_ns_type_operations *ops;
	const struct ns_common *ns = NULL;
	struct gemini_da921x_broadcast_observation *observation_ptr = NULL;

	ops = kobj_ns_ops(kobj);
	if (!ops && kobj->kset) {
		struct kobject *ksobj = &kobj->kset->kobj;

		if (ksobj->parent != NULL)
			ops = kobj_ns_ops(ksobj->parent);
	}

	/* kobjects currently only carry network namespace tags and they
	 * are the only tag relevant here since we want to decide which
	 * network namespaces to broadcast the uevent into.
	 */
	if (ops && ops->netlink_ns && kobj->ktype->namespace)
		if (ops->type == KOBJ_NS_TYPE_NET)
			ns = kobj->ktype->namespace(kobj);

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC)
	if (observe) {
		atomic_inc(&gemini_da921x_net_broadcast_namespace_checks);
		observation_ptr = &observation;
	}
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
	if (observe_natural) {
		atomic_inc(&gemini_da921x_natural_device_add_namespace_checks);
		observation_ptr = &observation;
	}
#endif
	if (!ns) {
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC)
		if (observe)
			atomic_inc(&gemini_da921x_net_broadcast_untagged_routes);
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
		if (observe_natural)
			atomic_inc(&gemini_da921x_natural_device_add_untagged_routes);
#endif
		ret = uevent_net_broadcast_untagged(env, action_string,
						    devpath, observation_ptr);
	} else {
		const struct net *net = container_of(ns, struct net, ns);

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC)
		if (observe)
			atomic_inc(&gemini_da921x_net_broadcast_tagged_routes);
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
		if (observe_natural)
			atomic_inc(&gemini_da921x_natural_device_add_tagged_routes);
#endif
		ret = uevent_net_broadcast_tagged(net->uevent_sock->sk, env,
						  action_string, devpath);
	}
#endif

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC)
	if (observe) {
		atomic_inc(&gemini_da921x_net_broadcast_returns);
		atomic_set(&gemini_da921x_net_broadcast_socket_count,
			   observation.sockets);
		atomic_set(&gemini_da921x_net_broadcast_listener_count,
			   observation.listeners);
		atomic_set(&gemini_da921x_net_broadcast_allocation_count,
			   observation.allocations);
		atomic_set(&gemini_da921x_net_broadcast_broadcast_count,
			   observation.broadcasts);
		atomic_set(&gemini_da921x_net_broadcast_retval, ret);
	}
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
	if (observe_natural) {
		atomic_inc(&gemini_da921x_natural_device_add_wrapper_returns);
		atomic_set(&gemini_da921x_natural_device_add_socket_count,
			   observation.sockets);
		atomic_set(&gemini_da921x_natural_device_add_listener_count,
			   observation.listeners);
		atomic_set(&gemini_da921x_natural_device_add_allocation_count,
			   observation.allocations);
		atomic_set(&gemini_da921x_natural_device_add_broadcast_count,
			   observation.broadcasts);
	}
#endif
	return ret;
}

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC)
static int gemini_da921x_net_broadcast(struct kobject *kobj,
				       struct kobj_uevent_env *env,
				       const char *action_string,
				       const char *devpath)
{
	int baseline_sockets =
		atomic_read(&gemini_da921x_untagged_dispatch_socket_count);
	int retval;

	atomic_set(&gemini_da921x_net_broadcast_baseline_socket_count,
		   baseline_sockets);
	mutex_lock(&gemini_da921x_net_broadcast_lock);
	gemini_da921x_net_broadcast_active_target = kobj;
	mutex_unlock(&gemini_da921x_net_broadcast_lock);
	retval = kobject_uevent_net_broadcast(kobj, env, action_string, devpath);
	mutex_lock(&gemini_da921x_net_broadcast_lock);
	gemini_da921x_net_broadcast_active_target = NULL;
	mutex_unlock(&gemini_da921x_net_broadcast_lock);

	if (retval || baseline_sockets != 1 ||
	    atomic_read(&gemini_da921x_net_broadcast_entries) != 1 ||
	    atomic_read(&gemini_da921x_net_broadcast_returns) != 1 ||
	    atomic_read(&gemini_da921x_net_broadcast_namespace_checks) != 1 ||
	    atomic_read(&gemini_da921x_net_broadcast_untagged_routes) != 1 ||
	    atomic_read(&gemini_da921x_net_broadcast_tagged_routes) != 0 ||
	    atomic_read(&gemini_da921x_net_broadcast_socket_count) != 1 ||
	    atomic_read(&gemini_da921x_net_broadcast_listener_count) != 1 ||
	    atomic_read(&gemini_da921x_net_broadcast_allocation_count) != 1 ||
	    atomic_read(&gemini_da921x_net_broadcast_broadcast_count) != 1)
		return retval ?: -EINVAL;

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC)
	retval = gemini_da921x_arm_normal_fallthrough(kobj);
	if (retval)
		return retval;
#endif

	atomic_set(&gemini_da921x_dual_modalias_stage, 24);
	return 0;
}
#endif

static void zap_modalias_env(struct kobj_uevent_env *env)
{
	static const char modalias_prefix[] = "MODALIAS=";
	size_t len;
	int i, j;

	for (i = 0; i < env->envp_idx;) {
		if (strncmp(env->envp[i], modalias_prefix,
			    sizeof(modalias_prefix) - 1)) {
			i++;
			continue;
		}

		len = strlen(env->envp[i]) + 1;

		if (i != env->envp_idx - 1) {
			/* @env->envp[] contains pointers to @env->buf[]
			 * with @env->buflen chars, and we are removing
			 * variable MODALIAS here pointed by @env->envp[i]
			 * with length @len as shown below:
			 *
			 * 0               @env->buf[]      @env->buflen
			 * ---------------------------------------------
			 * ^             ^              ^              ^
			 * |             |->   @len   <-| target block |
			 * @env->envp[0] @env->envp[i]  @env->envp[i + 1]
			 *
			 * so the "target block" indicated above is moved
			 * backward by @len, and its right size is
			 * @env->buflen - (@env->envp[i + 1] - @env->envp[0]).
			 */
			memmove(env->envp[i], env->envp[i + 1],
				env->buflen - (env->envp[i + 1] - env->envp[0]));

			for (j = i; j < env->envp_idx - 1; j++)
				env->envp[j] = env->envp[j + 1] - len;
		}

		env->envp_idx--;
		env->buflen -= len;
	}
}

static int gemini_da921x_pre_dispatch(struct kobject *kobj,
				      enum kobject_action action,
				      const char *subsystem,
				      const char *devpath,
				      struct kobj_uevent_env *env)
{
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_OF_MODALIAS_PRE_DISPATCH_SUPPRESSION_DIAGNOSTIC) || \
	IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_DUAL_MODALIAS_STATE_DIAGNOSTIC)
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_ROOT_PATH_CORRECTION_DIAGNOSTIC)
#define GEMINI_DA921X_EXPECTED_DEVPATH \
	"/devices/platform/1100e000.i2c/i2c-1/1-0068"
#define GEMINI_DA921X_EXPECTED_OF_FULLNAME \
	"OF_FULLNAME=/i2c@1100e000/regulator@68"
#else
#define GEMINI_DA921X_EXPECTED_DEVPATH \
	"/devices/platform/soc/1100e000.i2c/i2c-1/1-0068"
#define GEMINI_DA921X_EXPECTED_OF_FULLNAME \
	"OF_FULLNAME=/soc/i2c@1100e000/regulator@68"
#endif
	static const char expected_devpath[] =
		GEMINI_DA921X_EXPECTED_DEVPATH;
	static const char * const expected[] = {
		"ACTION=add",
		"DEVPATH=" GEMINI_DA921X_EXPECTED_DEVPATH,
		"SUBSYSTEM=i2c",
		"OF_NAME=regulator",
		GEMINI_DA921X_EXPECTED_OF_FULLNAME,
		"OF_COMPATIBLE_0=dlg,da9214-legacy",
		"OF_COMPATIBLE_N=1",
		"MODALIAS=of:NregulatorT(null)Cdlg,da9214-legacy",
#if !IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_OF_EVENT_LAYOUT_CORRECTION_DIAGNOSTIC)
		"MODALIAS=i2c:da9214-legacy",
#endif
	};
	const char *seqnum = NULL;
	const char *entry;
	int i;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_ENTRY_CLASSIFICATION_DIAGNOSTIC)
	unsigned int duplicate_mask = 0;
	unsigned int present_mask = 0;
	bool ordered = true;
	int ordered_prefix = 0;
	int seqnum_count = 0;
	int seqnum_index = -1;
	int unexpected_count = 0;
	int scan_count;
	int j;
#endif

	if (action != KOBJ_ADD || strcmp(subsystem, "i2c") ||
	    strcmp(kobject_name(kobj), "1-0068"))
		return 0;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC)
	atomic_set(&gemini_da921x_dual_modalias_stage, 1);
#endif
	if (strcmp(devpath, expected_devpath))
		return -EINVAL;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC)
	atomic_set(&gemini_da921x_dual_modalias_stage, 2);
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_ENVELOPE_STATE_DIAGNOSTIC)
	atomic_set(&gemini_da921x_dual_modalias_envp_idx, env->envp_idx);
	atomic_set(&gemini_da921x_dual_modalias_envp_capacity,
		   ARRAY_SIZE(env->envp));
	atomic_set(&gemini_da921x_dual_modalias_terminator_null,
		   env->envp_idx < ARRAY_SIZE(env->envp) ?
		   !env->envp[env->envp_idx] : -1);
	atomic_set(&gemini_da921x_dual_modalias_buflen, env->buflen);
	atomic_set(&gemini_da921x_dual_modalias_buf_capacity,
		   sizeof(env->buf));
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_ENTRY_CLASSIFICATION_DIAGNOSTIC)
	scan_count = clamp_t(int, env->envp_idx, 0, ARRAY_SIZE(env->envp));
	if (env->envp_idx > ARRAY_SIZE(env->envp))
		unexpected_count++;
	if (env->buflen > 0 && env->buflen <= sizeof(env->buf)) {
		for (i = 0; i < scan_count; i++) {
			size_t remaining;
			bool matched = false;

			entry = env->envp[i];
			if (!entry || entry < env->buf ||
			    entry >= env->buf + env->buflen) {
				unexpected_count++;
				ordered = false;
				continue;
			}
			remaining = env->buf + env->buflen - entry;
			if (strnlen(entry, remaining) == remaining) {
				unexpected_count++;
				ordered = false;
				continue;
			}
			for (j = 0; j < ARRAY_SIZE(expected); j++) {
				if (strcmp(entry, expected[j]))
					continue;
				if (present_mask & BIT(j))
					duplicate_mask |= BIT(j);
				present_mask |= BIT(j);
				matched = true;
				break;
			}
			if (!matched &&
			    !strncmp(entry, "SEQNUM=", sizeof("SEQNUM=") - 1)) {
				if (!seqnum_count)
					seqnum_index = i;
				seqnum_count++;
				matched = true;
			}
			if (!matched)
				unexpected_count++;
			if (ordered) {
				if (i < ARRAY_SIZE(expected) &&
				    !strcmp(entry, expected[i]))
					ordered_prefix++;
				else
					ordered = false;
			}
		}
	} else {
		unexpected_count = scan_count ? scan_count : 1;
	}
	atomic_set(&gemini_da921x_dual_modalias_present_mask, present_mask);
	atomic_set(&gemini_da921x_dual_modalias_duplicate_mask, duplicate_mask);
	atomic_set(&gemini_da921x_dual_modalias_ordered_prefix, ordered_prefix);
	atomic_set(&gemini_da921x_dual_modalias_seqnum_count, seqnum_count);
	atomic_set(&gemini_da921x_dual_modalias_seqnum_index, seqnum_index);
	atomic_set(&gemini_da921x_dual_modalias_unexpected_count,
		   unexpected_count);
#endif
	if (env->envp_idx != ARRAY_SIZE(expected) + 1 ||
	    env->envp_idx >= ARRAY_SIZE(env->envp) ||
	    env->buflen <= 0 || env->buflen > sizeof(env->buf) ||
	    env->envp[env->envp_idx])
		return -EINVAL;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC)
	atomic_set(&gemini_da921x_dual_modalias_stage, 3);
#endif

	for (i = 0; i < env->envp_idx; i++) {
		entry = env->envp[i];
		if (entry < env->buf || entry >= env->buf + env->buflen)
			return -EINVAL;
		if (!i) {
			if (entry != env->buf)
				return -EINVAL;
		} else if (entry != env->envp[i - 1] +
				   strlen(env->envp[i - 1]) + 1) {
			return -EINVAL;
		}
		if (i < ARRAY_SIZE(expected)) {
			if (strcmp(entry, expected[i]))
				return -EINVAL;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC)
			atomic_set(&gemini_da921x_dual_modalias_stage, 4 + i);
#endif
			continue;
		}
		if (strncmp(entry, "SEQNUM=", sizeof("SEQNUM=") - 1))
			return -EINVAL;
		seqnum = entry + sizeof("SEQNUM=") - 1;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC)
		atomic_set(&gemini_da921x_dual_modalias_stage, 13);
#endif
	}
	if (env->envp[env->envp_idx - 1] +
	    strlen(env->envp[env->envp_idx - 1]) + 1 !=
	    env->buf + env->buflen)
		return -EINVAL;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC)
	atomic_set(&gemini_da921x_dual_modalias_stage, 14);
#endif
	if (!seqnum || !*seqnum)
		return -EINVAL;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC)
	atomic_set(&gemini_da921x_dual_modalias_stage, 15);
#endif
	for (entry = seqnum; *entry; entry++)
		if (!isdigit(*entry))
			return -EINVAL;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC)
	atomic_set(&gemini_da921x_dual_modalias_stage, 16);
#endif

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_DUAL_MODALIAS_STATE_DIAGNOSTIC)
	atomic_set(&gemini_da921x_dual_modalias_state, 1);
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_VALIDATION_STAGE_DIAGNOSTIC)
	atomic_set(&gemini_da921x_dual_modalias_stage, 17);
#endif
#else
	pr_info("GEMINI_DA921X_DUAL_MODALIAS_PRE_DISPATCH full_env=validated envp_idx=%d buflen=%d of_modalias=present i2c_modalias=present seqnum=present transport=skipped return=success cleanup=normal\n",
		env->envp_idx, env->buflen);
#endif
	return 1;
#else
	return 0;
#endif
}

/**
 * kobject_uevent_env - send an uevent with environmental data
 *
 * @kobj: struct kobject that the action is happening to
 * @action: action that is happening
 * @envp_ext: pointer to environmental data
 *
 * Returns 0 if kobject_uevent_env() is completed with success or the
 * corresponding error when it fails.
 */
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
		       char *envp_ext[])
{
	struct kobj_uevent_env *env;
	const char *action_string = kobject_actions[action];
	const char *devpath = NULL;
	const char *subsystem;
	struct kobject *top_kobj;
	struct kset *kset;
	const struct kset_uevent_ops *uevent_ops;
	int i = 0;
	int retval = 0;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_BOUNDED_LISTENER_DIAGNOSTIC)
	int gemini_da921x_previous_stage = -1;
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC)
	bool gemini_da921x_normal_fallthrough = false;
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
	bool gemini_da921x_natural_device_add = false;
#endif

	/*
	 * Mark "remove" event done regardless of result, for some subsystems
	 * do not want to re-trigger "remove" event via automatic cleanup.
	 */
	if (action == KOBJ_REMOVE)
		kobj->state_remove_uevent_sent = 1;

	pr_debug("kobject: '%s' (%p): %s\n",
		 kobject_name(kobj), kobj, __func__);

	/* search the kset we belong to */
	top_kobj = kobj;
	while (!top_kobj->kset && top_kobj->parent)
		top_kobj = top_kobj->parent;

	if (!top_kobj->kset) {
		pr_debug("kobject: '%s' (%p): %s: attempted to send uevent "
			 "without kset!\n", kobject_name(kobj), kobj,
			 __func__);
		return -EINVAL;
	}

	kset = top_kobj->kset;
	uevent_ops = kset->uevent_ops;

	/* skip the event, if uevent_suppress is set*/
	if (kobj->uevent_suppress) {
		pr_debug("kobject: '%s' (%p): %s: uevent_suppress "
				 "caused the event to drop!\n",
				 kobject_name(kobj), kobj, __func__);
		return 0;
	}
	/* skip the event, if the filter returns zero. */
	if (uevent_ops && uevent_ops->filter)
		if (!uevent_ops->filter(kobj)) {
			pr_debug("kobject: '%s' (%p): %s: filter function "
				 "caused the event to drop!\n",
				 kobject_name(kobj), kobj, __func__);
			return 0;
		}

	/* originating subsystem */
	if (uevent_ops && uevent_ops->name)
		subsystem = uevent_ops->name(kobj);
	else
		subsystem = kobject_name(&kset->kobj);
	if (!subsystem) {
		pr_debug("kobject: '%s' (%p): %s: unset subsystem caused the "
			 "event to drop!\n", kobject_name(kobj), kobj,
			 __func__);
		return 0;
	}

	/* environment buffer */
	env = kzalloc_obj(struct kobj_uevent_env);
	if (!env)
		return -ENOMEM;

	/* complete object path */
	devpath = kobject_get_path(kobj, GFP_KERNEL);
	if (!devpath) {
		retval = -ENOENT;
		goto exit;
	}

	/* default keys */
	retval = add_uevent_var(env, "ACTION=%s", action_string);
	if (retval)
		goto exit;
	retval = add_uevent_var(env, "DEVPATH=%s", devpath);
	if (retval)
		goto exit;
	retval = add_uevent_var(env, "SUBSYSTEM=%s", subsystem);
	if (retval)
		goto exit;

	/* keys passed in from the caller */
	if (envp_ext) {
		for (i = 0; envp_ext[i]; i++) {
			retval = add_uevent_var(env, "%s", envp_ext[i]);
			if (retval)
				goto exit;
		}
	}

	/* let the kset specific function add its stuff */
	if (uevent_ops && uevent_ops->uevent) {
		retval = uevent_ops->uevent(kobj, env);
		if (retval) {
			pr_debug("kobject: '%s' (%p): %s: uevent() returned "
				 "%d\n", kobject_name(kobj), kobj,
				 __func__, retval);
			goto exit;
		}
	}

	switch (action) {
	case KOBJ_ADD:
		/*
		 * Mark "add" event so we can make sure we deliver "remove"
		 * event to userspace during automatic cleanup. If
		 * the object did send an "add" event, "remove" will
		 * automatically generated by the core, if not already done
		 * by the caller.
		 */
		kobj->state_add_uevent_sent = 1;
		break;

	case KOBJ_UNBIND:
		zap_modalias_env(env);
		break;

	default:
		break;
	}

	/* we will send an event, so request a new sequence number */
	retval = add_uevent_var(env, "SEQNUM=%llu",
				atomic64_inc_return(&uevent_seqnum));
	if (retval)
		goto exit;

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_BOUNDED_LISTENER_DIAGNOSTIC)
	gemini_da921x_previous_stage =
		atomic_read(&gemini_da921x_dual_modalias_stage);
#endif
	retval = gemini_da921x_pre_dispatch(kobj, action, subsystem, devpath, env);
	if (retval < 0)
		goto exit;
	if (retval) {
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NETLINK_SERIALIZATION_DIAGNOSTIC)
		retval = gemini_da921x_serialize_uevent(env, action_string,
							devpath);
		if (retval)
			goto exit;
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_LISTENER_DISCOVERY_DIAGNOSTIC)
		retval = gemini_da921x_discover_uevent_listeners();
		if (retval)
			goto exit;
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_BOUNDED_LISTENER_DIAGNOSTIC)
		if (gemini_da921x_previous_stage >
		    21 + IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_UNTAGGED_DISPATCH_DIAGNOSTIC) +
		    IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC) +
		    IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC)) {
			atomic_set(&gemini_da921x_dual_modalias_stage,
				   gemini_da921x_previous_stage);
			retval = -EALREADY;
			goto exit;
		}
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_BOUNDED_LISTENER_DIAGNOSTIC)
		if (gemini_da921x_previous_stage < 20) {
			retval = gemini_da921x_no_listener(env, action_string,
							   devpath);
			if (retval)
				goto exit;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
			if (gemini_da921x_natural_device_add_active(kobj))
				gemini_da921x_natural_device_add = true;
			else
#endif
			{
			retval = gemini_da921x_arm_bounded_listener(kobj);
			if (retval)
				goto exit;
			}
		} else if (gemini_da921x_previous_stage == 20) {
			retval = gemini_da921x_validate_bounded_listener();
			if (retval)
				goto exit;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_SINGLE_MULTICAST_DIAGNOSTIC)
			retval = gemini_da921x_arm_single_multicast(kobj);
			if (retval)
				goto exit;
		} else if (gemini_da921x_previous_stage == 21) {
			retval = gemini_da921x_single_multicast(env,
								action_string,
								devpath);
			if (retval)
				goto exit;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_UNTAGGED_DISPATCH_DIAGNOSTIC)
			retval = gemini_da921x_arm_untagged_dispatch(kobj);
			if (retval)
				goto exit;
		} else if (gemini_da921x_previous_stage == 22) {
			retval = gemini_da921x_untagged_dispatch(env,
								 action_string,
								 devpath);
			if (retval)
				goto exit;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NET_BROADCAST_DIAGNOSTIC)
			retval = gemini_da921x_arm_net_broadcast(kobj);
			if (retval)
				goto exit;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC)
		} else if (gemini_da921x_previous_stage == 23) {
#else
		} else {
#endif
			retval = gemini_da921x_net_broadcast(kobj, env,
							     action_string, devpath);
			if (retval)
				goto exit;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC)
		} else {
			if (!gemini_da921x_prepare_normal_fallthrough(kobj)) {
				retval = -EINVAL;
				goto exit;
			}
			gemini_da921x_normal_fallthrough = true;
#endif
#endif
#endif
#endif
		}
#elif IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NO_LISTENER_DELIVERY_DIAGNOSTIC)
		retval = gemini_da921x_no_listener(env, action_string, devpath);
		if (retval)
			goto exit;
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC)
		if (!gemini_da921x_normal_fallthrough &&
		    !gemini_da921x_natural_device_add) {
#endif
			/* The exact target skb is consumed above; never broadcast it from
			 * this experiment-only path.
			 */
			retval = 0;
			goto exit;
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC)
		}
#endif
	}

#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC)
	if (gemini_da921x_normal_fallthrough)
		atomic_inc(&gemini_da921x_normal_fallthrough_callsite_entries);
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
	if (gemini_da921x_natural_device_add)
		atomic_inc(&gemini_da921x_natural_device_add_callsite_entries);
#endif
	retval = kobject_uevent_net_broadcast(kobj, env, action_string,
					      devpath);
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
	if (gemini_da921x_natural_device_add) {
		atomic_inc(&gemini_da921x_natural_device_add_callsite_returns);
		atomic_set(&gemini_da921x_natural_device_add_uevent_retval,
			   retval);
	}
#endif
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_UEVENT_NORMAL_FALLTHROUGH_DIAGNOSTIC)
	if (gemini_da921x_normal_fallthrough) {
		atomic_inc(&gemini_da921x_normal_fallthrough_callsite_returns);
		if (!retval &&
		    atomic_read(&gemini_da921x_normal_fallthrough_callsite_entries) == 1 &&
		    atomic_read(&gemini_da921x_normal_fallthrough_callsite_returns) == 1)
			atomic_set(&gemini_da921x_dual_modalias_stage, 25);
	}
#endif

#ifdef CONFIG_UEVENT_HELPER
	/* call uevent_helper, usually only enabled during early boot */
	if (uevent_helper[0] && !kobj_usermode_filter(kobj)) {
		struct subprocess_info *info;

		retval = add_uevent_var(env, "HOME=/");
		if (retval)
			goto exit;
		retval = add_uevent_var(env,
					"PATH=/sbin:/bin:/usr/sbin:/usr/bin");
		if (retval)
			goto exit;
		retval = init_uevent_argv(env, subsystem);
		if (retval)
			goto exit;

		retval = -ENOMEM;
		info = call_usermodehelper_setup(env->argv[0], env->argv,
						 env->envp, GFP_KERNEL,
						 NULL, cleanup_uevent_env, env);
		if (info) {
			retval = call_usermodehelper_exec(info, UMH_NO_WAIT);
			env = NULL;	/* freed by cleanup_uevent_env */
		}
	}
#endif

exit:
#if IS_ENABLED(CONFIG_I2C_GEMINI_DA921X_NATURAL_DEVICE_ADD_DIAGNOSTIC)
	if (gemini_da921x_natural_device_add)
		atomic_inc(&gemini_da921x_natural_device_add_public_returns);
#endif
	kfree(devpath);
	kfree(env);
	return retval;
}
EXPORT_SYMBOL_GPL(kobject_uevent_env);

/**
 * kobject_uevent - notify userspace by sending an uevent
 *
 * @kobj: struct kobject that the action is happening to
 * @action: action that is happening
 *
 * Returns 0 if kobject_uevent() is completed with success or the
 * corresponding error when it fails.
 */
int kobject_uevent(struct kobject *kobj, enum kobject_action action)
{
	return kobject_uevent_env(kobj, action, NULL);
}
EXPORT_SYMBOL_GPL(kobject_uevent);

/**
 * add_uevent_var - add key value string to the environment buffer
 * @env: environment buffer structure
 * @format: printf format for the key=value pair
 *
 * Returns 0 if environment variable was added successfully or -ENOMEM
 * if no space was available.
 */
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...)
{
	va_list args;
	int len;

	if (env->envp_idx >= ARRAY_SIZE(env->envp)) {
		WARN(1, KERN_ERR "add_uevent_var: too many keys\n");
		return -ENOMEM;
	}

	va_start(args, format);
	len = vsnprintf(&env->buf[env->buflen],
			sizeof(env->buf) - env->buflen,
			format, args);
	va_end(args);

	if (len >= (sizeof(env->buf) - env->buflen)) {
		WARN(1, KERN_ERR "add_uevent_var: buffer size too small\n");
		return -ENOMEM;
	}

	env->envp[env->envp_idx++] = &env->buf[env->buflen];
	env->buflen += len + 1;
	return 0;
}
EXPORT_SYMBOL_GPL(add_uevent_var);

#if defined(CONFIG_NET)
static int uevent_net_broadcast(struct sock *usk, struct sk_buff *skb,
				struct netlink_ext_ack *extack)
{
	/* u64 to chars: 2^64 - 1 = 21 chars */
	char buf[sizeof("SEQNUM=") + 21];
	struct sk_buff *skbc;
	int ret;

	/* bump and prepare sequence number */
	ret = snprintf(buf, sizeof(buf), "SEQNUM=%llu",
		       atomic64_inc_return(&uevent_seqnum));
	if (ret < 0 || (size_t)ret >= sizeof(buf))
		return -ENOMEM;
	ret++;

	/* verify message does not overflow */
	if ((skb->len + ret) > UEVENT_BUFFER_SIZE) {
		NL_SET_ERR_MSG(extack, "uevent message too big");
		return -EINVAL;
	}

	/* copy skb and extend to accommodate sequence number */
	skbc = skb_copy_expand(skb, 0, ret, GFP_KERNEL);
	if (!skbc)
		return -ENOMEM;

	/* append sequence number */
	skb_put_data(skbc, buf, ret);

	/* remove msg header */
	skb_pull(skbc, NLMSG_HDRLEN);

	/* set portid 0 to inform userspace message comes from kernel */
	NETLINK_CB(skbc).portid = 0;
	NETLINK_CB(skbc).dst_group = 1;

	ret = netlink_broadcast(usk, skbc, 0, 1, GFP_KERNEL);
	/* ENOBUFS should be handled in userspace */
	if (ret == -ENOBUFS || ret == -ESRCH)
		ret = 0;

	return ret;
}

static int uevent_net_rcv_skb(struct sk_buff *skb, struct nlmsghdr *nlh,
			      struct netlink_ext_ack *extack)
{
	struct net *net;
	int ret;

	if (!nlmsg_data(nlh))
		return -EINVAL;

	/*
	 * Verify that we are allowed to send messages to the target
	 * network namespace. The caller must have CAP_SYS_ADMIN in the
	 * owning user namespace of the target network namespace.
	 */
	net = sock_net(NETLINK_CB(skb).sk);
	if (!netlink_ns_capable(skb, net->user_ns, CAP_SYS_ADMIN)) {
		NL_SET_ERR_MSG(extack, "missing CAP_SYS_ADMIN capability");
		return -EPERM;
	}

	ret = uevent_net_broadcast(net->uevent_sock->sk, skb, extack);

	return ret;
}

static void uevent_net_rcv(struct sk_buff *skb)
{
	netlink_rcv_skb(skb, &uevent_net_rcv_skb);
}

static int uevent_net_init(struct net *net)
{
	struct uevent_sock *ue_sk;
	struct netlink_kernel_cfg cfg = {
		.groups	= 1,
		.input = uevent_net_rcv,
		.flags	= NL_CFG_F_NONROOT_RECV
	};

	ue_sk = kzalloc_obj(*ue_sk);
	if (!ue_sk)
		return -ENOMEM;

	ue_sk->sk = netlink_kernel_create(net, NETLINK_KOBJECT_UEVENT, &cfg);
	if (!ue_sk->sk) {
		pr_err("kobject_uevent: unable to create netlink socket!\n");
		kfree(ue_sk);
		return -ENODEV;
	}

	net->uevent_sock = ue_sk;

	/* Restrict uevents to initial user namespace. */
	if (sock_net(ue_sk->sk)->user_ns == &init_user_ns) {
		mutex_lock(&uevent_sock_mutex);
		list_add_tail(&ue_sk->list, &uevent_sock_list);
		mutex_unlock(&uevent_sock_mutex);
	}

	return 0;
}

static void uevent_net_exit(struct net *net)
{
	struct uevent_sock *ue_sk = net->uevent_sock;

	if (sock_net(ue_sk->sk)->user_ns == &init_user_ns) {
		mutex_lock(&uevent_sock_mutex);
		list_del(&ue_sk->list);
		mutex_unlock(&uevent_sock_mutex);
	}

	netlink_kernel_release(ue_sk->sk);
	kfree(ue_sk);
}

static struct pernet_operations uevent_net_ops = {
	.init	= uevent_net_init,
	.exit	= uevent_net_exit,
};

static int __init kobject_uevent_init(void)
{
	return register_pernet_subsys(&uevent_net_ops);
}


postcore_initcall(kobject_uevent_init);
#endif

#ifdef CONFIG_UEVENT_HELPER
static const struct ctl_table uevent_helper_sysctl_table[] = {
	{
		.procname	= "hotplug",
		.data		= &uevent_helper,
		.maxlen		= UEVENT_HELPER_PATH_LEN,
		.mode		= 0644,
		.proc_handler	= proc_dostring,
	},
};

static int __init init_uevent_helper_sysctl(void)
{
	register_sysctl_init("kernel", uevent_helper_sysctl_table);
	return 0;
}

postcore_initcall(init_uevent_helper_sysctl);
#endif
