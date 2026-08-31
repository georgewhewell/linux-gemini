// SPDX-License-Identifier: GPL-2.0-only
/* Read target-local architected cache geometry without changing cache state. */

#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/sysfs.h>

#include <asm/barrier.h>
#include <asm/sysreg.h>

#define MT6797_CSSELR_L1_DATA		0
#define MT6797_CSSELR_L1_INSTRUCTION	1
#define MT6797_CSSELR_L2_UNIFIED	2

struct mt6797_cache_sample {
	u64 midr;
	u64 mpidr;
	u64 clidr;
	u64 ctr;
	u64 mmfr2;
	u64 ccsidr_l1d;
	u64 ccsidr_l1i;
	u64 ccsidr_l2;
};

static struct kobject *mt6797_cache_kobj;

static u64 mt6797_cache_read_ccsidr(u64 selector)
{
	unsigned long flags;
	u64 saved;
	u64 value;

	local_irq_save(flags);
	saved = read_sysreg(csselr_el1);
	write_sysreg(selector, csselr_el1);
	isb();
	value = read_sysreg(ccsidr_el1);
	write_sysreg(saved, csselr_el1);
	isb();
	local_irq_restore(flags);

	return value;
}

static void mt6797_cache_sample_local(void *data)
{
	struct mt6797_cache_sample *sample = data;

	sample->midr = read_sysreg(midr_el1);
	sample->mpidr = read_sysreg(mpidr_el1);
	sample->clidr = read_sysreg(clidr_el1);
	sample->ctr = read_sysreg(ctr_el0);
	sample->mmfr2 = read_sysreg(id_aa64mmfr2_el1);
	sample->ccsidr_l1d =
		mt6797_cache_read_ccsidr(MT6797_CSSELR_L1_DATA);
	sample->ccsidr_l1i =
		mt6797_cache_read_ccsidr(MT6797_CSSELR_L1_INSTRUCTION);
	sample->ccsidr_l2 =
		mt6797_cache_read_ccsidr(MT6797_CSSELR_L2_UNIFIED);
}

static ssize_t status_show(struct kobject *kobj,
			   struct kobj_attribute *attribute, char *buf)
{
	ssize_t count = 0;
	unsigned int cpu;

	guard(cpus_read_lock)();
	for_each_possible_cpu(cpu) {
		struct mt6797_cache_sample sample = { };
		int ret;

		if (!cpu_online(cpu)) {
			count += sysfs_emit_at(buf, count, "cpu%u offline\n", cpu);
			continue;
		}

		ret = smp_call_function_single(cpu, mt6797_cache_sample_local,
					       &sample, true);
		if (ret) {
			count += sysfs_emit_at(buf, count, "cpu%u error=%d\n",
					       cpu, ret);
			continue;
		}

		count += sysfs_emit_at(buf, count,
			"cpu%u midr=%016llx mpidr=%016llx clidr=%016llx "
			"ctr=%016llx mmfr2=%016llx ccsidr_l1d=%016llx "
			"ccsidr_l1i=%016llx ccsidr_l2=%016llx\n",
			cpu, sample.midr, sample.mpidr, sample.clidr,
			sample.ctr, sample.mmfr2, sample.ccsidr_l1d,
			sample.ccsidr_l1i, sample.ccsidr_l2);
	}

	return count;
}

static struct kobj_attribute status_attribute = __ATTR_RO(status);

static int __init mt6797_cache_observer_init(void)
{
	int ret;

	if (!of_machine_is_compatible("mediatek,mt6797"))
		return -ENODEV;

	mt6797_cache_kobj = kobject_create_and_add("mt6797-cache", kernel_kobj);
	if (!mt6797_cache_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(mt6797_cache_kobj, &status_attribute.attr);
	if (ret) {
		kobject_put(mt6797_cache_kobj);
		mt6797_cache_kobj = NULL;
		return ret;
	}

	pr_info("mt6797-cache-observer: target-local raw cache geometry ready\n");
	return 0;
}
module_init(mt6797_cache_observer_init);

static void __exit mt6797_cache_observer_exit(void)
{
	if (!mt6797_cache_kobj)
		return;
	sysfs_remove_file(mt6797_cache_kobj, &status_attribute.attr);
	kobject_put(mt6797_cache_kobj);
}
module_exit(mt6797_cache_observer_exit);

MODULE_DESCRIPTION("MT6797 target-local cache geometry observer");
MODULE_LICENSE("GPL");
