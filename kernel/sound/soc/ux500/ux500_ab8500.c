/*
 * Copyright (C) ST-Ericsson SA 2010
 *
 * Author: Mikko J. Lehto <mikko.lehto@symbio.com>,
 *         Mikko Sarmanne <mikko.sarmanne@symbio.com>,
 *         Jarmo K. Kuronen <jarmo.kuronen@symbio.com>.
 *         Ola Lilja <ola.o.lilja@stericsson.com>
 *         for ST-Ericsson.
 *
 * License terms:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/regulator/consumer.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <mach/hardware.h>
#include "ux500_pcm.h"
#include "ux500_msp_dai.h"
#include "../codecs/ab8500_audio.h"

#define TX_SLOT_MONO	0x0008
#define TX_SLOT_STEREO	0x000a
#define RX_SLOT_MONO	0x0001
#define RX_SLOT_STEREO	0x0003
#define TX_SLOT_8CH	0x00FF
#define RX_SLOT_8CH	0x00FF

#define DEF_TX_SLOTS	TX_SLOT_STEREO
#define DEF_RX_SLOTS	RX_SLOT_MONO

#define DRIVERMODE_NORMAL	0
#define DRIVERMODE_CODEC_ONLY	1

static struct snd_soc_jack jack;
static bool vibra_on;

/* Power-control */
static DEFINE_MUTEX(power_lock);
static int ab8500_power_count;

/* Clocks */
/* audioclk -> intclk -> sysclk/ulpclk */
static int master_clock_sel;
static struct clk *clk_ptr_audioclk;
static struct clk *clk_ptr_intclk;
static struct clk *clk_ptr_sysclk;
static struct clk *clk_ptr_ulpclk;
static struct clk *clk_ptr_p1_pclk9;

/* Regulators */
static enum regulator_idx {
	REGULATOR_AUDIO,
	REGULATOR_DMIC,
	REGULATOR_AMIC1,
	REGULATOR_AMIC2
};
static struct regulator_bulk_data reg_info[4] = {
	{	.supply = "v-audio"	},
	{	.supply = "v-dmic"	},
	{	.supply = "v-amic1"	},
	{	.supply = "v-amic2"	}
};
static bool reg_enabled[4] =  {
	false,
	false,
	false,
	false
};
static int reg_claim[4];
enum amic_idx { AMIC_1A, AMIC_1B, AMIC_2 };
struct amic_conf {
	enum regulator_idx reg_id;
	bool enabled;
	char *name;
};

#ifdef CONFIG_MACH_GAVINI
static struct amic_conf amic_info[3] = {
	{ REGULATOR_AMIC2, false, "amic1a" },
	{ REGULATOR_AMIC1, false, "amic1b" },
	{ REGULATOR_AMIC2, false, "amic2" }
};
#else /* default config, valid for Janice */
static struct amic_conf amic_info[3] = {
	{ REGULATOR_AMIC1, false, "amic1a" },
	{ REGULATOR_AMIC1, false, "amic1b" },
	{ REGULATOR_AMIC2, false, "amic2" }
};
#endif

static DEFINE_MUTEX(amic_conf_lock);

static const char *enum_amic_reg_conf[2] = { "v-amic1", "v-amic2" };
static SOC_ENUM_SINGLE_EXT_DECL(soc_enum_amicconf, enum_amic_reg_conf);

/* Slot configuration */
static unsigned int tx_slots = DEF_TX_SLOTS;
static unsigned int rx_slots = DEF_RX_SLOTS;

/* Regulators */

static int enable_regulator(enum regulator_idx idx)
{
	int ret;

	if (reg_enabled[idx])
		return 0;

	ret = regulator_enable(reg_info[idx].consumer);
	if (ret != 0) {
		pr_err("%s: Failure to enable regulator '%s' (ret = %d)\n",
			__func__, reg_info[idx].supply, ret);
		return ret;
	};

	reg_enabled[idx] = true;
	pr_debug("%s: Enabled regulator '%s', status: %d, %d, %d, %d\n",
		__func__,
		reg_info[idx].supply,
		(int)reg_enabled[0],
		(int)reg_enabled[1],
		(int)reg_enabled[2],
		(int)reg_enabled[3]);
	return 0;
}

static void disable_regulator(enum regulator_idx idx)
{
	if (!reg_enabled[idx])
		return;

	regulator_disable(reg_info[idx].consumer);

	reg_enabled[idx] = false;
	pr_debug("%s: Disabled regulator '%s', status: %d, %d, %d, %d\n",
		__func__,
		reg_info[idx].supply,
		(int)reg_enabled[0],
		(int)reg_enabled[1],
		(int)reg_enabled[2],
		(int)reg_enabled[3]);
}

static int create_regulators(void)
{
	int i, status = 0;

	pr_debug("%s: Enter.\n", __func__);

	for (i = 0; i < ARRAY_SIZE(reg_info); ++i)
		reg_info[i].consumer = NULL;

	for (i = 0; i < ARRAY_SIZE(reg_info); ++i) {
		reg_info[i].consumer = regulator_get(NULL, reg_info[i].supply);
		if (IS_ERR(reg_info[i].consumer)) {
			status = PTR_ERR(reg_info[i].consumer);
			pr_err("%s: ERROR: Failed to get regulator '%s' (ret = %d)!\n",
				__func__, reg_info[i].supply, status);
			reg_info[i].consumer = NULL;
			goto err_get;
		}
	}

	return 0;

err_get:

	for (i = 0; i < ARRAY_SIZE(reg_info); ++i) {
		if (reg_info[i].consumer) {
			regulator_put(reg_info[i].consumer);
			reg_info[i].consumer = NULL;
		}
	}

	return status;
}

static int claim_amic_regulator(enum amic_idx amic_id)
{
	enum regulator_idx reg_id = amic_info[amic_id].reg_id;
	int ret = 0;

	reg_claim[reg_id]++;
	if (reg_claim[reg_id] > 1)
		goto cleanup;

	ret = enable_regulator(reg_id);
	if (ret < 0) {
		pr_err("%s: Failed to claim %s for %s (ret = %d)!",
			__func__, reg_info[reg_id].supply,
			amic_info[amic_id].name, ret);
		reg_claim[reg_id]--;
	}

cleanup:
	amic_info[amic_id].enabled = (ret == 0);

	return ret;
}

static void release_amic_regulator(enum amic_idx amic_id)
{
	enum regulator_idx reg_id = amic_info[amic_id].reg_id;

	reg_claim[reg_id]--;
	if (reg_claim[reg_id] <= 0) {
		disable_regulator(reg_id);
		reg_claim[reg_id] = 0;
	}

	amic_info[amic_id].enabled = false;
}

/* Power/clock control */

static int ux500_ab8500_power_control_inc(void)
{
	int ret;

	mutex_lock(&power_lock);

	ab8500_power_count++;
	pr_debug("%s: ab8500_power_count changed from %d to %d",
		__func__,
		ab8500_power_count-1,
		ab8500_power_count);

	if (ab8500_power_count == 1) {
		/* Turn on audio-regulator */
		ret = enable_regulator(REGULATOR_AUDIO);

		/* Enable audio-clock */
		ret = clk_set_parent(clk_ptr_intclk,
				(master_clock_sel == 0) ? clk_ptr_sysclk : clk_ptr_ulpclk);
		if (ret) {
			pr_err("%s: ERROR: Setting master-clock to %s failed (ret = %d)!",
				__func__,
				(master_clock_sel == 0) ? "SYSCLK" : "ULPCLK",
				ret);
			return ret;
		}
		pr_debug("%s: Enabling master-clock (%s).",
			__func__,
			(master_clock_sel == 0) ? "SYSCLK" : "ULPCLK");
		ret = clk_enable(clk_ptr_audioclk);
		if (ret) {
			pr_err("%s: ERROR: clk_enable failed (ret = %d)!", __func__, ret);
			ab8500_power_count = 0;
			return ret;
		}

		/* Power on audio-parts of AB8500 */
		ab8500_audio_power_control(true);
	}

	mutex_unlock(&power_lock);

	return 0;
}

static void ux500_ab8500_power_control_dec(void)
{
	mutex_lock(&power_lock);

	ab8500_power_count--;

	pr_debug("%s: ab8500_power_count changed from %d to %d",
		__func__,
		ab8500_power_count+1,
		ab8500_power_count);

	if (ab8500_power_count == 0) {
		/* Power off audio-parts of AB8500 */
		ab8500_audio_power_control(false);

		/* Disable audio-clock */
		pr_debug("%s: Disabling master-clock (%s).",
			__func__,
			(master_clock_sel == 0) ? "SYSCLK" : "ULPCLK");
		clk_disable(clk_ptr_audioclk);

		/* Turn off audio-regulator */
		disable_regulator(REGULATOR_AUDIO);
	}

	mutex_unlock(&power_lock);
}

/* Controls - Non-DAPM Non-ASoC */

static int mclk_input_control_info(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 2;
	if (uinfo->value.enumerated.item) {
		uinfo->value.enumerated.item = 1;
		strcpy(uinfo->value.enumerated.name, "ULPCLK");
	} else {
		strcpy(uinfo->value.enumerated.name, "SYSCLK");
	}
	return 0;
}

static int mclk_input_control_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.enumerated.item[0] = master_clock_sel;
	return 0;
}

static int mclk_input_control_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	unsigned int val;

	val = (ucontrol->value.enumerated.item[0] != 0);
	if (master_clock_sel == val)
		return 0;

	master_clock_sel = val;

	return 1;
}

static const struct snd_kcontrol_new mclk_input_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Master Clock Select",
	.index = 0,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = mclk_input_control_info,
	.get = mclk_input_control_get,
	.put = mclk_input_control_put,
	.private_value = 1 /* ULPCLK */
};

static int amic_reg_control_get(struct snd_ctl_elem_value *ucontrol,
		enum amic_idx amic_id)
{
	ucontrol->value.integer.value[0] =
		(amic_info[amic_id].reg_id == REGULATOR_AMIC2);

	return 0;
}

static int amic_reg_control_put(struct snd_ctl_elem_value *ucontrol,
		enum amic_idx amic_id)
{
	enum regulator_idx old_reg_id, new_reg_id;
	int ret = 0;

	if (ucontrol->value.integer.value[0] == 0)
		new_reg_id = REGULATOR_AMIC1;
	else
		new_reg_id = REGULATOR_AMIC2;

	mutex_lock(&amic_conf_lock);

	old_reg_id = amic_info[amic_id].reg_id;
	if (old_reg_id == new_reg_id)
		goto cleanup;

	if (!amic_info[amic_id].enabled) {
		amic_info[amic_id].reg_id = new_reg_id;
		goto cleanup;
	}

	release_amic_regulator(amic_id);
	amic_info[amic_id].reg_id = new_reg_id;
	ret = claim_amic_regulator(amic_id);

cleanup:
	mutex_unlock(&amic_conf_lock);

	return (ret < 0) ? 0 : 1;
}

static int amic1a_reg_control_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	return amic_reg_control_get(ucontrol, AMIC_1A);
}

static int amic1a_reg_control_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	return amic_reg_control_put(ucontrol, AMIC_1A);
}

static int amic1b_reg_control_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	return amic_reg_control_get(ucontrol, AMIC_1B);
}

static int amic1b_reg_control_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	return amic_reg_control_put(ucontrol, AMIC_1B);
}

static int amic2_reg_control_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	return amic_reg_control_get(ucontrol, AMIC_2);
}

static int amic2_reg_control_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	return amic_reg_control_put(ucontrol, AMIC_2);
}

static const struct snd_kcontrol_new mic1a_regulator_control = \
	SOC_ENUM_EXT("Mic 1A Regulator", soc_enum_amicconf,
		amic1a_reg_control_get, amic1a_reg_control_put);
static const struct snd_kcontrol_new mic1b_regulator_control = \
	SOC_ENUM_EXT("Mic 1B Regulator", soc_enum_amicconf,
		amic1b_reg_control_get, amic1b_reg_control_put);
static const struct snd_kcontrol_new mic2_regulator_control = \
	SOC_ENUM_EXT("Mic 2 Regulator", soc_enum_amicconf,
		amic2_reg_control_get, amic2_reg_control_put);

/* DAPM-events */

static int dapm_audioreg_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *k, int event)
{
	if (SND_SOC_DAPM_EVENT_ON(event))
		ux500_ab8500_power_control_inc();
	else
		ux500_ab8500_power_control_dec();

	return 0;
}



static int dapm_amicreg_event(enum amic_idx amic_id, int event)
{
	int ret = 0;

	mutex_lock(&amic_conf_lock);

	if (SND_SOC_DAPM_EVENT_ON(event))
		ret = claim_amic_regulator(amic_id);
	else if (amic_info[amic_id].enabled)
		release_amic_regulator(amic_id);

	mutex_unlock(&amic_conf_lock);

	return ret;
}

static int dapm_amic1areg_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *k, int event)
{
	return dapm_amicreg_event(AMIC_1A, event);
}

static int dapm_amic1breg_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *k, int event)
{
	return dapm_amicreg_event(AMIC_1B, event);
}

static int dapm_amic2reg_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *k, int event)
{
	return dapm_amicreg_event(AMIC_2, event);
}

static int dapm_dmicreg_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *k, int event)
{
	int ret = 0;

	if (SND_SOC_DAPM_EVENT_ON(event))
		ret = enable_regulator(REGULATOR_DMIC);
	else
		disable_regulator(REGULATOR_DMIC);

	return ret;
}

/* DAPM-widgets */

static const struct snd_soc_dapm_widget ux500_ab8500_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY("AUDIO Regulator",
			SND_SOC_NOPM, 0, 0, dapm_audioreg_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("AMIC1A Regulator",
			SND_SOC_NOPM, 0, 0, dapm_amic1areg_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("AMIC1B Regulator",
			SND_SOC_NOPM, 0, 0, dapm_amic1breg_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("AMIC2 Regulator",
			SND_SOC_NOPM, 0, 0, dapm_amic2reg_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("DMIC Regulator",
			SND_SOC_NOPM, 0, 0, dapm_dmicreg_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
};

/* DAPM-routes */

static const struct snd_soc_dapm_route ux500_ab8500_dapm_intercon[] = {

	/* Power AB8500 audio-block when AD/DA is active */
	{"DAC", NULL, "AUDIO Regulator"},
	{"ADC", NULL, "AUDIO Regulator"},

	/* Power AB8500 audio-block when LineIn is active */
	{"LINL Enable", NULL, "AUDIO Regulator"},
	{"LINR Enable", NULL, "AUDIO Regulator"},

	/* Power AMIC1-regulator when MIC1 is enabled */
	{"MIC1A Input", NULL, "AMIC1A Regulator"},
	{"MIC1B Input", NULL, "AMIC1B Regulator"},

	/* Power AMIC2-regulator when MIC2 is enabled */
	{"MIC2 Input", NULL, "AMIC2 Regulator"},

	/* Power DMIC-regulator when any digital mic is enabled */
	{"DMic 1", NULL, "DMIC Regulator"},
	{"DMic 2", NULL, "DMIC Regulator"},
	{"DMic 3", NULL, "DMIC Regulator"},
	{"DMic 4", NULL, "DMIC Regulator"},
	{"DMic 5", NULL, "DMIC Regulator"},
	{"DMic 6", NULL, "DMIC Regulator"},
};

static int add_widgets(struct snd_soc_codec *codec)
{
	int ret;

	ret = snd_soc_dapm_new_controls(codec,
			ux500_ab8500_dapm_widgets,
			ARRAY_SIZE(ux500_ab8500_dapm_widgets));
	if (ret < 0) {
		pr_err("%s: Failed to create DAPM controls (%d).\n",
			__func__, ret);
		return ret;
	}

	ret = snd_soc_dapm_add_routes(codec,
				ux500_ab8500_dapm_intercon,
				ARRAY_SIZE(ux500_ab8500_dapm_intercon));
	if (ret < 0) {
		pr_err("%s: Failed to add DAPM routes (%d).\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

/* ASoC */

int ux500_ab8500_startup(struct snd_pcm_substream *substream)
{
	int ret = 0;

	pr_info("%s: Enter\n", __func__);

	/* Enable p1_pclk9-clock (needed in burst-mode) */
	ret = clk_enable(clk_ptr_p1_pclk9);
	if (ret) {
		pr_err("%s: ERROR: clk_enable failed (ret = %d)!", __func__, ret);
		return ret;
	}

	return 0;
}

void ux500_ab8500_shutdown(struct snd_pcm_substream *substream)
{
	pr_info("%s: Enter\n", __func__);

	/* Reset slots configuration to default(s) */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		tx_slots = DEF_TX_SLOTS;
	else
		rx_slots = DEF_RX_SLOTS;
}

int ux500_ab8500_hw_params(struct snd_pcm_substream *substream,
			struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	unsigned int fmt, fmt_if1;
	int channels, ret = 0, slots, slot_width, driver_mode;
	bool streamIsPlayback;

	pr_debug("%s: Enter\n", __func__);

	pr_debug("%s: substream->pcm->name = %s\n"
		"substream->pcm->id = %s.\n"
		"substream->name = %s.\n"
		"substream->number = %d.\n",
		__func__,
		substream->pcm->name,
		substream->pcm->id,
		substream->name,
		substream->number);

	channels = params_channels(params);

	/* Setup codec depending on driver-mode */
	driver_mode = (channels == 8) ?
		DRIVERMODE_CODEC_ONLY : DRIVERMODE_NORMAL;
	pr_debug("%s: Driver-mode: %s.\n",
		__func__,
		(driver_mode == DRIVERMODE_NORMAL) ? "NORMAL" : "CODEC_ONLY");
	if (driver_mode == DRIVERMODE_NORMAL) {
		ab8500_audio_set_bit_delay(codec_dai, 0);
		ab8500_audio_set_word_length(codec_dai, 16);
		fmt = SND_SOC_DAIFMT_DSP_B |
			SND_SOC_DAIFMT_CBM_CFM |
			SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CONT;
	} else {
		ab8500_audio_set_bit_delay(codec_dai, 1);
		ab8500_audio_set_word_length(codec_dai, 20);
		fmt = SND_SOC_DAIFMT_DSP_B |
			SND_SOC_DAIFMT_CBM_CFM |
			SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_GATED;
	}

	ret = snd_soc_dai_set_fmt(codec_dai, fmt);
	if (ret < 0) {
		pr_err("%s: ERROR: snd_soc_dai_set_fmt failed for codec_dai (ret = %d)!\n",
			__func__,
			ret);
		return ret;
	}

	ret = snd_soc_dai_set_fmt(cpu_dai, fmt);
	if (ret < 0) {
		pr_err("%s: ERROR: snd_soc_dai_set_fmt for cpu_dai (ret = %d)!\n",
			__func__,
			ret);
		return ret;
	}

	/* Setup TDM-slots */

	streamIsPlayback = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);
	switch (channels) {
	case 1:
		slots = 16;
		slot_width = 16;
		tx_slots = (streamIsPlayback) ? TX_SLOT_MONO : 0;
		rx_slots = (streamIsPlayback) ? 0 : RX_SLOT_MONO;
		break;
	case 2:
		slots = 16;
		slot_width = 16;
		tx_slots = (streamIsPlayback) ? TX_SLOT_STEREO : 0;
		rx_slots = (streamIsPlayback) ? 0 : RX_SLOT_STEREO;
		break;
	case 8:
		slots = 16;
		slot_width = 16;
		tx_slots = (streamIsPlayback) ? TX_SLOT_8CH : 0;
		rx_slots = (streamIsPlayback) ? 0 : RX_SLOT_8CH;
		break;
	default:
		return -EINVAL;
	}

	pr_debug("%s: CPU-DAI TDM: TX=0x%04X RX=0x%04x\n",
		__func__, tx_slots, rx_slots);
	ret = snd_soc_dai_set_tdm_slot(cpu_dai, tx_slots, rx_slots, slots, slot_width);
	if (ret)
		return ret;

	pr_debug("%s: CODEC-DAI TDM: TX=0x%04X RX=0x%04x\n",
		__func__, tx_slots, rx_slots);
	ret = snd_soc_dai_set_tdm_slot(codec_dai, tx_slots, rx_slots, slots, slot_width);
	if (ret)
		return ret;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		pr_debug("%s: Setup IF1 for FM-radio.\n", __func__);
		fmt_if1 = SND_SOC_DAIFMT_CBM_CFM | SND_SOC_DAIFMT_I2S;
		ret = ab8500_audio_setup_if1(codec_dai->codec, fmt_if1, 16, 1);
		if (ret)
			return ret;
	}

	return 0;
}

struct snd_soc_ops ux500_ab8500_ops[] = {
	{
	.hw_params = ux500_ab8500_hw_params,
	.startup = ux500_ab8500_startup,
	.shutdown = ux500_ab8500_shutdown,
	}
};

int ux500_ab8500_machine_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	int ret;

	pr_debug("%s Enter.\n", __func__);

	ret = snd_soc_jack_new(codec,
			"AB8500 Hs Status",
			SND_JACK_HEADPHONE     |
			SND_JACK_MICROPHONE    |
			SND_JACK_HEADSET       |
			SND_JACK_LINEOUT       |
			SND_JACK_MECHANICAL    |
			SND_JACK_VIDEOOUT,
			&jack);
	if (ret < 0) {
		pr_err("%s: ERROR: Failed to create Jack (ret = %d)!\n", __func__, ret);
		return ret;
	}

	/* Add controls */
	snd_ctl_add(codec->card->snd_card, snd_ctl_new1(&mclk_input_control, codec));
	snd_ctl_add(codec->card->snd_card, snd_ctl_new1(
		&mic1a_regulator_control, codec));
	snd_ctl_add(codec->card->snd_card, snd_ctl_new1(
		&mic1b_regulator_control, codec));
	snd_ctl_add(codec->card->snd_card, snd_ctl_new1(
		&mic2_regulator_control, codec));

	/* Get references to clock-nodes */
	clk_ptr_sysclk = NULL;
	clk_ptr_ulpclk = NULL;
	clk_ptr_intclk = NULL;
	clk_ptr_audioclk = NULL;
	clk_ptr_p1_pclk9 = NULL;
	clk_ptr_sysclk = clk_get(codec->dev, "sysclk");
	if (IS_ERR(clk_ptr_sysclk)) {
		pr_err("ERROR: clk_get failed (ret = %d)!", -EFAULT);
		return -EFAULT;
	}
	clk_ptr_ulpclk = clk_get(codec->dev, "ulpclk");
	if (IS_ERR(clk_ptr_sysclk)) {
		pr_err("ERROR: clk_get failed (ret = %d)!", -EFAULT);
		return -EFAULT;
	}
	clk_ptr_intclk = clk_get(codec->dev, "intclk");
	if (IS_ERR(clk_ptr_audioclk)) {
		pr_err("ERROR: clk_get failed (ret = %d)!", -EFAULT);
		return -EFAULT;
	}
	clk_ptr_audioclk = clk_get(codec->dev, "audioclk");
	if (IS_ERR(clk_ptr_audioclk)) {
		pr_err("ERROR: clk_get failed (ret = %d)!", -EFAULT);
		return -EFAULT;
	}
	clk_ptr_p1_pclk9 = clk_get_sys("gpio.1", NULL);
	if (IS_ERR(clk_ptr_p1_pclk9)) {
		pr_err("ERROR: clk_get_sys(gpio.1) failed (ret = %d)!", -EFAULT);
		return -EFAULT;
	}

	/* Set intclk default parent to ulpclk */
	ret = clk_set_parent(clk_ptr_intclk, clk_ptr_ulpclk);
	if (ret) {
		pr_err("%s: ERROR: Setting intclk parent to ulpclk failed (ret = %d)!",
			__func__,
			ret);
		return -EFAULT;
	}

	master_clock_sel = 1;

	ab8500_power_count = 0;

	reg_claim[REGULATOR_AMIC1] = 0;
	reg_claim[REGULATOR_AMIC2] = 0;

	/* Add DAPM-widgets */
	ret = add_widgets(codec);
	if (ret < 0) {
		pr_err("%s: Failed add widgets (%d).\n", __func__, ret);
		return ret;
	}

	return 0;
}

int ux500_ab8500_soc_machine_drv_init(void)
{
	int status = 0;

	pr_debug("%s: Enter.\n", __func__);

	status = create_regulators();
	if (status < 0) {
		pr_err("%s: ERROR: Failed to instantiate regulators (ret = %d)!\n",
			__func__, status);
		return status;
	}

	vibra_on = false;

	return 0;
}

void ux500_ab8500_soc_machine_drv_cleanup(void)
{
	pr_debug("%s: Enter.\n", __func__);

	regulator_bulk_free(ARRAY_SIZE(reg_info), reg_info);

	if (clk_ptr_sysclk != NULL)
		clk_put(clk_ptr_sysclk);
	if (clk_ptr_ulpclk != NULL)
		clk_put(clk_ptr_ulpclk);
	if (clk_ptr_intclk != NULL)
		clk_put(clk_ptr_intclk);
	if (clk_ptr_audioclk != NULL)
		clk_put(clk_ptr_audioclk);
}

/* Extended interface */

void ux500_ab8500_audio_pwm_vibra(unsigned char speed_left_pos,
			unsigned char speed_left_neg,
			unsigned char speed_right_pos,
			unsigned char speed_right_neg)
{
	bool vibra_on_new;

	vibra_on_new = speed_left_pos | speed_left_neg | speed_right_pos | speed_right_neg;
	if ((!vibra_on_new) && (vibra_on)) {
		pr_debug("%s: PWM-vibra off.\n", __func__);
		vibra_on = false;

		ux500_ab8500_power_control_dec();
	}

	if ((vibra_on_new) && (!vibra_on)) {
		pr_debug("%s: PWM-vibra on.\n", __func__);
		vibra_on = true;

		ux500_ab8500_power_control_inc();
	}

	ab8500_audio_pwm_vibra(speed_left_pos,
			speed_left_neg,
			speed_right_pos,
			speed_right_neg);
}

void ux500_ab8500_jack_report(int value)
{
	if (jack.jack)
		snd_soc_jack_report(&jack, value, 0xFF);
}
EXPORT_SYMBOL_GPL(ux500_ab8500_jack_report);

