// SPDX-License-Identifier: GPL-2.0-only
/* Execute the shared GEN3 AHB SDIO-like command encoder without hardware. */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <linux/soc/mediatek/mt6797-wifi-ahb-codec.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb-w0-phase.h>

struct fake_w0 { int uv, bt, wifi, set_fail, restore_fail, enable_fail, disable_fail, observe_fail, off_fail; int disables; int fatal; };
static int f_uv(void *p) { return ((struct fake_w0 *)p)->uv; }
static int f_on(void *p, bool wifi) { struct fake_w0 *f = p; return f->off_fail ? f->off_fail : (wifi ? f->wifi : f->bt); }
static int f_set(void *p, int uv) { struct fake_w0 *f = p; if ((f->set_fail && uv == 3300000) || (f->restore_fail && uv != 3300000)) return -EIO; f->uv = uv; return 0; }
static int f_enable(void *p) { struct fake_w0 *f = p; f->wifi = 1; return f->enable_fail; }
static int f_disable(void *p) { struct fake_w0 *f = p; f->disables++; if (f->disable_fail) return f->disable_fail; f->wifi = 0; return 0; }
static int f_observe(void *p) { return ((struct fake_w0 *)p)->observe_fail; }
static void f_fatal(void *p, int causal, int cleanup) { (void)causal; (void)cleanup; ((struct fake_w0 *)p)->fatal = 1; }

int main(void)
{
	unsigned int word;
	struct fake_w0 fake = { .uv = 1800000 };
	struct mt6797_wifi_ahb_w0_result result;
	struct mt6797_wifi_ahb_w0_ops ops = {
		.get_voltage = f_uv, .is_enabled = f_on, .set_voltage = f_set,
		.enable = f_enable, .disable = f_disable, .observe = f_observe,
		.fatal = f_fatal, .context = &fake,
	};

	assert(mt6797_wifi_ahb_cmd52_pack(0, 0, 0x02, 0, &word) == 0);
	assert(word == 0x00000400U);
	assert(mt6797_wifi_ahb_cmd52_pack(1, 0, 0x02, 0x02, &word) == 0);
	assert(word == 0x80000402U);
	assert(mt6797_wifi_ahb_cmd53_read_pack(0, 4, &word) == 0);
	assert(word == MT6797_WIFI_AHB_CMD53_WIFI_READ_WCIR);
	assert(mt6797_wifi_ahb_cmd52_pack(0, 8, 0, 0, &word) == -EINVAL);
	assert(mt6797_wifi_ahb_cmd52_pack(0, 0, 0x20000, 0, &word) == -EINVAL);
	assert(mt6797_wifi_ahb_cmd53_read_pack(0, 0, &word) == -EINVAL);
	assert(mt6797_wifi_ahb_cmd53_read_pack(0, 512, &word) == -EINVAL);

	/* Selector partial failure, failed enable, observation and cleanup faults. */
	fake.set_fail = -EIO;
	assert(mt6797_wifi_ahb_w0_run(&ops, &result, 3300000) == -EIO);
	assert(!result.fatal && !result.selector_dirty && fake.uv == 1800000);
	fake = (struct fake_w0){ .uv = 1800000, .enable_fail = -EIO };
	assert(mt6797_wifi_ahb_w0_run(&ops, &result, 3300000) == -EIO);
	assert(fake.disables == 1 && !result.fatal);
	fake = (struct fake_w0){ .uv = 1800000, .observe_fail = -EPROTO };
	assert(mt6797_wifi_ahb_w0_run(&ops, &result, 3300000) == -EPROTO);
	assert(fake.disables == 1 && !result.fatal);
	fake = (struct fake_w0){ .uv = 1800000, .disable_fail = -EIO };
	assert(mt6797_wifi_ahb_w0_run(&ops, &result, 3300000) == -EIO);
	assert(result.fatal && result.selector_dirty && fake.fatal);
	fake = (struct fake_w0){ .uv = 1800000, .restore_fail = 1 };
	assert(mt6797_wifi_ahb_w0_run(&ops, &result, 3300000) == -EIO);
	assert(result.fatal && result.selector_dirty && fake.fatal);
	fake = (struct fake_w0){ .uv = 1800000, .off_fail = -EIO };
	assert(mt6797_wifi_ahb_w0_run(&ops, &result, 3300000) == -EIO);
	assert(!fake.disables); /* refusal before any selector or rail operation */

	puts("PASS: MT6797 GEN3 WCIR CMD52/CMD53 encoder");
	return 0;
}
