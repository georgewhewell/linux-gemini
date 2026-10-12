// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 C2K MTCMOS transaction, from the active TOPAXI_PROTECT_LOCK path in
 * MediaTek clk-mt6797-pg.c:spm_mtcmos_ctrl_c2k(). Domain power is only one step
 * before C2K reset/handshake/PLL/CCIF runtime; it never means modem READY.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/soc/mediatek/mt6797-md3-power.h>

#define C2K_RST_B BIT(0)
#define C2K_ISO BIT(1)
#define C2K_ON BIT(2)
#define C2K_ON_2ND BIT(3)
#define C2K_CLK_DIS BIT(4)
#define C2K_CONTROL_MASK (C2K_RST_B | C2K_ISO | C2K_ON | C2K_ON_2ND | C2K_CLK_DIS)
#define C2K_COLD (C2K_ISO | C2K_CLK_DIS)
#define C2K_POWERED (C2K_RST_B | C2K_ON | C2K_ON_2ND)
#define C2K_ACK BIT(28)
#define C2K_PROTECT (BIT(13) | BIT(14) | BIT(15))

static int checked(int ret)
{
	return ret > 0 ? -EPROTO : ret;
}

static int live(struct mt6797_md3_power *p)
{
	return checked(p->ops->check_live(p->context));
}

static int read_reg(struct mt6797_md3_power *p,
		    enum mt6797_md3_power_reg reg, u32 *value)
{
	int ret = live(p);

	if (!ret)
		ret = checked(p->ops->read(p->context, reg, value));
	return ret ?: live(p);
}

static int update(struct mt6797_md3_power *p,
		  enum mt6797_md3_power_reg reg, u32 mask, u32 value)
{
	int ret = live(p);

	if (!ret)
		ret = checked(p->ops->update_bits(p->context, reg, mask, value));
	return ret ?: live(p);
}

static int verify(struct mt6797_md3_power *p, bool powered)
{
	u32 con, ack, ack2, en, sta;
	int ret;

	ret = read_reg(p, MT6797_MD3_SPM_CONTROL, &con);
	if (!ret)
		ret = read_reg(p, MT6797_MD3_SPM_STATUS, &ack);
	if (!ret)
		ret = read_reg(p, MT6797_MD3_SPM_STATUS_2ND, &ack2);
	if (ret)
		return ret;
	if ((con & C2K_CONTROL_MASK) != (powered ? C2K_POWERED : C2K_COLD) ||
	    (ack & C2K_ACK) != (powered ? C2K_ACK : 0) ||
	    (ack2 & C2K_ACK) != (powered ? C2K_ACK : 0))
		return -EBUSY;
	/* Cold bus-protect defaults are not assumed. No execution is released. */
	if (!powered)
		return 0;
	ret = read_reg(p, MT6797_MD3_INFRA_PROTECT_EN, &en);
	if (!ret)
		ret = read_reg(p, MT6797_MD3_INFRA_PROTECT_STA, &sta);
	if (ret)
		return ret;
	return ((en | sta) & C2K_PROTECT) ? -EIO : 0;
}

static int wait_ack(struct mt6797_md3_power *p, bool bus)
{
	u32 value, second;
	int ret;

	while (p->result.polls < MT6797_MD3_POWER_MAX_POLLS) {
		p->result.polls++;
		ret = read_reg(p, bus ? MT6797_MD3_INFRA_PROTECT_STA :
			       MT6797_MD3_SPM_STATUS, &value);
		if (ret)
			return ret;
		if (bus) {
			if (!(value & C2K_PROTECT))
				return 0;
		} else {
			ret = read_reg(p, MT6797_MD3_SPM_STATUS_2ND, &second);
			if (ret)
				return ret;
			if ((value & C2K_ACK) && (second & C2K_ACK))
				return 0;
		}
		if (p->result.polls < MT6797_MD3_POWER_MAX_POLLS)
			usleep_range(10, 20);
	}
	return -ETIMEDOUT;
}

int mt6797_md3_power_init(struct mt6797_md3_power *p,
			const struct mt6797_md3_power_ops *ops, void *context)
{
	if (!p || !ops || !ops->begin || !ops->end || !ops->check_live ||
	    !ops->read || !ops->update_bits || !ops->enable_spm)
		return -EINVAL;
	mutex_init(&p->lock);
	p->ops = ops;
	p->context = context;
	p->result = (struct mt6797_md3_power_result) {
		.state = MT6797_MD3_POWER_NEW,
	};
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md3_power_init);

int mt6797_md3_power_on(struct mt6797_md3_power *p)
{
	bool acquired = false;
	int ret;

	if (!p || !p->ops)
		return -EINVAL;
	mutex_lock(&p->lock);
	if (p->result.state != MT6797_MD3_POWER_NEW) {
		ret = p->result.state == MT6797_MD3_POWER_DOMAIN_ON ?
			-EALREADY : -ESHUTDOWN;
		goto unlock;
	}
	ret = checked(p->ops->begin(p->context));
	if (ret)
		goto finish;
	acquired = true;
	ret = verify(p, false);
	if (ret)
		goto finish;
	ret = live(p);
	if (ret)
		goto finish;
	/* Publish ambiguity BEFORE the first possible write, even on I/O error. */
	p->result.state = MT6797_MD3_POWER_TRANSITION;
	p->result.may_have_written = true;
	ret = checked(p->ops->enable_spm(p->context));
	if (!ret)
		ret = live(p);
	if (!ret)
		ret = update(p, MT6797_MD3_SPM_CONTROL, C2K_ON, C2K_ON);
	if (!ret)
		ret = update(p, MT6797_MD3_SPM_CONTROL, C2K_ON_2ND, C2K_ON_2ND);
	if (!ret)
		ret = wait_ack(p, false);
	if (!ret)
		ret = update(p, MT6797_MD3_SPM_CONTROL, C2K_CLK_DIS, 0);
	if (!ret)
		ret = update(p, MT6797_MD3_SPM_CONTROL, C2K_ISO, 0);
	if (!ret)
		ret = update(p, MT6797_MD3_SPM_CONTROL, C2K_RST_B, C2K_RST_B);
	if (!ret)
		ret = update(p, MT6797_MD3_INFRA_PROTECT_EN, C2K_PROTECT, 0);
	if (!ret)
		ret = wait_ack(p, true);
	if (!ret)
		ret = verify(p, true);
finish:
	p->result.error = ret;
	p->result.state = ret ? MT6797_MD3_POWER_FAILED : MT6797_MD3_POWER_DOMAIN_ON;
	if (acquired)
		p->ops->end(p->context, &p->result);
unlock:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md3_power_on);

int mt6797_md3_power_result(struct mt6797_md3_power *p,
			  struct mt6797_md3_power_result *result)
{
	if (!p || !p->ops || !result)
		return -EINVAL;
	mutex_lock(&p->lock);
	*result = p->result;
	mutex_unlock(&p->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md3_power_result);
