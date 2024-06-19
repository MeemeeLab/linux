// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PM domain driver for Lontium Semiconductor LT8618SXB
 *
 * This shouldn't really be a PM driver, but without datasheet
 * and register information available, I couldn't implement
 * bridge driver.
 *
 * Copyright 2024 MeemeeLab
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/pm_domain.h>

// Source: lt8618sxb_mcu_config
#define LT8618SXB_INPUT_RGB888		0x0
#define LT8618SXB_INPUT_RGB_12BIT	0x1
#define LT8618SXB_INPUT_RGB565		0x2
#define LT8618SXB_INPUT_YCBCR444	0x3
#define LT8618SXB_INPUT_YCBCR422_16BIT	0x4
#define LT8618SXB_INPUT_YCBCR422_20BIT	0x5
#define LT8618SXB_INPUT_YCBCR422_24BIT	0x6
#define LT8618SXB_INPUT_BT1120_16BIT	0x7
#define LT8618SXB_INPUT_BT1120_20BIT	0x8
#define LT8618SXB_INPUT_BT1120_24BIT	0x9
#define LT8618SXB_INPUT_BT656_8BIT	0xa
#define LT8618SXB_INPUT_BT656_10BIT	0xb
#define LT8618SXB_INPUT_BT656_12BIT	0xc
#define LT8618SXB_INPUT_BT601_8BIT	0xd

#define LT8618SXB_TX_OUTPUT_DVI		0x00
#define LT8618SXB_TX_OUTPUT_HDMI	0x8e

// We don't really know what frequency this is,
// but we are sure that this will be used in
// lt8618sxb_set_audio_i2s.
#define LT8618SXB_SAMPLE_FREQUENCY_44D1KHZ	0x00
#define LT8618SXB_SAMPLE_FREQUENCY_48KHZ	0x2b
#define LT8618SXB_SAMPLE_FREQUENCY_32KHZ	0x30
#define LT8618SXB_SAMPLE_FREQUENCY_88D2KHZ	0x80
#define LT8618SXB_SAMPLE_FREQUENCY_96KHZ	0xa0
#define LT8618SXB_SAMPLE_FREQUENCY_176KHZ	0xc0
#define LT8618SXB_SAMPLE_FREQUENCY_196KHZ	0xe0

#define LT8618SXB_AUDIO_I2S_44D1KHZ	0x1000
#define LT8618SXB_AUDIO_I2S_48KHZ	0x1800
#define LT8618SXB_AUDIO_I2S_32KHZ	0x1880
#define LT8618SXB_AUDIO_I2S_88D2KHZ	0x3000
#define LT8618SXB_AUDIO_I2S_96KHZ	0x3100
#define LT8618SXB_AUDIO_I2S_176KHZ	0x6000
#define LT8618SXB_AUDIO_I2S_196KHZ	0x6200

// Source: my imagination
#define LT8618SXB_REG_CHIP_ID_1 0x00
#define LT8618SXB_REG_CHIP_ID_2 0x01
#define LT8618SXB_REG_CHIP_ID_3 0x02

#define LT8618SXB_REG_UNKNOWN_30 0x30 // Seems like a output toggle
#define LT8618SXB_REG_UNKNOWN_EE 0xee
#define LT8618SXB_REG_UNKNOWN_FF 0xff // Frequenly being accessed, usually 0x8X
				      // My theory is this register is some kind
				      // of "function switch" that changes operation
				      // on another register, so virtually making
				      // register two bytes, except for 0xff.

#define LT8618SXB_INPUT_MODE LT8618SXB_INPUT_RGB888
#define LT8618SXB_SAMPLE_FREQUENCY_MODE LT8618SXB_SAMPLE_FREQUENCY_48KHZ
#define LT8618SXB_AUDIO_I2S_MODE LT8618SXB_AUDIO_I2S_48KHZ
#define LT8618SXB_TX_OUTPUT_MODE LT8618SXB_TX_OUTPUT_HDMI

struct lt8618sxb {
	struct generic_pm_domain genpd;
	struct device *dev;

	u8 chip_id[3];

	u8 input_mode;
	u8 sample_freq;
	u16 i2s_mode;
	u8 tx_output_mode;
};

#define to_lt8618sxb_pd(_genpd) container_of(_genpd, struct lt8618sxb, genpd)

static int lt8618sxb_i2c_read(struct i2c_client *client, u8 reg, u8 *val) {
	int ret;

	dev_dbg(&client->dev, "read: [%02x]", reg);

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		dev_err(&client->dev,
			"read fail: reg=%u ret=%d\n",
			reg, ret);
		return ret;
	}

	dev_dbg(&client->dev, "reply: %02x", ret);

	*val = ret;
	return 0;
}

static int lt8618sxb_i2c_read_continue(struct i2c_client *client, int *prev_ret, u8 reg, u8 *val) {
	if (*prev_ret < 0) {
		return *prev_ret;
	}

	return lt8618sxb_i2c_read(client, reg, val);
}

static int lt8618sxb_i2c_write(struct i2c_client *client, u8 reg, u8 val) {
	int ret;

	dev_dbg(&client->dev, "write: [%02x-%02x]", reg, val);

	ret = i2c_smbus_write_byte_data(client, reg, val);

	if (ret) {
		dev_err(&client->dev,
			"write fail: reg=%u ret=%d\n",
			reg, ret);
	}

	return ret;
}

static int lt8618sxb_i2c_write_continue(struct i2c_client *client, int *prev_ret, u8 reg, u8 val) {
	if (*prev_ret < 0) {
		return *prev_ret;
	}

	return lt8618sxb_i2c_write(client, reg, val);
}

static int lt8618sxb_read_chip_id(struct i2c_client *client, u8 *chip_id) {
	int ret;

	// Without this, chip id will always be 0x000000
	ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x80);
	ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_EE, 0x01);

	if (ret < 0) {
		dev_err(&client->dev,
			"Chip id read prepare fail\n");
		return ret;
	}

	ret = lt8618sxb_i2c_read(client, LT8618SXB_REG_CHIP_ID_1, &chip_id[0]);
	ret = lt8618sxb_i2c_read_continue(client, &ret, LT8618SXB_REG_CHIP_ID_2, &chip_id[1]);
	ret = lt8618sxb_i2c_read_continue(client, &ret, LT8618SXB_REG_CHIP_ID_3, &chip_id[2]);

	if (ret < 0) {
		dev_err(&client->dev,
			"Chip id read fail\n");
		return ret;
	}

	return 0;
}

static int lt8618sxb_set_hdmi_state(struct i2c_client *client, bool on) {
	int ret;

	ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x81);
	ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_30, on ? 0xea : 0x00);

	if (ret < 0) {
		dev_err(&client->dev,
			"HDMI state set fail\n");
		return ret;
	}

	return 0;
}

static int lt8618sxb_set_ttl_input_analog(struct i2c_client *client) {
	int ret;

	// Nobody knows what's going on here,
	// it's better to just not to touch.
	//
	// In fact, I don't even know what this function even does!

	ret = lt8618sxb_i2c_write(client, 0x2, 0x66);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0xa, 0x6);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x15, 0x6);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4e, 0xa8);

	ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_FF, 0x82);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x1b, 0x77);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x1c, 0xec);

	if (ret < 0) {
		dev_err(&client->dev,
			"TTL input analog fail\n");
		return ret;

	}

	return 0;
}

static int lt8618sxb_set_ttl_input_digital(struct i2c_client *client, u8 in_mode, u8 ddr_clk) {
	int ret;

	switch (in_mode) {
		case LT8618SXB_INPUT_RGB888:
			ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x82);

			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x45, 0x70);

			if (ddr_clk == 0) {
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0x40);
			}
			else {
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0xc0);
			}
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x50, 0x0);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x47, 0x7);
			break;

		case LT8618SXB_INPUT_RGB_12BIT:
			ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x80);

			ret = lt8618sxb_i2c_write_continue(client, &ret, 0xa, 0x80);
			ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_FF, 0x82);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x45, 0x70);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0x40);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x50, 0x0);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x51, 0x30);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x40, 0x0);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x41, 0xcd);
			break;

		case LT8618SXB_INPUT_YCBCR444:
			ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x82);

			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x45, 0x70);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0x40);
			break;

		case LT8618SXB_INPUT_YCBCR422_16BIT:
			ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x82);

			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x45, 0x0);
			if (ddr_clk == 0) {
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0x0);
			}
			else {
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0x40);
			}
			break;

		case LT8618SXB_INPUT_BT1120_16BIT:
			ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x82);

			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x45, 0x70);
			if (ddr_clk == 0) {
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0x40);
			}
			else {
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0x40);
			}
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x48, 0x8);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x51, 0x42);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x47, 0x37);
			break;

		case LT8618SXB_INPUT_BT656_8BIT:
			ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x82);

			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x45, 0x0);
			if (ddr_clk == 0) {
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0x40);
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x48, 0x48);
			}
			else {
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0x40);
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x48, 0x5c);
			}
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x51, 0x42);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x47, 0x87);
			break;

		case LT8618SXB_INPUT_BT601_8BIT:
			ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x81);

			ret = lt8618sxb_i2c_write_continue(client, &ret, 0xa, 0x90);
			ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_FF, 0x81);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4e, 0x2);
			ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_FF, 0x82);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x45, 0x0);
			if (ddr_clk == 0) {
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0xc0);
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x48, 0x40);
			}
			else {
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4f, 0xc0);
				ret = lt8618sxb_i2c_write_continue(client, &ret, 0x48, 0x5c);
			}
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x51, 0x0);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x47, 0x87);
			break;
	}

	if (ret < 0) {
		dev_err(&client->dev,
			"TTL input digital fail\n");
		return ret;

	}

	return 0;
}

static int lt8618sxb_rst_pd_init(struct i2c_client *client) {
	int ret;

	// Another type of function that I don't even know
	// what's going on.
	//
	// we can just speculate from function name

	ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x80);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0xee, 0x1);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x11, 0x0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x13, 0xf1);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x13, 0xf9);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0xa, 0x80);

	if (ret < 0) {
		dev_err(&client->dev,
			"RST PD init fail\n");
		return ret;

	}

	return 0;
}

static int lt8618sxb_set_audio_i2s(struct i2c_client *client, u8 tx_out_mode, u8 sample_freq, u16 i2s_mode) {
	int ret;

	// Logic for disabling audio and setting to SPDIF can be found
	// on lt8618sxb_mcu_config. However, M5Stack device does not
	// make any use of any mode except for I2S.
	//
	// In lt8618sxb_mcu_config, the name in function pointing to I2S
	// is called "IIS" which is a alternative way to saying I2S.
	// At least we can speculate that datasheet mentions "IIS" not "I2S."

	ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x82);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0xd6, tx_out_mode);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0xd7, 0x4);

	ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_FF, 0x84);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x6, 0x8);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x7, 0x10);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x9, 0x0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0xf, sample_freq);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x34, 0xd5);

	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x35, (u8)(i2s_mode >> 0x10));
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x36, (u8)(i2s_mode >> 8));
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x37, (u8)(i2s_mode));

	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x3c, 0x21);

	ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_FF, 0x82);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0xde, 0x0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0xde, 0xc0);

	ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_FF, 0x81);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x23, 0x40);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x24, 0x64);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x26, 0x55);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x29, 0x4);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4d, 0x0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x27, 0x60);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x28, 0x0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x25, 0x1);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x2c, 0x94);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x2d, 0x99);

	if (ret < 0) {
		dev_err(&client->dev,
			"Audio IIS fail\n");
		return ret;
	}

	return 0;
}

static int lt8618sxb_pll_u3_csc(struct i2c_client *client, u8 input_mode) {
	int ret;

	if (input_mode == LT8618SXB_INPUT_YCBCR444) {
		ret = lt8618sxb_i2c_write(client, 0xb9, 0x8);
	} else if ((input_mode == LT8618SXB_INPUT_YCBCR422_16BIT) ||
		   (input_mode == LT8618SXB_INPUT_BT1120_16BIT) ||
		   (input_mode == LT8618SXB_INPUT_BT1120_20BIT) ||
		   (input_mode == LT8618SXB_INPUT_BT1120_24BIT) ||
		   (input_mode == LT8618SXB_INPUT_BT656_8BIT) ||
		   (input_mode == LT8618SXB_INPUT_BT656_10BIT) ||
		   (input_mode == LT8618SXB_INPUT_BT656_12BIT) ||
		   (input_mode == LT8618SXB_INPUT_BT601_8BIT)) {
		ret = lt8618sxb_i2c_write(client, 0xb9, 0x18);
	}
	else {
		ret = lt8618sxb_i2c_write(client, 0xb9, 0x0);
	}

	if (ret < 0) {
		dev_err(&client->dev,
			"U3 CSC fail\n");
		return ret;
	}

	return 0;
}

#define LT8618SXB_HDMI_VIC 0x4
static int lt8618sxb_pll_u3_hdmi_tx_digital(struct i2c_client *client) {
	int ret;

	ret = lt8618sxb_i2c_write(client, LT8618SXB_REG_UNKNOWN_FF, 0x84);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x43, 0x31);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x44, 0x10);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x45, 0x2a);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x47, LT8618SXB_HDMI_VIC);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x10, 0x2c);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x12, 0x64);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x3d, 0xa);

	ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_FF, 0x80);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x11, 0x0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x13, 0xf1);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x13, 0xf9);

	if (ret < 0) {
		dev_err(&client->dev,
			"U3 HDMI TX fail\n");
		return ret;
	}

	return 0;
}

static int lt8618sxb_pll_u3_hdmi_tx_phy(struct i2c_client *client) {
	int ret;

	ret = lt8618sxb_set_hdmi_state(client, true);

	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x31, 0x44);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x32, 0x4a);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x33, 0xb);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x34, 0x0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x35, 0x0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x36, 0x0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x37, 0x44);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x3f, 0xf);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x40, 0xa0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x41, 0xa0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x42, 0xa0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x43, 0xa0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x44, 0xa);

	if (ret < 0) {
		dev_err(&client->dev,
			"U3 HDMI TX Phy fail\n");
		return ret;
	}

	return 0;
}

static int lt8618sxb_pll_u3(struct i2c_client *client, struct lt8618sxb *data) {
	int ret = 0;
	u8 reg_val;
	bool tx_pll_locked = false;

	switch (data->input_mode) {
		case LT8618SXB_INPUT_RGB888:
		case LT8618SXB_INPUT_RGB_12BIT:
		case LT8618SXB_INPUT_YCBCR444:
		case LT8618SXB_INPUT_YCBCR422_16BIT:
		case LT8618SXB_INPUT_BT1120_16BIT:
			ret = lt8618sxb_i2c_write(client, 0x25, 0x0);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x2c, 0x9e);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x2d, 0x99);
			ret = lt8618sxb_i2c_write_continue(client, &ret, 0x28, 0x88);
	}

	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x4d, 0x9);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x27, 0x66);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x2a, 0x0);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x2a, 0x20);

	if (ret < 0) {
		dev_err(&client->dev,
			"PLL early fail\n");
		return ret;
	}

	for (int i = 0; i < 5; ++i) {
		msleep(10);

		ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_FF, 0x80);
		ret = lt8618sxb_i2c_write_continue(client, &ret, 0x16, 0xf1);
		ret = lt8618sxb_i2c_write_continue(client, &ret, 0x18, 0xdc);
		ret = lt8618sxb_i2c_write_continue(client, &ret, 0x18, 0xfc);
		ret = lt8618sxb_i2c_write_continue(client, &ret, 0x16, 0xf3);
		ret = lt8618sxb_i2c_write_continue(client, &ret, 0x16, 0xe3);
		ret = lt8618sxb_i2c_write_continue(client, &ret, 0x16, 0xf3);

		ret = lt8618sxb_i2c_write_continue(client, &ret, LT8618SXB_REG_UNKNOWN_FF, 0x82);

		if (ret < 0) break;

		ret = lt8618sxb_i2c_read(client, 0x15, &reg_val);
		if (ret < 0) break;
		if ((reg_val & 0x80) == 0) {
			continue;
		}

		ret = lt8618sxb_i2c_read(client, 0xea, &reg_val);
		if (ret < 0) break;
		if (reg_val == 0xff) {
			continue;
		}

		ret = lt8618sxb_i2c_read(client, 0xeb, &reg_val);
		if (ret < 0) break;
		if ((reg_val & 0x80) == 0) {
			continue;
		}

		tx_pll_locked = true;
		break;
	}

	if (ret < 0) {
		dev_err(&client->dev,
			"PLL fail\n");
		return ret;
	}

	if (!tx_pll_locked) {
		dev_info(&client->dev, "failed to TXPLL lock; output may not work properly.\n");

		/* For M5Stack devices, the power domain for panel is set to lt8618sxb and this function
		will be called before panel signal initialization thus reaching here is expected behavior. */

		/* return -ETIMEDOUT; */
	}

	ret = lt8618sxb_pll_u3_csc(client, data->input_mode);
	if (ret >= 0) ret = lt8618sxb_pll_u3_hdmi_tx_digital(client);
	if (ret >= 0) ret = lt8618sxb_pll_u3_hdmi_tx_phy(client);

	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int lt8618sxb_pll(struct i2c_client *client, struct lt8618sxb *data) {
	int ret;
	u8 reg_2b;
	u8 reg_2e;

	ret = lt8618sxb_i2c_read(client, 0x2b, &reg_2b);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x2b, reg_2b & 0xfd);
	ret = lt8618sxb_i2c_read_continue(client, &ret, 0x2e, &reg_2e);
	ret = lt8618sxb_i2c_write_continue(client, &ret, 0x2e, reg_2e & 0xfe);

	if (ret < 0) {
		dev_err(&client->dev, "PLL register fail\n");
		return ret;
	}

	if (data->chip_id[2] == 0xe1) {
		dev_info(&client->dev, "Chip is U2C, no need to take action.\n");
	} else if (data->chip_id[2] == 0xe2) {
		dev_info(&client->dev, "Chip is U3C\n");
		ret = lt8618sxb_pll_u3(client, data);
	} else {
		dev_err(&client->dev, "Unknown chip!\n");
		return -EINVAL;
	}

	return ret;
}

static int lt8618sxb_pd_off(struct generic_pm_domain *genpd) {
	int ret;
	struct lt8618sxb *data = to_lt8618sxb_pd(genpd);
	struct i2c_client *client = to_i2c_client(data->dev);

	dev_dbg(&client->dev, "suspend\n");

	ret = lt8618sxb_set_hdmi_state(client, false);

	dev_dbg(&client->dev, "%s(): %d\n", __func__, ret);

	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int lt8618sxb_pd_on(struct generic_pm_domain *genpd) {
	int ret;
	struct lt8618sxb *data = to_lt8618sxb_pd(genpd);
	struct i2c_client *client = to_i2c_client(data->dev);

	dev_dbg(&client->dev, "resume\n");

	ret = lt8618sxb_set_hdmi_state(client, false);
	if (ret >= 0) ret = lt8618sxb_set_ttl_input_analog(client);
	if (ret >= 0) ret = lt8618sxb_rst_pd_init(client);
	if (ret >= 0) ret = lt8618sxb_set_ttl_input_digital(client, data->input_mode, 0);
	if (ret >= 0) ret = lt8618sxb_set_audio_i2s(client, data->tx_output_mode, data->sample_freq, data->i2s_mode);
	if (ret >= 0) ret = lt8618sxb_pll(client, data);

	dev_dbg(&client->dev, "%s(): %d\n", __func__, ret);

	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int lt8618sxb_probe(struct i2c_client *client) {
	int ret;
	struct lt8618sxb *data;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		return -ENOMEM;
	}

	data->dev = &client->dev;

	data->genpd.name = dev_name(&client->dev);
	data->genpd.power_off = lt8618sxb_pd_off;
	data->genpd.power_on = lt8618sxb_pd_on;

	data->i2s_mode = LT8618SXB_AUDIO_I2S_MODE;
	data->input_mode = LT8618SXB_INPUT_MODE;
	data->sample_freq = LT8618SXB_SAMPLE_FREQUENCY_MODE;
	data->tx_output_mode = LT8618SXB_TX_OUTPUT_MODE;

	lt8618sxb_read_chip_id(client, data->chip_id);

	dev_info(&client->dev,
		 "chip id = %02x %02x %02x\n",
		 data->chip_id[0], data->chip_id[1], data->chip_id[2]);

	// We aren't sure what "prod device" is, but we include this just in case.
	if (((data->chip_id[0] == data->chip_id[1]) && (data->chip_id[1] != data->chip_id[2])) &&
			(data->chip_id[2] != data->chip_id[0])) {
		dev_err(&client->dev, "not prod device!\n");
		return -ENODEV;
	}

	dev_set_drvdata(&client->dev, data);

	ret = pm_genpd_init(&data->genpd, NULL, true);
	if (ret < 0) {
		dev_err(&client->dev, "pm_genpd_init fail: %d\n", ret);
		return ret;
	}

	ret = of_genpd_add_provider_simple(client->dev.of_node, &data->genpd);
	if (ret < 0) {
		dev_err(&client->dev, "of_genpd_add_provider_simple fail: %d\n", ret);
		pm_genpd_remove(&data->genpd);
		return ret;
	}

	return 0;
}

static const struct i2c_device_id lt8618sxb_id[] = {
	{ "lt8618sxb-lontium", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lt8618sxb_id);

static const struct of_device_id lt8618sxb_of_match[] = {
        { .compatible = "lontium,lt8618sxb" },
        { },
};
MODULE_DEVICE_TABLE(of, lt8618sxb_of_match);

static struct i2c_driver lt8618sxb_driver = {
	.driver = {
		.name	= KBUILD_MODNAME,
		.of_match_table = of_match_ptr(lt8618sxb_of_match)
	},
	.probe = lt8618sxb_probe,
	.id_table = lt8618sxb_id
};

module_i2c_driver(lt8618sxb_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("MeemeeLab");
MODULE_DESCRIPTION("LT8618SXB PM domain driver");
