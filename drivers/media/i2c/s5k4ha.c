// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K4HA 8MP raw image sensor driver
 *
 * The register map follows the MIPI CCS layout. Register settings come from
 * the Samsung Galaxy Tab S5e (SM-T720, gts4lv) camera HAL sensor module data
 * (24 MHz EXTCLK, 4 lane MIPI CSI-2 D-PHY, RAW10); sequencing, gain format,
 * exposure margin and bayer order follow Samsung's fimc-is-cis-4ha driver.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define S5K4HA_MCLK_FREQ_24MHZ		(24 * HZ_PER_MHZ)
#define S5K4HA_DATA_LANES		4

/*
 * OP PLL: EXTCLK 24 MHz / 0x030d (6) * 0x030f (167) = 668 Mbps per lane,
 * i.e. a 334 MHz DDR link frequency; 0x0820 = 0x029c (668) matches.
 */
#define S5K4HA_LINK_FREQ_334MHZ		(334 * HZ_PER_MHZ)

/*
 * VT PLL: EXTCLK 24 MHz / 0x0305 (6) * 0x0307 (140) = 560 MHz. The line
 * length (0x0342) counts in units of a 280 MHz pixel clock, i.e. 560 MHz / 2.
 * This is what the vendor's own frame rates come out of:
 *
 *   3264x2448: 280e6 / (FLL 2530 * LLP 3688) = 30.008 fps  (vendor: 30.01)
 *    800x600:  280e6 / (FLL  632 * LLP 3688) = 120.13 fps  (vendor: 120.13)
 *
 * so the pixel rate (for timing, i.e. fps = PIXEL_RATE / (HTS * VTS)) is
 * 280 MHz. The CSI-2 output rate is lower, 668 Mbps * 4 / 10 = 267.2 Mpix/s
 * (the vendor's "outputPixelClock"); line blanking absorbs the difference.
 */
#define S5K4HA_PIXEL_RATE		(280 * HZ_PER_MHZ)

/* Register map is similar to MIPI CCS compliant camera sensors */
#define S5K4HA_REG_CHIP_ID		CCI_REG16(0x0000)
#define S5K4HA_CHIP_ID			0x48ab
#define S5K4HA_REG_REVISION		CCI_REG8(0x0002)

#define S5K4HA_REG_MODE_SELECT		CCI_REG8(0x0100)
#define S5K4HA_MODE_STANDBY		0x00
#define S5K4HA_MODE_STREAMING		0x01

/* Samsung "ARM" software reset, as done by the vendor before the global list */
#define S5K4HA_REG_PAGE_SELECT		CCI_REG16(0xfcfc)
#define S5K4HA_REG_SW_RESET		CCI_REG16(0x6010)

#define S5K4HA_REG_EXPOSURE		CCI_REG16(0x0202)
#define S5K4HA_EXPOSURE_MIN		2
#define S5K4HA_EXPOSURE_STEP		1
#define S5K4HA_EXPOSURE_MARGIN		4

/* Analogue gain code: 0x20 = 1x (5 fractional bits) */
#define S5K4HA_REG_AGAIN		CCI_REG16(0x0204)
#define S5K4HA_REG_AGAIN_MIN		CCI_REG16(0x0084)
#define S5K4HA_REG_AGAIN_MAX		CCI_REG16(0x0086)
#define S5K4HA_AGAIN_MIN		0x0020
#define S5K4HA_AGAIN_MAX		0x0200
#define S5K4HA_AGAIN_STEP		1
#define S5K4HA_AGAIN_DEFAULT		0x0020

#define S5K4HA_REG_FLL			CCI_REG16(0x0340)
#define S5K4HA_FLL_MAX			0xffff
#define S5K4HA_REG_LLP			CCI_REG16(0x0342)

#define S5K4HA_REG_TEST_PATTERN		CCI_REG16(0x0600)

/* Pixel array; the vendor modes read out x/y 8..3271 / 8..2455 at most */
#define S5K4HA_NATIVE_WIDTH		3280
#define S5K4HA_NATIVE_HEIGHT		2464
#define S5K4HA_ACTIVE_LEFT		8
#define S5K4HA_ACTIVE_TOP		8
#define S5K4HA_ACTIVE_WIDTH		3264
#define S5K4HA_ACTIVE_HEIGHT		2448

/* fimc-is-cis-4ha: OTF_INPUT_ORDER_BAYER_GR_BG */
#define S5K4HA_MBUS_CODE		MEDIA_BUS_FMT_SGRBG10_1X10

#define to_s5k4ha(_sd)			container_of(_sd, struct s5k4ha, sd)

static const s64 s5k4ha_link_freq_menu[] = {
	S5K4HA_LINK_FREQ_334MHZ,
};

struct s5k4ha_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct s5k4ha_mode {
	u32 width;			/* Output width in pixels */
	u32 height;			/* Output height in pixels */
	u32 hts;			/* Line length in pixel clocks (0x0342) */
	u32 vts;			/* Default frame length in lines (0x0340) */
	u32 exposure;			/* Default exposure in lines */
	struct v4l2_rect crop;		/* Analogue crop in the pixel array */

	const struct s5k4ha_reg_list reg_list;
};

static const char * const s5k4ha_test_pattern_menu[] = {
	"Disabled",
	"Solid colour",
	"Colour bars",
	"Fade to grey colour bars",
	"PN9",
};

static const char * const s5k4ha_supply_names[] = {
	"vddio",	/* Digital I/O power */
	"vdda",		/* Analog power */
	"vddd",		/* Digital core power */
};

#define S5K4HA_NUM_SUPPLIES	ARRAY_SIZE(s5k4ha_supply_names)

struct s5k4ha {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[S5K4HA_NUM_SUPPLIES];

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *again;

	u32 again_min;
	u32 again_max;
};

/* Global init, written after a software reset (vendor "initSettings") */
static const struct cci_reg_sequence s5k4ha_global_regs[] = {
	{ CCI_REG8(0x0a02), 0x7f },
	{ CCI_REG8(0x3b45), 0x01 },
	{ CCI_REG8(0x3264), 0x01 },
	{ CCI_REG8(0x3290), 0x10 },
	{ CCI_REG8(0x0b05), 0x01 },
	{ CCI_REG8(0x3069), 0xc7 },
	{ CCI_REG8(0x3074), 0x06 },
	{ CCI_REG8(0x3075), 0x32 },
	{ CCI_REG8(0x3068), 0xf7 },
	{ CCI_REG8(0x30c6), 0x01 },
	{ CCI_REG8(0x301f), 0x20 },
	{ CCI_REG8(0x306b), 0x9a },
	{ CCI_REG8(0x3091), 0x1f },
	{ CCI_REG8(0x306e), 0x71 },
	{ CCI_REG8(0x306f), 0x28 },
	{ CCI_REG8(0x306d), 0x08 },
	{ CCI_REG8(0x3084), 0x16 },
	{ CCI_REG8(0x3070), 0x0f },
	{ CCI_REG8(0x306a), 0x79 },
	{ CCI_REG8(0x30b0), 0xff },
	{ CCI_REG8(0x30c2), 0x05 },
	{ CCI_REG8(0x30c4), 0x06 },
	{ CCI_REG8(0x3081), 0x07 },
	{ CCI_REG8(0x307b), 0x85 },
	{ CCI_REG8(0x307a), 0x0a },
	{ CCI_REG8(0x3079), 0x0a },
	{ CCI_REG8(0x308a), 0x20 },
	{ CCI_REG8(0x308b), 0x08 },
	{ CCI_REG8(0x308c), 0x0b },
	{ CCI_REG8(0x392f), 0x01 },
	{ CCI_REG8(0x3930), 0x00 },
	{ CCI_REG8(0x3924), 0x7f },
	{ CCI_REG8(0x3925), 0xfd },
	{ CCI_REG8(0x3c08), 0xff },
	{ CCI_REG8(0x3c09), 0xff },
	{ CCI_REG8(0x3c31), 0xff },
	{ CCI_REG8(0x3c32), 0xff },
};

/* 3264x2448 full readout, 30 fps, 24 MHz EXTCLK, 4 lanes, RAW10 */
static const struct cci_reg_sequence s5k4ha_3264x2448_regs[] = {
	{ CCI_REG8(0x0136), 0x18 },	/* EXTCLK 24 MHz */
	{ CCI_REG8(0x0137), 0x00 },
	{ CCI_REG8(0x0301), 0x04 },	/* vt_pix_clk_div */
	{ CCI_REG8(0x0305), 0x06 },	/* pre_pll_clk_div */
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0x8c },	/* pll_multiplier 140 */
	{ CCI_REG8(0x3c1f), 0x00 },
	{ CCI_REG8(0x030d), 0x06 },	/* op_pre_pll_clk_div */
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xa7 },	/* op_pll_multiplier 167 */
	{ CCI_REG8(0x3c17), 0x00 },
	{ CCI_REG8(0x0340), 0x09 },	/* frame_length_lines 2530 */
	{ CCI_REG8(0x0341), 0xe2 },
	{ CCI_REG8(0x0342), 0x0e },	/* line_length_pck 3688 */
	{ CCI_REG8(0x0343), 0x68 },
	{ CCI_REG8(0x0344), 0x00 },	/* x_addr_start 8 */
	{ CCI_REG8(0x0345), 0x08 },
	{ CCI_REG8(0x0346), 0x00 },	/* y_addr_start 8 */
	{ CCI_REG8(0x0347), 0x08 },
	{ CCI_REG8(0x0348), 0x0c },	/* x_addr_end 3271 */
	{ CCI_REG8(0x0349), 0xc7 },
	{ CCI_REG8(0x034a), 0x09 },	/* y_addr_end 2455 */
	{ CCI_REG8(0x034b), 0x97 },
	{ CCI_REG8(0x034c), 0x0c },	/* x_output_size 3264 */
	{ CCI_REG8(0x034d), 0xc0 },
	{ CCI_REG8(0x034e), 0x09 },	/* y_output_size 2448 */
	{ CCI_REG8(0x034f), 0x90 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x00 },	/* binning off */
	{ CCI_REG8(0x0901), 0x00 },
	{ CCI_REG8(0x0204), 0x00 },	/* analogue gain 1x */
	{ CCI_REG8(0x0205), 0x20 },
	{ CCI_REG8(0x0200), 0x0d },	/* fine integration time */
	{ CCI_REG8(0x0201), 0xd8 },
	{ CCI_REG8(0x0202), 0x00 },	/* coarse integration time */
	{ CCI_REG8(0x0203), 0x02 },
	{ CCI_REG8(0x0820), 0x02 },	/* requested link rate 668 Mbps */
	{ CCI_REG8(0x0821), 0x9c },
	{ CCI_REG8(0x0822), 0x00 },
	{ CCI_REG8(0x0823), 0x00 },
	{ CCI_REG8(0x0112), 0x0a },	/* RAW10 */
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },	/* 4 lanes */
	{ CCI_REG8(0x3929), 0x3f },
	{ CCI_REG8(0x0101), 0x00 },	/* no flip */
	{ CCI_REG8(0x3941), 0x0e },
	{ CCI_REG8(0x3942), 0x11 },
};

/* 1632x1224 2x2 binned, 30 fps, 24 MHz EXTCLK, 4 lanes, RAW10 */
static const struct cci_reg_sequence s5k4ha_1632x1224_regs[] = {
	{ CCI_REG8(0x0136), 0x18 },	/* EXTCLK 24 MHz */
	{ CCI_REG8(0x0137), 0x00 },
	{ CCI_REG8(0x0301), 0x04 },
	{ CCI_REG8(0x0305), 0x06 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0x8c },
	{ CCI_REG8(0x3c1f), 0x00 },
	{ CCI_REG8(0x030d), 0x06 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xa7 },
	{ CCI_REG8(0x3c17), 0x00 },
	{ CCI_REG8(0x0340), 0x09 },	/* frame_length_lines 2530 */
	{ CCI_REG8(0x0341), 0xe2 },
	{ CCI_REG8(0x0342), 0x0e },	/* line_length_pck 3688 */
	{ CCI_REG8(0x0343), 0x68 },
	{ CCI_REG8(0x0344), 0x00 },	/* x_addr_start 8 */
	{ CCI_REG8(0x0345), 0x08 },
	{ CCI_REG8(0x0346), 0x00 },	/* y_addr_start 8 */
	{ CCI_REG8(0x0347), 0x08 },
	{ CCI_REG8(0x0348), 0x0c },	/* x_addr_end 3271 */
	{ CCI_REG8(0x0349), 0xc7 },
	{ CCI_REG8(0x034a), 0x09 },	/* y_addr_end 2455 */
	{ CCI_REG8(0x034b), 0x97 },
	{ CCI_REG8(0x034c), 0x06 },	/* x_output_size 1632 */
	{ CCI_REG8(0x034d), 0x60 },
	{ CCI_REG8(0x034e), 0x04 },	/* y_output_size 1224 */
	{ CCI_REG8(0x034f), 0xc8 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x03 },
	{ CCI_REG8(0x0900), 0x01 },	/* binning on, 2x2 */
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x20 },
	{ CCI_REG8(0x0200), 0x0d },
	{ CCI_REG8(0x0201), 0xd8 },
	{ CCI_REG8(0x0202), 0x00 },
	{ CCI_REG8(0x0203), 0x02 },
	{ CCI_REG8(0x0820), 0x02 },
	{ CCI_REG8(0x0821), 0x9c },
	{ CCI_REG8(0x0822), 0x00 },
	{ CCI_REG8(0x0823), 0x00 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x3929), 0x3f },
	{ CCI_REG8(0x0101), 0x00 },
	{ CCI_REG8(0x3941), 0x0e },
	{ CCI_REG8(0x3942), 0x11 },
};

/* 800x600 4x4 binned, 120 fps, 24 MHz EXTCLK, 4 lanes, RAW10 */
static const struct cci_reg_sequence s5k4ha_800x600_regs[] = {
	{ CCI_REG8(0x0136), 0x18 },	/* EXTCLK 24 MHz */
	{ CCI_REG8(0x0137), 0x00 },
	{ CCI_REG8(0x0301), 0x04 },
	{ CCI_REG8(0x0305), 0x06 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0x8c },
	{ CCI_REG8(0x3c1f), 0x00 },
	{ CCI_REG8(0x030d), 0x06 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xa7 },
	{ CCI_REG8(0x3c17), 0x00 },
	{ CCI_REG8(0x0340), 0x02 },	/* frame_length_lines 632 */
	{ CCI_REG8(0x0341), 0x78 },
	{ CCI_REG8(0x0342), 0x0e },	/* line_length_pck 3688 */
	{ CCI_REG8(0x0343), 0x68 },
	{ CCI_REG8(0x0344), 0x00 },	/* x_addr_start 40 */
	{ CCI_REG8(0x0345), 0x28 },
	{ CCI_REG8(0x0346), 0x00 },	/* y_addr_start 32 */
	{ CCI_REG8(0x0347), 0x20 },
	{ CCI_REG8(0x0348), 0x0c },	/* x_addr_end 3239 */
	{ CCI_REG8(0x0349), 0xa7 },
	{ CCI_REG8(0x034a), 0x09 },	/* y_addr_end 2431 */
	{ CCI_REG8(0x034b), 0x7f },
	{ CCI_REG8(0x034c), 0x03 },	/* x_output_size 800 */
	{ CCI_REG8(0x034d), 0x20 },
	{ CCI_REG8(0x034e), 0x02 },	/* y_output_size 600 */
	{ CCI_REG8(0x034f), 0x58 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x07 },
	{ CCI_REG8(0x0900), 0x01 },	/* binning on, 4x4 */
	{ CCI_REG8(0x0901), 0x44 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x20 },
	{ CCI_REG8(0x0200), 0x0d },
	{ CCI_REG8(0x0201), 0xd8 },
	{ CCI_REG8(0x0202), 0x00 },
	{ CCI_REG8(0x0203), 0x02 },
	{ CCI_REG8(0x0820), 0x02 },
	{ CCI_REG8(0x0821), 0x9c },
	{ CCI_REG8(0x0822), 0x00 },
	{ CCI_REG8(0x0823), 0x00 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x3929), 0x3f },
	{ CCI_REG8(0x0101), 0x00 },
	{ CCI_REG8(0x3941), 0x0e },
	{ CCI_REG8(0x3942), 0x11 },
};

static const struct s5k4ha_mode s5k4ha_modes[] = {
	{
		.width = 3264,
		.height = 2448,
		.hts = 3688,
		.vts = 2530,
		.exposure = 2000,
		.crop = {
			.left = 8,
			.top = 8,
			.width = 3264,
			.height = 2448,
		},
		.reg_list = {
			.regs = s5k4ha_3264x2448_regs,
			.num_regs = ARRAY_SIZE(s5k4ha_3264x2448_regs),
		},
	},
	{
		.width = 1632,
		.height = 1224,
		.hts = 3688,
		.vts = 2530,
		.exposure = 2000,
		.crop = {
			.left = 8,
			.top = 8,
			.width = 3264,
			.height = 2448,
		},
		.reg_list = {
			.regs = s5k4ha_1632x1224_regs,
			.num_regs = ARRAY_SIZE(s5k4ha_1632x1224_regs),
		},
	},
	{
		.width = 800,
		.height = 600,
		.hts = 3688,
		.vts = 632,
		.exposure = 600,
		.crop = {
			.left = 40,
			.top = 32,
			.width = 3200,
			.height = 2400,
		},
		.reg_list = {
			.regs = s5k4ha_800x600_regs,
			.num_regs = ARRAY_SIZE(s5k4ha_800x600_regs),
		},
	},
};

static const struct s5k4ha_mode *
s5k4ha_find_mode(const struct v4l2_mbus_framefmt *fmt)
{
	return v4l2_find_nearest_size(s5k4ha_modes, ARRAY_SIZE(s5k4ha_modes),
				      width, height, fmt->width, fmt->height);
}

static int s5k4ha_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k4ha *s5k4ha = container_of(ctrl->handler, struct s5k4ha,
					     ctrl_handler);
	struct v4l2_subdev_state *state;
	const struct v4l2_mbus_framefmt *fmt;
	s64 exposure_max;
	int ret;

	state = v4l2_subdev_get_locked_active_state(&s5k4ha->sd);
	fmt = v4l2_subdev_state_get_format(state, 0);

	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Update max exposure while meeting expected vblanking */
		exposure_max = fmt->height + ctrl->val - S5K4HA_EXPOSURE_MARGIN;
		ret = __v4l2_ctrl_modify_range(s5k4ha->exposure,
					       s5k4ha->exposure->minimum,
					       exposure_max,
					       s5k4ha->exposure->step,
					       min(s5k4ha->exposure->default_value,
						   exposure_max));
		if (ret)
			return ret;
	}

	/* V4L2 controls are applied when the sensor is powered up for streaming */
	if (!pm_runtime_get_if_active(s5k4ha->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(s5k4ha->regmap, S5K4HA_REG_AGAIN, ctrl->val,
				NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(s5k4ha->regmap, S5K4HA_REG_EXPOSURE, ctrl->val,
				NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(s5k4ha->regmap, S5K4HA_REG_FLL,
				fmt->height + ctrl->val, NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(s5k4ha->regmap, S5K4HA_REG_TEST_PATTERN,
				ctrl->val, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(s5k4ha->dev);

	return ret;
}

static const struct v4l2_ctrl_ops s5k4ha_ctrl_ops = {
	.s_ctrl = s5k4ha_set_ctrl,
};

static int s5k4ha_init_controls(struct s5k4ha *s5k4ha)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &s5k4ha->ctrl_handler;
	const struct s5k4ha_mode *mode = &s5k4ha_modes[0];
	struct v4l2_fwnode_device_properties props;
	s64 hblank, vblank, exposure_max;
	int ret;

	ret = v4l2_fwnode_device_parse(s5k4ha->dev, &props);
	if (ret)
		return ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 10);

	s5k4ha->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &s5k4ha_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(s5k4ha_link_freq_menu) - 1,
						   0, s5k4ha_link_freq_menu);
	if (s5k4ha->link_freq)
		s5k4ha->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s5k4ha->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &s5k4ha_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       S5K4HA_PIXEL_RATE,
					       S5K4HA_PIXEL_RATE, 1,
					       S5K4HA_PIXEL_RATE);

	hblank = mode->hts - mode->width;
	s5k4ha->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k4ha_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank, 1,
					   hblank);
	if (s5k4ha->hblank)
		s5k4ha->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	s5k4ha->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k4ha_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   S5K4HA_FLL_MAX - mode->height, 1,
					   vblank);

	s5k4ha->again = v4l2_ctrl_new_std(ctrl_hdlr, &s5k4ha_ctrl_ops,
					  V4L2_CID_ANALOGUE_GAIN,
					  s5k4ha->again_min, s5k4ha->again_max,
					  S5K4HA_AGAIN_STEP,
					  clamp_t(u32, S5K4HA_AGAIN_DEFAULT,
						  s5k4ha->again_min,
						  s5k4ha->again_max));

	exposure_max = mode->vts - S5K4HA_EXPOSURE_MARGIN;
	s5k4ha->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &s5k4ha_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     S5K4HA_EXPOSURE_MIN, exposure_max,
					     S5K4HA_EXPOSURE_STEP,
					     mode->exposure);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &s5k4ha_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k4ha_test_pattern_menu) - 1,
				     0, 0, s5k4ha_test_pattern_menu);

	v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &s5k4ha_ctrl_ops, &props);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ret;
	}

	s5k4ha->sd.ctrl_handler = ctrl_hdlr;

	return 0;
}

static int s5k4ha_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct s5k4ha *s5k4ha = to_s5k4ha(sd);
	const struct s5k4ha_mode *mode;
	int ret;

	mode = s5k4ha_find_mode(v4l2_subdev_state_get_format(state, 0));

	ret = pm_runtime_resume_and_get(s5k4ha->dev);
	if (ret)
		return ret;

	/* Software reset, then let the sensor firmware come up (vendor: 3 ms) */
	cci_write(s5k4ha->regmap, S5K4HA_REG_MODE_SELECT, S5K4HA_MODE_STANDBY,
		  &ret);
	cci_write(s5k4ha->regmap, S5K4HA_REG_PAGE_SELECT, 0x4000, &ret);
	cci_write(s5k4ha->regmap, S5K4HA_REG_SW_RESET, 0x0001, &ret);
	if (ret)
		goto error;

	usleep_range(3000, 4000);

	cci_multi_reg_write(s5k4ha->regmap, s5k4ha_global_regs,
			    ARRAY_SIZE(s5k4ha_global_regs), &ret);
	cci_multi_reg_write(s5k4ha->regmap, mode->reg_list.regs,
			    mode->reg_list.num_regs, &ret);
	if (ret)
		goto error;

	/* Apply the current exposure, gain, frame length and test pattern */
	ret = __v4l2_ctrl_handler_setup(s5k4ha->sd.ctrl_handler);
	if (ret)
		goto error;

	ret = cci_write(s5k4ha->regmap, S5K4HA_REG_MODE_SELECT,
			S5K4HA_MODE_STREAMING, NULL);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(s5k4ha->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(s5k4ha->dev);

	return ret;
}

static int s5k4ha_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct s5k4ha *s5k4ha = to_s5k4ha(sd);
	int ret;

	ret = cci_write(s5k4ha->regmap, S5K4HA_REG_MODE_SELECT,
			S5K4HA_MODE_STANDBY, NULL);
	if (ret)
		dev_err(s5k4ha->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(s5k4ha->dev);

	return ret;
}

static void s5k4ha_update_pad_format(const struct s5k4ha_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = S5K4HA_MBUS_CODE;
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5k4ha_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct s5k4ha *s5k4ha = to_s5k4ha(sd);
	const struct s5k4ha_mode *mode;
	s64 hblank, vblank, exposure_max;
	int ret;

	mode = s5k4ha_find_mode(&fmt->format);
	s5k4ha_update_pad_format(mode, &fmt->format);

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		if (v4l2_subdev_is_streaming(sd))
			return -EBUSY;

		/* Update limits and reset frame rate and exposure to defaults */
		hblank = mode->hts - mode->width;
		ret = __v4l2_ctrl_modify_range(s5k4ha->hblank, hblank, hblank,
					       1, hblank);
		if (ret)
			return ret;

		vblank = mode->vts - mode->height;
		ret = __v4l2_ctrl_modify_range(s5k4ha->vblank, vblank,
					       S5K4HA_FLL_MAX - mode->height,
					       1, vblank);
		if (ret)
			return ret;

		ret = __v4l2_ctrl_s_ctrl(s5k4ha->vblank, vblank);
		if (ret)
			return ret;

		exposure_max = mode->vts - S5K4HA_EXPOSURE_MARGIN;
		ret = __v4l2_ctrl_modify_range(s5k4ha->exposure,
					       S5K4HA_EXPOSURE_MIN,
					       exposure_max,
					       S5K4HA_EXPOSURE_STEP,
					       mode->exposure);
		if (ret)
			return ret;

		ret = __v4l2_ctrl_s_ctrl(s5k4ha->exposure, mode->exposure);
		if (ret)
			return ret;
	}

	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	*v4l2_subdev_state_get_crop(state, 0) = mode->crop;

	return 0;
}

static int s5k4ha_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = S5K4HA_MBUS_CODE;

	return 0;
}

static int s5k4ha_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(s5k4ha_modes))
		return -EINVAL;

	if (fse->code != S5K4HA_MBUS_CODE)
		return -EINVAL;

	fse->min_width = s5k4ha_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = s5k4ha_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int s5k4ha_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(sd_state, 0);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = S5K4HA_NATIVE_WIDTH;
		sel->r.height = S5K4HA_NATIVE_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.left = S5K4HA_ACTIVE_LEFT;
		sel->r.top = S5K4HA_ACTIVE_TOP;
		sel->r.width = S5K4HA_ACTIVE_WIDTH;
		sel->r.height = S5K4HA_ACTIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int s5k4ha_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = S5K4HA_MBUS_CODE,
			.width = s5k4ha_modes[0].width,
			.height = s5k4ha_modes[0].height,
		},
	};

	return s5k4ha_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops s5k4ha_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5k4ha_pad_ops = {
	.set_fmt = s5k4ha_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = s5k4ha_get_selection,
	.enum_mbus_code = s5k4ha_enum_mbus_code,
	.enum_frame_size = s5k4ha_enum_frame_size,
	.enable_streams = s5k4ha_enable_streams,
	.disable_streams = s5k4ha_disable_streams,
};

static const struct v4l2_subdev_ops s5k4ha_subdev_ops = {
	.video = &s5k4ha_video_ops,
	.pad = &s5k4ha_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k4ha_internal_ops = {
	.init_state = s5k4ha_init_state,
};

static const struct media_entity_operations s5k4ha_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int s5k4ha_identify_sensor(struct s5k4ha *s5k4ha)
{
	u64 val, rev = 0, gmin = 0, gmax = 0;
	int ret;

	ret = cci_read(s5k4ha->regmap, S5K4HA_REG_CHIP_ID, &val, NULL);
	if (ret) {
		dev_err(s5k4ha->dev, "failed to read chip id: %d\n", ret);
		return ret;
	}

	if (val != S5K4HA_CHIP_ID) {
		dev_err(s5k4ha->dev, "chip id mismatch: expected 0x%04x, read 0x%04llx\n",
			S5K4HA_CHIP_ID, val);
		return -ENODEV;
	}

	cci_read(s5k4ha->regmap, S5K4HA_REG_REVISION, &rev, &ret);
	cci_read(s5k4ha->regmap, S5K4HA_REG_AGAIN_MIN, &gmin, &ret);
	cci_read(s5k4ha->regmap, S5K4HA_REG_AGAIN_MAX, &gmax, &ret);

	/* Use the sensor's own analogue gain limits when they look sane */
	s5k4ha->again_min = S5K4HA_AGAIN_MIN;
	s5k4ha->again_max = S5K4HA_AGAIN_MAX;
	if (!ret && gmin >= S5K4HA_AGAIN_MIN && gmax > gmin && gmax <= 0x0400) {
		s5k4ha->again_min = gmin;
		s5k4ha->again_max = gmax;
	}

	dev_dbg(s5k4ha->dev,
		"S5K4HA rev 0x%02llx, analogue gain code 0x%04llx..0x%04llx, using 0x%04x..0x%04x\n",
		rev, gmin, gmax, s5k4ha->again_min, s5k4ha->again_max);

	return 0;
}

static int s5k4ha_check_hwcfg(struct s5k4ha *s5k4ha)
{
	struct fwnode_handle *fwnode = dev_fwnode(s5k4ha->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long freq_bitmap;
	int ret;

	if (!fwnode)
		return -ENODEV;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != S5K4HA_DATA_LANES) {
		dev_err(s5k4ha->dev, "invalid number of data lanes: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(s5k4ha->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       s5k4ha_link_freq_menu,
				       ARRAY_SIZE(s5k4ha_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int s5k4ha_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k4ha *s5k4ha = to_s5k4ha(sd);
	int ret;

	gpiod_set_value_cansleep(s5k4ha->reset_gpio, 1);

	ret = regulator_bulk_enable(S5K4HA_NUM_SUPPLIES, s5k4ha->supplies);
	if (ret)
		return ret;

	usleep_range(1000, 2000);

	ret = clk_prepare_enable(s5k4ha->mclk);
	if (ret)
		goto disable_regulators;

	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(s5k4ha->reset_gpio, 0);

	/* Wait for the sensor to leave reset before talking to it over CCI */
	usleep_range(10000, 11000);

	return 0;

disable_regulators:
	regulator_bulk_disable(S5K4HA_NUM_SUPPLIES, s5k4ha->supplies);

	return ret;
}

static int s5k4ha_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k4ha *s5k4ha = to_s5k4ha(sd);

	gpiod_set_value_cansleep(s5k4ha->reset_gpio, 1);
	usleep_range(1000, 2000);

	clk_disable_unprepare(s5k4ha->mclk);

	regulator_bulk_disable(S5K4HA_NUM_SUPPLIES, s5k4ha->supplies);

	return 0;
}

static int s5k4ha_probe(struct i2c_client *client)
{
	struct s5k4ha *s5k4ha;
	unsigned long freq;
	unsigned int i;
	int ret;

	s5k4ha = devm_kzalloc(&client->dev, sizeof(*s5k4ha), GFP_KERNEL);
	if (!s5k4ha)
		return -ENOMEM;

	s5k4ha->dev = &client->dev;
	v4l2_i2c_subdev_init(&s5k4ha->sd, client, &s5k4ha_subdev_ops);

	s5k4ha->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s5k4ha->regmap))
		return dev_err_probe(s5k4ha->dev, PTR_ERR(s5k4ha->regmap),
				     "failed to init CCI\n");

	s5k4ha->mclk = devm_v4l2_sensor_clk_get(s5k4ha->dev, NULL);
	if (IS_ERR(s5k4ha->mclk))
		return dev_err_probe(s5k4ha->dev, PTR_ERR(s5k4ha->mclk),
				     "failed to get MCLK clock\n");

	freq = clk_get_rate(s5k4ha->mclk);
	if (freq != S5K4HA_MCLK_FREQ_24MHZ)
		return dev_err_probe(s5k4ha->dev, -EINVAL,
				     "MCLK clock frequency %lu is not supported\n",
				     freq);

	ret = s5k4ha_check_hwcfg(s5k4ha);
	if (ret)
		return dev_err_probe(s5k4ha->dev, ret,
				     "failed to check HW configuration\n");

	s5k4ha->reset_gpio = devm_gpiod_get_optional(s5k4ha->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(s5k4ha->reset_gpio))
		return dev_err_probe(s5k4ha->dev, PTR_ERR(s5k4ha->reset_gpio),
				     "cannot get reset GPIO\n");

	for (i = 0; i < S5K4HA_NUM_SUPPLIES; i++)
		s5k4ha->supplies[i].supply = s5k4ha_supply_names[i];

	ret = devm_regulator_bulk_get(s5k4ha->dev, S5K4HA_NUM_SUPPLIES,
				      s5k4ha->supplies);
	if (ret)
		return dev_err_probe(s5k4ha->dev, ret,
				     "failed to get supply regulators\n");

	/* The sensor must be powered on to read the CHIP_ID register */
	ret = s5k4ha_power_on(s5k4ha->dev);
	if (ret)
		return dev_err_probe(s5k4ha->dev, ret, "failed to power on\n");

	ret = s5k4ha_identify_sensor(s5k4ha);
	if (ret) {
		dev_err_probe(s5k4ha->dev, ret, "failed to find sensor\n");
		goto power_off;
	}

	ret = s5k4ha_init_controls(s5k4ha);
	if (ret) {
		dev_err_probe(s5k4ha->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	s5k4ha->sd.state_lock = s5k4ha->ctrl_handler.lock;
	s5k4ha->sd.internal_ops = &s5k4ha_internal_ops;
	s5k4ha->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s5k4ha->sd.entity.ops = &s5k4ha_subdev_entity_ops;
	s5k4ha->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s5k4ha->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&s5k4ha->sd.entity, 1, &s5k4ha->pad);
	if (ret) {
		dev_err_probe(s5k4ha->dev, ret,
			      "failed to init media entity pads\n");
		goto v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&s5k4ha->sd);
	if (ret < 0) {
		dev_err_probe(s5k4ha->dev, ret, "failed to init subdev\n");
		goto media_entity_cleanup;
	}

	pm_runtime_set_active(s5k4ha->dev);
	pm_runtime_enable(s5k4ha->dev);

	ret = v4l2_async_register_subdev_sensor(&s5k4ha->sd);
	if (ret < 0) {
		dev_err_probe(s5k4ha->dev, ret,
			      "failed to register V4L2 subdev\n");
		goto subdev_cleanup;
	}

	pm_runtime_set_autosuspend_delay(s5k4ha->dev, 1000);
	pm_runtime_use_autosuspend(s5k4ha->dev);
	pm_runtime_idle(s5k4ha->dev);

	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&s5k4ha->sd);
	pm_runtime_disable(s5k4ha->dev);
	pm_runtime_set_suspended(s5k4ha->dev);

media_entity_cleanup:
	media_entity_cleanup(&s5k4ha->sd.entity);

v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(s5k4ha->sd.ctrl_handler);

power_off:
	s5k4ha_power_off(s5k4ha->dev);

	return ret;
}

static void s5k4ha_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k4ha *s5k4ha = to_s5k4ha(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(s5k4ha->dev);

	if (!pm_runtime_status_suspended(s5k4ha->dev)) {
		s5k4ha_power_off(s5k4ha->dev);
		pm_runtime_set_suspended(s5k4ha->dev);
	}
}

static DEFINE_RUNTIME_DEV_PM_OPS(s5k4ha_pm_ops, s5k4ha_power_off,
				 s5k4ha_power_on, NULL);

static const struct of_device_id s5k4ha_of_match[] = {
	{ .compatible = "samsung,s5k4ha" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s5k4ha_of_match);

static struct i2c_driver s5k4ha_i2c_driver = {
	.driver = {
		.name = "s5k4ha",
		.pm = pm_ptr(&s5k4ha_pm_ops),
		.of_match_table = s5k4ha_of_match,
	},
	.probe = s5k4ha_probe,
	.remove = s5k4ha_remove,
};

module_i2c_driver(s5k4ha_i2c_driver);

MODULE_DESCRIPTION("Samsung S5K4HA image sensor driver");
MODULE_LICENSE("GPL");
