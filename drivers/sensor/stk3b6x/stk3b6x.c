// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 angellfear
/*
 * Minimal Chiron STK3B6X ambient-light input driver.
 *
 * The Android vendor driver exposes this part as the input device "als" and
 * reports the 16-bit ALS register through ABS_X. Keep that ABI for the Chiron
 * userspace policy while avoiding the much larger Android ALS/PS framework.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/workqueue.h>

#define STK3B6X_REG_STATE 0x00
#define STK3B6X_REG_ALS_DATA 0x13
#define STK3B6X_REG_ID 0x3e
#define STK3B6X_ID 0x25
#define STK3B6X_STATE_ALS_PS 0x07

struct stk3b6x_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work work;
};

static int stk3b6x_read_als(struct stk3b6x_data *data)
{
	u8 buf[2];
	int ret;

	ret = i2c_smbus_read_i2c_block_data(data->client, STK3B6X_REG_ALS_DATA,
					    sizeof(buf), buf);
	if (ret != sizeof(buf))
		return ret < 0 ? ret : -EIO;

	return (buf[0] << 8) | buf[1];
}

static void stk3b6x_poll(struct work_struct *work)
{
	struct stk3b6x_data *data = container_of(to_delayed_work(work),
							struct stk3b6x_data, work);
	int value;

	value = stk3b6x_read_als(data);
	if (value >= 0) {
		input_report_abs(data->input, ABS_X, value);
		input_sync(data->input);
	}

	if (data->input->users)
		schedule_delayed_work(&data->work, msecs_to_jiffies(500));
}

static int stk3b6x_input_open(struct input_dev *input)
{
	struct stk3b6x_data *data = input_get_drvdata(input);

	schedule_delayed_work(&data->work, 0);
	return 0;
}

static void stk3b6x_input_close(struct input_dev *input)
{
	struct stk3b6x_data *data = input_get_drvdata(input);

	cancel_delayed_work_sync(&data->work);
}

static int stk3b6x_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct stk3b6x_data *data;
	struct input_dev *input;
	int chip_id;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_I2C_BLOCK))
		return -EOPNOTSUPP;

	chip_id = i2c_smbus_read_byte_data(client, STK3B6X_REG_ID);
	if (chip_id < 0)
		return chip_id;
	if (chip_id != STK3B6X_ID) {
		dev_err(&client->dev, "unexpected chip id 0x%02x\n", chip_id);
		return -ENODEV;
	}

	ret = i2c_smbus_write_byte_data(client, STK3B6X_REG_STATE,
					STK3B6X_STATE_ALS_PS);
	if (ret < 0)
		return ret;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	input = devm_input_allocate_device(&client->dev);
	if (!input)
		return -ENOMEM;

	data->client = client;
	data->input = input;
	INIT_DELAYED_WORK(&data->work, stk3b6x_poll);

	input->name = "als";
	input->id.bustype = BUS_I2C;
	input->dev.parent = &client->dev;
	input->open = stk3b6x_input_open;
	input->close = stk3b6x_input_close;
	input_set_drvdata(input, data);
	input_set_abs_params(input, ABS_X, 0, 65535, 0, 0);

	i2c_set_clientdata(client, data);
	ret = input_register_device(input);
	if (ret)
		return ret;

	dev_info(&client->dev, "STK3B6X ambient light input ready\n");
	return 0;
}

static int stk3b6x_remove(struct i2c_client *client)
{
	struct stk3b6x_data *data = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&data->work);
	i2c_smbus_write_byte_data(client, STK3B6X_REG_STATE, 0);
	return 0;
}

static const struct of_device_id stk3b6x_of_match[] = {
	{ .compatible = "stk,stk3b6x" },
	{ }
};
MODULE_DEVICE_TABLE(of, stk3b6x_of_match);

static const struct i2c_device_id stk3b6x_id[] = {
	{ "stk3b6x", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, stk3b6x_id);

static struct i2c_driver stk3b6x_driver = {
	.driver = {
		.name = "stk3b6x",
		.of_match_table = stk3b6x_of_match,
	},
	.probe = stk3b6x_probe,
	.remove = stk3b6x_remove,
	.id_table = stk3b6x_id,
};

module_i2c_driver(stk3b6x_driver);

MODULE_AUTHOR("Chiron port");
MODULE_DESCRIPTION("Chiron STK3B6X ambient light input driver");
MODULE_LICENSE("GPL");
