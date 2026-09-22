// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 angellfear
/*
 * Silan SC7A20E 3-axis accelerometer.
 *
 * The part is register-compatible with ST's LIS3DH -- same CTRL_REG1/CTRL_REG4
 * layout, same auto-incrementing OUT_[XYZ]_[LH] block, same 12-bit
 * left-justified samples in high-resolution mode -- but it answers WHO_AM_I
 * with 0x11 instead of 0x33.  st_accel checks that byte and would refuse the
 * device, and giving it a new entry means duplicating the whole LIS3DH settings
 * struct and pulling in four external modules (st_sensors, st_sensors_i2c,
 * st_accel, st_accel_i2c), each of which has to CRC-match the pinned kernel.
 * For a single chip whose consumers only need polled x/y/z and a scale, one
 * self-contained driver against the built-in IIO core is the smaller thing to
 * get right.
 *
 * Chiron carries this at i2c-1 0x19; the MC34X9 at 0x6c in the same DT is the
 * other board variant and is not populated.
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/regmap.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define SC7A20E_REG_WHO_AM_I	0x0f
#define SC7A20E_WHO_AM_I_VAL	0x11

#define SC7A20E_REG_CTRL1	0x20
#define SC7A20E_CTRL1_XYZ_EN	0x07
#define SC7A20E_CTRL1_ODR_SHIFT	4

#define SC7A20E_REG_CTRL4	0x23
#define SC7A20E_CTRL4_BDU	BIT(7)
#define SC7A20E_CTRL4_HR	BIT(3)
#define SC7A20E_CTRL4_FS_SHIFT	4

/* Reading more than one byte requires the auto-increment bit. */
#define SC7A20E_REG_OUT_X_L	0x28
#define SC7A20E_AUTO_INCREMENT	0x80

/*
 * High-resolution mode gives 12 bits left-justified in 16.  Sensitivity per
 * full-scale setting is 1, 2, 4 and 12 mg/LSB; the values below are those in
 * m/s^2, which is what IIO expects for IIO_ACCEL.
 */
static const struct {
	int fs;
	int scale_nano;
} sc7a20e_scale_table[] = {
	{ 0,   9806650 },	/* +/-2g,  1 mg/LSB */
	{ 1,  19613300 },	/* +/-4g,  2 mg/LSB */
	{ 2,  39226600 },	/* +/-8g,  4 mg/LSB */
	{ 3, 117679800 },	/* +/-16g, 12 mg/LSB */
};

static const int sc7a20e_odr_table[][2] = {
	{ 1, 0x1 }, { 10, 0x2 }, { 25, 0x3 }, { 50, 0x4 },
	{ 100, 0x5 }, { 200, 0x6 }, { 400, 0x7 },
};

struct sc7a20e_data {
	struct i2c_client *client;
	struct mutex lock;		/* serialises config + sample reads */
	struct iio_mount_matrix orientation;
	int scale_idx;
	int odr_idx;
};

#define SC7A20E_CHANNEL(_axis, _index) {				\
	.type = IIO_ACCEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_axis,					\
	.address = _index,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |		\
				    BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.scan_index = _index,						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = 12,						\
		.storagebits = 16,					\
		.shift = 4,						\
		.endianness = IIO_LE,					\
	},								\
	.ext_info = sc7a20e_ext_info,					\
}

static const struct iio_mount_matrix *
sc7a20e_get_mount_matrix(const struct iio_dev *indio_dev,
			 const struct iio_chan_spec *chan)
{
	struct sc7a20e_data *data = iio_priv(indio_dev);

	return &data->orientation;
}

static const struct iio_chan_spec_ext_info sc7a20e_ext_info[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_TYPE, sc7a20e_get_mount_matrix),
	{ }
};

static const struct iio_chan_spec sc7a20e_channels[] = {
	SC7A20E_CHANNEL(X, 0),
	SC7A20E_CHANNEL(Y, 1),
	SC7A20E_CHANNEL(Z, 2),
};

static int sc7a20e_apply_config(struct sc7a20e_data *data)
{
	int ret;

	ret = i2c_smbus_write_byte_data(data->client, SC7A20E_REG_CTRL1,
			(sc7a20e_odr_table[data->odr_idx][1] << SC7A20E_CTRL1_ODR_SHIFT) |
			SC7A20E_CTRL1_XYZ_EN);
	if (ret < 0)
		return ret;

	return i2c_smbus_write_byte_data(data->client, SC7A20E_REG_CTRL4,
			SC7A20E_CTRL4_BDU | SC7A20E_CTRL4_HR |
			(sc7a20e_scale_table[data->scale_idx].fs << SC7A20E_CTRL4_FS_SHIFT));
}

static int sc7a20e_read_axis(struct sc7a20e_data *data, int axis, int *val)
{
	u8 buf[6];
	int ret;

	/* BDU is set, so a whole-block read cannot mix samples. */
	ret = i2c_smbus_read_i2c_block_data(data->client,
					    SC7A20E_REG_OUT_X_L | SC7A20E_AUTO_INCREMENT,
					    sizeof(buf), buf);
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	*val = sign_extend32(le16_to_cpup((__le16 *)&buf[axis * 2]) >> 4, 11);

	return 0;
}

static int sc7a20e_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct sc7a20e_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&data->lock);
		ret = sc7a20e_read_axis(data, chan->address, val);
		mutex_unlock(&data->lock);
		if (ret)
			return ret;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = sc7a20e_scale_table[data->scale_idx].scale_nano;
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = sc7a20e_odr_table[data->odr_idx][0];
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int sc7a20e_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct sc7a20e_data *data = iio_priv(indio_dev);
	int i, ret = -EINVAL;

	mutex_lock(&data->lock);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		for (i = 0; i < ARRAY_SIZE(sc7a20e_scale_table); i++) {
			if (val != 0 || val2 != sc7a20e_scale_table[i].scale_nano)
				continue;
			data->scale_idx = i;
			ret = sc7a20e_apply_config(data);
			break;
		}
		break;
	case IIO_CHAN_INFO_SAMP_FREQ:
		for (i = 0; i < ARRAY_SIZE(sc7a20e_odr_table); i++) {
			if (val != sc7a20e_odr_table[i][0])
				continue;
			data->odr_idx = i;
			ret = sc7a20e_apply_config(data);
			break;
		}
		break;
	}

	mutex_unlock(&data->lock);

	return ret;
}

static IIO_CONST_ATTR(in_accel_scale_available,
		      "0.009806650 0.019613300 0.039226600 0.117679800");
static IIO_CONST_ATTR(in_accel_sampling_frequency_available,
		      "1 10 25 50 100 200 400");

static struct attribute *sc7a20e_attributes[] = {
	&iio_const_attr_in_accel_scale_available.dev_attr.attr,
	&iio_const_attr_in_accel_sampling_frequency_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group sc7a20e_attribute_group = {
	.attrs = sc7a20e_attributes,
};

static const struct iio_info sc7a20e_info = {
	.read_raw = sc7a20e_read_raw,
	.write_raw = sc7a20e_write_raw,
	.attrs = &sc7a20e_attribute_group,
};

static int sc7a20e_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct sc7a20e_data *data;
	struct iio_dev *indio_dev;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_I2C_BLOCK))
		return -EOPNOTSUPP;

	ret = i2c_smbus_read_byte_data(client, SC7A20E_REG_WHO_AM_I);
	if (ret < 0)
		return ret;
	if (ret != SC7A20E_WHO_AM_I_VAL) {
		dev_err(&client->dev, "unexpected WHO_AM_I 0x%02x\n", ret);
		return -ENODEV;
	}

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	data->scale_idx = 0;	/* +/-2g: best resolution, enough for tilt */
	data->odr_idx = 4;	/* 100 Hz */
	mutex_init(&data->lock);
	i2c_set_clientdata(client, indio_dev);

	/*
	 * The vendor node describes placement with its own "orient" index
	 * rather than the standard property, so a board that has not been given
	 * a mount-matrix falls back to the identity and the transform is left
	 * to whoever consumes the samples.
	 */
	ret = iio_read_mount_matrix(&client->dev, &data->orientation);
	if (ret)
		return ret;

	indio_dev->dev.parent = &client->dev;
	indio_dev->name = "sc7a20e";
	indio_dev->info = &sc7a20e_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = sc7a20e_channels;
	indio_dev->num_channels = ARRAY_SIZE(sc7a20e_channels);

	ret = sc7a20e_apply_config(data);
	if (ret < 0) {
		dev_err(&client->dev, "failed to configure sensor: %d\n", ret);
		return ret;
	}

	return devm_iio_device_register(&client->dev, indio_dev);
}

static int sc7a20e_remove(struct i2c_client *client)
{
	/* ODR 0 is power-down. */
	i2c_smbus_write_byte_data(client, SC7A20E_REG_CTRL1, 0);

	return 0;
}

static const struct i2c_device_id sc7a20e_id[] = {
	{ "sc7a20e", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sc7a20e_id);

static const struct of_device_id sc7a20e_of_match[] = {
	/* The spelling the vendor DT already uses. */
	{ .compatible = "silan,sc7a20e_acc" },
	{ .compatible = "silan,sc7a20e" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc7a20e_of_match);

static struct i2c_driver sc7a20e_driver = {
	.driver = {
		.name = "sc7a20e",
		.of_match_table = sc7a20e_of_match,
	},
	.probe = sc7a20e_probe,
	.remove = sc7a20e_remove,
	.id_table = sc7a20e_id,
};
module_i2c_driver(sc7a20e_driver);

MODULE_DESCRIPTION("Silan SC7A20E 3-axis accelerometer");
MODULE_LICENSE("GPL v2");
