// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 angellfear
/* Minimal FS2105 ASoC codec for Yandex Chiron playback. */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define FS2105_REG_SYSCTRL 0x11
#define FS2105_POWER_DOWN  0x0000
#define FS2105_POWER_UP    0x0300

struct fs2105_priv {
	struct regmap *regmap;
	struct i2c_client *client;
	int reset_gpio;
	bool configured;
};

static const struct regmap_config fs2105_regmap = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = 0xff,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type = REGCACHE_NONE,
};

static int fs2105_apply_text_firmware(struct fs2105_priv *fs2105,
				       const char *name)
{
	const struct firmware *fw;
	char *text, *cursor, *line;
	unsigned int addr, reg, val, delay;
	int ret, line_no = 0, writes = 0;

	ret = request_firmware(&fw, name, &fs2105->client->dev);
	if (ret) {
		dev_err(&fs2105->client->dev, "failed to load %s: %d\n",
			name, ret);
		return ret;
	}

	text = kmalloc(fw->size + 1, GFP_KERNEL);
	if (!text) {
		release_firmware(fw);
		return -ENOMEM;
	}
	memcpy(text, fw->data, fw->size);
	text[fw->size] = '\0';
	cursor = text;

	while ((line = strsep(&cursor, "\n")) != NULL) {
		line_no++;
		if (sscanf(line, "w %x %x %x", &addr, &reg, &val) == 3) {
			if (addr != fs2105->client->addr)
				continue;
			ret = regmap_write(fs2105->regmap, reg, val);
			if (ret) {
				dev_err(&fs2105->client->dev,
					"%s:%d: write %02x=%04x failed: %d\n",
					name, line_no, reg, val, ret);
				goto out;
			}
			writes++;
		} else if (sscanf(line, "d %x", &delay) == 1) {
			msleep(delay);
		}
	}

	if (!writes) {
		ret = -EINVAL;
		dev_err(&fs2105->client->dev,
			"%s contained no writes for address 0x%02x\n",
			name, fs2105->client->addr);
		goto out;
	}

	dev_info(&fs2105->client->dev, "applied %s (%d writes)\n",
		 name, writes);
	ret = 0;
out:
	kfree(text);
	release_firmware(fw);
	return ret;
}

static int fs2105_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBS_CFS)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EINVAL;
	return 0;
}

static int fs2105_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S32_LE:
		return 0;
	default:
		return -EINVAL;
	}
}

static int fs2105_trigger(struct snd_pcm_substream *substream, int cmd,
			  struct snd_soc_dai *dai)
{
	struct fs2105_priv *fs2105 = snd_soc_component_get_drvdata(dai->component);
	unsigned int value;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		value = FS2105_POWER_UP;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		value = FS2105_POWER_DOWN;
		break;
	default:
		return -EINVAL;
	}

	return regmap_write(fs2105->regmap, FS2105_REG_SYSCTRL, value);
}

static const struct snd_soc_dai_ops fs2105_dai_ops = {
	.set_fmt = fs2105_set_fmt,
	.hw_params = fs2105_hw_params,
	.trigger = fs2105_trigger,
};

static struct snd_soc_dai_driver fs2105_dai = {
	.name = "fs2105",
	.playback = {
		.stream_name = "FS HIFI Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
			 SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
			 SNDRV_PCM_RATE_96000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &fs2105_dai_ops,
};

static const struct snd_soc_dapm_widget fs2105_widgets[] = {
	SND_SOC_DAPM_DAC("DAC", "FS HIFI Playback", SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("OUT"),
};

static const struct snd_soc_dapm_route fs2105_routes[] = {
	{ "OUT", NULL, "DAC" },
};

static const struct snd_soc_component_driver fs2105_component = {
	.dapm_widgets = fs2105_widgets,
	.num_dapm_widgets = ARRAY_SIZE(fs2105_widgets),
	.dapm_routes = fs2105_routes,
	.num_dapm_routes = ARRAY_SIZE(fs2105_routes),
};

static int fs2105_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct fs2105_priv *fs2105;
	enum of_gpio_flags flags;
	const char *firmware_name;
	unsigned int devid;
	int ret;

	fs2105 = devm_kzalloc(&client->dev, sizeof(*fs2105), GFP_KERNEL);
	if (!fs2105)
		return -ENOMEM;

	fs2105->client = client;
	fs2105->regmap = devm_regmap_init_i2c(client, &fs2105_regmap);
	if (IS_ERR(fs2105->regmap))
		return PTR_ERR(fs2105->regmap);

	fs2105->reset_gpio = of_get_named_gpio_flags(client->dev.of_node,
						 "fsm,sdz-gpio", 0, &flags);
	if (gpio_is_valid(fs2105->reset_gpio)) {
		ret = devm_gpio_request_one(&client->dev, fs2105->reset_gpio,
					    GPIOF_OUT_INIT_HIGH, "fs2105-sdz");
		if (ret)
			return ret;
		usleep_range(5000, 6000);
	}

	firmware_name = client->addr == 0x68 ?
		"chiron-fs2105-woofer.cfg" : "chiron-fs2105-tweeters.cfg";
	ret = fs2105_apply_text_firmware(fs2105, firmware_name);
	if (ret)
		return ret;

	ret = regmap_read(fs2105->regmap, 0x03, &devid);
	if (ret)
		return ret;
	dev_info(&client->dev, "FS2105 device id 0x%04x at 0x%02x\n",
		 devid, client->addr);

	i2c_set_clientdata(client, fs2105);
	fs2105->configured = true;
	return devm_snd_soc_register_component(&client->dev, &fs2105_component,
					       &fs2105_dai, 1);
}

static const struct of_device_id fs2105_of_match[] = {
	{ .compatible = "foursemi, fs2105" },
	{ .compatible = "foursemi,fs2105" },
	{ }
};
MODULE_DEVICE_TABLE(of, fs2105_of_match);

static const struct i2c_device_id fs2105_i2c_id[] = {
	{ "fs2105", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fs2105_i2c_id);

static struct i2c_driver fs2105_i2c_driver = {
	.driver = {
		.name = "fs2105",
		.of_match_table = fs2105_of_match,
	},
	.probe = fs2105_i2c_probe,
	.id_table = fs2105_i2c_id,
};
module_i2c_driver(fs2105_i2c_driver);

MODULE_AUTHOR("angellfear");
MODULE_DESCRIPTION("FourSemi FS2105 playback codec for Yandex Chiron");
MODULE_LICENSE("GPL");
