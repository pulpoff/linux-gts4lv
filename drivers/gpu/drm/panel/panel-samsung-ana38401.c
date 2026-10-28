// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung AMSA05RB01 AMOLED panel behind an Anapass ANA38401 TCON, as used in
 * the Galaxy Tab S5e (SM-T720): 2560x1600, two 1280x1600 halves on two DSI
 * links, command mode.
 *
 * Sequences and tables are from Samsung's downstream device tree
 * (dsi_panel_ANA38401_AMSA05RB01_wqxga_octa_cmd.dtsi) and the "anapass power
 * sequence" in its dsi_panel.c: VDD on -> LP11 -> reset -> TCON ready -> init
 * -> display on no earlier than 220 ms after the reset. The TCON gets no
 * sleep-out command.
 *
 * Brightness is AID (emission duty) plus a gamma (83h) per luminance step, as
 * Samsung's smart dimming does; the gamma tables are computed offline without
 * the panel's MTP offsets (panel-samsung-ana38401-gamma.h). Changes fade over
 * about half a second (module parameter "fade").
 */

#include <linux/backlight.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include "panel-samsung-ana38401-gamma.h"

#define ANA38401_SLEEP_OUT_DELAY_MS	220

/*
 * After a warm reboot ABL leaves the panel powered but not refreshed (the
 * last frame stays up, black once MDP takes over), so take it over only on
 * request and otherwise run the whole power-on sequence.
 */
static bool handover;
module_param(handover, bool, 0444);
MODULE_PARM_DESC(handover, "Keep the bootloader's panel state instead of re-initialising it");

static bool keep_power = true;
module_param(keep_power, bool, 0644);
MODULE_PARM_DESC(keep_power, "Blank with display off only, keeping the panel powered");

static int bl_mode = 1;
module_param(bl_mode, int, 0644);
MODULE_PARM_DESC(bl_mode, "Brightness commands: 0 none, 1 AID + gamma, 2 AID only");

/* The panel's lowest steps (2 cd and up) look black; the slider starts here. */
static bool fade = true;
module_param(fade, bool, 0644);
MODULE_PARM_DESC(fade, "Fade brightness changes over ~0.5 s instead of jumping");

static int min_level = 55;
module_param(min_level, int, 0644);
MODULE_PARM_DESC(min_level, "Panel level (0-255) that brightness 0 maps to");

struct ana38401_cmd {
	bool long_write;	/* downstream sends these as 0x39 even for 2 bytes */
	u8 len;
	u8 data[34];
};

/* Generated from Samsung's DT (qcom,mdss-dsi-on-command), in order. */
static const struct ana38401_cmd ana38401_on_cmds[] = {
	{ false, 2, { 0xb0, 0x7f } },
	{ true, 2, { 0xb2, 0x15 } },
	{ false, 2, { 0xb0, 0xbc } },
	{ true, 2, { 0xb2, 0x04 } },
	{ true, 34, { 0x83, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x00 } },
	{ true, 4, { 0x90, 0x00, 0x00, 0x0e } },
	{ false, 2, { 0xb0, 0x67 } },
	{ true, 2, { 0xb2, 0x0f } },
	{ false, 2, { 0xb0, 0x4d } },
	{ true, 2, { 0xb2, 0x04 } },
	{ false, 2, { 0xb0, 0x49 } },
	{ true, 2, { 0xb2, 0x99 } },
	{ false, 2, { 0xb0, 0x36 } },
	{ true, 2, { 0xb2, 0x10 } },
	{ false, 2, { 0xb0, 0x35 } },
	{ true, 2, { 0xb2, 0x01 } },
};

/* AID (0x90) per brightness level 0..255 (samsung,aid_subdivision_tx_cmds). */
static const u8 ana38401_aid[256][3] = {
	{ 0x01, 0x00, 0x8d }, { 0x01, 0x00, 0x8a }, { 0x01, 0x00, 0x87 }, { 0x01, 0x00, 0x84 },
	{ 0x01, 0x00, 0x80 }, { 0x01, 0x00, 0x7e }, { 0x01, 0x00, 0x7a }, { 0x01, 0x00, 0x77 },
	{ 0x01, 0x00, 0x74 }, { 0x01, 0x00, 0x6f }, { 0x01, 0x00, 0x6d }, { 0x01, 0x00, 0x6a },
	{ 0x01, 0x00, 0x66 }, { 0x01, 0x00, 0x63 }, { 0x01, 0x00, 0x5f }, { 0x01, 0x00, 0x5c },
	{ 0x01, 0x00, 0x55 }, { 0x01, 0x00, 0x52 }, { 0x01, 0x00, 0x4e }, { 0x01, 0x00, 0x4a },
	{ 0x01, 0x00, 0x44 }, { 0x01, 0x00, 0x40 }, { 0x01, 0x00, 0x3d }, { 0x01, 0x00, 0x3a },
	{ 0x01, 0x00, 0x36 }, { 0x01, 0x00, 0x32 }, { 0x01, 0x00, 0x30 }, { 0x01, 0x00, 0x2f },
	{ 0x01, 0x00, 0x2c }, { 0x01, 0x00, 0x28 }, { 0x01, 0x00, 0x24 }, { 0x01, 0x00, 0x20 },
	{ 0x01, 0x00, 0x1b }, { 0x01, 0x00, 0x16 }, { 0x01, 0x00, 0x12 }, { 0x01, 0x00, 0x0e },
	{ 0x01, 0x00, 0x0b }, { 0x01, 0x00, 0x07 }, { 0x01, 0x00, 0x02 }, { 0x00, 0x00, 0xfc },
	{ 0x00, 0x00, 0xf7 }, { 0x00, 0x00, 0xf2 }, { 0x00, 0x00, 0xec }, { 0x00, 0x00, 0xe6 },
	{ 0x00, 0x00, 0xe1 }, { 0x00, 0x00, 0xdb }, { 0x00, 0x00, 0xd7 }, { 0x00, 0x00, 0xd3 },
	{ 0x00, 0x00, 0xcf }, { 0x00, 0x00, 0xca }, { 0x00, 0x00, 0xc4 }, { 0x00, 0x00, 0xbf },
	{ 0x00, 0x00, 0xba }, { 0x00, 0x00, 0xb6 }, { 0x00, 0x00, 0xb1 }, { 0x00, 0x00, 0xac },
	{ 0x00, 0x00, 0xa8 }, { 0x00, 0x00, 0xa3 }, { 0x00, 0x00, 0xa5 }, { 0x00, 0x00, 0xa1 },
	{ 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa7 }, { 0x00, 0x00, 0xa1 }, { 0x00, 0x00, 0x9c },
	{ 0x00, 0x00, 0xa6 }, { 0x00, 0x00, 0xa1 }, { 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa5 },
	{ 0x00, 0x00, 0xa1 }, { 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa7 }, { 0x00, 0x00, 0xa1 },
	{ 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa5 }, { 0x00, 0x00, 0xa2 }, { 0x00, 0x00, 0x9f },
	{ 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa8 }, { 0x00, 0x00, 0xa4 }, { 0x00, 0x00, 0xa0 },
	{ 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa7 }, { 0x00, 0x00, 0xa5 }, { 0x00, 0x00, 0xa3 },
	{ 0x00, 0x00, 0xa0 }, { 0x00, 0x00, 0x9e }, { 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa9 },
	{ 0x00, 0x00, 0xa6 }, { 0x00, 0x00, 0xa3 }, { 0x00, 0x00, 0x9f }, { 0x00, 0x00, 0x9c },
	{ 0x00, 0x00, 0xa7 }, { 0x00, 0x00, 0xa5 }, { 0x00, 0x00, 0xa3 }, { 0x00, 0x00, 0xa1 },
	{ 0x00, 0x00, 0x9e }, { 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa9 }, { 0x00, 0x00, 0xa7 },
	{ 0x00, 0x00, 0xa4 }, { 0x00, 0x00, 0xa2 }, { 0x00, 0x00, 0xa0 }, { 0x00, 0x00, 0x9e },
	{ 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa9 }, { 0x00, 0x00, 0xa6 }, { 0x00, 0x00, 0xa4 },
	{ 0x00, 0x00, 0xa1 }, { 0x00, 0x00, 0x9f }, { 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa9 },
	{ 0x00, 0x00, 0xa7 }, { 0x00, 0x00, 0xa5 }, { 0x00, 0x00, 0xa3 }, { 0x00, 0x00, 0xa1 },
	{ 0x00, 0x00, 0xa0 }, { 0x00, 0x00, 0x9e }, { 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0xa9 },
	{ 0x00, 0x00, 0xa7 }, { 0x00, 0x00, 0xa5 }, { 0x00, 0x00, 0xa3 }, { 0x00, 0x00, 0xa0 },
	{ 0x00, 0x00, 0x9e }, { 0x00, 0x00, 0x9c }, { 0x00, 0x00, 0x9b }, { 0x00, 0x00, 0x99 },
	{ 0x00, 0x00, 0x98 }, { 0x00, 0x00, 0x96 }, { 0x00, 0x00, 0x94 }, { 0x00, 0x00, 0x92 },
	{ 0x00, 0x00, 0x90 }, { 0x00, 0x00, 0x8e }, { 0x00, 0x00, 0x8c }, { 0x00, 0x00, 0x8a },
	{ 0x00, 0x00, 0x88 }, { 0x00, 0x00, 0x86 }, { 0x00, 0x00, 0x84 }, { 0x00, 0x00, 0x82 },
	{ 0x00, 0x00, 0x80 }, { 0x00, 0x00, 0x7e }, { 0x00, 0x00, 0x7c }, { 0x00, 0x00, 0x79 },
	{ 0x00, 0x00, 0x77 }, { 0x00, 0x00, 0x74 }, { 0x00, 0x00, 0x72 }, { 0x00, 0x00, 0x70 },
	{ 0x00, 0x00, 0x6d }, { 0x00, 0x00, 0x6a }, { 0x00, 0x00, 0x68 }, { 0x00, 0x00, 0x66 },
	{ 0x00, 0x00, 0x64 }, { 0x00, 0x00, 0x62 }, { 0x00, 0x00, 0x60 }, { 0x00, 0x00, 0x5e },
	{ 0x00, 0x00, 0x5c }, { 0x00, 0x00, 0x5a }, { 0x00, 0x00, 0x58 }, { 0x00, 0x00, 0x56 },
	{ 0x00, 0x00, 0x54 }, { 0x00, 0x00, 0x52 }, { 0x00, 0x00, 0x50 }, { 0x00, 0x00, 0x4d },
	{ 0x00, 0x00, 0x4b }, { 0x00, 0x00, 0x49 }, { 0x00, 0x00, 0x47 }, { 0x00, 0x00, 0x45 },
	{ 0x00, 0x00, 0x42 }, { 0x00, 0x00, 0x40 }, { 0x00, 0x00, 0x3e }, { 0x00, 0x00, 0x3c },
	{ 0x00, 0x00, 0x39 }, { 0x00, 0x00, 0x37 }, { 0x00, 0x00, 0x35 }, { 0x00, 0x00, 0x32 },
	{ 0x00, 0x00, 0x30 }, { 0x00, 0x00, 0x2d }, { 0x00, 0x00, 0x2b }, { 0x00, 0x00, 0x29 },
	{ 0x00, 0x00, 0x26 }, { 0x00, 0x00, 0x24 }, { 0x00, 0x00, 0x22 }, { 0x00, 0x00, 0x20 },
	{ 0x00, 0x00, 0x1e }, { 0x00, 0x00, 0x1c }, { 0x00, 0x00, 0x1a }, { 0x00, 0x00, 0x18 },
	{ 0x00, 0x00, 0x16 }, { 0x00, 0x00, 0x14 }, { 0x00, 0x00, 0x12 }, { 0x00, 0x00, 0x10 },
	{ 0x00, 0x00, 0x0e }, { 0x00, 0x00, 0x23 }, { 0x00, 0x00, 0x21 }, { 0x00, 0x00, 0x1f },
	{ 0x00, 0x00, 0x1d }, { 0x00, 0x00, 0x1b }, { 0x00, 0x00, 0x19 }, { 0x00, 0x00, 0x17 },
	{ 0x00, 0x00, 0x14 }, { 0x00, 0x00, 0x12 }, { 0x00, 0x00, 0x10 }, { 0x00, 0x00, 0x0e },
	{ 0x00, 0x00, 0x23 }, { 0x00, 0x00, 0x21 }, { 0x00, 0x00, 0x1f }, { 0x00, 0x00, 0x1d },
	{ 0x00, 0x00, 0x1b }, { 0x00, 0x00, 0x19 }, { 0x00, 0x00, 0x17 }, { 0x00, 0x00, 0x14 },
	{ 0x00, 0x00, 0x12 }, { 0x00, 0x00, 0x10 }, { 0x00, 0x00, 0x0e }, { 0x00, 0x00, 0x23 },
	{ 0x00, 0x00, 0x21 }, { 0x00, 0x00, 0x1f }, { 0x00, 0x00, 0x1d }, { 0x00, 0x00, 0x1b },
	{ 0x00, 0x00, 0x19 }, { 0x00, 0x00, 0x17 }, { 0x00, 0x00, 0x14 }, { 0x00, 0x00, 0x12 },
	{ 0x00, 0x00, 0x10 }, { 0x00, 0x00, 0x0e }, { 0x00, 0x00, 0x20 }, { 0x00, 0x00, 0x1e },
	{ 0x00, 0x00, 0x1c }, { 0x00, 0x00, 0x1a }, { 0x00, 0x00, 0x18 }, { 0x00, 0x00, 0x16 },
	{ 0x00, 0x00, 0x14 }, { 0x00, 0x00, 0x12 }, { 0x00, 0x00, 0x10 }, { 0x00, 0x00, 0x0e },
	{ 0x00, 0x00, 0x20 }, { 0x00, 0x00, 0x1e }, { 0x00, 0x00, 0x1c }, { 0x00, 0x00, 0x1a },
	{ 0x00, 0x00, 0x18 }, { 0x00, 0x00, 0x16 }, { 0x00, 0x00, 0x14 }, { 0x00, 0x00, 0x12 },
	{ 0x00, 0x00, 0x10 }, { 0x00, 0x00, 0x0e }, { 0x00, 0x00, 0x28 }, { 0x00, 0x00, 0x25 },
	{ 0x00, 0x00, 0x22 }, { 0x00, 0x00, 0x20 }, { 0x00, 0x00, 0x1d }, { 0x00, 0x00, 0x1a },
	{ 0x00, 0x00, 0x17 }, { 0x00, 0x00, 0x14 }, { 0x00, 0x00, 0x11 }, { 0x00, 0x00, 0x0e },
};

struct ana38401 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi[2];
	struct regulator *vddlcd;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *tcon_rdy_gpio;
	ktime_t reset_time;
	/* Left initialised by the bootloader: keep its gamma, skip the reset. */
	bool handover;
	bool powered;	/* vddlcd on, reset released */
	bool blanked;	/* display off with power kept */
	/* Brightness fade: panel level shown now, and where it is heading. */
	struct delayed_work fade_work;
	int level_shown;	/* -1: nothing sent yet */
	int level_target;
	bool off;	/* between disable and enable: brightness is only stored */
	/* debugfs "dcs": last read result */
	u8 rx[32];
	int rx_len;
};

static inline struct ana38401 *to_ana38401(struct drm_panel *panel)
{
	return container_of(panel, struct ana38401, panel);
}

static void ana38401_write(struct mipi_dsi_multi_context *dsi_ctx,
			   const u8 *data, size_t len, bool long_write)
{
	struct mipi_dsi_msg msg = {
		.channel = dsi_ctx->dsi->channel,
		.type = long_write ? MIPI_DSI_DCS_LONG_WRITE :
			len == 1 ? MIPI_DSI_DCS_SHORT_WRITE :
				   MIPI_DSI_DCS_SHORT_WRITE_PARAM,
		.tx_buf = data,
		.tx_len = len,
	};
	ssize_t ret;

	if (dsi_ctx->accum_err)
		return;
	if (dsi_ctx->dsi->mode_flags & MIPI_DSI_MODE_LPM)
		msg.flags |= MIPI_DSI_MSG_USE_LPM;
	/* mipi_dsi_device_transfer() is static; this is all it does. */
	ret = dsi_ctx->dsi->host->ops->transfer(dsi_ctx->dsi->host, &msg);
	if (ret < 0) {
		dev_err(&dsi_ctx->dsi->dev, "write %02x failed: %zd\n", data[0], ret);
		dsi_ctx->accum_err = ret;
	}
}

static int ana38401_init(struct ana38401 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };
	int i;

	for (i = 0; i < ARRAY_SIZE(ana38401_on_cmds); i++)
		ana38401_write(&dsi_ctx, ana38401_on_cmds[i].data,
			       ana38401_on_cmds[i].len,
			       ana38401_on_cmds[i].long_write);

	return dsi_ctx.accum_err;
}

static int ana38401_prepare(struct drm_panel *panel)
{
	struct ana38401 *ctx = to_ana38401(panel);
	struct device *dev = &ctx->dsi[0]->dev;
	int ret, rdy;

	/* Blanked with power kept (unprepare): enable() wakes it. */
	if (ctx->powered)
		return 0;

	ret = regulator_enable(ctx->vddlcd);
	if (ret)
		return ret;
	ctx->powered = true;

	if (ctx->handover) {
		ctx->handover = false;
		ctx->reset_time = 0;
		return 0;
	}

	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(70);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);

	ret = read_poll_timeout(gpiod_get_value_cansleep, rdy, rdy, 1000, 100000,
				false, ctx->tcon_rdy_gpio);
	if (ret)
		dev_warn(dev, "TCON not ready after 100 ms, going on\n");
	ctx->reset_time = ktime_get();
	dev_info(dev, "reset done, TCON ready=%d\n", rdy);

	ret = ana38401_init(ctx);
	/* The init left the panel at its own level: send ours at once. */
	ctx->level_shown = -1;
	if (ret) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_disable(ctx->vddlcd);
	}
	return ret;
}

static int ana38401_enable(struct drm_panel *panel)
{
	struct ana38401 *ctx = to_ana38401(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };
	s64 since = ktime_ms_delta(ktime_get(), ctx->reset_time);

	/* The brightness drm_panel_enable() sends next goes out at once. */
	ctx->off = false;

	/* Woken from a blank: the panel kept power and its settings. */
	if (ctx->blanked) {
		ctx->blanked = false;
		/* drm_panel_enable() re-sends the brightness after this. */
		mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
		return dsi_ctx.accum_err;
	}

	/*
	 * Taken over: no reset (that is the boot flash), but the bootloader does
	 * not always leave the panel awake and displaying, and then the boot
	 * stayed black until a blank/unblank. Sleep out and display on are
	 * no-ops for a panel that is already on.
	 */
	if (!ctx->reset_time) {
		mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
		mipi_dsi_msleep(&dsi_ctx, ANA38401_SLEEP_OUT_DELAY_MS);
		mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
		dev_info(&ctx->dsi[0]->dev, "taken over: sleep out, display on: %d\n",
			 dsi_ctx.accum_err);
		return dsi_ctx.accum_err;
	}

	if (since < ANA38401_SLEEP_OUT_DELAY_MS)
		msleep(ANA38401_SLEEP_OUT_DELAY_MS - since);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	dev_info(&ctx->dsi[0]->dev, "display on (%lld ms after reset): %d\n",
		 ctx->reset_time ? ktime_ms_delta(ktime_get(), ctx->reset_time) : -1LL,
		 dsi_ctx.accum_err);
	return dsi_ctx.accum_err;
}

static int ana38401_disable(struct drm_panel *panel)
{
	struct ana38401 *ctx = to_ana38401(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };

	/*
	 * No brightness writes from here to enable: a fade step or a level
	 * set while the link went down left lines and garbage on the screen.
	 */
	ctx->off = true;
	cancel_delayed_work_sync(&ctx->fade_work);
	ctx->level_shown = -1;
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);
	return dsi_ctx.accum_err;
}

static int ana38401_unprepare(struct drm_panel *panel)
{
	struct ana38401 *ctx = to_ana38401(panel);

	/*
	 * Blanking keeps the panel powered: a cold start (reset, init) ends
	 * black with this DSI host, which forces the HS clock before the
	 * panel's TCON is ready. Display off in disable() is the blank.
	 */
	if (keep_power) {
		ctx->blanked = true;
		return 0;
	}

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	regulator_disable(ctx->vddlcd);
	ctx->powered = false;
	return 0;
}

/* Halves of 1280x1600 per link; the DSI host splits the bonded mode. */
static const struct drm_display_mode ana38401_mode = {
	.clock = (2560 + 424 + 32 + 216) * (1600 + 10 + 2 + 6) * 60 / 1000,
	.hdisplay = 2560,
	.hsync_start = 2560 + 424,
	.hsync_end = 2560 + 424 + 32,
	.htotal = 2560 + 424 + 32 + 216,
	.vdisplay = 1600,
	.vsync_start = 1600 + 10,
	.vsync_end = 1600 + 10 + 2,
	.vtotal = 1600 + 10 + 2 + 6,
	.width_mm = 227,
	.height_mm = 142,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int ana38401_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &ana38401_mode);
}

static void ana38401_debugfs_init(struct drm_panel *panel, struct dentry *root);

static const struct drm_panel_funcs ana38401_panel_funcs = {
	.prepare = ana38401_prepare,
	.enable = ana38401_enable,
	.disable = ana38401_disable,
	.unprepare = ana38401_unprepare,
	.get_modes = ana38401_get_modes,
	.debugfs_init = ana38401_debugfs_init,
};

static int ana38401_send_level(struct ana38401 *ctx, int level)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };
	u8 aid[4] = { 0x90 }, gamma[34] = { 0x83 };

	/*
	 * Global parameter 188 = 0: gamma offset index 0, which the offline
	 * tables are made for (index 4 makes them black-and-white).
	 * Global parameter 53 = 1: "gamma update", which latches 83h and AID.
	 * 83h has to be rewritten before every latch, or the panel latches
	 * whatever it holds then and the picture goes contrasty.
	 */
	static const u8 gpara_index[] = { 0xb0, 0xbc }, index0[] = { 0xb2, 0x00 };
	static const u8 gpara_update[] = { 0xb0, 0x35 }, update[] = { 0xb2, 0x01 };

	if (!bl_mode)
		return 0;
	memcpy(&aid[1], ana38401_aid[level], 3);
	memcpy(&gamma[1], ana38401_gamma[ana38401_gamma_step[level]], 33);
	ana38401_write(&dsi_ctx, aid, sizeof(aid), true);
	if (bl_mode == 2)
		return dsi_ctx.accum_err;
	ana38401_write(&dsi_ctx, gpara_index, sizeof(gpara_index), false);
	ana38401_write(&dsi_ctx, index0, sizeof(index0), true);
	ana38401_write(&dsi_ctx, gamma, sizeof(gamma), true);
	ana38401_write(&dsi_ctx, gpara_update, sizeof(gpara_update), false);
	ana38401_write(&dsi_ctx, update, sizeof(update), true);
	return dsi_ctx.accum_err;
}

#define ANA38401_FADE_STEP_MS	16

/*
 * Brightness changes fade, as on Android: each frame the panel moves an eighth
 * of the way to the target (at least one level), so a full sweep takes about
 * half a second and auto-brightness never jumps.
 */
static void ana38401_fade_work(struct work_struct *work)
{
	struct ana38401 *ctx = container_of(to_delayed_work(work), struct ana38401, fade_work);
	int diff = ctx->level_target - ctx->level_shown;
	int step;

	if (!diff || ctx->off)
		return;
	step = diff / 8;
	if (!step)
		step = diff > 0 ? 1 : -1;
	if (ana38401_send_level(ctx, ctx->level_shown + step))
		return;	/* panel off: the next enable re-sends the brightness */
	ctx->level_shown += step;
	if (ctx->level_shown != ctx->level_target)
		schedule_delayed_work(&ctx->fade_work, msecs_to_jiffies(ANA38401_FADE_STEP_MS));
}

static int ana38401_bl_update_status(struct backlight_device *bl)
{
	struct ana38401 *ctx = bl_get_data(bl);
	int floor = clamp(min_level, 0, 255);
	int level = floor + clamp(backlight_get_brightness(bl), 0, 255) * (255 - floor) / 255;
	int ret;

	ctx->level_target = level;
	if (ctx->off)
		return 0;
	/* The first level, or fading off: set it at once. */
	if (!fade || ctx->level_shown < 0) {
		cancel_delayed_work_sync(&ctx->fade_work);
		ret = ana38401_send_level(ctx, level);
		if (!ret)
			ctx->level_shown = level;
		return ret;
	}
	mod_delayed_work(system_wq, &ctx->fade_work, 0);
	return 0;
}

/*
 * debugfs "dcs", for bring-up: write "<link> w <hex bytes>" (long write),
 * "<link> s <hex bytes>" (short write) or "<link> r <reg> <len>" (DCS read);
 * reading the file returns the last read's bytes.
 */
static ssize_t ana38401_dcs_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct ana38401 *ctx = file->private_data;
	char buf[160], *p = buf, *tok;
	u8 data[40];
	int link, n = 0, val;
	char op;
	struct mipi_dsi_msg msg = { };
	ssize_t ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = 0;
	if (sscanf(buf, "%d %c", &link, &op) != 2 || link < 0 || link > 1 || !ctx->dsi[link])
		return -EINVAL;
	strsep(&p, " ");
	strsep(&p, " ");
	while ((tok = strsep(&p, " \n")) && n < ARRAY_SIZE(data)) {
		if (!*tok)
			continue;
		if (kstrtoint(tok, 16, &val) || val < 0 || val > 255)
			return -EINVAL;
		data[n++] = val;
	}
	if (!n)
		return -EINVAL;

	msg.channel = ctx->dsi[link]->channel;
	switch (op) {
	case 'w':
		msg.type = MIPI_DSI_DCS_LONG_WRITE;
		break;
	case 's':
		msg.type = n == 1 ? MIPI_DSI_DCS_SHORT_WRITE : MIPI_DSI_DCS_SHORT_WRITE_PARAM;
		break;
	case 'r':
		msg.type = MIPI_DSI_DCS_READ;
		msg.rx_buf = ctx->rx;
		msg.rx_len = n > 1 ? min_t(int, data[1], sizeof(ctx->rx)) : 1;
		n = 1;
		break;
	case 'g':
		/* Samsung's MTP reads: generic read, register + offset. */
		if (n < 2)
			return -EINVAL;
		msg.type = MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM;
		msg.rx_buf = ctx->rx;
		msg.rx_len = n > 2 ? min_t(int, data[2], sizeof(ctx->rx)) : 1;
		n = 2;
		break;
	default:
		return -EINVAL;
	}
	if (op == 'r' || op == 'g') {
		/* Without it the panel answers at most 2 bytes and msm rejects longer replies. */
		ret = mipi_dsi_set_maximum_return_packet_size(ctx->dsi[link], msg.rx_len);
		if (ret < 0)
			return ret;
	}
	msg.tx_buf = data;
	msg.tx_len = n;
	ret = ctx->dsi[link]->host->ops->transfer(ctx->dsi[link]->host, &msg);
	if (ret < 0)
		return ret;
	if (op == 'r' || op == 'g')
		ctx->rx_len = ret;
	return count;
}

static ssize_t ana38401_dcs_read(struct file *file, char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct ana38401 *ctx = file->private_data;
	char buf[32 * 3 + 2];
	int i, len = 0;

	for (i = 0; i < ctx->rx_len; i++)
		len += scnprintf(buf + len, sizeof(buf) - len, "%02x ", ctx->rx[i]);
	len += scnprintf(buf + len, sizeof(buf) - len, "\n");
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static const struct file_operations ana38401_dcs_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ana38401_dcs_write,
	.read = ana38401_dcs_read,
};

static void ana38401_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
	debugfs_create_file("dcs", 0600, root, to_ana38401(panel), &ana38401_dcs_fops);
}

static const struct backlight_ops ana38401_bl_ops = {
	.update_status = ana38401_bl_update_status,
};

static int ana38401_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		/* Until systemd restores the saved level; 255 flashed at boot. */
		.brightness = 160,
		.max_brightness = 255,
	};
	struct mipi_dsi_host *dsi_sec_host;
	struct device_node *dsi_sec;
	struct ana38401 *ctx;
	int ret, i;

	ctx = devm_drm_panel_alloc(dev, struct ana38401, panel,
				   &ana38401_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->vddlcd = devm_regulator_get(dev, "vddlcd");
	if (IS_ERR(ctx->vddlcd))
		return dev_err_probe(dev, PTR_ERR(ctx->vddlcd), "no vddlcd\n");

	/* As is: the bootloader may have the panel running. */
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio), "no reset-gpios\n");
	ctx->tcon_rdy_gpio = devm_gpiod_get(dev, "tcon-rdy", GPIOD_IN);
	if (IS_ERR(ctx->tcon_rdy_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->tcon_rdy_gpio), "no tcon-rdy-gpios\n");

	ctx->handover = handover && regulator_is_enabled(ctx->vddlcd) > 0 &&
			gpiod_get_value_cansleep(ctx->reset_gpio) == 0 &&
			gpiod_get_value_cansleep(ctx->tcon_rdy_gpio) == 1;
	if (!ctx->handover)
		gpiod_direction_output(ctx->reset_gpio, 1);
	dev_info(dev, "panel %s\n", ctx->handover ? "taken over from the bootloader" :
		 "re-initialised");

	dsi_sec = of_graph_get_remote_node(dev->of_node, 1, -1);
	if (!dsi_sec)
		return dev_err_probe(dev, -ENODEV, "no second DSI link in the graph\n");
	{
		const struct mipi_dsi_device_info info = { "ANA38401 DSI1", 0, dsi_sec };

		dsi_sec_host = of_find_mipi_dsi_host_by_node(dsi_sec);
		of_node_put(dsi_sec);
		if (!dsi_sec_host)
			return dev_err_probe(dev, -EPROBE_DEFER, "second DSI host not ready\n");
		ctx->dsi[1] = devm_mipi_dsi_device_register_full(dev, dsi_sec_host, &info);
		if (IS_ERR(ctx->dsi[1]))
			return dev_err_probe(dev, PTR_ERR(ctx->dsi[1]), "cannot add DSI1 device\n");
		mipi_dsi_set_drvdata(ctx->dsi[1], ctx);
	}
	ctx->dsi[0] = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->level_shown = -1;
	/*
	 * Off until the first enable(): a brightness write before the DSI link is up (systemd
	 * restoring the saved level, the initial 160) flashed the panel bright, with lines over
	 * half of it. enable() then sends the level once.
	 */
	ctx->off = true;
	ret = devm_delayed_work_autocancel(dev, &ctx->fade_work, ana38401_fade_work);
	if (ret)
		return ret;

	ctx->panel.prepare_prev_first = true;
	ctx->panel.backlight = devm_backlight_device_register(dev, dev_name(dev), dev,
							      ctx, &ana38401_bl_ops, &props);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight), "no backlight\n");

	drm_panel_add(&ctx->panel);

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		ctx->dsi[i]->lanes = 4;
		ctx->dsi[i]->format = MIPI_DSI_FMT_RGB888;
		/*
		 * Command mode, EoT on, continuous HS clock: the TCON runs off it
		 * (downstream samsung,tcon-clk-on-support); without it frames
		 * arrive torn (2026-10-27).
		 */
		ctx->dsi[i]->mode_flags = 0;
		ret = devm_mipi_dsi_attach(dev, ctx->dsi[i]);
		if (ret < 0) {
			drm_panel_remove(&ctx->panel);
			return dev_err_probe(dev, ret, "cannot attach to DSI%d\n", i);
		}
	}
	return 0;
}

static void ana38401_remove(struct mipi_dsi_device *dsi)
{
	struct ana38401 *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ana38401_of_match[] = {
	{ .compatible = "samsung,ana38401-amsa05rb01" },
	{ }
};
MODULE_DEVICE_TABLE(of, ana38401_of_match);

static struct mipi_dsi_driver ana38401_driver = {
	.probe = ana38401_probe,
	.remove = ana38401_remove,
	.driver = {
		.name = "panel-samsung-ana38401",
		.of_match_table = ana38401_of_match,
	},
};
module_mipi_dsi_driver(ana38401_driver);

MODULE_DESCRIPTION("Samsung AMSA05RB01 / Anapass ANA38401 dual-DSI AMOLED panel");
MODULE_LICENSE("GPL");
