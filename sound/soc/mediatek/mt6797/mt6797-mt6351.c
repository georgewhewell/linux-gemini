// SPDX-License-Identifier: GPL-2.0
//
// mt6797-mt6351.c  --  MT6797 MT6351 ALSA SoC machine driver
//
// Copyright (c) 2018 MediaTek Inc.
// Author: KaiChieh Chuang <kaichieh.chuang@mediatek.com>

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <sound/soc.h>

#include "mt6797-afe-common.h"

struct mt6797_mt6351_priv {
	struct gpio_desc *mic_enable;
};

static void gemini_mic_disable(void *data)
{
	struct mt6797_mt6351_priv *priv = data;

	gpiod_set_value_cansleep(priv->mic_enable, 0);
}

static int gemini_mic_event(struct snd_soc_dapm_widget *w,
			    struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct mt6797_mt6351_priv *priv = snd_soc_card_get_drvdata(card);
	int enable, value;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		enable = 1;
		break;
	case SND_SOC_DAPM_POST_PMD:
		enable = 0;
		break;
	default:
		return -EINVAL;
	}

	dev_info(card->dev, "Gemini microphone gate before enable=%d\n", enable);
	gpiod_set_value_cansleep(priv->mic_enable, enable);
	value = gpiod_get_value_cansleep(priv->mic_enable);
	dev_info(card->dev, "Gemini microphone gate after enable=%d readback=%d\n",
		 enable, value);
	if (value != enable) {
		gemini_mic_disable(priv);
		return value < 0 ? value : -EIO;
	}

	return 0;
}

/*
 * The Gemini vendor TurnOnADcPowerDCC() raises headphone_en (GPIO234)
 * before either DCC preamplifier is started, and lowers it after capture.
 * It is a shared capture gate, despite its legacy "headphone" name. Keep
 * it under DAPM ownership, including when both capture channels are used.
 * GPIO235 is a separate vendor-selected headset contact switch; it is not
 * needed to select the internal AIN0 microphone and is not driven here.
 */
static const struct snd_soc_dapm_widget gemini_widgets[] = {
	SND_SOC_DAPM_SUPPLY("Gemini Mic Enable", SND_SOC_NOPM, 0, 0,
			    gemini_mic_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
};

static const struct snd_soc_dapm_route gemini_routes[] = {
	{ "PGA L", NULL, "Gemini Mic Enable" },
	{ "PGA R", NULL, "Gemini Mic Enable" },
};

SND_SOC_DAILINK_DEFS(playback_1,
	DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback_2,
	DAILINK_COMP_ARRAY(COMP_CPU("DL2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback_3,
	DAILINK_COMP_ARRAY(COMP_CPU("DL3")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_1,
	DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_2,
	DAILINK_COMP_ARRAY(COMP_CPU("UL2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_3,
	DAILINK_COMP_ARRAY(COMP_CPU("UL3")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_mono_1,
	DAILINK_COMP_ARRAY(COMP_CPU("UL_MONO_1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(hostless_lpbk,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless LPBK DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(hostless_speech,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless Speech DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(primary_codec,
	DAILINK_COMP_ARRAY(COMP_CPU("ADDA")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "mt6351-snd-codec-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(pcm1,
	DAILINK_COMP_ARRAY(COMP_CPU("PCM 1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(pcm2,
	DAILINK_COMP_ARRAY(COMP_CPU("PCM 2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link mt6797_mt6351_dai_links[] = {
	/* FE */
	{
		.name = "Playback_1",
		.stream_name = "Playback_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback_1),
	},
	{
		.name = "Playback_2",
		.stream_name = "Playback_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback_2),
	},
	{
		.name = "Playback_3",
		.stream_name = "Playback_3",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback_3),
	},
	{
		.name = "Capture_1",
		.stream_name = "Capture_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture_1),
	},
	{
		.name = "Capture_2",
		.stream_name = "Capture_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture_2),
	},
	{
		.name = "Capture_3",
		.stream_name = "Capture_3",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture_3),
	},
	{
		.name = "Capture_Mono_1",
		.stream_name = "Capture_Mono_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture_mono_1),
	},
	{
		.name = "Hostless_LPBK",
		.stream_name = "Hostless_LPBK",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_lpbk),
	},
	{
		.name = "Hostless_Speech",
		.stream_name = "Hostless_Speech",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_speech),
	},
	/* BE */
	{
		.name = "Primary Codec",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(primary_codec),
	},
	{
		.name = "PCM 1",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(pcm1),
	},
	{
		.name = "PCM 2",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(pcm2),
	},
};

static struct snd_soc_card mt6797_mt6351_card = {
	.name = "mt6797-mt6351",
	.owner = THIS_MODULE,
	.dai_link = mt6797_mt6351_dai_links,
	.num_links = ARRAY_SIZE(mt6797_mt6351_dai_links),
};

static int mt6797_mt6351_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct mt6797_mt6351_priv *priv;
	struct device_node *platform_node, *codec_node;
	struct snd_soc_dai_link *dai_link;
	int ret, i;

	card = devm_kmemdup(&pdev->dev, &mt6797_mt6351_card,
			    sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;
	card->dai_link = devm_kmemdup(&pdev->dev, mt6797_mt6351_dai_links,
				      sizeof(mt6797_mt6351_dai_links), GFP_KERNEL);
	if (!card->dai_link)
		return -ENOMEM;
	/* Each card owns the component references populated below. */
	for_each_card_prelinks(card, i, dai_link) {
		dai_link->platforms = devm_kmemdup(&pdev->dev, dai_link->platforms,
						   sizeof(*dai_link->platforms) *
						   dai_link->num_platforms, GFP_KERNEL);
		dai_link->codecs = devm_kmemdup(&pdev->dev, dai_link->codecs,
						sizeof(*dai_link->codecs) *
						dai_link->num_codecs, GFP_KERNEL);
		if (!dai_link->platforms || !dai_link->codecs)
			return -ENOMEM;
	}

	card->dev = &pdev->dev;
	if (of_device_is_compatible(pdev->dev.of_node,
				    "planet,gemini-mt6797-mt6351")) {
		priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
		if (!priv)
			return -ENOMEM;
		priv->mic_enable = devm_gpiod_get(&pdev->dev, "microphone-enable",
						  GPIOD_OUT_LOW);
		if (IS_ERR(priv->mic_enable))
			return dev_err_probe(&pdev->dev, PTR_ERR(priv->mic_enable),
					     "Cannot acquire Gemini microphone gate\n");
		ret = devm_add_action_or_reset(&pdev->dev, gemini_mic_disable, priv);
		if (ret)
			return ret;
		snd_soc_card_set_drvdata(card, priv);
		card->dapm_widgets = gemini_widgets;
		card->num_dapm_widgets = ARRAY_SIZE(gemini_widgets);
		card->dapm_routes = gemini_routes;
		card->num_dapm_routes = ARRAY_SIZE(gemini_routes);
	}

	platform_node = of_parse_phandle(pdev->dev.of_node,
					 "mediatek,platform", 0);
	if (!platform_node) {
		dev_err(&pdev->dev, "Property 'platform' missing or invalid\n");
		return -EINVAL;
	}
	for_each_card_prelinks(card, i, dai_link) {
		if (dai_link->platforms->name)
			continue;
		dai_link->platforms->of_node = platform_node;
	}

	codec_node = of_parse_phandle(pdev->dev.of_node,
				      "mediatek,audio-codec", 0);
	if (!codec_node) {
		dev_err(&pdev->dev,
			"Property 'audio-codec' missing or invalid\n");
		ret = -EINVAL;
		goto put_platform_node;
	}
	for_each_card_prelinks(card, i, dai_link) {
		if (dai_link->codecs->name)
			continue;
		dai_link->codecs->of_node = codec_node;
	}

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		dev_err(&pdev->dev, "%s snd_soc_register_card fail %d\n",
			__func__, ret);

	of_node_put(codec_node);
put_platform_node:
	of_node_put(platform_node);
	return ret;
}

#ifdef CONFIG_OF
static const struct of_device_id mt6797_mt6351_dt_match[] = {
	{.compatible = "planet,gemini-mt6797-mt6351",},
	{.compatible = "mediatek,mt6797-mt6351-sound",},
	{}
};
MODULE_DEVICE_TABLE(of, mt6797_mt6351_dt_match);
#endif

static struct platform_driver mt6797_mt6351_driver = {
	.driver = {
		.name = "mt6797-mt6351",
#ifdef CONFIG_OF
		.of_match_table = mt6797_mt6351_dt_match,
#endif
	},
	.probe = mt6797_mt6351_dev_probe,
};

module_platform_driver(mt6797_mt6351_driver);

/* Module information */
MODULE_DESCRIPTION("MT6797 MT6351 ALSA SoC machine driver");
MODULE_AUTHOR("KaiChieh Chuang <kaichieh.chuang@mediatek.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("mt6797 mt6351 soc card");
