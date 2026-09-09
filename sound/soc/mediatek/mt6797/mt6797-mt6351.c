// SPDX-License-Identifier: GPL-2.0
//
// mt6797-mt6351.c  --  MT6797 MT6351 ALSA SoC machine driver
//
// Copyright (c) 2018 MediaTek Inc.
// Author: KaiChieh Chuang <kaichieh.chuang@mediatek.com>

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/irqflags.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <sound/jack.h>
#include <sound/soc.h>

#include "mt6797-afe-common.h"

#define GEMINI_SPEAKER_AMPS 2
#define GEMINI_SPEAKER_MODE 3

struct mt6797_mt6351_priv {
	struct gpio_desc *mic_enable;
	struct snd_soc_card *card;
	/* Serializes DAPM requests with asynchronous jack notifications. */
	struct mutex speaker_lock;
	struct snd_soc_jack headset_jack;
	struct notifier_block jack_notifier;
	bool jack_present;
	bool speaker_requested;
	bool speaker_active;
	bool jack_notifier_registered;
	struct gpio_desc *speaker_enable[GEMINI_SPEAKER_AMPS];
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

static void gemini_speaker_disable(void *data)
{
	struct mt6797_mt6351_priv *priv = data;
	int i;

	for (i = 0; i < GEMINI_SPEAKER_AMPS; i++)
		gpiod_set_value(priv->speaker_enable[i], 0);
	usleep_range(500, 1000);
}

/* Caller serializes DAPM and jack-notifier changes with speaker_lock. */
static int gemini_speaker_power(struct snd_soc_card *card, bool enable)
{
	struct mt6797_mt6351_priv *priv = snd_soc_card_get_drvdata(card);
	unsigned long flags;
	int i, pulse, value[GEMINI_SPEAKER_AMPS];

	dev_info(card->dev, "Gemini speaker amplifiers before enable=%d mode=%d\n",
		 enable, GEMINI_SPEAKER_MODE);
	gemini_speaker_disable(priv);
	if (enable) {
		usleep_range(1000, 2000);
		/*
		 * AudDrv_GPIO_EXTAMP{,2}_Select() programs mode 3 with
		 * three low/high pulses, each phase lasting 2 us. Both
		 * descriptors are non-sleeping SoC GPIOs. Prevent local
		 * interrupts from extending the short programming pulses.
		 */
		local_irq_save(flags);
		for (i = 0; i < GEMINI_SPEAKER_AMPS; i++) {
			for (pulse = 0; pulse < GEMINI_SPEAKER_MODE; pulse++) {
				gpiod_set_value(priv->speaker_enable[i], 0);
				udelay(2);
				gpiod_set_value(priv->speaker_enable[i], 1);
				udelay(2);
			}
		}
		local_irq_restore(flags);
		msleep(25);
	}

	for (i = 0; i < GEMINI_SPEAKER_AMPS; i++)
		value[i] = gpiod_get_value(priv->speaker_enable[i]);
	dev_info(card->dev,
		 "Gemini speaker amplifiers after enable=%d readback=%d,%d\n",
		 enable, value[0], value[1]);
	for (i = 0; i < GEMINI_SPEAKER_AMPS; i++) {
		if (value[i] != enable) {
			gemini_speaker_disable(priv);
			return value[i] < 0 ? value[i] : -EIO;
		}
	}

	return 0;
}

static int gemini_speaker_update(struct mt6797_mt6351_priv *priv)
{
	bool enable = priv->speaker_requested && !priv->jack_present;
	int ret;

	if (enable == priv->speaker_active)
		return 0;
	ret = gemini_speaker_power(priv->card, enable);
	priv->speaker_active = !ret && enable;
	return ret;
}

static int gemini_speaker_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct mt6797_mt6351_priv *priv = snd_soc_card_get_drvdata(card);
	int ret;

	if (event != SND_SOC_DAPM_POST_PMU && event != SND_SOC_DAPM_PRE_PMD)
		return -EINVAL;
	mutex_lock(&priv->speaker_lock);
	priv->speaker_requested = event == SND_SOC_DAPM_POST_PMU;
	ret = gemini_speaker_update(priv);
	if (ret)
		priv->speaker_requested = false;
	mutex_unlock(&priv->speaker_lock);
	return ret;
}

static int gemini_jack_notify(struct notifier_block *nb, unsigned long status,
			      void *data)
{
	struct mt6797_mt6351_priv *priv = container_of(nb, struct mt6797_mt6351_priv,
						     jack_notifier);
	int ret;

	mutex_lock(&priv->speaker_lock);
	priv->jack_present = !!(status & SND_JACK_HEADPHONE);
	ret = gemini_speaker_update(priv);
	mutex_unlock(&priv->speaker_lock);
	if (ret)
		dev_err(priv->card->dev, "speaker update after headset event failed: %d\n", ret);
	return NOTIFY_OK;
}

static int gemini_late_probe(struct snd_soc_card *card)
{
	struct mt6797_mt6351_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_component *codec = NULL;
	int ret;

	ret = snd_soc_dapm_disable_pin(snd_soc_card_to_dapm(card), "Speaker");
	if (ret || !device_property_read_bool(card->dev, "mediatek,headset-jack"))
		return ret;
	for_each_card_rtds(card, rtd) {
		if (!strcmp(rtd->dai_link->name, "Primary Codec")) {
			codec = snd_soc_rtd_to_codec(rtd, 0)->component;
			break;
		}
	}
	if (!codec)
		return -ENODEV;
	/* No DAPM pins: speaker and headphones share the codec HP buffers. */
	ret = snd_soc_card_jack_new_pins(card, "Headset", SND_JACK_HEADSET,
					 &priv->headset_jack, NULL, 0);
	if (ret)
		return ret;
	ret = snd_jack_add_new_kctl(priv->headset_jack.jack, "Headphone Jack",
				    SND_JACK_HEADPHONE);
	if (ret)
		return ret;
	ret = snd_jack_add_new_kctl(priv->headset_jack.jack, "Headset Mic Jack",
				    SND_JACK_MICROPHONE);
	if (ret)
		return ret;
	/* Keep the amplifiers off until the initial detector report arrives. */
	priv->jack_present = true;
	priv->jack_notifier.notifier_call = gemini_jack_notify;
	snd_soc_jack_notifier_register(&priv->headset_jack, &priv->jack_notifier);
	priv->jack_notifier_registered = true;
	ret = snd_soc_component_set_jack(codec, &priv->headset_jack, NULL);
	if (ret) {
		snd_soc_jack_notifier_unregister(&priv->headset_jack, &priv->jack_notifier);
		priv->jack_notifier_registered = false;
	}
	return ret;
}

static int gemini_card_remove(struct snd_soc_card *card)
{
	struct mt6797_mt6351_priv *priv = snd_soc_card_get_drvdata(card);

	/* The codec remove callback has already stopped detector IRQs/work. */
	if (priv->jack_notifier_registered) {
		snd_soc_jack_notifier_unregister(&priv->headset_jack, &priv->jack_notifier);
		priv->jack_notifier_registered = false;
	}
	return 0;
}

static const struct snd_kcontrol_new gemini_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
};

/*
 * The Gemini vendor TurnOnADcPowerDCC() raises headphone_en (GPIO234)
 * before either DCC preamplifier is started, and lowers it after capture.
 * It is a shared capture gate, despite its legacy "headphone" name. Keep
 * it under DAPM ownership, including when both capture channels are used.
 * GPIO235 is a separate vendor-selected headset contact switch; it is not
 * needed to select the internal AIN0 microphone and is not driven here.
 */
static const struct snd_soc_dapm_widget gemini_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", gemini_speaker_event),
	SND_SOC_DAPM_SUPPLY("Gemini Mic Enable", SND_SOC_NOPM, 0, 0,
			    gemini_mic_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
};

static const struct snd_soc_dapm_route gemini_routes[] = {
	/* The shipped HAL uses int_hp_buf, then the external amplifiers. */
	{ "Speaker", NULL, "Headphone L" },
	{ "Speaker", NULL, "Headphone R" },
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
	struct snd_soc_dai_link_component *component;
	int ret, i, j;

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
		if (dai_link->num_platforms) {
			dai_link->platforms = devm_kmemdup(&pdev->dev, dai_link->platforms,
						   sizeof(*dai_link->platforms) *
						   dai_link->num_platforms, GFP_KERNEL);
			if (!dai_link->platforms)
				return -ENOMEM;
		}
		/* COMP_DUMMY() is empty until snd_soc_fill_dummy_dai(). */
		if (dai_link->num_codecs) {
			dai_link->codecs = devm_kmemdup(&pdev->dev, dai_link->codecs,
						sizeof(*dai_link->codecs) *
						dai_link->num_codecs, GFP_KERNEL);
			if (!dai_link->codecs)
				return -ENOMEM;
		}
	}

	card->dev = &pdev->dev;
	if (of_device_is_compatible(pdev->dev.of_node,
				    "planet,gemini-mt6797-mt6351")) {
		priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
		if (!priv)
			return -ENOMEM;
		priv->card = card;
		mutex_init(&priv->speaker_lock);
		priv->mic_enable = devm_gpiod_get(&pdev->dev, "microphone-enable",
						  GPIOD_OUT_LOW);
		if (IS_ERR(priv->mic_enable))
			return dev_err_probe(&pdev->dev, PTR_ERR(priv->mic_enable),
					     "Cannot acquire Gemini microphone gate\n");
		ret = devm_add_action_or_reset(&pdev->dev, gemini_mic_disable, priv);
		if (ret)
			return ret;
		ret = gpiod_count(&pdev->dev, "speaker-enable");
		if (ret != GEMINI_SPEAKER_AMPS)
			return dev_err_probe(&pdev->dev, ret < 0 ? ret : -EINVAL,
					     "Gemini requires two speaker enables\n");
		for (i = 0; i < GEMINI_SPEAKER_AMPS; i++) {
			priv->speaker_enable[i] = devm_gpiod_get_index(&pdev->dev,
					"speaker-enable", i, GPIOD_OUT_LOW);
			if (IS_ERR(priv->speaker_enable[i]))
				return dev_err_probe(&pdev->dev,
						     PTR_ERR(priv->speaker_enable[i]),
						     "Cannot acquire speaker enable\n");
			if (gpiod_cansleep(priv->speaker_enable[i]) ||
			    gpiod_is_active_low(priv->speaker_enable[i]))
				return dev_err_probe(&pdev->dev, -EINVAL,
					"Speaker enables need non-sleeping active-high GPIOs\n");
		}
		ret = devm_add_action_or_reset(&pdev->dev, gemini_speaker_disable, priv);
		if (ret)
			return ret;
		snd_soc_card_set_drvdata(card, priv);
		card->controls = gemini_controls;
		card->num_controls = ARRAY_SIZE(gemini_controls);
		card->late_probe = gemini_late_probe;
		card->remove = gemini_card_remove;
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
		for_each_link_platforms(dai_link, j, component) {
			if (!component->name)
				component->of_node = platform_node;
		}
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
		for_each_link_codecs(dai_link, j, component) {
			if (!component->name)
				component->of_node = codec_node;
		}
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
