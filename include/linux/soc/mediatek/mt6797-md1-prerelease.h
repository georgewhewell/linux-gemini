/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_PRERELEASE_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_PRERELEASE_H

#include <linux/mutex.h>
#include <linux/types.h>

struct regmap;
#include <linux/types.h>

/* A backend operation returns -EAGAIN while its bounded observation is pending. */
#define MT6797_MD1_PRERELEASE_ABI		1
#define MT6797_MD1_PRERELEASE_MAX_POLLS	3

enum mt6797_md1_prerelease_state {
	MT6797_MD1_PRERELEASE_OFF,
	MT6797_MD1_PRERELEASE_ERROR,
};

/* All three fields are saved for every rail, even where 3.18 does not write
 * its selector. This makes restoration independent of a future value change.
 */
#define MT6797_MD1_PMIC_FIELDS_PER_RAIL	4
#define MT6797_MD1_PMIC_FIELD_COUNT		(3 * MT6797_MD1_PMIC_FIELDS_PER_RAIL)

struct mt6797_md1_pmic_snapshot {
	u16 value[MT6797_MD1_PMIC_FIELD_COUNT];
	bool valid;
};

struct mt6797_md1_spm_snapshot {
	u32 pwr_con;
	bool valid;
};

/* Only SPM_MDBSI_CON[2:0] is owned by the vendor RF clock-buffer path. */
struct mt6797_md1_clkbuf_snapshot {
	u32 mdbsi_con_low;
	bool valid;
};

struct mt6797_md1_prerelease_ops {
	u32 abi;
	int (*pmic_on)(void *context);
	int (*pmic_off)(void *context);
	int (*domain_on)(void *context);
	int (*domain_off)(void *context);
	int (*poll)(void *context);
};

struct mt6797_md1_prerelease {
	const struct mt6797_md1_prerelease_ops *ops;
	void *context;
	struct mutex lock;
	enum mt6797_md1_prerelease_state state;
};

int mt6797_md1_prerelease_init(struct mt6797_md1_prerelease *prerelease,
		const struct mt6797_md1_prerelease_ops *ops, void *context);
int mt6797_md1_prerelease_run(struct mt6797_md1_prerelease *prerelease);
enum mt6797_md1_prerelease_state
mt6797_md1_prerelease_state(struct mt6797_md1_prerelease *prerelease);

int mt6797_md1_pmic_snapshot(struct regmap *pmic,
			     struct mt6797_md1_pmic_snapshot *snapshot);
int mt6797_md1_pmic_apply_vendor_values(struct regmap *pmic,
					 struct mt6797_md1_pmic_snapshot *snapshot);
int mt6797_md1_pmic_restore(struct regmap *pmic,
			    struct mt6797_md1_pmic_snapshot *snapshot);

int mt6797_md1_clkbuf_snapshot(struct regmap *spm,
			       struct mt6797_md1_clkbuf_snapshot *snapshot);
int mt6797_md1_clkbuf_set_modem_on(struct regmap *spm,
				   struct mt6797_md1_clkbuf_snapshot *snapshot);
int mt6797_md1_clkbuf_restore(struct regmap *spm,
			      struct mt6797_md1_clkbuf_snapshot *snapshot);

int mt6797_md1_spm_snapshot(void __iomem *spm,
			    struct mt6797_md1_spm_snapshot *snapshot);
int mt6797_md1_spm_power_on(void __iomem *spm,
			    struct mt6797_md1_spm_snapshot *snapshot);
int mt6797_md1_spm_restore(void __iomem *spm,
			    struct mt6797_md1_spm_snapshot *snapshot);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_PRERELEASE_H */
