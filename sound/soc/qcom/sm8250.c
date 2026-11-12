// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <linux/input-event-codes.h>
#include "qdsp6/q6afe.h"
#include "common.h"
#include "usb_offload_utils.h"
#include "sdw.h"

#define MI2S_BCLK_RATE		1536000
#define TDM_BCLK_RATE		6144000

struct sm8250_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack usb_offload_jack;
	bool usb_offload_jack_setup;
	struct snd_soc_jack dp_jack;
	bool dp_idle;
	bool jack_setup;
};

/*
 * DP with no display attached: its back end must still prepare. PipeWire probes a UCM profile by
 * opening (hw_params, which prepares) every device in it, and one failure dropped the whole HiFi
 * profile, the speakers with it, whenever the session started without the TV (11-10). Both DAI
 * prepares fail then: q6afe's AFE start (the ADSP holds it until the DP audio engine runs, see
 * sm8250_snd_prepare) and hdmi-codec's (msm_dp_audio_prepare, -EINVAL while the DP link is off).
 * So while the link prepare finds the jack out, both are skipped: their DAI ops are wrapped once at
 * init and restored at exit. A stream here plays into nothing; WirePlumber does not pick a sink
 * whose jack is out.
 */
static struct snd_soc_dai_ops sm8250_dp_cpu_ops, sm8250_dp_codec_ops;
static const struct snd_soc_dai_ops *sm8250_dp_cpu_orig, *sm8250_dp_codec_orig;
static struct snd_soc_dai *sm8250_dp_cpu_dai, *sm8250_dp_codec_dai;

static bool sm8250_dp_idle(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return data->dp_idle;
}

static int sm8250_dp_cpu_prepare(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	if (sm8250_dp_idle(substream) || !sm8250_dp_cpu_orig->prepare)
		return 0;
	return sm8250_dp_cpu_orig->prepare(substream, dai);
}

static int sm8250_dp_codec_prepare(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	if (sm8250_dp_idle(substream) || !sm8250_dp_codec_orig->prepare)
		return 0;
	return sm8250_dp_codec_orig->prepare(substream, dai);
}

static void sm8250_dp_wrap(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *cpu = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec = snd_soc_rtd_to_codec(rtd, 0);

	if (sm8250_dp_cpu_dai || !cpu->driver->ops || !codec->driver->ops)
		return;

	sm8250_dp_cpu_orig = cpu->driver->ops;
	sm8250_dp_cpu_ops = *sm8250_dp_cpu_orig;
	sm8250_dp_cpu_ops.prepare = sm8250_dp_cpu_prepare;
	sm8250_dp_codec_orig = codec->driver->ops;
	sm8250_dp_codec_ops = *sm8250_dp_codec_orig;
	sm8250_dp_codec_ops.prepare = sm8250_dp_codec_prepare;

	cpu->driver->ops = &sm8250_dp_cpu_ops;
	codec->driver->ops = &sm8250_dp_codec_ops;
	sm8250_dp_cpu_dai = cpu;
	sm8250_dp_codec_dai = codec;
}

static void sm8250_dp_unwrap(void)
{
	if (!sm8250_dp_cpu_dai)
		return;
	sm8250_dp_cpu_dai->driver->ops = sm8250_dp_cpu_orig;
	sm8250_dp_codec_dai->driver->ops = sm8250_dp_codec_orig;
	sm8250_dp_cpu_dai = NULL;
	sm8250_dp_codec_dai = NULL;
}

static int sm8250_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case DISPLAY_PORT_RX: {
		int ret = qcom_snd_dp_jack_setup(rtd, &data->dp_jack, 0);

		if (!ret)
			sm8250_dp_wrap(rtd);
		return ret;
	}
	case USB_RX:
		return qcom_snd_usb_offload_jack_setup(rtd, &data->usb_offload_jack,
						       &data->usb_offload_jack_setup);
	default:
		return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
	}
}

static void sm8250_snd_exit(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	if (cpu_dai->id == USB_RX)
		qcom_snd_usb_offload_jack_remove(rtd,
						 &data->usb_offload_jack_setup);
	if (cpu_dai->id == DISPLAY_PORT_RX)
		sm8250_dp_unwrap();

}

static int sm8250_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int sm8250_snd_startup(struct snd_pcm_substream *substream)
{
	unsigned int fmt = SND_SOC_DAIFMT_BP_FP;
	unsigned int codec_dai_fmt = SND_SOC_DAIFMT_BC_FC;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	int i;

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case SECONDARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_SEC_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case TERTIARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case QUINARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_QUI_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case PRIMARY_TDM_TX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_IB_IF | SND_SOC_DAIFMT_DSP_B;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT,
			TDM_BCLK_RATE, SNDRV_PCM_STREAM_CAPTURE);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);

		snd_soc_dai_set_pll(codec_dai, 0, 1,
				    TDM_BCLK_RATE,
				    TDM_BCLK_RATE * 2);
		snd_soc_dai_set_sysclk(codec_dai, 1,
				       TDM_BCLK_RATE * 2,
				       SNDRV_PCM_STREAM_CAPTURE);

		break;
	case SECONDARY_TDM_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_DSP_A;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_SEC_TDM_IBIT,
			TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);

		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
			snd_soc_dai_set_sysclk(codec_dai, 0,
					       TDM_BCLK_RATE,
					       SNDRV_PCM_STREAM_PLAYBACK);
			snd_soc_component_set_sysclk(codec_dai->component,
					       0, 0,
					       TDM_BCLK_RATE,
					       SNDRV_PCM_STREAM_PLAYBACK);
		}

		break;
	case LPI_MI2S_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_INT0_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case LPI_MI2S_TX_3:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_INT3_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_CAPTURE);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	default:
		break;
	}

	return qcom_snd_sdw_startup(substream);
}

/*
 * The bit clock sm8250_snd_startup() turned on is an AFE clock vote held in
 * the ADSP. Left on after the stream, it keeps the ADSP from ever power
 * collapsing, and the SoC from reaching its low power modes in suspend.
 */
static void sm8250_snd_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int dir = SNDRV_PCM_STREAM_PLAYBACK;
	int clk_id;

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
		clk_id = Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT;
		break;
	case SECONDARY_MI2S_RX:
		clk_id = Q6AFE_LPASS_CLK_ID_SEC_MI2S_IBIT;
		break;
	case TERTIARY_MI2S_RX:
		clk_id = Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT;
		break;
	case QUINARY_MI2S_RX:
		clk_id = Q6AFE_LPASS_CLK_ID_QUI_MI2S_IBIT;
		break;
	case PRIMARY_TDM_TX_0:
		clk_id = Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT;
		dir = SNDRV_PCM_STREAM_CAPTURE;
		break;
	case SECONDARY_TDM_RX_0:
		clk_id = Q6AFE_LPASS_CLK_ID_SEC_TDM_IBIT;
		break;
	case LPI_MI2S_RX_0:
		clk_id = Q6AFE_LPASS_CLK_ID_INT0_MI2S_IBIT;
		break;
	case LPI_MI2S_TX_3:
		clk_id = Q6AFE_LPASS_CLK_ID_INT3_MI2S_IBIT;
		dir = SNDRV_PCM_STREAM_CAPTURE;
		break;
	default:
		clk_id = 0;
		break;
	}

	if (clk_id)
		snd_soc_dai_set_sysclk(cpu_dai, clk_id, 0, dir);

	qcom_snd_sdw_shutdown(substream);
}

static unsigned int tdm_slot_off[] = {
	0, 4, 8, 12, 16, 20, 24, 28
};

static int sm8250_snd_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	unsigned int channels;
	int ret;

	switch (cpu_dai->id) {
	case PRIMARY_TDM_TX_0:
		channels = params_channels(params);

		ret = snd_soc_dai_set_tdm_slot(cpu_dai, (1 << channels) - 1, 0, 8, 16);
		if (ret) {
			dev_err(cpu_dai->dev, "set tdm slot failed\n");
			return ret;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai,
						  channels, tdm_slot_off,
						  0, NULL);
		if (ret) {
			dev_err(cpu_dai->dev, "set channel map failed\n");
			return ret;
		}

		ret = snd_soc_dai_set_tdm_slot(codec_dai, 0xff, 0, 8, 16);
		if (ret) {
			dev_err(cpu_dai->dev, "set tdm slot failed\n");
			return ret;
		}

		break;
	case SECONDARY_TDM_RX_0:
		channels = params_channels(params);

		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, (1 << channels) - 1, 8, 16);
		if (ret) {
			dev_err(cpu_dai->dev, "set tdm slot failed\n");
			return ret;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai,
						  0, NULL,
						  channels, tdm_slot_off);
		if (ret) {
			dev_err(cpu_dai->dev, "set channel map failed\n");
			return ret;
		}

		break;
	default:
		break;
	}

	return 0;
}

static int sm8250_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	/*
	 * gts4lv (sdm670): the ADSP holds AFE_PORT_CMD_DEVICE_START for the DP port until the DP
	 * controller's audio engine runs, the q6afe ack times out (-110) and the port stays started
	 * (EALREADY from then on). ASoC prepares the CPU DAI (the AFE start) before the codec DAI
	 * (hdmi-codec -> msm_dp_audio_prepare, which enables the engine); downstream 4.9 prepared
	 * the codec first. The link prepare runs before both: start the engine here. Preparing
	 * hdmi-codec again afterwards only rewrites the same registers.
	 */
	if (cpu_dai->id == DISPLAY_PORT_RX) {
		struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);

		/* no display on the DP output: nothing to start, and both DAI prepares skip (sm8250_dp_wrap) */
		data->dp_idle = !(data->dp_jack.status & SND_JACK_AVOUT);
		if (data->dp_idle)
			return 0;

		if (codec_dai->driver->ops && codec_dai->driver->ops->prepare) {
			int ret = codec_dai->driver->ops->prepare(substream, codec_dai);

			if (ret)
				return ret;
		}
	}

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sm8250_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	/*
	 * gts4lv: the same in reverse. Shutdown runs the codec DAI (DP audio engine off) before the
	 * CPU DAI, and AFE_PORT_CMD_DEVICE_STOP then times out. hw_free comes before either: stop the
	 * AFE port while the engine still runs. q6afe's own shutdown is then a no-op, and a prepare
	 * after this starts the port again.
	 */
	if (cpu_dai->id == DISPLAY_PORT_RX && cpu_dai->driver->ops && cpu_dai->driver->ops->shutdown)
		cpu_dai->driver->ops->shutdown(substream, cpu_dai);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops sm8250_be_ops = {
	.startup = sm8250_snd_startup,
	.hw_params = sm8250_snd_hw_params,
	.shutdown = sm8250_snd_shutdown,
	.hw_free = sm8250_snd_hw_free,
	.prepare = sm8250_snd_prepare,
};

static void sm8250_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sm8250_snd_init;
			link->exit = sm8250_snd_exit;
			link->be_hw_params_fixup = sm8250_be_hw_params_fixup;
			link->ops = &sm8250_be_ops;
		}
	}
}

static int sm8250_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sm8250_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->owner = THIS_MODULE;
	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = of_device_get_match_data(dev);
	sm8250_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id snd_sm8250_dt_match[] = {
	{ .compatible = "fairphone,fp4-sndcard", .data = "sm7225" },
	{ .compatible = "fairphone,fp5-sndcard", .data = "qcm6490" },
	{ .compatible = "qcom,qrb2210-sndcard", .data = "qcm2290" },
	{ .compatible = "qcom,qrb4210-rb2-sndcard", .data = "sm4250" },
	{ .compatible = "qcom,qrb5165-rb5-sndcard", .data = "sm8250" },
	{ .compatible = "qcom,sdm660-sndcard", .data = "sdm660" },
	{ .compatible = "qcom,sm8250-sndcard", .data = "sm8250" },
	{}
};

MODULE_DEVICE_TABLE(of, snd_sm8250_dt_match);

static struct platform_driver snd_sm8250_driver = {
	.probe  = sm8250_platform_probe,
	.driver = {
		.name = "snd-sm8250",
		.of_match_table = snd_sm8250_dt_match,
	},
};
module_platform_driver(snd_sm8250_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SM8250 ASoC Machine Driver");
MODULE_LICENSE("GPL");
