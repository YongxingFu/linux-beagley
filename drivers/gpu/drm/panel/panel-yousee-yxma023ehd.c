// SPDX-License-Identifier: GPL-2.0-only
/*
 * YXMA023EHD: I2C configuration with a single-lane DSI video input.
 * Board power and reset are controlled by GPIOs.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct yxma023ehd {
	struct drm_panel panel;
	struct regmap *regmap;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *supply_1v8;
	struct gpio_desc *supply_5v;
	struct gpio_desc *ctrl;
	struct gpio_desc *reset;
	bool prepared;
};

static const struct regmap_config yxma023ehd_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.max_register = 0x3fff,
	.cache_type = REGCACHE_NONE,
};

static const struct drm_display_mode yxma023ehd_mode = {
	.clock = 27027,
	.hdisplay = 640,
	.hsync_start = 736,
	.hsync_end = 800,
	.htotal = 858,
	.vdisplay = 400,
	.vsync_start = 463,
	.vsync_end = 469,
	.vtotal = 525,
	/* Sync polarities are not specified in the supplied timing table. */
};

static void yxma023ehd_power_off(struct yxma023ehd *ctx)
{
	gpiod_set_value_cansleep(ctx->reset, 1);
	gpiod_set_value_cansleep(ctx->ctrl, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->supply_5v, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->supply_1v8, 0);
}

static int yxma023ehd_prepare(struct drm_panel *panel)
{
	struct yxma023ehd *ctx = container_of(panel, struct yxma023ehd, panel);
	unsigned int value;
	int ret;

	if (ctx->prepared)
		return 0;

	gpiod_set_value_cansleep(ctx->reset, 1);
	gpiod_set_value_cansleep(ctx->ctrl, 0);
	gpiod_set_value_cansleep(ctx->supply_1v8, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->supply_5v, 1);
	usleep_range(5000, 6000);
	/* Open drain: release CTRL; never drive the panel SWIRE output high. */
	gpiod_set_value_cansleep(ctx->ctrl, 1);
	msleep(30);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(100); /* OTP loading may stretch I2C SCL. */

	/* Keep factory OTP settings and disable the internal test pattern. */
	ret = regmap_update_bits(ctx->regmap, 0x2850, BIT(7), 0);
	if (ret)
		goto fail;
	ret = regmap_update_bits(ctx->regmap, 0x2401, BIT(0), 0);
	if (ret)
		goto fail;

	msleep(20);
	ret = regmap_read(ctx->regmap, 0x2850, &value);
	if (ret)
		goto fail;
	if (value & BIT(7)) {
		ret = -EIO;
		goto fail;
	}
	ret = regmap_read(ctx->regmap, 0x2401, &value);
	if (ret)
		goto fail;
	if (value & BIT(0)) {
		ret = -EIO;
		goto fail;
	}

	ctx->prepared = true;
	return 0;

fail:
	yxma023ehd_power_off(ctx);
	return ret;
}

static int yxma023ehd_unprepare(struct drm_panel *panel)
{
	struct yxma023ehd *ctx = container_of(panel, struct yxma023ehd, panel);

	if (ctx->prepared)
		yxma023ehd_power_off(ctx);
	ctx->prepared = false;
	return 0;
}

static int yxma023ehd_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &yxma023ehd_mode);
	if (!mode)
		return -ENOMEM;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	connector->display_info.bpc = 8;
	return 1;
}

static const struct drm_panel_funcs yxma023ehd_panel_funcs = {
	.prepare = yxma023ehd_prepare,
	.unprepare = yxma023ehd_unprepare,
	.get_modes = yxma023ehd_get_modes,
};

static int yxma023ehd_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mipi_dsi_device_info info = { .type = "yxma023ehd", .channel = 0 };
	struct device_node *host_node;
	struct mipi_dsi_host *host;
	struct yxma023ehd *ctx;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	host_node = of_graph_get_remote_node(dev->of_node, 0, 0);
	if (!host_node)
		return dev_err_probe(dev, -EINVAL, "missing DSI graph endpoint\n");
	host = of_find_mipi_dsi_host_by_node(host_node);
	of_node_put(host_node);
	if (!host)
		return -EPROBE_DEFER;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "failed to get reset GPIO\n");
	ctx->ctrl = devm_gpiod_get(dev, "ctrl", GPIOD_OUT_LOW_OPEN_DRAIN);
	if (IS_ERR(ctx->ctrl))
		return dev_err_probe(dev, PTR_ERR(ctx->ctrl), "failed to get ctrl GPIO\n");
	ctx->supply_1v8 = devm_gpiod_get(dev, "enable-1v8", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->supply_1v8))
		return dev_err_probe(dev, PTR_ERR(ctx->supply_1v8),
				     "failed to get enable-1v8 GPIO\n");
	ctx->supply_5v = devm_gpiod_get(dev, "enable-5v", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->supply_5v))
		return dev_err_probe(dev, PTR_ERR(ctx->supply_5v),
				     "failed to get enable-5v GPIO\n");
	ctx->regmap = devm_regmap_init_i2c(client, &yxma023ehd_regmap_config);
	if (IS_ERR(ctx->regmap))
		return dev_err_probe(dev, PTR_ERR(ctx->regmap), "regmap init failed\n");
	i2c_set_clientdata(client, ctx);

	drm_panel_init(&ctx->panel, dev, &yxma023ehd_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	/* Cadence attach resolves this I2C panel through the OF graph. */
	drm_panel_add(&ctx->panel);
	ctx->dsi = mipi_dsi_device_register_full(host, &info);
	if (IS_ERR(ctx->dsi)) {
		ret = PTR_ERR(ctx->dsi);
		goto remove_panel;
	}
	ctx->dsi->lanes = 1;
	ctx->dsi->format = MIPI_DSI_FMT_RGB888;
	/* Continuous clock and non-burst sync-pulse video; configure over I2C. */
	ctx->dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	ret = mipi_dsi_attach(ctx->dsi);
	if (ret) {
		mipi_dsi_device_unregister(ctx->dsi);
		goto remove_panel;
	}
	return 0;

remove_panel:
	drm_panel_remove(&ctx->panel);
	return dev_err_probe(dev, ret, "DSI registration/attach failed\n");
}

static void yxma023ehd_remove(struct i2c_client *client)
{
	struct yxma023ehd *ctx = i2c_get_clientdata(client);

	drm_panel_disable(&ctx->panel);
	drm_panel_unprepare(&ctx->panel);
	mipi_dsi_detach(ctx->dsi);
	mipi_dsi_device_unregister(ctx->dsi);
	drm_panel_remove(&ctx->panel);
}

static void yxma023ehd_shutdown(struct i2c_client *client)
{
	struct yxma023ehd *ctx = i2c_get_clientdata(client);

	drm_panel_disable(&ctx->panel);
	drm_panel_unprepare(&ctx->panel);
}

static const struct of_device_id yxma023ehd_of_match[] = {
	{ .compatible = "yousee,yxma023ehd" },
	{ }
};
MODULE_DEVICE_TABLE(of, yxma023ehd_of_match);

static struct i2c_driver yxma023ehd_driver = {
	.driver = {
		.name = "panel-yousee-yxma023ehd",
		.of_match_table = yxma023ehd_of_match,
	},
	.probe = yxma023ehd_probe,
	.remove = yxma023ehd_remove,
	.shutdown = yxma023ehd_shutdown,
};
module_i2c_driver(yxma023ehd_driver);

MODULE_DESCRIPTION("Yousee YXMA023EHD I2C/DSI panel");
MODULE_LICENSE("GPL");
