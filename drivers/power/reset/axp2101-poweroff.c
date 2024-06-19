// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Poweroff driver for AXP2101
 *
 * Copyright 2024 MeemeeLab
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/reboot.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>

#define AXP2101_REG_COMM_CFG 0x10

struct axp2101_poweroff {
	struct device *dev;
	struct regmap *regmap;
};

static int axp2101_poweroff_do_poweroff(struct sys_off_data *data) {
	struct axp2101_poweroff *axp2101 = data->cb_data;
	dev_info(axp2101->dev, "Committing seppuku...");

	regmap_write(axp2101->regmap, AXP2101_REG_COMM_CFG, 1);
	/* The CPU should be dead at this point */

	dev_info(axp2101->dev, "Failed to commit seppuku!!!");
	return NOTIFY_DONE;
}

static int axp2101_probe(struct platform_device *pdev) {
	struct axp2101_poweroff *axp2101;
	int priority = SYS_OFF_PRIO_DEFAULT;
	int ret;

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

	device_property_read_u32(&pdev->dev, "priority", &priority);
	if (priority > 255) {
		dev_err(&pdev->dev, "Invalid priority property: %u\n", priority);
		return -EINVAL;
	}

	ret = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF,
					    priority, axp2101_poweroff_do_poweroff, axp2101);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Cannot register poweroff handler\n");

	return 0;
}

static const struct platform_device_id axp2101_id[] = {
	{ "axp2101-poweroff", 0 },
	{ },
};
MODULE_DEVICE_TABLE(platform, axp2101_id);

static const struct of_device_id axp2101_of_match[] = {
        { .compatible = "x-powers,axp2101-poweroff" },
        { },
};
MODULE_DEVICE_TABLE(of, axp2101_of_match);

static struct platform_driver axp2101_driver = {
	.driver = {
		.name	= KBUILD_MODNAME,
		.of_match_table = axp2101_of_match,
	},
	.probe = axp2101_probe,
	.id_table = axp2101_id,
};

module_platform_driver(axp2101_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("MeemeeLab");
MODULE_DESCRIPTION("AXP2101 Poweroff driver");
