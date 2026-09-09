// SPDX-License-Identifier: GPL-2.0-only
/* MT6351 internal, active-low PMIC EINT headset detection. */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <sound/jack.h>
#include <sound/soc.h>

#include "mt6351.h"
#include "mt6351-accdet.h"

#define ACCDET_CKPDN		0x0246
#define ACCDET_RESET		0x02a8
#define ACCDET_RSV		0x0f46
#define ACCDET_CTRL		0x0f48
#define ACCDET_SWCTRL		0x0f4a
#define ACCDET_PWM_WIDTH		0x0f4c
#define ACCDET_PWM_THRESH		0x0f4e
#define ACCDET_DELAY		0x0f50
#define ACCDET_DEBOUNCE0		0x0f52
#define ACCDET_DEBOUNCE1		0x0f54
#define ACCDET_DEBOUNCE3		0x0f58
#define ACCDET_DEBOUNCE4		0x0f5a
#define ACCDET_IRQ		0x0f5e
#define ACCDET_STATE		0x0f62
#define ACCDET_EINT_CTL		0x0f64
#define ACCDET_EINT_STATE		0x0f6a

#define ACCDET_CLK_OFF		BIT(9)
#define ACCDET_RESET_BIT		BIT(4)
#define ACCDET_ENABLE		BIT(0)
#define ACCDET_EINT_ENABLE	BIT(2)
#define ACCDET_IRQ_PENDING	BIT(0)
#define ACCDET_EINT_PENDING	BIT(2)
#define ACCDET_IRQ_CLEAR		BIT(8)
#define ACCDET_EINT_CLEAR		BIT(10)
#define ACCDET_EINT_HIGH		BIT(15)
#define ACCDET_EINT_MEM		BIT(10)
#define ACCDET_JACK_MASK		SND_JACK_HEADSET

struct mt6351_accdet_saved {
	unsigned int reg;
	unsigned int mask;
	unsigned int value;
};

struct mt6351_accdet {
	struct device *dev;
	struct regmap *regmap;
	/* Serializes set_jack, teardown and restart. */
	struct mutex lifecycle_lock;
	/* Protects registers and detector state; released before jack reports. */
	struct mutex lock;
	struct delayed_work work;
	struct delayed_work bias_off_work;
	struct snd_soc_jack *jack;
	unsigned int settings[7];
	struct mt6351_accdet_saved saved[16];
	unsigned int saved_count;
	unsigned int status;
	unsigned long classify_at;
	unsigned long bias_off_at;
	int irq[2];
	bool enabled;
	bool present;
	bool bias_on;
	bool fault;
	bool reported;
	unsigned int reported_status;
};

/* Only the detector-owned fields are restored; audio has separate ownership. */
static const struct mt6351_accdet_saved mt6351_accdet_registers[] = {
	{ ACCDET_CKPDN, ACCDET_CLK_OFF },
	{ ACCDET_RESET, ACCDET_RESET_BIT },
	{ ACCDET_RSV, 0xffff },
	{ ACCDET_CTRL, 0x0005 },
	{ ACCDET_SWCTRL, 0x00ff },
	{ ACCDET_PWM_WIDTH, 0xffff },
	{ ACCDET_PWM_THRESH, 0xffff },
	{ ACCDET_DELAY, 0xffff },
	{ ACCDET_DEBOUNCE0, 0xffff },
	{ ACCDET_DEBOUNCE1, 0xffff },
	{ ACCDET_DEBOUNCE3, 0xffff },
	{ ACCDET_DEBOUNCE4, 0xffff },
	{ ACCDET_EINT_CTL, 0x3770 },
	{ ACCDET_IRQ, ACCDET_EINT_HIGH },
	{ MT6351_AUDENC_ANA_CON11, 0x080f },
	{ MT6351_AUDENC_ANA_CON10, 0x00f0 },
};

static int mt6351_accdet_ack(struct mt6351_accdet *accdet, unsigned int status,
			     unsigned int polarity)
{
	unsigned int clear = 0, value, pending;
	int ret;

	pending = status & (ACCDET_IRQ_PENDING | ACCDET_EINT_PENDING);
	if (pending & ACCDET_IRQ_PENDING)
		clear |= ACCDET_IRQ_CLEAR;
	if (pending & ACCDET_EINT_PENDING)
		clear |= ACCDET_EINT_CLEAR;
	/* Status bits are read-only. Never echo unrelated clear commands. */
	ret = regmap_write(accdet->regmap, ACCDET_IRQ, polarity | clear);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(accdet->regmap, ACCDET_IRQ, value,
				       !(value & pending), 100, 5000);
	if (ret)
		return ret;
	return regmap_write(accdet->regmap, ACCDET_IRQ, polarity);
}

static int mt6351_accdet_save(struct mt6351_accdet *accdet)
{
	unsigned int i;
	int ret;

	accdet->saved_count = 0;
	for (i = 0; i < ARRAY_SIZE(mt6351_accdet_registers); i++) {
		accdet->saved[i] = mt6351_accdet_registers[i];
		ret = regmap_read(accdet->regmap, accdet->saved[i].reg,
				  &accdet->saved[i].value);
		if (ret)
			return ret;
		/* Do not reset a running ACCDET/EINT/negative-voltage detector. */
		if ((accdet->saved[i].reg == ACCDET_RESET &&
		     accdet->saved[i].value & ACCDET_RESET_BIT) ||
		    (accdet->saved[i].reg == ACCDET_CTRL &&
		     accdet->saved[i].value & 0x001f))
			return -EBUSY;
	}
	accdet->saved_count = i;
	return 0;
}

static int mt6351_accdet_restore(struct mt6351_accdet *accdet)
{
	struct mt6351_accdet_saved *saved;
	unsigned int mask, value, status;
	int ret, error = 0;

	/* A failed reset deassertion must not discard subsequent restoration. */
	ret = regmap_update_bits(accdet->regmap, ACCDET_RESET, ACCDET_RESET_BIT, 0);
	if (ret)
		error = ret;
	/* Stop PWM before restoring bias and configuration; gate the clock last. */
	ret = regmap_update_bits(accdet->regmap, ACCDET_CTRL, 0x0005, 0);
	if (ret && !error)
		error = ret;
	ret = regmap_update_bits(accdet->regmap, ACCDET_SWCTRL, 0x00ff, 0);
	if (ret && !error)
		error = ret;
	while (accdet->saved_count) {
		saved = &accdet->saved[--accdet->saved_count];
		mask = saved->mask;
		value = saved->value;
		if (saved->reg == ACCDET_IRQ) {
			ret = regmap_read(accdet->regmap, ACCDET_IRQ, &status);
			if (!ret)
				ret = mt6351_accdet_ack(accdet, status, value & mask);
			if (ret && !error)
				error = ret;
			/* Deassert clear commands even if a previous poll timed out. */
			ret = regmap_write(accdet->regmap, ACCDET_IRQ, value & mask);
			if (ret && !error)
				error = ret;
			continue;
		}
		if (saved->reg == MT6351_AUDENC_ANA_CON10 && accdet->bias_on) {
			/* Capture keeps normal-power 2.7V bias until its own close. */
			value = (value & ~0x00f0) | 0x0070;
		}
		ret = regmap_update_bits(accdet->regmap, saved->reg, mask, value);
		if (ret && !error)
			error = ret;
	}
	return error;
}

static int mt6351_accdet_init(struct mt6351_accdet *accdet)
{
	const struct reg_sequence sequence[] = {
		{ ACCDET_PWM_WIDTH, accdet->settings[0] - 1 },
		{ ACCDET_PWM_THRESH, accdet->settings[1] - 1 },
		{ ACCDET_DELAY, (accdet->settings[2] << 15) | accdet->settings[3] },
		{ ACCDET_DEBOUNCE0, accdet->settings[4] },
		{ ACCDET_DEBOUNCE1, accdet->settings[5] },
		{ ACCDET_DEBOUNCE3, accdet->settings[6] },
		{ ACCDET_DEBOUNCE4, 0x0042 },
		{ ACCDET_RSV, 0x0010 },
	};
	unsigned int status;
	int ret;

	ret = regmap_update_bits(accdet->regmap, ACCDET_CKPDN, ACCDET_CLK_OFF, 0);
	if (ret)
		return ret;
	/* The reset is confined to ACCDET, not the codec or the PMIC IRQ core. */
	ret = regmap_update_bits(accdet->regmap, ACCDET_RESET,
				 ACCDET_RESET_BIT, ACCDET_RESET_BIT);
	if (ret)
		return ret;
	ret = regmap_update_bits(accdet->regmap, ACCDET_RESET, ACCDET_RESET_BIT, 0);
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(accdet->regmap, sequence, ARRAY_SIZE(sequence));
	if (ret)
		return ret;
	/* Comparator thresholds and internal PMIC EINT connection, ACC mode. */
	ret = regmap_update_bits(accdet->regmap, MT6351_AUDENC_ANA_CON11,
				 0x080f, 0x080f);
	if (ret)
		return ret;
	/* Detector PWM owns bias when idle; DAPM owns bias during capture. */
	ret = regmap_update_bits(accdet->regmap, MT6351_AUDENC_ANA_CON10,
				 0x00f0, accdet->bias_on ? 0x0070 : 0x00f0);
	if (ret)
		return ret;
	/* 256ms insertion debounce; EINT PWM threshold 4, vendor reset width. */
	ret = regmap_update_bits(accdet->regmap, ACCDET_EINT_CTL, 0x3770, 0x0460);
	if (ret)
		return ret;
	ret = regmap_read(accdet->regmap, ACCDET_IRQ, &status);
	if (ret)
		return ret;
	ret = mt6351_accdet_ack(accdet, status, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(accdet->regmap, ACCDET_SWCTRL, 0x00ff, BIT(3));
	if (ret)
		return ret;
	return regmap_update_bits(accdet->regmap, ACCDET_CTRL,
				 0x0005, ACCDET_EINT_ENABLE);
}

static int mt6351_accdet_presence(struct mt6351_accdet *accdet, bool present)
{
	int ret;

	ret = regmap_update_bits(accdet->regmap, ACCDET_EINT_CTL, 0x0070,
				 present ? 0 : 0x0060);
	if (ret)
		return ret;
	ret = regmap_update_bits(accdet->regmap, ACCDET_SWCTRL, 0x007f,
				 present ? 0x007f : BIT(3));
	if (ret)
		return ret;
	ret = regmap_update_bits(accdet->regmap, ACCDET_CTRL, ACCDET_ENABLE,
				 present ? ACCDET_ENABLE : 0);
	if (ret)
		return ret;
	if (present && !accdet->present) {
		accdet->status = SND_JACK_HEADPHONE;
		accdet->classify_at = jiffies + msecs_to_jiffies(100);
		accdet->bias_off_at = jiffies + msecs_to_jiffies(6000);
		mod_delayed_work(system_wq, &accdet->bias_off_work, msecs_to_jiffies(6000));
	}
	if (!present) {
		accdet->status = 0;
		cancel_delayed_work(&accdet->bias_off_work);
	}
	accdet->present = present;
	return 0;
}

/* A button pull-down must not turn an already identified headset into TRS. */
static unsigned int mt6351_accdet_classify(unsigned int previous, unsigned int ab)
{
	if (ab == 1)
		return SND_JACK_HEADSET;
	if (ab == 0)
		return previous & SND_JACK_MICROPHONE ? SND_JACK_HEADSET : SND_JACK_HEADPHONE;
	/* AB=3 is not proof of removal; the mechanical EINT owns removal. */
	return previous;
}

/* Called with lock held. IRQs stay masked until stop/start explicitly recovers. */
static void mt6351_accdet_fault(struct mt6351_accdet *accdet, int error)
{
	if (accdet->fault)
		return;
	accdet->fault = true;
	disable_irq_nosync(accdet->irq[0]);
	disable_irq_nosync(accdet->irq[1]);
	regmap_update_bits(accdet->regmap, ACCDET_CTRL, 0x0005, 0);
	regmap_update_bits(accdet->regmap, ACCDET_SWCTRL, 0x00ff, 0);
	dev_err(accdet->dev, "headset detector stopped after register error: %d\n", error);
}

static void mt6351_accdet_bias_off_work(struct work_struct *work)
{
	struct mt6351_accdet *accdet = container_of(to_delayed_work(work),
						 struct mt6351_accdet, bias_off_work);
	int ret;

	mutex_lock(&accdet->lock);
	if (!accdet->enabled || accdet->fault || !accdet->present)
		goto out;
	/* A timer from a previous plug must not shorten the new plug's interval. */
	if (time_before(jiffies, accdet->bias_off_at)) {
		mod_delayed_work(system_wq, &accdet->bias_off_work, accdet->bias_off_at - jiffies);
		goto out;
	}
	if (accdet->status != SND_JACK_HEADPHONE)
		goto out;
	/* Keep mechanical EINT active. DAPM retains any live capture bias claim. */
	ret = regmap_update_bits(accdet->regmap, ACCDET_CTRL, ACCDET_ENABLE, 0);
	if (!ret)
		ret = regmap_update_bits(accdet->regmap, ACCDET_SWCTRL, 0x007f, BIT(3));
	if (ret)
		mt6351_accdet_fault(accdet, ret);
out:
	mutex_unlock(&accdet->lock);
}

static void mt6351_accdet_work(struct work_struct *work)
{
	struct mt6351_accdet *accdet = container_of(to_delayed_work(work),
						 struct mt6351_accdet, work);
	struct snd_soc_jack *jack;
	unsigned int eint, ab, status;
	bool present;
	int ret;

	mutex_lock(&accdet->lock);
	if (!accdet->enabled || accdet->fault)
		goto unlock;
	ret = regmap_read(accdet->regmap, ACCDET_EINT_STATE, &eint);
	if (ret)
		goto fault;
	present = !(eint & ACCDET_EINT_MEM);
	if (present != accdet->present) {
		ret = mt6351_accdet_presence(accdet, present);
		if (ret)
			goto fault;
		ret = regmap_read(accdet->regmap, ACCDET_IRQ, &status);
		if (ret)
			goto fault;
		ret = mt6351_accdet_ack(accdet, status, present ? ACCDET_EINT_HIGH : 0);
		if (ret)
			goto fault;
	}
	if (present) {
		if (time_before(jiffies, accdet->classify_at)) {
			mod_delayed_work(system_wq, &accdet->work, accdet->classify_at - jiffies);
			goto report;
		}
		ret = regmap_read(accdet->regmap, ACCDET_STATE, &ab);
		if (ret)
			goto fault;
		accdet->status = mt6351_accdet_classify(accdet->status, (ab >> 6) & 3);
	}
report:
	jack = accdet->jack;
	status = accdet->status;
	if (!accdet->reported || status != accdet->reported_status) {
		dev_info(accdet->dev, "MT6351 headset report present=%u microphone=%u\n",
			 !!(status & SND_JACK_HEADPHONE), !!(status & SND_JACK_MICROPHONE));
		accdet->reported = true;
		accdet->reported_status = status;
	}
	mutex_unlock(&accdet->lock);
	/* Jack notifiers/DAPM may call back into the codec's bias event. */
	snd_soc_jack_report(jack, status, ACCDET_JACK_MASK);
	return;
fault:
	mt6351_accdet_fault(accdet, ret);
unlock:
	mutex_unlock(&accdet->lock);
}

static irqreturn_t mt6351_accdet_irq(int irq, void *data)
{
	struct mt6351_accdet *accdet = data;
	unsigned int status, eint;
	bool present;
	int ret;

	mutex_lock(&accdet->lock);
	if (!accdet->enabled || accdet->fault)
		goto out;
	ret = regmap_read(accdet->regmap, ACCDET_IRQ, &status);
	if (ret)
		goto fault;
	if (!(status & (ACCDET_IRQ_PENDING | ACCDET_EINT_PENDING)))
		goto out;
	present = accdet->present;
	if (status & ACCDET_EINT_PENDING) {
		ret = regmap_read(accdet->regmap, ACCDET_EINT_STATE, &eint);
		if (ret)
			goto fault;
		present = !(eint & ACCDET_EINT_MEM);
		ret = mt6351_accdet_presence(accdet, present);
		if (ret)
			goto fault;
	}
	ret = mt6351_accdet_ack(accdet, status, present ? ACCDET_EINT_HIGH : 0);
	if (ret)
		goto fault;
	/* Report presence promptly; work defers microphone classification only. */
	mod_delayed_work(system_wq, &accdet->work, 0);
	goto out;
fault:
	mt6351_accdet_fault(accdet, ret);
out:
	mutex_unlock(&accdet->lock);
	return IRQ_HANDLED;
}

int mt6351_accdet_set_jack(struct mt6351_accdet *accdet, struct snd_soc_jack *jack)
{
	int ret = 0, restore;

	if (!accdet)
		return jack ? -EOPNOTSUPP : 0;
	mutex_lock(&accdet->lifecycle_lock);
	if (accdet->enabled) {
		/* Never hold the register lock while waiting for IRQ/work completion. */
		disable_irq(accdet->irq[0]);
		disable_irq(accdet->irq[1]);
		mutex_lock(&accdet->lock);
		accdet->enabled = false;
		if (accdet->fault) {
			/* Drop the extra disable references acquired by the fault path. */
			enable_irq(accdet->irq[0]);
			enable_irq(accdet->irq[1]);
		}
		mutex_unlock(&accdet->lock);
		cancel_delayed_work_sync(&accdet->work);
		cancel_delayed_work_sync(&accdet->bias_off_work);
		mutex_lock(&accdet->lock);
		ret = mt6351_accdet_restore(accdet);
		mutex_unlock(&accdet->lock);
		snd_soc_jack_report(accdet->jack, 0, ACCDET_JACK_MASK);
		accdet->jack = NULL;
	}
	if (!jack || ret)
		goto out;
	mutex_lock(&accdet->lock);
	accdet->fault = false;
	accdet->present = false;
	accdet->status = 0;
	accdet->reported = false;
	ret = mt6351_accdet_save(accdet);
	if (ret)
		goto unlock;
	ret = mt6351_accdet_init(accdet);
	if (ret) {
		restore = mt6351_accdet_restore(accdet);
		if (restore)
			dev_err(accdet->dev, "headset detector rollback failed: %d\n", restore);
		goto unlock;
	}
	accdet->jack = jack;
	accdet->enabled = true;
unlock:
	mutex_unlock(&accdet->lock);
	if (!ret) {
		enable_irq(accdet->irq[0]);
		enable_irq(accdet->irq[1]);
		/* Also detects a plug already inserted at boot, without an edge. */
		mod_delayed_work(system_wq, &accdet->work, msecs_to_jiffies(300));
		dev_info(accdet->dev, "internal PMIC headset detector enabled\n");
	}
out:
	mutex_unlock(&accdet->lifecycle_lock);
	return ret;
}

int mt6351_accdet_micbias(struct mt6351_accdet *accdet, bool enable)
{
	unsigned int value;
	int ret;

	mutex_lock(&accdet->lock);
	value = enable ? 0x0070 : accdet->enabled && !accdet->fault ? 0x00f0 : 0;
	ret = regmap_update_bits(accdet->regmap, MT6351_AUDENC_ANA_CON10, 0x00f0, value);
	if (!ret)
		accdet->bias_on = enable;
	mutex_unlock(&accdet->lock);
	return ret;
}

static void mt6351_accdet_remove(void *data)
{
	struct mt6351_accdet *accdet = data;
	int ret;

	ret = mt6351_accdet_set_jack(accdet, NULL);
	if (ret)
		dev_err(accdet->dev, "headset detector shutdown failed: %d\n", ret);
}

struct mt6351_accdet *mt6351_accdet_probe(struct platform_device *pdev,
					  struct regmap *regmap)
{
	static const char * const names[] = { "accdet", "accdet-eint" };
	struct mt6351_accdet *accdet;
	unsigned int *s;
	int i, ret;

	if (!device_property_read_bool(&pdev->dev, "mediatek,jack-detection"))
		return NULL;
	accdet = devm_kzalloc(&pdev->dev, sizeof(*accdet), GFP_KERNEL);
	if (!accdet)
		return ERR_PTR(-ENOMEM);
	accdet->dev = &pdev->dev;
	accdet->regmap = regmap;
	mutex_init(&accdet->lock);
	mutex_init(&accdet->lifecycle_lock);
	INIT_DELAYED_WORK(&accdet->work, mt6351_accdet_work);
	INIT_DELAYED_WORK(&accdet->bias_off_work, mt6351_accdet_bias_off_work);
	s = accdet->settings;
	ret = device_property_count_u32(&pdev->dev, "mediatek,accdet-pwm-settings");
	if (ret != 7)
		return ERR_PTR(ret < 0 ? ret : -EINVAL);
	ret = device_property_read_u32_array(&pdev->dev, "mediatek,accdet-pwm-settings", s, 7);
	if (ret)
		return ERR_PTR(ret);
	if (!s[0] || s[0] > 65535 || !s[1] || s[1] > s[0] || s[2] > 1 ||
	    s[3] > 32767 || !s[4] || s[4] > 65535 || !s[5] || s[5] > 65535 ||
	    !s[6] || s[6] > 65535)
		return ERR_PTR(-EINVAL);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		accdet->irq[i] = platform_get_irq_byname(pdev, names[i]);
		if (accdet->irq[i] < 0)
			return ERR_PTR(accdet->irq[i]);
		ret = devm_request_threaded_irq(&pdev->dev, accdet->irq[i], NULL,
						mt6351_accdet_irq, IRQF_ONESHOT | IRQF_NO_AUTOEN,
						names[i], accdet);
		if (ret)
			return ERR_PTR(ret);
	}
	ret = devm_add_action_or_reset(&pdev->dev, mt6351_accdet_remove, accdet);
	if (ret)
		return ERR_PTR(ret);
	return accdet;
}
