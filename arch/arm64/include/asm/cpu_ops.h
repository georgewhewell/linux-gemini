/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 ARM Ltd.
 */
#ifndef __ASM_CPU_OPS_H
#define __ASM_CPU_OPS_H

#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/threads.h>

struct cpu_up_rollback_trace;

/**
 * struct cpu_operations - Callback operations for hotplugging CPUs.
 *
 * @name:	Name of the property as appears in a devicetree cpu node's
 *		enable-method property. On systems booting with ACPI, @name
 *		identifies the struct cpu_operations entry corresponding to
 *		the boot protocol specified in the ACPI MADT table.
 * @cpu_init:	Reads any data necessary for a specific enable-method for a
 *		proposed logical id.
 * @cpu_prepare: Early one-time preparation step for a cpu. If there is a
 *		mechanism for doing so, tests whether it is possible to boot
 *		the given CPU.
 * @cpu_up_preflight: Optional public CPU-up admission check before CPU-map
 *		locking or generic CPUHP work.
 * @cpu_up_validate: Optional leaf-only CPU-up admission check for public and
 *		direct internal paths. Must not sleep or acquire CPU-map, CPUHP, or
 *		outer transition locks.
 * @cpu_up_rollback: Optional controller-side publication before generic CPUHP
 *		outer rollback. It receives the failing callback state, result, and trace.
 * @cpu_up_secondary_complete: Optional controller callback immediately after
 *		a successful architecture secondary completion.
 * @cpu_up_complete: Optional controller callback after the full requested
 *		generic CPUHP target completes.
 * @cpu_boot:	Boots a cpu into the kernel.
 * @cpu_postboot: Optionally, perform any post-boot cleanup or necessary
 *		synchronisation. Called from the cpu being booted.
 * @cpu_can_disable: Determines whether a CPU can be disabled based on
 *		mechanism-specific information.
 * @cpu_disable: Prepares a cpu to die. May fail for some mechanism-specific
 * 		reason, which will cause the hot unplug to be aborted. Called
 * 		from the cpu to be killed.
 * @cpu_die:	Makes a cpu leave the kernel. Must not fail. Called from the
 *		cpu being killed.
 * @cpu_kill:  Ensures a cpu has left the kernel. Called from another cpu.
 */
struct cpu_operations {
	const char	*name;
	int		(*cpu_init)(unsigned int);
	int		(*cpu_prepare)(unsigned int);
	int		(*cpu_up_preflight)(unsigned int cpu,
					    enum cpuhp_state target);
	int		(*cpu_up_validate)(unsigned int cpu, int tasks_frozen,
					   enum cpuhp_state target);
	int		(*cpu_up_rollback)(unsigned int cpu, enum cpuhp_state state,
					   int error, const struct cpu_up_rollback_trace *trace);
	int		(*cpu_up_secondary_complete)(unsigned int cpu);
	int		(*cpu_up_complete)(unsigned int cpu,
					   enum cpuhp_state target);
	int		(*cpu_boot)(unsigned int);
	void		(*cpu_postboot)(void);
#ifdef CONFIG_HOTPLUG_CPU
	bool		(*cpu_can_disable)(unsigned int cpu);
	int		(*cpu_disable)(unsigned int cpu);
	void		(*cpu_die)(unsigned int cpu);
	int		(*cpu_kill)(unsigned int cpu);
#endif
};

int __init init_cpu_ops(int cpu);
extern const struct cpu_operations *get_cpu_ops(int cpu);

#ifdef CONFIG_ARM64_MT6797_A72_CAPABILITY_PROFILE
void __init mt6797_activate_a72_capability_profile(void);
#endif

static inline void __init init_bootcpu_ops(void)
{
	init_cpu_ops(0);
}

#endif /* ifndef __ASM_CPU_OPS_H */
