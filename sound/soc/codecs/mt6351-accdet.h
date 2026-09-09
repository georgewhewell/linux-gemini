/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6351_ACCDET_H__
#define __MT6351_ACCDET_H__

struct platform_device;
struct regmap;
struct snd_soc_jack;
struct mt6351_accdet;

struct mt6351_accdet *mt6351_accdet_probe(struct platform_device *pdev,
					  struct regmap *regmap);
int mt6351_accdet_set_jack(struct mt6351_accdet *accdet,
			   struct snd_soc_jack *jack);
int mt6351_accdet_micbias(struct mt6351_accdet *accdet, bool enable);

#endif
