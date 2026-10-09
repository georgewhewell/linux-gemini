// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 native compound CCI clock and shared CPU/SRAM regulator consumer. */
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/devfreq.h>
#include <linux/devfreq-governor.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/soc/mediatek/mt6797-cci-policy.h>
#include <linux/soc/mediatek/mt6797-cci.h>
#include <linux/soc/mediatek/mt6797-a72-power.h>
#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>
#include <linux/workqueue.h>

struct mt6797_cci {
	struct device *dev;
	struct devfreq *devfreq;
	struct devfreq_dev_profile profile;
	struct regulator *supply;
	struct clk *clock;
	struct clk *cpu_clock[3];
	struct device *cpu_dev[3];
	cpumask_t cpus[3];
	struct mt6797_dvfsp_handoff *handoff;
	struct mt6797_cci_state state;
	struct delayed_work work;
	struct notifier_block frequency_nb;
	struct notifier_block policy_nb;
	struct hlist_node hotplug_node;
	int hotplug_state;
	/* Nonnegative only while the serialized A72 pre-raise is in progress. */
	int a72_target;
	bool stopping;
	bool suspended;
};

static DEFINE_MUTEX(mt6797_cci_owner_lock);
static struct mt6797_cci *mt6797_cci_owner;

static int mt6797_cci_get_hz(void *context, u32 *hz)
{
	struct mt6797_cci *cci = context;
	unsigned long rate = clk_get_rate(cci->clock);

	if (!rate || rate > U32_MAX)
		return -EIO;
	*hz = rate;
	return 0;
}

static int mt6797_cci_get_uv(void *context, u32 *uv)
{
	struct mt6797_cci *cci = context;
	int ret = regulator_get_voltage(cci->supply);

	if (ret < 0)
		return ret;
	*uv = ret;
	return 0;
}

static int mt6797_cci_set_hz(void *context, u32 hz)
{
	struct mt6797_cci *cci = context;

	/* The native provider owns PLL, divider, fallback clock and settling. */
	return clk_set_rate(cci->clock, hz);
}

static int mt6797_cci_vote_uv(void *context, u32 uv)
{
	struct mt6797_cci *cci = context;

	return regulator_set_voltage(cci->supply, uv, 1200000);
}

static void mt6797_cci_fence(void *context)
{
	struct mt6797_cci *cci = context;

	dev_err(cci->dev, "fencing CPU DVFS: CCI state %u Hz/%u uV no longer matches hardware\n",
		cci->state.hz, cci->state.vote_uv);
	mt6797_dvfsp_handoff_rail_fault(cci->handoff);
}

static const struct mt6797_cci_io mt6797_cci_io = {
	.get_hz = mt6797_cci_get_hz,
	.get_uv = mt6797_cci_get_uv,
	.set_hz = mt6797_cci_set_hz,
	.vote_uv = mt6797_cci_vote_uv,
	.fence = mt6797_cci_fence,
};

/* Caller holds the CPU hotplug read lock; A72 deliberately has no OPP table. */
static int mt6797_cci_sample_policy(struct mt6797_cci *cci)
{
	struct mt6797_cci_cpu cpus[3] = { };
	struct dev_pm_opp *opp;
	unsigned long hz;
	unsigned int i;

	for (i = 0; i < 3; i++) {
		if (!cpumask_intersects(&cci->cpus[i], cpu_online_mask))
			continue;
		hz = clk_get_rate(cci->cpu_clock[i]);
		if (!hz || hz > U32_MAX)
			return -EAGAIN;
		cpus[i].hz = hz;
		cpus[i].available = true;
		if (i == 2)
			continue;
		/* cpufreq-dt can register later; avoid noisy lookups while absent. */
		if (dev_pm_opp_get_opp_count(cci->cpu_dev[i]) <= 0)
			return -EAGAIN;
		opp = dev_pm_opp_find_freq_ceil(cci->cpu_dev[i], &hz);
		if (IS_ERR(opp))
			return -EAGAIN;
		cpus[i].uv = dev_pm_opp_get_voltage(opp);
		dev_pm_opp_put(opp);
	}
	return mt6797_cci_select(cpus);
}

static int mt6797_cci_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct mt6797_cci *cci = dev_get_drvdata(dev);
	int selected, requested, ret;
	bool cpus_locked = false;

	if (READ_ONCE(cci->stopping) || READ_ONCE(cci->suspended))
		return -EBUSY;
	if (READ_ONCE(cci->state.faulted))
		return -EHOSTDOWN;
	if (cci->a72_target >= 0) {
		/* The A72 owner excludes MP2 power changes for this transaction. */
		selected = cci->a72_target;
	} else {
		/* Do not wait for a CPUHP writer while holding devfreq->lock. */
		if (!cpus_read_trylock())
			return -EAGAIN;
		cpus_locked = true;
		selected = mt6797_cci_sample_policy(cci);
	}
	if (selected < 0) {
		ret = selected;
		goto out;
	}
	/* Re-sample after the governor: CPU transitions may have intervened. */
	for (requested = 0; requested < MT6797_CCI_OPP_COUNT; requested++)
		if (*freq == mt6797_cci_opps[requested].hz)
			break;
	if (requested == MT6797_CCI_OPP_COUNT) {
		ret = -EINVAL;
		goto out;
	}
	if (requested < selected) {
		ret = -EAGAIN;
		goto out;
	}
	ret = regulator_is_supported_voltage(cci->supply,
					     mt6797_cci_opps[requested].uv, 1200000);
	if (ret <= 0) {
		ret = ret ? ret : -ERANGE;
		goto out;
	}
	selected = requested;
	ret = mt6797_cci_transition(&cci->state, &mt6797_cci_io, cci, selected);
out:
	if (cpus_locked)
		cpus_read_unlock();
	return ret;
}

static int mt6797_cci_current(struct device *dev, unsigned long *freq)
{
	struct mt6797_cci *cci = dev_get_drvdata(dev);
	u32 hz;
	int ret = mt6797_cci_get_hz(cci, &hz);

	if (!ret)
		*freq = hz;
	return ret;
}

static int mt6797_cci_governor_target(struct devfreq *devfreq, unsigned long *freq)
{
	struct mt6797_cci *cci = devfreq->data;
	unsigned long minimum, maximum;
	int selected;

	if (cci->a72_target >= 0) {
		selected = cci->a72_target;
	} else {
		/* Do not wait for a CPUHP writer while holding devfreq->lock. */
		if (!cpus_read_trylock())
			return -EAGAIN;
		selected = mt6797_cci_sample_policy(cci);
		cpus_read_unlock();
	}
	if (selected < 0)
		return selected;
	devfreq_get_freq_range(devfreq, &minimum, &maximum);
	while (selected + 1 < MT6797_CCI_OPP_COUNT &&
	       mt6797_cci_opps[selected].hz < minimum)
		selected++;
	if (mt6797_cci_opps[selected].hz < minimum ||
	    mt6797_cci_opps[selected].hz > maximum)
		return -ERANGE;
	*freq = mt6797_cci_opps[selected].hz;
	return 0;
}

static void mt6797_cci_queue(struct mt6797_cci *cci, unsigned long delay);

static int mt6797_cci_governor_event(struct devfreq *devfreq,
				     unsigned int event, void *data)
{
	struct mt6797_cci *cci = devfreq->data;

	if (event == DEVFREQ_GOV_SUSPEND)
		WRITE_ONCE(cci->suspended, true);
	else if (event == DEVFREQ_GOV_RESUME) {
		WRITE_ONCE(cci->suspended, false);
		mt6797_cci_queue(cci, 0);
	}
	return 0;
}

static struct devfreq_governor mt6797_cci_governor = {
	.name = "mt6797-cci",
	.flags = DEVFREQ_GOV_FLAG_IMMUTABLE | DEVFREQ_GOV_FLAG_IRQ_DRIVEN,
	.get_target_freq = mt6797_cci_governor_target,
	.event_handler = mt6797_cci_governor_event,
};

static void mt6797_cci_queue(struct mt6797_cci *cci, unsigned long delay)
{
	if (!READ_ONCE(cci->stopping) && !READ_ONCE(cci->suspended) &&
	    !READ_ONCE(cci->state.faulted))
		mod_delayed_work(system_unbound_wq, &cci->work, delay);
}

int mt6797_cci_run_a72(struct device *consumer, unsigned long rate,
		     int (*change)(void *), void *context)
{
	struct mt6797_cci *cci;
	unsigned long minimum;
	unsigned int selected = 0;
	int ret;

	if (!change || !rate || rate > 3000000000UL)
		return -EINVAL;
	if (!mt6797_a72_power_owns_dvfs(consumer))
		return -EPERM;
	mutex_lock(&mt6797_cci_owner_lock);
	cci = mt6797_cci_owner;
	if (!cci) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	mutex_lock(&cci->devfreq->lock);
	if (cci->stopping || cci->suspended) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!cci->state.ready || cci->state.faulted) {
		ret = cci->state.faulted ? -EUCLEAN : -EAGAIN;
		goto unlock;
	}
	/* Never reduce an existing fabric vote before the CPU change. */
	minimum = min(rate / 2 + rate % 2, 988000000UL);
	if (minimum < cci->state.hz)
		minimum = cci->state.hz;
	while (selected + 1 < MT6797_CCI_OPP_COUNT &&
	       mt6797_cci_opps[selected].hz < minimum)
		selected++;
	if (mt6797_cci_opps[selected].hz < minimum) {
		ret = -EUCLEAN;
		goto fence;
	}
	cci->a72_target = selected;
	/* Use devfreq's normal accounting and frequency constraints. */
	ret = devfreq_update_target(cci->devfreq, 0);
	cci->a72_target = -1;
	if (!ret)
		ret = change(context);
	if (cci->state.faulted || ret == -EUCLEAN) {
		ret = -EUCLEAN;
		goto fence;
	}
	mt6797_cci_queue(cci, 0);
	goto unlock;
fence:
	cci->state.faulted = true;
	mt6797_cci_fence(cci);
unlock:
	mutex_unlock(&cci->devfreq->lock);
out:
	mutex_unlock(&mt6797_cci_owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_cci_run_a72);

static void mt6797_cci_update(struct work_struct *work)
{
	struct mt6797_cci *cci = container_of(to_delayed_work(work),
					     struct mt6797_cci, work);
	int ret = 0;

	mutex_lock(&cci->devfreq->lock);
	if (!READ_ONCE(cci->stopping) && !READ_ONCE(cci->suspended) &&
	    !cci->state.faulted)
		ret = devfreq_update_target(cci->devfreq, 0);
	mutex_unlock(&cci->devfreq->lock);
	/* Initial CPU OPP registration and concurrent frequency changes retry. */
	if (ret == -EAGAIN)
		mt6797_cci_queue(cci, msecs_to_jiffies(20));
	else if (ret)
		dev_err_ratelimited(cci->dev, "CCI update failed: %d\n", ret);
}

static int mt6797_cci_frequency_event(struct notifier_block *nb,
				      unsigned long event, void *data)
{
	struct mt6797_cci *cci = container_of(nb, struct mt6797_cci, frequency_nb);

	if (event == CPUFREQ_POSTCHANGE)
		mt6797_cci_queue(cci, 0);
	return NOTIFY_DONE;
}

static int mt6797_cci_policy_event(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct mt6797_cci *cci = container_of(nb, struct mt6797_cci, policy_nb);
	struct cpufreq_policy *policy = data;

	if (event == CPUFREQ_REMOVE_POLICY && !READ_ONCE(cci->stopping) &&
	    (cpumask_intersects(policy->related_cpus, &cci->cpus[0]) ||
	     cpumask_intersects(policy->related_cpus, &cci->cpus[1]))) {
		/* CPU clocks can outlive their voltage votes after driver removal. */
		WRITE_ONCE(cci->stopping, true);
		mt6797_cci_fence(cci);
	} else if (event == CPUFREQ_CREATE_POLICY) {
		mt6797_cci_queue(cci, 0);
	}
	return NOTIFY_DONE;
}

static int mt6797_cci_hotplug(unsigned int cpu, struct hlist_node *node)
{
	struct mt6797_cci *cci = hlist_entry(node, struct mt6797_cci, hotplug_node);

	/* Never wait for devfreq here while CPU hotplug holds its write lock. */
	mt6797_cci_queue(cci, 0);
	return 0;
}

static void mt6797_cci_stop(void *context)
{
	struct mt6797_cci *cci = context;

	mutex_lock(&mt6797_cci_owner_lock);
	if (mt6797_cci_owner == cci)
		mt6797_cci_owner = NULL;
	mutex_unlock(&mt6797_cci_owner_lock);
	WRITE_ONCE(cci->stopping, true);
	cpuhp_state_remove_instance_nocalls(cci->hotplug_state, &cci->hotplug_node);
	cpuhp_remove_multi_state(cci->hotplug_state);
	cpufreq_unregister_notifier(&cci->policy_nb, CPUFREQ_POLICY_NOTIFIER);
	cpufreq_unregister_notifier(&cci->frequency_nb, CPUFREQ_TRANSITION_NOTIFIER);
	cancel_delayed_work_sync(&cci->work);
	/* The live CCI clock must not outlive its voltage consumer unfenced. */
	mt6797_cci_fence(cci);
}

static void mt6797_cci_release_supply(void *context)
{
	struct mt6797_cci *cci = context;

	mt6797_cci_fence(cci);
	regulator_disable(cci->supply);
}

static int mt6797_cci_cpu_resources(struct mt6797_cci *cci)
{
	struct device *dev;
	struct clk *clock;
	unsigned int cpu, i;

	for_each_possible_cpu(cpu) {
		dev = get_cpu_device(cpu);
		if (!dev)
			return -EPROBE_DEFER;
		clock = clk_get(dev, NULL);
		if (IS_ERR(clock))
			return PTR_ERR(clock);
		for (i = 0; i < 3; i++) {
			if (!clk_is_match(clock, cci->cpu_clock[i]))
				continue;
			cpumask_set_cpu(cpu, &cci->cpus[i]);
			if (!cci->cpu_dev[i])
				cci->cpu_dev[i] = dev;
			break;
		}
		clk_put(clock);
		if (i == 3)
			return -EINVAL;
	}
	for (i = 0; i < 3; i++)
		if (!cci->cpu_dev[i])
			return -EINVAL;
	return 0;
}

static int mt6797_cci_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "ll", "l", "a72" };
	struct device *dev = &pdev->dev;
	struct mt6797_cci *cci;
	unsigned int i;
	int ret;

	cci = devm_kzalloc(dev, sizeof(*cci), GFP_KERNEL);
	if (!cci)
		return -ENOMEM;
	cci->a72_target = -1;
	cci->dev = dev;
	platform_set_drvdata(pdev, cci);
	INIT_DELAYED_WORK(&cci->work, mt6797_cci_update);
	cci->handoff = mt6797_dvfsp_handoff_get(dev);
	if (IS_ERR(cci->handoff))
		return dev_err_probe(dev, PTR_ERR(cci->handoff), "handoff unavailable\n");
	ret = mt6797_dvfsp_handoff_require_ready(cci->handoff);
	if (ret)
		return dev_err_probe(dev, ret, "handoff not ready\n");
	cci->clock = devm_clk_get(dev, "cci");
	if (IS_ERR(cci->clock))
		return PTR_ERR(cci->clock);
	for (i = 0; i < 3; i++) {
		cci->cpu_clock[i] = devm_clk_get(dev, names[i]);
		if (IS_ERR(cci->cpu_clock[i]))
			return PTR_ERR(cci->cpu_clock[i]);
	}
	ret = mt6797_cci_cpu_resources(cci);
	if (ret)
		return ret;
	cci->supply = devm_regulator_get(dev, "proc");
	if (IS_ERR(cci->supply))
		return dev_err_probe(dev, PTR_ERR(cci->supply), "CPU rail unavailable\n");
	ret = devm_pm_opp_of_add_table(dev);
	if (ret)
		return ret;
	/* Require the board OPPs to agree with the vendor policy table. */
	if (dev_pm_opp_get_opp_count(dev) != MT6797_CCI_OPP_COUNT)
		return -EINVAL;
	for (i = 0; i < MT6797_CCI_OPP_COUNT; i++) {
		struct dev_pm_opp *opp;

		opp = dev_pm_opp_find_freq_exact(dev, mt6797_cci_opps[i].hz, true);
		if (IS_ERR(opp))
			return PTR_ERR(opp);
		ret = dev_pm_opp_get_voltage(opp) == mt6797_cci_opps[i].uv ? 0 : -EINVAL;
		dev_pm_opp_put(opp);
		if (ret)
			return ret;
	}
	ret = mt6797_cci_adopt(&cci->state, &mt6797_cci_io, cci);
	if (ret)
		return dev_err_probe(dev, ret, "CCI adoption failed\n");
	ret = regulator_enable(cci->supply);
	if (ret)
		goto fault;
	ret = devm_add_action_or_reset(dev, mt6797_cci_release_supply, cci);
	if (ret)
		return ret;
	cci->profile.initial_freq = cci->state.hz;
	cci->profile.target = mt6797_cci_target;
	cci->profile.get_cur_freq = mt6797_cci_current;
	cci->devfreq = devm_devfreq_add_device(dev, &cci->profile,
					     mt6797_cci_governor.name, cci);
	if (IS_ERR(cci->devfreq)) {
		ret = PTR_ERR(cci->devfreq);
		goto fault;
	}
	cci->frequency_nb.notifier_call = mt6797_cci_frequency_event;
	ret = cpufreq_register_notifier(&cci->frequency_nb, CPUFREQ_TRANSITION_NOTIFIER);
	if (ret)
		goto fault;
	cci->policy_nb.notifier_call = mt6797_cci_policy_event;
	ret = cpufreq_register_notifier(&cci->policy_nb, CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		goto frequency;
	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "devfreq/mt6797-cci:online",
				     mt6797_cci_hotplug, mt6797_cci_hotplug);
	if (ret < 0)
		goto policy;
	cci->hotplug_state = ret;
	ret = cpuhp_state_add_instance_nocalls(cci->hotplug_state, &cci->hotplug_node);
	if (ret)
		goto hotplug;
	ret = devm_add_action_or_reset(dev, mt6797_cci_stop, cci);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_handoff_cci_claim(cci->handoff);
	if (ret)
		goto fault;
	mutex_lock(&mt6797_cci_owner_lock);
	if (mt6797_cci_owner)
		ret = -EBUSY;
	else
		mt6797_cci_owner = cci;
	mutex_unlock(&mt6797_cci_owner_lock);
	if (ret)
		goto fault;
	mt6797_cci_queue(cci, 0);
	dev_info(dev, "adopted CCI at %u Hz, vote %u uV\n", cci->state.hz, cci->state.vote_uv);
	return 0;
hotplug:
	cpuhp_remove_multi_state(cci->hotplug_state);
policy:
	cpufreq_unregister_notifier(&cci->policy_nb, CPUFREQ_POLICY_NOTIFIER);
frequency:
	cpufreq_unregister_notifier(&cci->frequency_nb, CPUFREQ_TRANSITION_NOTIFIER);
	WRITE_ONCE(cci->stopping, true);
	cancel_delayed_work_sync(&cci->work);
fault:
	mt6797_cci_fence(cci);
	return ret;
}

static int mt6797_cci_suspend(struct device *dev)
{
	struct mt6797_cci *cci = dev_get_drvdata(dev);

	WRITE_ONCE(cci->suspended, true);
	cancel_delayed_work_sync(&cci->work);
	return devfreq_suspend_device(cci->devfreq);
}

static int mt6797_cci_resume(struct device *dev)
{
	struct mt6797_cci *cci = dev_get_drvdata(dev);

	/* The final nested resume event reopens notification-driven work. */
	return devfreq_resume_device(cci->devfreq);
}

/*
 * A restored hibernation image holds the CCI state cached when the snapshot
 * was taken, but the boot kernel (or this kernel writing the image) has
 * changed the fabric clock and vote since. Adopt the hardware again, exactly
 * as at probe, before devfreq resumes.
 */
static int mt6797_cci_restore(struct device *dev)
{
	struct mt6797_cci *cci = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&cci->devfreq->lock);
	if (cci->state.faulted) {
		ret = -EHOSTDOWN;
	} else {
		cci->state.ready = false;
		ret = mt6797_cci_adopt(&cci->state, &mt6797_cci_io, cci);
		if (!ret)
			cci->devfreq->previous_freq = cci->state.hz;
	}
	mutex_unlock(&cci->devfreq->lock);
	if (ret) {
		dev_err(dev, "CCI re-adoption after restore failed: %d\n", ret);
		return ret;
	}
	dev_info(dev, "re-adopted CCI at %u Hz, vote %u uV after restore\n",
		 cci->state.hz, cci->state.vote_uv);
	return devfreq_resume_device(cci->devfreq);
}

static const struct dev_pm_ops mt6797_cci_pm = {
	.suspend = mt6797_cci_suspend,
	.resume = mt6797_cci_resume,
	.freeze = mt6797_cci_suspend,
	.thaw = mt6797_cci_resume,
	.poweroff = mt6797_cci_suspend,
	.restore = mt6797_cci_restore,
};

static const struct of_device_id mt6797_cci_match[] = {
	{ .compatible = "mediatek,mt6797-cci" },
	{ }
};

static struct platform_driver mt6797_cci_driver = {
	.probe = mt6797_cci_probe,
	.driver = {
		.name = "mt6797-cci",
		.of_match_table = mt6797_cci_match,
		.pm = pm_sleep_ptr(&mt6797_cci_pm),
		.suppress_bind_attrs = true,
	},
};

static int __init mt6797_cci_init(void)
{
	int ret = devfreq_add_governor(&mt6797_cci_governor);

	if (ret)
		return ret;
	ret = platform_driver_register(&mt6797_cci_driver);
	if (ret)
		devfreq_remove_governor(&mt6797_cci_governor);
	return ret;
}
device_initcall(mt6797_cci_init);
MODULE_DESCRIPTION("MT6797 CCI frequency and shared voltage policy");
MODULE_LICENSE("GPL");
