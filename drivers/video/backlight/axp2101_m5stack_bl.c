// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Backlight driver for AXP2101, used for M5Stack devices.
 *
 * Copyright 2024 MeemeeLab
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/backlight.h>

#define AXP2101_MAX_BACKLIGHT_REG 28 // 3.3v, (3.3v - 0.5) / 0.1 = 28

#define AXP2101_REG_LDO_EN_CFG0 0x90
#define AXP2101_REG_DLDO1_CFG 0x99

struct axp2101_bl {
	struct i2c_client *client;
};

static int axp2101_i2c_read(struct i2c_client *client, u8 reg, u8 *val)
{
	int ret;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		dev_err(&client->dev,
			"read fail: reg=%u ret=%d\n",
			reg, ret);
		return ret;
	}

	*val = ret;
	return 0;
}

static int axp2101_i2c_write(struct i2c_client *client, u8 reg, u8 val)
{
	int ret = i2c_smbus_write_byte_data(client, reg, val);

	if (ret) {
		dev_err(&client->dev,
			"write fail: reg=%u ret=%d\n",
			reg, ret);
	}

	return ret;
}

static int axp2101_bl_set(struct backlight_device *bl, int brightness)
{
	struct axp2101_bl *data = bl_get_data(bl);
	struct i2c_client *client = data->client;
	int ret;
	u8 ldo_en_cfg0;
	u8 updated_ldo_en_cfg0;
	u8 dldo1_cfg;

	dev_dbg(&client->dev,
		"brightness=%d, power=%d, fb_blank=%d",
		brightness, bl->props.power, bl->props.fb_blank);

	if (backlight_is_blank(bl)) {
		brightness = 0;
	}

	ret = axp2101_i2c_read(client, AXP2101_REG_LDO_EN_CFG0, &ldo_en_cfg0);
	if (ret < 0)
		return ret;

	if (brightness == 0) {
		updated_ldo_en_cfg0 = ldo_en_cfg0 & 0x7f; // dldo1_en (dldo1 enable) = 0
	} else {
		updated_ldo_en_cfg0 = ldo_en_cfg0 | 0x80; // dldo1_en = 1
	}

	if (updated_ldo_en_cfg0 != ldo_en_cfg0) {
		axp2101_i2c_write(client, AXP2101_REG_LDO_EN_CFG0, updated_ldo_en_cfg0);
	}

	if (brightness == 0) {
		return 0;
	}

	dldo1_cfg = brightness / 3;
	if (dldo1_cfg > AXP2101_MAX_BACKLIGHT_REG) {
		dldo1_cfg = AXP2101_MAX_BACKLIGHT_REG;
	}

	axp2101_i2c_write(client, AXP2101_REG_DLDO1_CFG, dldo1_cfg);

	return 0;
}

static int axp2101_bl_update_status(struct backlight_device *bl)
{
	return axp2101_bl_set(bl, backlight_get_brightness(bl));
}

static const struct backlight_ops axp2101_bl_ops = {
	.update_status	= axp2101_bl_update_status,
};

static int axp2101_probe(struct i2c_client *client) {
	struct backlight_properties props;
	struct axp2101_bl *data;
	struct backlight_device *bl;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;

	data->client = client;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = 100;
	props.brightness = 100;

	bl = devm_backlight_device_register(&client->dev,
				dev_driver_string(&client->dev),
				&client->dev, data, &axp2101_bl_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&client->dev, "failed to register backlight\n");
		return PTR_ERR(bl);
	}

	return axp2101_bl_set(bl, props.brightness);
}

static const struct i2c_device_id axp2101_id[] = {
	{ "axp2101-m5stack", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, axp2101_id);

static const struct of_device_id axp2101_of_match[] = {
	/*
	 * AXP2101 isn't made by m5stack, but using what voltage and LDO is
	 * up to backlight hardware and vendor decision.
	 * We temporary made this "m5stack,axp2101" because this driver only
	 * supports 3.3v DLDO1 connected backlights.
	 *
	 * TODO: make this driver not only to M5Stack devices
	 */
        { .compatible = "m5stack,axp2101" },
        { },
};
MODULE_DEVICE_TABLE(of, axp2101_of_match);

static struct i2c_driver axp2101_driver = {
	.driver = {
		.name	= KBUILD_MODNAME,
		.of_match_table = of_match_ptr(axp2101_of_match),
	},
	.probe = axp2101_probe,
	.id_table = axp2101_id,
};

module_i2c_driver(axp2101_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("MeemeeLab");
MODULE_DESCRIPTION("AXP2101 Backlight driver");
