// SPDX-License-Identifier: GPL v2
//
// drivers/media/i2c/hm1055.c
//
// This file contains a driver for the HM1055 sensor
//
// Copyright (C) 2021 Nuvoton Technology Corp.

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <mach/regs-gcr.h>
#include <mach/regs-gpio.h>
#include <mach/regs-clock.h>
#include <mach/map.h>

#include <mach/regs-gpio.h>
#include <mach/gpio.h>

/* min/typical/max system clock (xclk) frequencies */
#define HM1055_XCLK_MIN  6000000
#define HM1055_XCLK_MAX 48000000

#define HM1055_DEFAULT_SLAVE_ID 0x48

enum hm1055_mode_id {
	HM1055_MODE_720P_1280_720 = 0,
	HM1055_MODE_VGA_640_480 = 1,
	HM1055_NUM_MODES,
};


enum hm1055_format_mux {
	HM1055_FMT_MUX_YUV422 = 0,
	HM1055_FMT_MUX_RGB,
};

struct hm1055_pixfmt {
	u32 code;
	u32 colorspace;
};

static const struct hm1055_pixfmt hm1055_formats[] = {
	{ MEDIA_BUS_FMT_YUYV8_2X8, V4L2_COLORSPACE_SRGB, },
};

struct reg_value {
	u16 reg_addr;
	u8 val;
};

struct hm1055_mode_info {
	enum hm1055_mode_id id;
	u32 hact;
	u32 htot;
	u32 vact;
	u32 vtot;
	const struct reg_value *reg_data;
	u32 reg_data_size;
};

struct hm1055_ctrls {
	struct v4l2_ctrl_handler handler;
	struct {
		struct v4l2_ctrl *auto_exp;
		struct v4l2_ctrl *exposure;
	};
	struct {
		struct v4l2_ctrl *auto_wb;
		struct v4l2_ctrl *blue_balance;
		struct v4l2_ctrl *red_balance;
	};
	struct {
		struct v4l2_ctrl *auto_gain;
		struct v4l2_ctrl *gain;
	};
	struct v4l2_ctrl *brightness;
	struct v4l2_ctrl *light_freq;
	struct v4l2_ctrl *saturation;
	struct v4l2_ctrl *contrast;
	struct v4l2_ctrl *hue;
	struct v4l2_ctrl *test_pattern;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
};

struct hm1055_dev {
	struct i2c_client *i2c_client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_fwnode_endpoint ep; /* the parsed DT endpoint info */
	struct clk *xclk; /* system clock to HM1055 */
	struct clk *cclk;
	u32 xclk_freq;

	//struct regulator_bulk_data supplies[HM1055_NUM_SUPPLIES];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;
	bool   upside_down;

	/* lock to protect all members below */
	struct mutex lock;

	int power_count;

	struct v4l2_mbus_framefmt fmt;
	bool pending_fmt_change;

	const struct hm1055_mode_info *current_mode;
	const struct hm1055_mode_info *last_mode;
	//enum hm1055_frame_rate current_fr;
	struct v4l2_fract frame_interval;

	struct hm1055_ctrls ctrls;

	u32 prev_sysclk, prev_hts;
	u32 ae_low, ae_high, ae_target;

	bool pending_mode_change;
	bool streaming;
};

static inline struct hm1055_dev *to_hm1055_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct hm1055_dev, sd);
}

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler, struct hm1055_dev,
			     ctrls.handler)->sd;
}
static struct reg_value hm1055_setting_YUV_720P[] = {
/* HM1055 , I2C Slave address : 0x24
 * PWDN = Low (Preview mode) ; = High(Power down)
 * Reset = High (Preview mode) ; =Low (Reset)
 */

/* 1280 x 720 , YUV mode ,frame rate = 30fps */
{0x0022, 0x00}, {0x0023, 0xCF}, {0x0020, 0x08}, {0x0027, 0x30},
{0x0004, 0x10}, {0x0006, 0x03}, {0x0012, 0x0F},
/* {0x0026, 0x77}, */ /*48Mhz */
{0x0026, 0x37}, /*68Mhz */
{0x002A, 0x44}, {0x002B, 0x01}, {0x002C, 0x00}, {0x0025, 0x00},
{0x004A, 0x0A}, {0x004B, 0x72}, {0x0070, 0x2A}, {0x0071, 0x46},
{0x0072, 0x55}, {0x0080, 0xC2}, {0x0082, 0xA2}, {0x0083, 0xF0},
{0x0085, 0x10}, {0x0086, 0x22}, {0x0087, 0x08}, {0x0088, 0x6D},
{0x0089, 0x2A}, {0x008A, 0x2F}, {0x008D, 0x20}, {0x0090, 0x01},
{0x0091, 0x02}, {0x0092, 0x03}, {0x0093, 0x04}, {0x0094, 0x14},
{0x0095, 0x09}, {0x0096, 0x0A}, {0x0097, 0x0B}, {0x0098, 0x0C},
{0x0099, 0x04}, {0x009A, 0x14}, {0x009B, 0x34}, {0x00A0, 0x00},
{0x00A1, 0x00}, {0x0B3B, 0x0B}, {0x0040, 0x0A}, {0x0053, 0x0A},
{0x0120, 0x37}, {0x0121, 0x80}, {0x0122, 0xAB}, //0xEB
{0x0123, 0xCC}, {0x0124, 0xDE}, {0x0125, 0xDF}, {0x0126, 0x70},
{0x0128, 0x1F}, {0x0132, 0xF8}, {0x011F, 0x08}, {0x0144, 0x04},
{0x0145, 0x00}, {0x0146, 0x20}, {0x0147, 0x20}, {0x0148, 0x14},
{0x0149, 0x14}, {0x0156, 0x0C}, {0x0157, 0x0C}, {0x0158, 0x0A},
{0x0159, 0x0A}, {0x015A, 0x03}, {0x015B, 0x40}, {0x015C, 0x21},
{0x015E, 0x0F}, {0x0168, 0xC8}, {0x0169, 0xC8}, {0x016A, 0x96},
{0x016B, 0x96}, {0x016C, 0x64}, {0x016D, 0x64}, {0x016E, 0x32},
{0x016F, 0x32}, {0x01EF, 0xF1}, {0x0131, 0x44}, {0x014C, 0x60},
{0x014D, 0x24}, {0x015D, 0x90}, {0x01D8, 0x40}, {0x01D9, 0x20},
{0x01DA, 0x23}, {0x0150, 0x05}, {0x0155, 0x07}, {0x0178, 0x10},
{0x017A, 0x10}, {0x01BA, 0x10}, {0x0176, 0x00}, {0x0179, 0x10},
{0x017B, 0x10}, {0x01BB, 0x10}, {0x0177, 0x00}, {0x01E7, 0x20},
{0x01E8, 0x30}, {0x01E9, 0x50}, {0x01E4, 0x18}, {0x01E5, 0x20},
{0x01E6, 0x04}, {0x0210, 0x21}, {0x0211, 0x0A}, {0x0212, 0x21},
{0x01DB, 0x04}, {0x01DC, 0x14}, {0x0151, 0x08}, {0x01F2, 0x18},
{0x01F8, 0x3C}, {0x01FE, 0x24}, {0x0213, 0x03}, {0x0214, 0x03},
{0x0215, 0x10}, {0x0216, 0x08}, {0x0217, 0x05}, {0x0218, 0xB8},
{0x0219, 0x01}, {0x021A, 0xB8}, {0x021B, 0x01}, {0x021C, 0xB8},
{0x021D, 0x01}, {0x021E, 0xB8}, {0x021F, 0x01}, {0x0220, 0xF1},
{0x0221, 0x5D}, {0x0222, 0x0A}, {0x0223, 0x80}, {0x0224, 0x50},
{0x0225, 0x09}, {0x0226, 0x80}, {0x022A, 0x56}, {0x022B, 0x13},
{0x022C, 0x80}, {0x022D, 0x11}, {0x022E, 0x08}, {0x022F, 0x11},
{0x0230, 0x08}, {0x0233, 0x11}, {0x0234, 0x08}, {0x0235, 0x88},
{0x0236, 0x02}, {0x0237, 0x88}, {0x0238, 0x02}, {0x023B, 0x88},
{0x023C, 0x02}, {0x023D, 0x68}, {0x023E, 0x01}, {0x023F, 0x68},
{0x0240, 0x01}, {0x0243, 0x68}, {0x0244, 0x01}, {0x0251, 0x0F},
{0x0252, 0x00}, {0x0260, 0x00}, {0x0261, 0x4A}, {0x0262, 0x2C},
{0x0263, 0x68}, {0x0264, 0x40}, {0x0265, 0x2C}, {0x0266, 0x6A},
{0x026A, 0x40}, {0x026B, 0x30}, {0x026C, 0x66}, {0x0278, 0x98},
{0x0279, 0x20}, {0x027A, 0x80}, {0x027B, 0x73}, {0x027C, 0x08},
{0x027D, 0x80}, {0x0280, 0x0D}, {0x0282, 0x1A}, {0x0284, 0x30},
{0x0286, 0x53}, {0x0288, 0x62}, {0x028a, 0x6E}, {0x028c, 0x7A},
{0x028e, 0x83}, {0x0290, 0x8B}, {0x0292, 0x92}, {0x0294, 0x9D},
{0x0296, 0xA8}, {0x0298, 0xBC}, {0x029a, 0xCF}, {0x029c, 0xE2},
{0x029e, 0x2A}, {0x02A0, 0x02}, {0x02C0, 0x7D}, {0x02C1, 0x01},
{0x02C2, 0x7C}, {0x02C3, 0x04}, {0x02C4, 0x01}, {0x02C5, 0x04},
{0x02C6, 0x3E}, {0x02C7, 0x04}, {0x02C8, 0x90}, {0x02C9, 0x01},
{0x02CA, 0x52}, {0x02CB, 0x04}, {0x02CC, 0x04}, {0x02CD, 0x04},
{0x02CE, 0xA9}, {0x02CF, 0x04}, {0x02D0, 0xAD}, {0x02D1, 0x01},
{0x0302, 0x00}, {0x0303, 0x00}, {0x0304, 0x00}, {0x02e0, 0x04},
{0x02F0, 0x4E}, {0x02F1, 0x04}, {0x02F2, 0xB1}, {0x02F3, 0x00},
{0x02F4, 0x63}, {0x02F5, 0x04}, {0x02F6, 0x28}, {0x02F7, 0x04},
{0x02F8, 0x29}, {0x02F9, 0x04}, {0x02FA, 0x51}, {0x02FB, 0x00},
{0x02FC, 0x64}, {0x02FD, 0x04}, {0x02FE, 0x6B}, {0x02FF, 0x04},
{0x0300, 0xCF}, {0x0301, 0x00}, {0x0305, 0x08}, {0x0306, 0x40},
{0x0307, 0x00}, {0x032D, 0x70}, {0x032E, 0x01}, {0x032F, 0x00},
{0x0330, 0x01}, {0x0331, 0x70}, {0x0332, 0x01}, {0x0333, 0x82},
{0x0334, 0x82}, {0x0335, 0x86}, {0x0340, 0x30}, {0x0341, 0x44},
{0x0342, 0x4A}, {0x0343, 0x3C}, {0x0344, 0x83}, {0x0345, 0x4D},
{0x0346, 0x75}, {0x0347, 0x56}, {0x0348, 0x68}, {0x0349, 0x5E},
{0x034A, 0x5C}, {0x034B, 0x65}, {0x034C, 0x52}, {0x0350, 0x88},
{0x0352, 0x18}, {0x0354, 0x80}, {0x0355, 0x50}, {0x0356, 0x88},
{0x0357, 0xE0}, {0x0358, 0x00}, {0x035A, 0x00}, {0x035B, 0xAC},
{0x0360, 0x02}, {0x0361, 0x18}, {0x0362, 0x50}, {0x0363, 0x6C},
{0x0364, 0x00}, {0x0365, 0xF0}, {0x0366, 0x08}, {0x036A, 0x10},
{0x036B, 0x18}, {0x036E, 0x10}, {0x0370, 0x10}, {0x0371, 0x18},
{0x0372, 0x0C}, {0x0373, 0x38}, {0x0374, 0x3A}, {0x0375, 0x12},
{0x0376, 0x20}, {0x0380, 0xFF}, {0x0381, 0x44}, {0x0382, 0x34},
{0x038A, 0x80}, {0x038B, 0x0A}, {0x038C, 0xC1}, {0x038E, 0x3C},
{0x038F, 0x09}, {0x0390, 0xE0}, {0x0391, 0x01}, {0x0392, 0x03},
{0x0393, 0x80}, {0x0395, 0x22}, {0x0398, 0x02}, {0x0399, 0xF0},
{0x039A, 0x03}, {0x039B, 0xAC}, {0x039C, 0x04}, {0x039D, 0x68},
{0x039E, 0x05}, {0x039F, 0xE0}, {0x03A0, 0x07}, {0x03A1, 0x58},
{0x03A2, 0x08}, {0x03A3, 0xD0}, {0x03A4, 0x0B}, {0x03A5, 0xC0},
{0x03A6, 0x18}, {0x03A7, 0x1C}, {0x03A8, 0x20}, {0x03A9, 0x24},
{0x03AA, 0x28}, {0x03AB, 0x30}, {0x03AC, 0x24}, {0x03AD, 0x21},
{0x03AE, 0x1C}, {0x03AF, 0x18}, {0x03B0, 0x17}, {0x03B1, 0x13},
{0x03B7, 0x64}, {0x03B8, 0x00}, {0x03B9, 0xB4}, {0x03BA, 0x00},
{0x03bb, 0xff}, {0x03bc, 0xff}, {0x03bd, 0xff}, {0x03be, 0xff},
{0x03bf, 0xff}, {0x03c0, 0xff}, {0x03c1, 0x01}, {0x03e0, 0x04},
{0x03e1, 0x11}, {0x03e2, 0x01}, {0x03e3, 0x04}, {0x03e4, 0x10},
{0x03e5, 0x21}, {0x03e6, 0x11}, {0x03e7, 0x00}, {0x03e8, 0x11},
{0x03e9, 0x32}, {0x03ea, 0x12}, {0x03eb, 0x01}, {0x03ec, 0x21},
{0x03ed, 0x33}, {0x03ee, 0x23}, {0x03ef, 0x01}, {0x03f0, 0x11},
{0x03f1, 0x32}, {0x03f2, 0x12}, {0x03f3, 0x01}, {0x03f4, 0x10},
{0x03f5, 0x21}, {0x03f6, 0x11}, {0x03f7, 0x00}, {0x03f8, 0x04},
{0x03f9, 0x11}, {0x03fa, 0x01}, {0x03fb, 0x04}, {0x03DC, 0x47},
{0x03DD, 0x5A}, {0x03DE, 0x41}, {0x03DF, 0x53}, {0x0420, 0x82},
{0x0421, 0x00}, {0x0422, 0x00}, {0x0423, 0x88}, {0x0430, 0x08},
{0x0431, 0x30}, {0x0432, 0x0c}, {0x0433, 0x04}, {0x0435, 0x08},
{0x0450, 0xFF}, {0x0451, 0xD0}, {0x0452, 0xB8}, {0x0453, 0x88},
{0x0454, 0x00}, {0x0458, 0x80}, {0x0459, 0x03}, {0x045A, 0x00},
{0x045B, 0x50}, {0x045C, 0x00}, {0x045D, 0x90}, {0x0465, 0x02},
{0x0466, 0x14}, {0x047A, 0x00}, {0x047B, 0x00}, {0x047C, 0x04},
{0x047D, 0x50}, {0x047E, 0x04}, {0x047F, 0x90}, {0x0480, 0x58},
{0x0481, 0x06}, {0x0482, 0x08}, {0x04B0, 0x50}, {0x04B6, 0x30},
{0x04B9, 0x10}, {0x04B3, 0x00}, {0x04B1, 0x85}, {0x04B4, 0x00},
{0x0540, 0x00}, {0x0541, 0xBC}, {0x0542, 0x00}, {0x0543, 0xE1},
{0x0580, 0x04}, {0x0581, 0x0F}, {0x0582, 0x04}, {0x05A1, 0x0A},
{0x05A2, 0x21}, {0x05A3, 0x84}, {0x05A4, 0x24}, {0x05A5, 0xFF},
{0x05A6, 0x00}, {0x05A7, 0x24}, {0x05A8, 0x24}, {0x05A9, 0x02},
{0x05B1, 0x24}, {0x05B2, 0x0C}, {0x05B4, 0x1F}, {0x05AE, 0x75},
{0x05AF, 0x78}, {0x05B6, 0x00}, {0x05B7, 0x10}, {0x05BF, 0x20},
{0x05C1, 0x06}, {0x05C2, 0x18}, {0x05C7, 0x00}, {0x05CC, 0x04},
{0x05CD, 0x00}, {0x05CE, 0x03}, {0x05E4, 0x08}, {0x05E5, 0x00},
{0x05E6, 0x07}, {0x05E7, 0x05}, {0x05E8, 0x06}, {0x05E9, 0x00},
{0x05EA, 0x25}, {0x05EB, 0x03}, {0x0660, 0x00}, {0x0661, 0x16},
{0x0662, 0x07}, {0x0663, 0xf1}, {0x0664, 0x07}, {0x0665, 0xde},
{0x0666, 0x07}, {0x0667, 0xe7}, {0x0668, 0x00}, {0x0669, 0x35},
{0x066a, 0x07}, {0x066b, 0xf9}, {0x066c, 0x07}, {0x066d, 0xb7},
{0x066e, 0x00}, {0x066f, 0x27}, {0x0670, 0x07}, {0x0671, 0xf3},
{0x0672, 0x07}, {0x0673, 0xc5}, {0x0674, 0x07}, {0x0675, 0xee},
{0x0676, 0x00}, {0x0677, 0x16}, {0x0678, 0x01}, {0x0679, 0x80},
{0x067a, 0x00}, {0x067b, 0x85}, {0x067c, 0x07}, {0x067d, 0xe1},
{0x067e, 0x07}, {0x067f, 0xf5}, {0x0680, 0x07}, {0x0681, 0xb9},
{0x0682, 0x00}, {0x0683, 0x31}, {0x0684, 0x07}, {0x0685, 0xe6},
{0x0686, 0x07}, {0x0687, 0xd3}, {0x0688, 0x00}, {0x0689, 0x18},
{0x068a, 0x07}, {0x068b, 0xfa}, {0x068c, 0x07}, {0x068d, 0xd2},
{0x068e, 0x00}, {0x068f, 0x08}, {0x0690, 0x00}, {0x0691, 0x02},
{0xAFD0, 0x03}, {0xAFD3, 0x18}, {0xAFD4, 0x04}, {0xAFD5, 0xB8},
{0xAFD6, 0x02}, {0xAFD7, 0x44}, {0xAFD8, 0x02},
{0x0000, 0x01}, //
{0x0100, 0x01}, //
{0x0101, 0x01}, //
{0x0005, 0x01}, // Turn on rolling shutter

#ifdef CONFIG_FLICKER_50HZ_DEV1
	{0x0542, 0x00},
	{0x0543, 0xE1},
#endif
#ifdef CONFIG_FLICKER_60HZ_DEV1
	{0x0540, 0x00},
	{0x0541, 0xBC},
#endif
};

static struct reg_value hm1055_setting_YUV_VGA[] = {
{0x0022, 0x00}, {0x0023, 0xCF}, {0x0020, 0x08}, {0x0027, 0x30},
{0x0004, 0x10}, {0x0006, 0x03}, {0x0012, 0x0F},
/* {0x0026, 0x77}, */ /*48Mhz */
{0x0026, 0x37}, /*68Mhz */
{0x002A, 0x44}, {0x002B, 0x01}, {0x002C, 0x00}, {0x0025, 0x00},
{0x004A, 0x0A}, {0x004B, 0x72}, {0x0070, 0x2A}, {0x0071, 0x46},
{0x0072, 0x55}, {0x0080, 0xC2}, {0x0082, 0xA2}, {0x0083, 0xF0},
{0x0085, 0x10}, {0x0086, 0x22}, {0x0087, 0x08}, {0x0088, 0x6D},
{0x0089, 0x2A}, {0x008A, 0x2F}, {0x008D, 0x20}, {0x0090, 0x01},
{0x0091, 0x02}, {0x0092, 0x03}, {0x0093, 0x04}, {0x0094, 0x14},
{0x0095, 0x09}, {0x0096, 0x0A}, {0x0097, 0x0B}, {0x0098, 0x0C},
{0x0099, 0x04}, {0x009A, 0x14}, {0x009B, 0x34}, {0x00A0, 0x00},
{0x00A1, 0x00}, {0x0B3B, 0x0B}, {0x0040, 0x0A}, {0x0053, 0x0A},
{0x0120, 0x37}, {0x0121, 0x80}, {0x0122, 0xAB}, //0xEB
{0x0123, 0xCC}, {0x0124, 0xDE}, {0x0125, 0xDF}, {0x0126, 0x70},
{0x0128, 0x1F}, {0x0132, 0xF8}, {0x011F, 0x08}, {0x0144, 0x04},
{0x0145, 0x00}, {0x0146, 0x20}, {0x0147, 0x20}, {0x0148, 0x14},
{0x0149, 0x14}, {0x0156, 0x0C}, {0x0157, 0x0C}, {0x0158, 0x0A},
{0x0159, 0x0A}, {0x015A, 0x03}, {0x015B, 0x40}, {0x015C, 0x21},
{0x015E, 0x0F}, {0x0168, 0xC8}, {0x0169, 0xC8}, {0x016A, 0x96},
{0x016B, 0x96}, {0x016C, 0x64}, {0x016D, 0x64}, {0x016E, 0x32},
{0x016F, 0x32}, {0x01EF, 0xF1}, {0x0131, 0x44}, {0x014C, 0x60},
{0x014D, 0x24}, {0x015D, 0x90}, {0x01D8, 0x40}, {0x01D9, 0x20},
{0x01DA, 0x23}, {0x0150, 0x05}, {0x0155, 0x07}, {0x0178, 0x10},
{0x017A, 0x10}, {0x01BA, 0x10}, {0x0176, 0x00}, {0x0179, 0x10},
{0x017B, 0x10}, {0x01BB, 0x10}, {0x0177, 0x00}, {0x01E7, 0x20},
{0x01E8, 0x30}, {0x01E9, 0x50}, {0x01E4, 0x18}, {0x01E5, 0x20},
{0x01E6, 0x04}, {0x0210, 0x21}, {0x0211, 0x0A}, {0x0212, 0x21},
{0x01DB, 0x04}, {0x01DC, 0x14}, {0x0151, 0x08}, {0x01F2, 0x18},
{0x01F8, 0x3C}, {0x01FE, 0x24}, {0x0213, 0x03}, {0x0214, 0x03},
{0x0215, 0x10}, {0x0216, 0x08}, {0x0217, 0x05}, {0x0218, 0xB8},
{0x0219, 0x01}, {0x021A, 0xB8}, {0x021B, 0x01}, {0x021C, 0xB8},
{0x021D, 0x01}, {0x021E, 0xB8}, {0x021F, 0x01}, {0x0220, 0xF1},
{0x0221, 0x5D}, {0x0222, 0x0A}, {0x0223, 0x80}, {0x0224, 0x50},
{0x0225, 0x09}, {0x0226, 0x80}, {0x022A, 0x56}, {0x022B, 0x13},
{0x022C, 0x80}, {0x022D, 0x11}, {0x022E, 0x08}, {0x022F, 0x11},
{0x0230, 0x08}, {0x0233, 0x11}, {0x0234, 0x08}, {0x0235, 0x88},
{0x0236, 0x02}, {0x0237, 0x88}, {0x0238, 0x02}, {0x023B, 0x88},
{0x023C, 0x02}, {0x023D, 0x68}, {0x023E, 0x01}, {0x023F, 0x68},
{0x0240, 0x01}, {0x0243, 0x68}, {0x0244, 0x01}, {0x0251, 0x0F},
{0x0252, 0x00}, {0x0260, 0x00}, {0x0261, 0x4A}, {0x0262, 0x2C},
{0x0263, 0x68}, {0x0264, 0x40}, {0x0265, 0x2C}, {0x0266, 0x6A},
{0x026A, 0x40}, {0x026B, 0x30}, {0x026C, 0x66}, {0x0278, 0x98},
{0x0279, 0x20}, {0x027A, 0x80}, {0x027B, 0x73}, {0x027C, 0x08},
{0x027D, 0x80}, {0x0280, 0x0D}, {0x0282, 0x1A}, {0x0284, 0x30},
{0x0286, 0x53}, {0x0288, 0x62}, {0x028a, 0x6E}, {0x028c, 0x7A},
{0x028e, 0x83}, {0x0290, 0x8B}, {0x0292, 0x92}, {0x0294, 0x9D},
{0x0296, 0xA8}, {0x0298, 0xBC}, {0x029a, 0xCF}, {0x029c, 0xE2},
{0x029e, 0x2A}, {0x02A0, 0x02}, {0x02C0, 0x7D}, {0x02C1, 0x01},
{0x02C2, 0x7C}, {0x02C3, 0x04}, {0x02C4, 0x01}, {0x02C5, 0x04},
{0x02C6, 0x3E}, {0x02C7, 0x04}, {0x02C8, 0x90}, {0x02C9, 0x01},
{0x02CA, 0x52}, {0x02CB, 0x04}, {0x02CC, 0x04}, {0x02CD, 0x04},
{0x02CE, 0xA9}, {0x02CF, 0x04}, {0x02D0, 0xAD}, {0x02D1, 0x01},
{0x0302, 0x00}, {0x0303, 0x00}, {0x0304, 0x00}, {0x02e0, 0x04},
{0x02F0, 0x4E}, {0x02F1, 0x04}, {0x02F2, 0xB1}, {0x02F3, 0x00},
{0x02F4, 0x63}, {0x02F5, 0x04}, {0x02F6, 0x28}, {0x02F7, 0x04},
{0x02F8, 0x29}, {0x02F9, 0x04}, {0x02FA, 0x51}, {0x02FB, 0x00},
{0x02FC, 0x64}, {0x02FD, 0x04}, {0x02FE, 0x6B}, {0x02FF, 0x04},
{0x0300, 0xCF}, {0x0301, 0x00}, {0x0305, 0x08}, {0x0306, 0x40},
{0x0307, 0x00}, {0x032D, 0x70}, {0x032E, 0x01}, {0x032F, 0x00},
{0x0330, 0x01}, {0x0331, 0x70}, {0x0332, 0x01}, {0x0333, 0x82},
{0x0334, 0x82}, {0x0335, 0x86}, {0x0340, 0x30}, {0x0341, 0x44},
{0x0342, 0x4A}, {0x0343, 0x3C}, {0x0344, 0x83}, {0x0345, 0x4D},
{0x0346, 0x75}, {0x0347, 0x56}, {0x0348, 0x68}, {0x0349, 0x5E},
{0x034A, 0x5C}, {0x034B, 0x65}, {0x034C, 0x52}, {0x0350, 0x88},
{0x0352, 0x18}, {0x0354, 0x80}, {0x0355, 0x50}, {0x0356, 0x88},
{0x0357, 0xE0}, {0x0358, 0x00}, {0x035A, 0x00}, {0x035B, 0xAC},
{0x0360, 0x02}, {0x0361, 0x18}, {0x0362, 0x50}, {0x0363, 0x6C},
{0x0364, 0x00}, {0x0365, 0xF0}, {0x0366, 0x08}, {0x036A, 0x10},
{0x036B, 0x18}, {0x036E, 0x10}, {0x0370, 0x10}, {0x0371, 0x18},
{0x0372, 0x0C}, {0x0373, 0x38}, {0x0374, 0x3A}, {0x0375, 0x12},
{0x0376, 0x20}, {0x0380, 0xFF}, {0x0381, 0x44}, {0x0382, 0x34},
{0x038A, 0x80}, {0x038B, 0x0A}, {0x038C, 0xC1}, {0x038E, 0x3C},
{0x038F, 0x09}, {0x0390, 0xE0}, {0x0391, 0x01}, {0x0392, 0x03},
{0x0393, 0x80}, {0x0395, 0x22}, {0x0398, 0x02}, {0x0399, 0xF0},
{0x039A, 0x03}, {0x039B, 0xAC}, {0x039C, 0x04}, {0x039D, 0x68},
{0x039E, 0x05}, {0x039F, 0xE0}, {0x03A0, 0x07}, {0x03A1, 0x58},
{0x03A2, 0x08}, {0x03A3, 0xD0}, {0x03A4, 0x0B}, {0x03A5, 0xC0},
{0x03A6, 0x18}, {0x03A7, 0x1C}, {0x03A8, 0x20}, {0x03A9, 0x24},
{0x03AA, 0x28}, {0x03AB, 0x30}, {0x03AC, 0x24}, {0x03AD, 0x21},
{0x03AE, 0x1C}, {0x03AF, 0x18}, {0x03B0, 0x17}, {0x03B1, 0x13},
{0x03B7, 0x64}, {0x03B8, 0x00}, {0x03B9, 0xB4}, {0x03BA, 0x00},
{0x03bb, 0xff}, {0x03bc, 0xff}, {0x03bd, 0xff}, {0x03be, 0xff},
{0x03bf, 0xff}, {0x03c0, 0xff}, {0x03c1, 0x01}, {0x03e0, 0x04},
{0x03e1, 0x11}, {0x03e2, 0x01}, {0x03e3, 0x04}, {0x03e4, 0x10},
{0x03e5, 0x21}, {0x03e6, 0x11}, {0x03e7, 0x00}, {0x03e8, 0x11},
{0x03e9, 0x32}, {0x03ea, 0x12}, {0x03eb, 0x01}, {0x03ec, 0x21},
{0x03ed, 0x33}, {0x03ee, 0x23}, {0x03ef, 0x01}, {0x03f0, 0x11},
{0x03f1, 0x32}, {0x03f2, 0x12}, {0x03f3, 0x01}, {0x03f4, 0x10},
{0x03f5, 0x21}, {0x03f6, 0x11}, {0x03f7, 0x00}, {0x03f8, 0x04},
{0x03f9, 0x11}, {0x03fa, 0x01}, {0x03fb, 0x04}, {0x03DC, 0x47},
{0x03DD, 0x5A}, {0x03DE, 0x41}, {0x03DF, 0x53}, {0x0420, 0x82},
{0x0421, 0x00}, {0x0422, 0x00}, {0x0423, 0x88}, {0x0430, 0x08},
{0x0431, 0x30}, {0x0432, 0x0c}, {0x0433, 0x04}, {0x0435, 0x08},
{0x0450, 0xFF}, {0x0451, 0xD0}, {0x0452, 0xB8}, {0x0453, 0x88},
{0x0454, 0x00}, {0x0458, 0x80}, {0x0459, 0x03}, {0x045A, 0x00},
{0x045B, 0x50}, {0x045C, 0x00}, {0x045D, 0x90}, {0x0465, 0x02},
{0x0466, 0x14}, {0x047A, 0x00}, {0x047B, 0x00}, {0x047C, 0x04},
{0x047D, 0x50}, {0x047E, 0x04}, {0x047F, 0x90}, {0x0480, 0x58},
{0x0481, 0x06}, {0x0482, 0x08}, {0x04B0, 0x50}, {0x04B6, 0x30},
{0x04B9, 0x10}, {0x04B3, 0x00}, {0x04B1, 0x85}, {0x04B4, 0x00},
{0x0540, 0x00}, {0x0541, 0xBC}, {0x0542, 0x00}, {0x0543, 0xE1},
{0x0580, 0x04}, {0x0581, 0x0F}, {0x0582, 0x04}, {0x05A1, 0x0A},
{0x05A2, 0x21}, {0x05A3, 0x84}, {0x05A4, 0x24}, {0x05A5, 0xFF},
{0x05A6, 0x00}, {0x05A7, 0x24}, {0x05A8, 0x24}, {0x05A9, 0x02},
{0x05B1, 0x24}, {0x05B2, 0x0C}, {0x05B4, 0x1F}, {0x05AE, 0x75},
{0x05AF, 0x78}, {0x05B6, 0x00}, {0x05B7, 0x10}, {0x05BF, 0x20},
{0x05C1, 0x06}, {0x05C2, 0x18}, {0x05C7, 0x00}, {0x05CC, 0x04},
{0x05CD, 0x00}, {0x05CE, 0x03}, {0x05E4, 0x08}, {0x05E5, 0x00},
{0x05E6, 0x07}, {0x05E7, 0x05}, {0x05E8, 0x06}, {0x05E9, 0x00},
{0x05EA, 0x25}, {0x05EB, 0x03}, {0x0660, 0x00}, {0x0661, 0x16},
{0x0662, 0x07}, {0x0663, 0xf1}, {0x0664, 0x07}, {0x0665, 0xde},
{0x0666, 0x07}, {0x0667, 0xe7}, {0x0668, 0x00}, {0x0669, 0x35},
{0x066a, 0x07}, {0x066b, 0xf9}, {0x066c, 0x07}, {0x066d, 0xb7},
{0x066e, 0x00}, {0x066f, 0x27}, {0x0670, 0x07}, {0x0671, 0xf3},
{0x0672, 0x07}, {0x0673, 0xc5}, {0x0674, 0x07}, {0x0675, 0xee},
{0x0676, 0x00}, {0x0677, 0x16}, {0x0678, 0x01}, {0x0679, 0x80},
{0x067a, 0x00}, {0x067b, 0x85}, {0x067c, 0x07}, {0x067d, 0xe1},
{0x067e, 0x07}, {0x067f, 0xf5}, {0x0680, 0x07}, {0x0681, 0xb9},
{0x0682, 0x00}, {0x0683, 0x31}, {0x0684, 0x07}, {0x0685, 0xe6},
{0x0686, 0x07}, {0x0687, 0xd3}, {0x0688, 0x00}, {0x0689, 0x18},
{0x068a, 0x07}, {0x068b, 0xfa}, {0x068c, 0x07}, {0x068d, 0xd2},
{0x068e, 0x00}, {0x068f, 0x08}, {0x0690, 0x00}, {0x0691, 0x02},
{0xAFD0, 0x03}, {0xAFD3, 0x18}, {0xAFD4, 0x04}, {0xAFD5, 0xB8},
{0xAFD6, 0x02}, {0xAFD7, 0x44}, {0xAFD8, 0x02},
{0x0000, 0x01}, //
{0x0100, 0x01}, //
{0x0101, 0x01}, //
{0x0005, 0x01}, // Turn on rolling shutter

{0x002B, 0x01}, {0x0023, 0xCF}, {0x0027, 0x30}, {0x0005, 0x00},
{0x0006, 0x10}, {0x000D, 0x00}, {0x000E, 0x00}, {0x0122, 0x6B},
{0x0125, 0xFF}, {0x0126, 0x70}, {0x05E0, 0xC1}, {0x05E1, 0x00},
{0x05E2, 0xC1}, {0x05E3, 0x00}, {0x05E4, 0x03}, {0x05E5, 0x00},
{0x05E6, 0x82}, {0x05E7, 0x02}, {0x05E8, 0x04}, {0x05E9, 0x00},
{0x05EA, 0xE3}, {0x05EB, 0x01}, {0x0000, 0x01},	{0x0100, 0x01},
{0x0101, 0x01},	{0x0005, 0x01},
#ifdef CONFIG_FLICKER_50HZ_DEV1
	{0x0542, 0x00},
	{0x0543, 0xE1},
#endif
#ifdef CONFIG_FLICKER_60HZ_DEV1
	{0x0540, 0x00},
	{0x0541, 0xBC},
#endif
};

static const struct hm1055_mode_info
hm1055_mode_data[HM1055_NUM_MODES] = {
	{HM1055_MODE_720P_1280_720,
	 1280, 1280, 720, 720,
	 hm1055_setting_YUV_720P,
	 ARRAY_SIZE(hm1055_setting_YUV_720P)},
	{HM1055_MODE_VGA_640_480,
	 640, 640, 480, 480,
	 hm1055_setting_YUV_VGA,
	 ARRAY_SIZE(hm1055_setting_YUV_VGA)},
};

static int hm1055_write_reg(struct hm1055_dev *sensor, u16 reg, u8 val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg;
	u8 buf[3];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	buf[2] = val;

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = sizeof(buf);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "%s: error: reg=%x, val=%x ret=%d\n",
			__func__, reg, val, ret);
		return ret;
	}

	return 0;
}

static int hm1055_read_reg(struct hm1055_dev *sensor, u16 reg, u8 *val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg[2];
	u8 buf[2];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = buf;
	msg[0].len = sizeof(buf);

	msg[1].addr = client->addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = buf;
	msg[1].len = 1;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		dev_err(&client->dev, "%s: error: reg=%x i2c addr %x ret %d\n",
			__func__, reg, client->addr, ret);
		return ret;
	}

	*val = buf[0];
	return 0;
}

static int hm1055_load_regs(struct hm1055_dev *sensor,
			    const struct hm1055_mode_info *mode)
{
	int i;
	int ret = 0;
	const struct reg_value *regs = mode->reg_data;
	u16 reg_addr;
	u8 val;

	for (i = 0; i < mode->reg_data_size; ++i, ++regs) {
		reg_addr = regs->reg_addr;
		val = regs->val;
		ret = hm1055_write_reg(sensor, reg_addr, val);
		if (ret)
			break;
	}
	return 0;
}

static void hm1055_power(struct hm1055_dev *sensor, bool enable)
{
	if (enable) {
		gpiod_set_value_cansleep(sensor->pwdn_gpio, 1);
		udelay(100);
		gpiod_set_value_cansleep(sensor->pwdn_gpio, 0);
		udelay(100);
	}
}

static void hm1055_reset(struct hm1055_dev *sensor)
{
	if (!sensor->reset_gpio)
		return;
	/* camera power cycle */
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	udelay(100);
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	udelay(100);
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	udelay(100);

}

static int hm1055_set_power_on(struct hm1055_dev *sensor)
{
	hm1055_power(sensor, true);
	hm1055_reset(sensor);
	return 0;
}

static void hm1055_set_power_off(struct hm1055_dev *sensor)
{
	//hm1055_power(sensor, false);
	//clk_disable_unprepare(sensor->xclk);
}

static int hm1055_s_std(struct v4l2_subdev *sd, v4l2_std_id norm)
{
	return 0;
}

static int hm1055_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad != 0)
		return -EINVAL;
	if (code->index >= ARRAY_SIZE(hm1055_formats))
		return -EINVAL;

	code->code = hm1055_formats[code->index].code;
	return 0;
}

static int hm1055_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad != 0)
		return -EINVAL;
	if (fse->index >= HM1055_NUM_MODES)
		return -EINVAL;

	fse->min_width =
		hm1055_mode_data[fse->index].hact;
	fse->max_width = fse->min_width;
	fse->min_height =
		hm1055_mode_data[fse->index].vact;
	fse->max_height = fse->min_height;

	return 0;
}

static int hm1055_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct hm1055_dev *sensor = to_hm1055_dev(sd);
	struct v4l2_mbus_framefmt *fmt;

	if (format->pad != 0)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt = v4l2_subdev_get_try_format(&sensor->sd, cfg,
						 format->pad);
	else
		fmt = &sensor->fmt;

	format->format = *fmt;

	mutex_unlock(&sensor->lock);

	return 0;
}

static const struct hm1055_mode_info *
hm1055_find_mode(struct hm1055_dev *sensor, int width, int height, bool nearest)
{
	const struct hm1055_mode_info *mode;

	mode = v4l2_find_nearest_size(hm1055_mode_data,
				      ARRAY_SIZE(hm1055_mode_data),
				      hact, vact,
				      width, height);

	if (!mode ||
	    (!nearest && (mode->hact != width || mode->vact != height)))
		return NULL;

	return mode;
}

static int hm1055_try_fmt_internal(struct v4l2_subdev *sd,
				   struct v4l2_mbus_framefmt *fmt,
				   const struct hm1055_mode_info **new_mode)
{
	struct hm1055_dev *sensor = to_hm1055_dev(sd);
	const struct hm1055_mode_info *mode;
	int i;

	mode = hm1055_find_mode(sensor, fmt->width, fmt->height, true);
	if (!mode)
		return -EINVAL;
	fmt->width = mode->hact;
	fmt->height = mode->vact;

	if (new_mode)
		*new_mode = mode;

	for (i = 0; i < ARRAY_SIZE(hm1055_formats); i++)
		if (hm1055_formats[i].code == fmt->code)
			break;
	if (i >= ARRAY_SIZE(hm1055_formats))
		i = 0;

	fmt->code = hm1055_formats[i].code;
	fmt->colorspace = hm1055_formats[i].colorspace;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);

	return 0;
}

static int hm1055_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{

	struct hm1055_dev *sensor = to_hm1055_dev(sd);
	const struct hm1055_mode_info *new_mode;
	struct v4l2_mbus_framefmt *mbus_fmt = &format->format;
	struct v4l2_mbus_framefmt *fmt;
	int ret;

	if (format->pad != 0)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (sensor->streaming) {
		ret = -EBUSY;
		goto out;
	}

	ret = hm1055_try_fmt_internal(sd, mbus_fmt, &new_mode);
	if (ret)
		goto out;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt = v4l2_subdev_get_try_format(sd, cfg, 0);
	else
		fmt = &sensor->fmt;

	*fmt = *mbus_fmt;

	if (new_mode != sensor->current_mode) {
		sensor->current_mode = new_mode;
		dev_dbg(&sensor->i2c_client->dev,
			"id %d, width %d, height %d\n",
			sensor->current_mode->id,
			fmt->width,
			fmt->height);
		hm1055_load_regs(sensor, sensor->current_mode);
		sensor->pending_mode_change = true;
	}
	if (mbus_fmt->code != sensor->fmt.code)
		sensor->pending_fmt_change = true;

out:
	mutex_unlock(&sensor->lock);
	return ret;
}

static int hm1055_s_power(struct v4l2_subdev *sd, int on)
{
	struct hm1055_dev *sensor = to_hm1055_dev(sd);
	int ret = 0;

	mutex_lock(&sensor->lock);
	if (sensor->power_count == !on) {
		ret = hm1055_set_power_on(sensor);
		if (ret)
			goto out;
		hm1055_load_regs(sensor, sensor->current_mode);
	}

	/* Update the power count. */
	sensor->power_count += on ? 1 : -1;
	WARN_ON(sensor->power_count < 0);
out:
	mutex_unlock(&sensor->lock);
	return ret;
}

static int hm1055_log_status(struct v4l2_subdev *sd)
{
	return 0;
}


static const struct v4l2_subdev_core_ops hm1055_core_ops = {
	.s_power = hm1055_s_power,
	.log_status = hm1055_log_status,
};

static const struct v4l2_subdev_video_ops hm1055_video_ops = {
	.s_std = hm1055_s_std,
};

static const struct v4l2_subdev_pad_ops hm1055_pad_ops = {
	.enum_mbus_code = hm1055_enum_mbus_code,
	.enum_frame_size = hm1055_enum_frame_size,
	.get_fmt = hm1055_get_fmt,
	.set_fmt = hm1055_set_fmt,
};

static const struct v4l2_subdev_ops hm1055_subdev_ops = {
	.core = &hm1055_core_ops,
	.video = &hm1055_video_ops,
	.pad = &hm1055_pad_ops,
};

static int hm1055_check_chip_id(struct hm1055_dev *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int ret = 0;
	u8 chip_id[2];

	ret = hm1055_set_power_on(sensor);
	if (ret)
		return ret;

	hm1055_read_reg(sensor, 0x001, &chip_id[0]);
	hm1055_read_reg(sensor, 0x002, &chip_id[1]);
	dev_info(&client->dev, "chip id 0x%x%x\n", chip_id[0], chip_id[1]);
	if (chip_id[0] != 0x09 && chip_id[1] != 0x55) {
		dev_err(&client->dev, "%s: wrong chip identifier, expected 0x0955, got 0x%x%x\n",
			__func__, chip_id[0], chip_id[1]);
		ret = -ENXIO;
		hm1055_set_power_off(sensor);
		return ret;
	}
	return 0;
}

int hm1055_clk_init(struct device *dev)
{
	int ret;
	struct clk *clkcap,*clkaplldiv,*clkmux;
	struct clk *clk;
	int i32Div;
	u32 video_freq = 24000000;
	//ENTRY();
	
	of_property_read_u32_array(dev->of_node,"frequency", &video_freq,1);
	
	clk = clk_get(NULL, "cap_eclk");
	if (IS_ERR(clk)) {
		return -ENOENT;
	}
	clk_prepare(clk);
	clk_enable(clk);
	clk_prepare(clk_get(NULL, "cap_hclk"));
	clk_enable(clk_get(NULL, "cap_hclk"));
	clk_prepare(clk_get(NULL, "sensor_hclk"));
	clk_enable(clk_get(NULL, "sensor_hclk"));
	clkmux = clk_get(NULL, "cap_eclk_mux");
	if (IS_ERR(clkmux)) {
		printk(KERN_ERR "nuc970-cap:failed to get clock source\n");
		ret = PTR_ERR(clkmux);
		return ret;
	}
	clkcap = clk_get(NULL, "cap_eclk");
	if (IS_ERR(clkcap)) {
		printk(KERN_ERR "nuc970-cap:failed to get clock source\n");
		ret = PTR_ERR(clkcap);
		return ret;
	}
	clkaplldiv = clk_get(NULL, "cap_uplldiv");
	//clkaplldiv = clk_get(NULL, "cap0_eclk_div");
	if (IS_ERR(clkaplldiv)) {
		printk(KERN_ERR "nuc970-cap:failed to get clock source\n");
		ret = PTR_ERR(clkaplldiv);
		return ret;
	}
	clk_set_parent(clkmux, clkaplldiv);
	clk_set_rate(clkcap, video_freq);

	i32Div=(300000000/video_freq)-1;
	if(i32Div>0xF) i32Div=0xf;
	__raw_writel((__raw_readl(REG_CLK_DIV3) & ~(0xF<<24) ) | (i32Div<<24),REG_CLK_DIV3);
	printk("ccap0 clock setting %dHz OK\n",video_freq);

	return 0;
}

static int hm1055_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct hm1055_dev *sensor;
	struct v4l2_mbus_framefmt *fmt;
	int ret;

	/* let's see whether this adapter can support what we need */
	if (!i2c_check_functionality(client->adapter,
			I2C_FUNC_SMBUS_READ_BYTE_DATA |
			I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		return -EIO;

	v4l_info(client, "chip found @ 0x%x (%s)\n",
                        client->addr << 1, client->adapter->name);

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->i2c_client = client;

	/*
	 * default init sequence initialize sensor to
	 * YUV422 UYVY VGA@30fps
	 */
	fmt = &sensor->fmt;
	fmt->code = MEDIA_BUS_FMT_UYVY8_2X8;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
	fmt->width = 640;
	fmt->height = 480;
	fmt->field = V4L2_FIELD_NONE;
	sensor->current_mode =
		&hm1055_mode_data[HM1055_MODE_VGA_640_480];
	sensor->last_mode = sensor->current_mode;

	sensor->ae_target = 52;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev),
						  NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(endpoint, &sensor->ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "Could not parse endpoint\n");
		return ret;
	}

	hm1055_clk_init(dev);

	/* request optional power down pin */
	sensor->pwdn_gpio = devm_gpiod_get_optional(dev, "powerdown",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->pwdn_gpio)){
		printk("%s get pd pins failed ret %d\n",__func__,ret);
		return PTR_ERR(sensor->pwdn_gpio);
	}

	/* request optional reset pin */
	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio)){
		printk("%s get rest pins failed  ret %d\n",__func__,ret);
		return PTR_ERR(sensor->reset_gpio);
	}

	v4l2_i2c_subdev_init(&sensor->sd, client, &hm1055_subdev_ops);

	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret){
		printk("%s ret %d\n",__func__,ret);
		return ret;
	}

	mutex_init(&sensor->lock);

	ret = hm1055_check_chip_id(sensor);
	if (ret)
		//return ERR_PTR(-EPROBE_DEFER);
		goto entity_cleanup;

	ret = v4l2_async_register_subdev_sensor_common(&sensor->sd);
	if (ret){
		printk("%s ret 2%d\n",__func__,ret);
		goto free_ctrls;
	}

	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls.handler);
entity_cleanup:
	media_entity_cleanup(&sensor->sd.entity);
	mutex_destroy(&sensor->lock);
	return ret;
}

static int hm1055_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct hm1055_dev *sensor = to_hm1055_dev(sd);

	v4l2_async_unregister_subdev(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(&sensor->ctrls.handler);
	mutex_destroy(&sensor->lock);

	return 0;
}

static const struct i2c_device_id hm1055_id[] = {
	{"hm1055", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, hm1055_id);

static const struct of_device_id hm1055_dt_ids[] = {
	{.compatible = "himax,hm1055"},
	{ /* sentinel */ }
};

static struct i2c_driver hm1055_i2c_driver = {
	.driver = {
		   .name = "hm1055",
		   .of_match_table = hm1055_dt_ids,
		   },
	.id_table = hm1055_id,
	.probe = hm1055_probe,
	//.probe_new = hm1055_probe,
	.remove = hm1055_remove,
};

module_i2c_driver(hm1055_i2c_driver);

MODULE_DESCRIPTION("HM1055 Camera Subdev Driver");
MODULE_LICENSE("GPL v2");

