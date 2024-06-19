// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Backlight driver for AXP2101, used for M5Stack devices.
 *
 * Copyright 2024 MeemeeLab
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define AXP2101_MAX_BACKLIGHT_REG 28 // 3.3v, (3.3v - 0.5) / 0.1 = 28

#define AXP2101_REG_LDO_EN_CFG0 0x90
#define AXP2101_REG_DLDO1_CFG 0x99

struct axp2101_bl {
	struct device *dev;
	struct regmap *regmap;
};

static int axp2101_bl_set(struct backlight_device *bl, int brightness)
{
	struct axp2101_bl *data = bl_get_data(bl);
	struct regmap *regmap = data->regmap;
	int ret;
	unsigned int ldo_en_cfg0;
	u8 updated_ldo_en_cfg0;
	u8 dldo1_cfg;

	dev_dbg(data->dev,
		"brightness=%d, power=%d, fb_blank=%d",
		brightness, bl->props.power, bl->props.fb_blank);

	if (backlight_is_blank(bl)) {
		brightness = 0;
	}

	ret = regmap_read(regmap, AXP2101_REG_LDO_EN_CFG0, &ldo_en_cfg0);
	if (ret < 0)
		return ret;

	if (brightness == 0) {
		updated_ldo_en_cfg0 = ((u8)ldo_en_cfg0) & 0x7f; // dldo1_en (dldo1 enable) = 0
	} else {
		updated_ldo_en_cfg0 = ((u8)ldo_en_cfg0) | 0x80; // dldo1_en = 1
	}

	if (updated_ldo_en_cfg0 != ldo_en_cfg0) {
		regmap_write(regmap, AXP2101_REG_LDO_EN_CFG0, updated_ldo_en_cfg0);
	}

	if (brightness == 0) {
		return 0;
	}

	dldo1_cfg = brightness / 3;
	if (dldo1_cfg > AXP2101_MAX_BACKLIGHT_REG) {
		dldo1_cfg = AXP2101_MAX_BACKLIGHT_REG;
	}

	regmap_write(regmap, AXP2101_REG_DLDO1_CFG, dldo1_cfg);

	return 0;
}

static int axp2101_bl_update_status(struct backlight_device *bl)
{
	return axp2101_bl_set(bl, backlight_get_brightness(bl));
}

static const struct backlight_ops axp2101_bl_ops = {
	.update_status	= axp2101_bl_update_status,
};

static int axp2101_probe(struct platform_device *pdev) {
	struct backlight_properties props;
	struct axp2101_bl *axp2101;
	struct backlight_device *bl;

	if (!pdev->dev.parent) {
		dev_err(&pdev->dev, "No parent found!");
		return -ENODEV;
	}

	axp2101 = devm_kzalloc(&pdev->dev, sizeof(*axp2101), GFP_KERNEL);
	if (axp2101 == NULL)
		return -ENOMEM;

	axp2101->dev = &pdev->dev;
	axp2101->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!axp2101->regmap) {
		dev_err(&pdev->dev, "No regmap found!");
		return -ENODEV;
	}

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = 100;
	props.brightness = 100;

	bl = devm_backlight_device_register(&pdev->dev,
				dev_driver_string(&pdev->dev),
				&pdev->dev, axp2101, &axp2101_bl_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		return PTR_ERR(bl);
	}

	return axp2101_bl_set(bl, props.brightness);
}

static const struct platform_device_id axp2101_id[] = {
	{ "axp2101-backlight", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, axp2101_id);

static const struct of_device_id axp2101_of_match[] = {
        { .compatible = "m5stack,axp2101-backlight" },
        { },
};
MODULE_DEVICE_TABLE(of, axp2101_of_match);

static struct platform_driver axp2101_driver = {
	.driver = {
		.name	= KBUILD_MODNAME,
		.of_match_table = of_match_ptr(axp2101_of_match),
	},
	.probe = axp2101_probe,
	.id_table = axp2101_id,
};

module_platform_driver(axp2101_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("MeemeeLab");
MODULE_DESCRIPTION("AXP2101 Backlight driver");
