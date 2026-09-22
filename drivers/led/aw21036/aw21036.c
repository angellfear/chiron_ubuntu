// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 angellfear
/*
 * Awinic AW21036 36-channel LED driver.
 *
 * Register map follows the AW21036 datasheet and was confirmed on Chiron
 * hardware: channel n is driven by PWM register 0x01+n, the whole 36-byte
 * frame is committed by writing register 0x49, register 0x7f written with
 * 0x00 performs a soft reset that clears GCC and COL to 0x00, and control
 * register 0x00 bit7 must be set to 1 for normal operation.
 *
 * Chiron carries four parts at i2c-3 0x34..0x37; each drives one quarter of
 * the 48-RGB-ring assembly (12 rings).  The board DT already describes every
 * channel as a child node with a "reg" channel index and a label, plus one
 * "led-sync" child that commits the whole frame.  Defaults (GCC/COL/WB and
 * PWM frequency/phase) come from the board's DT properties.
 *
 * The Amlogic vendor i2c adapter (amlogic_i2c) is not reliable with regmap's
 * internal message shapes, so every access here is a plain i2c_transfer built
 * from the same message patterns as i2cset/i2ctransfer/i2cdump, which the
 * hardware probe verified.
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/leds.h>
#include <linux/gpio/consumer.h>
#include <linux/property.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/math64.h>

#define AW21036_NUM_CHANNELS		36

#define AW21036_REG_CONTROL		0x00
#define AW21036_REG_PWM			0x01
#define AW21036_REG_COMMIT		0x49
#define AW21036_REG_COL			0x4a
#define AW21036_REG_GCC			0x6e
#define AW21036_REG_PHASE		0x70
#define AW21036_REG_WB			0x90
#define AW21036_REG_RESET		0x7f

#define AW21036_RESET_CMD		0x00
#define AW21036_CONTROL_RESET		BIT(7)
#define AW21036_CONTROL_FREQ_SHIFT	4
#define AW21036_CONTROL_FREQ_MASK	0x70
#define AW21036_CONTROL_ENABLE		BIT(0)
#define AW21036_PHASE_DELAY_SHIFT	7
#define AW21036_PHASE_INVERT_MASK	0x3f

#define AW21036_BOOT_INTERVAL_MS	100
#define AW21036_BOOT_ROTATION_MS	10000
#define AW21036_BOOT_HUE_PERIOD		768
#define AW21036_BOOT_HUE_STEP		48
#define AW21036_BOOT_PHASE_SPAN		(AW21036_BOOT_HUE_PERIOD * 3)
#define AW21036_BOOT_MAX_BRIGHTNESS	102

static bool boot_animation = true;
module_param(boot_animation, bool, 0444);
MODULE_PARM_DESC(boot_animation,
		 "run the Chiron boot rainbow until userspace writes an LED");

static atomic_t aw21036_userspace_claimed = ATOMIC_INIT(0);

/* Physical ring order measured on Chiron, not the stale logical DT labels. */
static const u8 aw21036_ch34_channels[12][3] = {
	{ 0x18, 0x19, 0x1a }, { 0x15, 0x16, 0x17 },
	{ 0x12, 0x13, 0x14 }, { 0x0f, 0x10, 0x11 },
	{ 0x0c, 0x0d, 0x0e }, { 0x09, 0x0a, 0x0b },
	{ 0x06, 0x07, 0x08 }, { 0x03, 0x04, 0x05 },
	{ 0x00, 0x01, 0x02 }, { 0x1b, 0x1c, 0x1d },
	{ 0x1e, 0x1f, 0x20 }, { 0x21, 0x22, 0x23 },
};

struct aw21036_led {
	struct led_classdev cdev;
	struct aw21036_data *data;
	u8 channel;
	u8 sync;
};

struct aw21036_data {
	struct i2c_client *client;
	struct gpio_desc *enable_gpio;
	struct mutex lock;
	struct work_struct work;
	struct delayed_work boot_work;
	struct aw21036_led *leds;
	int num_leds;
	int num_sync;
	bool removing;
	u64 dirty;
	u8 cur[AW21036_NUM_CHANNELS];

	u8 gcc_default;
	u8 col_default;
	u8 wb_default[3];
	u8 pwm_freq_default;
	u8 pwm_phase_delay_default;
	u8 pwm_phase_invert_default;
};

static void aw21036_claim_userspace(void)
{
	atomic_set(&aw21036_userspace_claimed, 1);
}

static int aw21036_physical_ring_base(struct aw21036_data *data)
{
	switch (data->client->addr) {
	case 0x37:
		return 0;
	case 0x36:
		return 12;
	case 0x34:
		return 24;
	case 0x35:
		return 36;
	default:
		return -EINVAL;
	}
}

static int aw21036_channel_to_pixel(struct aw21036_data *data, u8 channel,
				    unsigned int *pixel,
				    unsigned int *colour)
{
	int i, component;

	if (data->client->addr != 0x34) {
		*pixel = channel / 3;
		*colour = channel % 3;
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(aw21036_ch34_channels); i++) {
		for (component = 0; component < 3; component++) {
			if (aw21036_ch34_channels[i][component] == channel) {
				*pixel = i;
				*colour = component;
				return 0;
			}
		}
	}
	return -EINVAL;
}

static void aw21036_hue_to_rgb(unsigned int hue, u8 *r, u8 *g, u8 *b)
{
	unsigned int sector = hue >> 7;
	unsigned int ramp = DIV_ROUND_CLOSEST((hue & 0x7f) *
						AW21036_BOOT_MAX_BRIGHTNESS, 0x7f);
	u8 high = AW21036_BOOT_MAX_BRIGHTNESS;
	u8 low = high - ramp;

	switch (sector) {
	case 0:
		*r = high;
		*g = ramp;
		*b = 0;
		break;
	case 1:
		*r = low;
		*g = high;
		*b = 0;
		break;
	case 2:
		*r = 0;
		*g = high;
		*b = ramp;
		break;
	case 3:
		*r = 0;
		*g = low;
		*b = high;
		break;
	case 4:
		*r = ramp;
		*g = 0;
		*b = high;
		break;
	default:
		*r = high;
		*g = 0;
		*b = low;
		break;
	}
}

static void aw21036_boot_work(struct work_struct *work)
{
	struct aw21036_data *data = container_of(to_delayed_work(work),
						  struct aw21036_data, boot_work);
	u64 now_ms = div_u64(ktime_get_mono_fast_ns(), NSEC_PER_MSEC);
	unsigned int phase, hue;
	int base, i;
	u8 r, g, b;

	if (!boot_animation || atomic_read(&aw21036_userspace_claimed))
		return;

	base = aw21036_physical_ring_base(data);
	if (base < 0)
		return;

	/*
	 * Three exact hue periods over 48 pixels.  Advancing three periods in
	 * ten seconds moves every colour point through one physical revolution;
	 * modulo arithmetic keeps the 47 -> 0 boundary identical to every other
	 * neighbouring pair.
	 */
	phase = div_u64((now_ms % AW21036_BOOT_ROTATION_MS) *
			AW21036_BOOT_PHASE_SPAN, AW21036_BOOT_ROTATION_MS);

	mutex_lock(&data->lock);
	if (data->removing || atomic_read(&aw21036_userspace_claimed)) {
		mutex_unlock(&data->lock);
		return;
	}
	memset(data->cur, 0, sizeof(data->cur));
	for (i = 0; i < 12; i++) {
		unsigned int red, green, blue;

		hue = ((base + i) * AW21036_BOOT_HUE_STEP +
		       AW21036_BOOT_HUE_PERIOD -
		       (phase % AW21036_BOOT_HUE_PERIOD)) %
		      AW21036_BOOT_HUE_PERIOD;
		aw21036_hue_to_rgb(hue, &r, &g, &b);
		if (data->client->addr == 0x34) {
			red = aw21036_ch34_channels[i][0];
			green = aw21036_ch34_channels[i][1];
			blue = aw21036_ch34_channels[i][2];
		} else {
			red = i * 3;
			green = red + 1;
			blue = red + 2;
		}
		data->cur[red] = r;
		data->cur[green] = g;
		data->cur[blue] = b;
	}
	data->dirty = GENMASK_ULL(AW21036_NUM_CHANNELS - 1, 0);
	mutex_unlock(&data->lock);

	schedule_work(&data->work);
	if (!atomic_read(&aw21036_userspace_claimed))
		schedule_delayed_work(&data->boot_work,
			msecs_to_jiffies(AW21036_BOOT_INTERVAL_MS));
}

static int aw21036_write(struct aw21036_data *data, u8 reg, const u8 *vals,
			 size_t n)
{
	struct i2c_msg msg;
	u8 buf[AW21036_NUM_CHANNELS + 1];
	int ret;

	if (n > AW21036_NUM_CHANNELS)
		return -EINVAL;
	buf[0] = reg;
	memcpy(buf + 1, vals, n);
	msg.addr = data->client->addr;
	msg.flags = 0;
	msg.len = n + 1;
	msg.buf = buf;
	ret = i2c_transfer(data->client->adapter, &msg, 1);
	if (ret == 1)
		return 0;
	return ret < 0 ? ret : -EIO;
}

static int aw21036_reg_write(struct aw21036_data *data, u8 reg, u8 val)
{
	return aw21036_write(data, reg, &val, 1);
}

static int aw21036_reg_read(struct aw21036_data *data, u8 reg, u8 *val)
{
	struct i2c_msg xfer[2];
	u8 r = reg;
	int ret;

	xfer[0].addr = data->client->addr;
	xfer[0].flags = 0;
	xfer[0].len = 1;
	xfer[0].buf = &r;
	xfer[1].addr = data->client->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = 1;
	xfer[1].buf = val;
	ret = i2c_transfer(data->client->adapter, xfer, 2);
	if (ret == 2)
		return 0;
	return ret < 0 ? ret : -EIO;
}

static void aw21036_work(struct work_struct *work)
{
	struct aw21036_data *data = container_of(work, struct aw21036_data, work);
	int ret = 0, i;
	u64 dirty;

	mutex_lock(&data->lock);
	dirty = data->dirty;
	data->dirty = 0;
	/*
	 * One two-byte transaction per channel: the amlogic_i2c adapter is not
	 * reliable with a 37-byte bulk message (reg + whole frame), which
	 * stalled the workqueue on hardware. i2cset-style per-register writes
	 * are the verified shape.
	 */
	for (i = 0; i < AW21036_NUM_CHANNELS; i++) {
		if (!(dirty & BIT_ULL(i)))
			continue;
		ret = aw21036_reg_write(data, AW21036_REG_PWM + i,
					 data->cur[i]);
		if (ret)
			break;
	}
	if (!ret)
		ret = aw21036_reg_write(data, AW21036_REG_COMMIT, 0x00);
	if (ret)
		dev_err(&data->client->dev, "work failed: %d\n", ret);
	mutex_unlock(&data->lock);
}

static int aw21036_brightness_set_blocking(struct led_classdev *cdev,
					   enum led_brightness brightness)
{
	struct aw21036_led *led = container_of(cdev, struct aw21036_led, cdev);
	struct aw21036_data *data = led->data;

	mutex_lock(&data->lock);
	if (data->removing) {
		mutex_unlock(&data->lock);
		return 0;
	}
	aw21036_claim_userspace();
	if (led->sync)
		data->dirty = GENMASK_ULL(AW21036_NUM_CHANNELS - 1, 0);
	else {
		data->cur[led->channel] = brightness;
		data->dirty |= BIT_ULL(led->channel);
	}
	mutex_unlock(&data->lock);
	schedule_work(&data->work);
	return 0;
}

static int aw21036_init_controller(struct aw21036_data *data)
{
	int i, ret;
	u8 ctrl;

	ret = aw21036_reg_write(data, AW21036_REG_RESET, AW21036_RESET_CMD);
	if (ret)
		return ret;
	usleep_range(4000, 5000);

	ctrl = AW21036_CONTROL_RESET | AW21036_CONTROL_ENABLE |
	       ((data->pwm_freq_default << AW21036_CONTROL_FREQ_SHIFT) &
		AW21036_CONTROL_FREQ_MASK);
	ret = aw21036_reg_write(data, AW21036_REG_CONTROL, ctrl);
	if (ret)
		return ret;
	usleep_range(200, 300);

	ret = aw21036_reg_write(data, AW21036_REG_GCC, data->gcc_default);
	if (ret)
		return ret;
	for (i = 0; i < 3; i++) {
		ret = aw21036_reg_write(data, AW21036_REG_WB + i,
					data->wb_default[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < AW21036_NUM_CHANNELS; i++) {
		ret = aw21036_reg_write(data, AW21036_REG_COL + i,
					data->col_default);
		if (ret)
			return ret;
	}

	{
		u8 rb;

		if (!aw21036_reg_read(data, AW21036_REG_GCC, &rb))
			dev_info(&data->client->dev, "init GCC=0x%02x", rb);
		if (!aw21036_reg_read(data, AW21036_REG_WB, &rb))
			dev_info(&data->client->dev, "init WB0=0x%02x", rb);
		if (!aw21036_reg_read(data, AW21036_REG_PWM, &rb))
			dev_info(&data->client->dev, "init PWM0=0x%02x", rb);
	}

	return 0;
}

static int aw21036_parse_defaults(struct aw21036_data *data)
{
	struct device *dev = &data->client->dev;
	u32 val, wb[3];
	int i;

	if (!device_property_read_u32(dev, "gcc-default", &val))
		data->gcc_default = val;
	else
		data->gcc_default = 0xff;

	if (!device_property_read_u32(dev, "col-default", &val))
		data->col_default = val;
	else
		data->col_default = 0xff;

	if (device_property_read_u32_array(dev, "wb-default", wb, 3))
		memset(data->wb_default, 0xff, sizeof(data->wb_default));
	else
		for (i = 0; i < 3; i++)
			data->wb_default[i] = wb[i];

	if (!device_property_read_u32(dev, "pwm-freq-default", &val))
		data->pwm_freq_default = val;

	if (!device_property_read_u32(dev, "pwm-phase-delay-default", &val))
		data->pwm_phase_delay_default = val;

	if (!device_property_read_u32(dev, "pwm-phase-invert-default", &val))
		data->pwm_phase_invert_default = val;

	return 0;
}

static int aw21036_phase_update(struct aw21036_data *data, u8 bits, u8 mask)
{
	u8 val;
	int ret;

	ret = aw21036_reg_read(data, AW21036_REG_PHASE, &val);
	if (ret)
		return ret;
	val = (val & ~mask) | (bits & mask);
	return aw21036_reg_write(data, AW21036_REG_PHASE, val);
}

static ssize_t frame_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct aw21036_data *data = dev_get_drvdata(dev);
	u8 vals[AW21036_NUM_CHANNELS];
	const char *p = buf;
	int i;

	for (i = 0; i < AW21036_NUM_CHANNELS; i++) {
		unsigned long v;
		char *endp;

		while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n')
			p++;
		if (!*p)
			return -EINVAL;
		v = simple_strtoul(p, &endp, 0);
		if (v > 255)
			return -EINVAL;
		vals[i] = v;
		p = endp;
	}

	aw21036_claim_userspace();
	mutex_lock(&data->lock);
	memcpy(data->cur, vals, sizeof(vals));
	data->dirty = GENMASK_ULL(AW21036_NUM_CHANNELS - 1, 0);
	mutex_unlock(&data->lock);
	schedule_work(&data->work);

	return count;
}

static DEVICE_ATTR_WO(frame);

static ssize_t gcc_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct aw21036_data *data = dev_get_drvdata(dev);
	u8 val;
	int ret;

	ret = kstrtou8(buf, 0, &val);
	if (ret)
		return ret;
	aw21036_claim_userspace();
	ret = aw21036_reg_write(data, AW21036_REG_GCC, val);
	if (ret)
		return ret;
	return count;
}

static DEVICE_ATTR_WO(gcc);

static ssize_t col_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct aw21036_data *data = dev_get_drvdata(dev);
	u8 val;
	int i, ret;

	ret = kstrtou8(buf, 0, &val);
	if (ret)
		return ret;
	aw21036_claim_userspace();
	for (i = 0; i < AW21036_NUM_CHANNELS; i++) {
		ret = aw21036_reg_write(data, AW21036_REG_COL + i, val);
		if (ret)
			return ret;
	}
	return count;
}

static DEVICE_ATTR_WO(col);

static ssize_t wb_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct aw21036_data *data = dev_get_drvdata(dev);
	u8 val, rb = 0;
	int i, ret;

	if (kstrtou8(skip_spaces(buf), 0, &val))
		return -EINVAL;
	aw21036_claim_userspace();
	for (i = 0; i < 3; i++) {
		ret = aw21036_reg_write(data, AW21036_REG_WB + i, val);
		if (ret)
			return ret;
	}
	if (!aw21036_reg_read(data, AW21036_REG_WB, &rb))
		dev_info(&data->client->dev, "wb store wb0=0x%02x", rb);
	return count;
}

static DEVICE_ATTR_WO(wb);

static ssize_t pwm_freq_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct aw21036_data *data = dev_get_drvdata(dev);
	u8 val;
	u8 ctrl;
	int ret;

	ret = kstrtou8(buf, 0, &val);
	if (ret)
		return ret;
	aw21036_claim_userspace();
	ret = aw21036_reg_read(data, AW21036_REG_CONTROL, &ctrl);
	if (ret)
		return ret;
	ctrl = (ctrl & ~AW21036_CONTROL_FREQ_MASK) |
	       ((val << AW21036_CONTROL_FREQ_SHIFT) & AW21036_CONTROL_FREQ_MASK);
	return aw21036_reg_write(data, AW21036_REG_CONTROL, ctrl);
}

static DEVICE_ATTR_WO(pwm_freq);

static ssize_t pwm_phase_delay_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct aw21036_data *data = dev_get_drvdata(dev);
	u8 val;
	int ret;

	ret = kstrtou8(buf, 0, &val);
	if (ret)
		return ret;
	aw21036_claim_userspace();
	ret = aw21036_phase_update(data, val ? BIT(7) : 0, BIT(7));
	if (ret)
		return ret;
	return count;
}

static DEVICE_ATTR_WO(pwm_phase_delay);

static ssize_t pwm_phase_invert_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct aw21036_data *data = dev_get_drvdata(dev);
	u8 val;
	int ret;

	ret = kstrtou8(buf, 0, &val);
	if (ret)
		return ret;
	aw21036_claim_userspace();
	ret = aw21036_phase_update(data, val & AW21036_PHASE_INVERT_MASK,
				   AW21036_PHASE_INVERT_MASK);
	if (ret)
		return ret;
	return count;
}

static DEVICE_ATTR_WO(pwm_phase_invert);

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct aw21036_data *data = dev_get_drvdata(dev);
	u8 val;
	int ret;

	if (!data->enable_gpio)
		return -EOPNOTSUPP;
	ret = kstrtou8(buf, 0, &val);
	if (ret)
		return ret;
	aw21036_claim_userspace();
	gpiod_set_value(data->enable_gpio, !!val);
	return count;
}

static DEVICE_ATTR_WO(enable);

static struct attribute *aw21036_device_attrs[] = {
	&dev_attr_frame.attr,
	&dev_attr_enable.attr,
	&dev_attr_gcc.attr,
	&dev_attr_col.attr,
	&dev_attr_wb.attr,
	&dev_attr_pwm_freq.attr,
	&dev_attr_pwm_phase_delay.attr,
	&dev_attr_pwm_phase_invert.attr,
	NULL,
};

static const struct attribute_group aw21036_device_group = {
	.attrs = aw21036_device_attrs,
};

static const struct attribute_group *aw21036_device_groups[] = {
	&aw21036_device_group,
	NULL,
};

static int aw21036_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct aw21036_data *data;
	struct fwnode_handle *node;
	int count, i, ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->lock);
	INIT_WORK(&data->work, aw21036_work);
	INIT_DELAYED_WORK(&data->boot_work, aw21036_boot_work);
	i2c_set_clientdata(client, data);

	ret = aw21036_parse_defaults(data);
	if (ret)
		return ret;

	data->enable_gpio = devm_gpiod_get_optional(dev, "led-sync",
						    GPIOD_ASIS);
	if (IS_ERR(data->enable_gpio))
		return PTR_ERR(data->enable_gpio);

	if (!device_property_present(dev, "do-not-reset")) {
		ret = aw21036_init_controller(data);
		if (ret) {
			dev_err(dev, "failed to init controller: %d\n", ret);
			return ret;
		}
	}

	count = device_get_child_node_count(dev);
	if (!count) {
		dev_err(dev, "not defined any leds\n");
		return -EINVAL;
	}
	data->leds = devm_kcalloc(dev, count, sizeof(*data->leds), GFP_KERNEL);
	if (!data->leds)
		return -ENOMEM;

	node = device_get_next_child_node(dev, NULL);
	for (i = 0; node && i < count; i++) {
		struct aw21036_led *led = &data->leds[i];
		static const char * const colours[] = { "red", "green", "blue" };
		unsigned int pixel, colour;
		int base;
		u32 chan;
		const char *label;

		led->data = data;
		if (fwnode_property_present(node, "led-sync")) {
			led->sync = 1;
			data->num_sync++;
		} else if (!fwnode_property_read_u32(node, "reg", &chan) &&
			   chan < AW21036_NUM_CHANNELS) {
			led->channel = chan;
		}

		if (fwnode_property_read_string(node, "label", &label)) {
			dev_err(dev, "led child without a label\n");
			ret = -EINVAL;
			goto put_node;
		}
		base = aw21036_physical_ring_base(data);
		if (base < 0) {
			ret = base;
			goto put_node;
		}
		if (led->sync) {
			label = devm_kasprintf(dev, GFP_KERNEL,
					       "chiron:ring%d-%d:sync",
					       base, base + 11);
		} else {
			ret = aw21036_channel_to_pixel(data, led->channel,
						  &pixel, &colour);
			if (ret)
				goto put_node;
			label = devm_kasprintf(dev, GFP_KERNEL,
					       "chiron:%s:ring%d",
					       colours[colour], base + pixel);
		}
		if (!label) {
			ret = -ENOMEM;
			goto put_node;
		}
		led->cdev.name = label;
		led->cdev.brightness_set_blocking = aw21036_brightness_set_blocking;
		led->cdev.max_brightness = 255;

		ret = devm_led_classdev_register_ext(dev, &led->cdev, NULL);
		if (ret) {
			dev_err(dev, "register led class failed: %d\n", ret);
			goto put_node;
		}
		data->num_leds++;
		fwnode_handle_put(node);
		node = device_get_next_child_node(dev, node);
	}
	if (node)
		fwnode_handle_put(node);

	ret = devm_device_add_groups(dev, aw21036_device_groups);
	if (ret)
		return ret;

	if (!data->num_sync)
		dev_warn(dev, "no sync led; full frames unavailable\n");

	dev_info(dev, "aw21036 ready: %d leds, %d sync\n",
		 data->num_leds, data->num_sync);
	if (boot_animation && !atomic_read(&aw21036_userspace_claimed))
		schedule_delayed_work(&data->boot_work, 0);
	return 0;

put_node:
	fwnode_handle_put(node);
	return ret;
}

static int aw21036_remove(struct i2c_client *client)
{
	struct aw21036_data *data = i2c_get_clientdata(client);

	mutex_lock(&data->lock);
	data->removing = true;
	mutex_unlock(&data->lock);
	cancel_delayed_work_sync(&data->boot_work);
	cancel_work_sync(&data->work);
	return 0;
}

static const struct i2c_device_id aw21036_id[] = {
	{ "aw21036", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw21036_id);

static const struct of_device_id aw21036_of_match[] = {
	/* The compatible the board DT already uses. */
	{ .compatible = "awinic,aw21036" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw21036_of_match);

static struct i2c_driver aw21036_driver = {
	.driver = {
		.name = "aw21036",
		.of_match_table = aw21036_of_match,
	},
	.probe = aw21036_probe,
	.remove = aw21036_remove,
	.id_table = aw21036_id,
};
module_i2c_driver(aw21036_driver);

MODULE_DESCRIPTION("Awinic AW21036 36-channel LED driver");
MODULE_LICENSE("GPL v2");
