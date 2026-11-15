// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX96716A fixed-topology GMSL2 to CSI-2 bridge
 *
 * This driver implements the legacy one-input/one-output V4L2 sub-device
 * topology. It deliberately does not use the generic MAX9296A framework.
 */

#include <linux/delay.h>
#include <linux/fwnode.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c-mux.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "beagley_max96716a_regs.h"

#define MAX96716A_PAD_SINK	0
#define MAX96716A_PAD_SOURCE	1
#define MAX96716A_NPADS		2

struct max96716a {
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *powerdown_gpio;
	struct regulator *poc;
	struct i2c_mux_core *mux;

	struct v4l2_subdev sd;
	struct media_pad pads[MAX96716A_NPADS];
	struct v4l2_mbus_config_mipi_csi2 mipi_csi2;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_async_notifier notifier;

	struct fwnode_handle *source_ep;
	struct v4l2_subdev *source_sd;
	u16 source_pad;
	u64 enabled_streams;
	s64 link_frequency;
};

static inline struct max96716a *to_max96716a(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max96716a, sd);
}

static int max96716a_write_sequence(struct max96716a *priv,
				    const struct reg_sequence *regs,
				    unsigned int num_regs)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_regs; i++) {
		ret = cci_write(priv->regmap, CCI_REG8(regs[i].reg),
				regs[i].def, NULL);
		if (ret)
			return dev_err_probe(priv->dev, ret,
					     "register write failed at 0x%04x\n",
					     (u16)regs[i].reg);
	}

	return 0;
}

static int max96716a_hw_init(struct max96716a *priv)
{
	int ret;

	ret = max96716a_write_sequence(priv, max96716a_init_regs,
				       ARRAY_SIZE(max96716a_init_regs));
	if (ret)
		return ret;

	ret = max96716a_write_sequence(priv, max96716a_ssc_regs,
				       ARRAY_SIZE(max96716a_ssc_regs));
	if (ret)
		return ret;

	return max96716a_write_sequence(priv, max96716a_aeq_regs,
					ARRAY_SIZE(max96716a_aeq_regs));
}

/*
 * Add the validated sequence that starts the CSI-2 output here. It is called
 * before streaming is propagated to the serializer.
 */
static int max96716a_start_output(struct max96716a *priv)
{
	return cci_write(priv->regmap, CCI_REG8(0x0313), 0x02, NULL);
}

/*
 * Add the validated sequence that stops the CSI-2 output here. The serializer
 * and sensor are stopped first by max96716a_disable_streams().
 */
static void max96716a_stop_output(struct max96716a *priv)
{
	cci_write(priv->regmap, CCI_REG8(0x0313), 0x00, NULL);
}

static int max96716a_set_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *format)
{
	struct max96716a *priv = to_max96716a(sd);
	struct v4l2_mbus_framefmt *fmt;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    priv->enabled_streams)
		return -EBUSY;

	/* This bridge does not transcode. */
	if (format->pad == MAX96716A_PAD_SOURCE)
		return v4l2_subdev_get_fmt(sd, state, format);

	fmt = v4l2_subdev_state_get_format(state, format->pad, format->stream);
	if (!fmt)
		return -EINVAL;

	*fmt = format->format;

	fmt = v4l2_subdev_state_get_opposite_stream_format(state, format->pad,
							   format->stream);
	if (!fmt)
		return -EINVAL;

	*fmt = format->format;

	return 0;
}

static int max96716a_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				    struct v4l2_mbus_frame_desc *fd)
{
	struct max96716a *priv = to_max96716a(sd);
	struct v4l2_mbus_frame_desc upstream_fd;
	struct v4l2_subdev_state *state;
	struct v4l2_subdev_route *route;
	int ret;

	if (pad != MAX96716A_PAD_SOURCE)
		return -EINVAL;
	if (!priv->source_sd)
		return -ENODEV;

	ret = v4l2_subdev_call(priv->source_sd, pad, get_frame_desc,
			       priv->source_pad, &upstream_fd);
	if (ret)
		return ret;

	memset(fd, 0, sizeof(*fd));
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	for_each_active_route(&state->routing, route) {
		struct v4l2_mbus_frame_desc_entry *entry = NULL;
		unsigned int i;

		if (route->source_pad != pad)
			continue;

		for (i = 0; i < upstream_fd.num_entries; i++) {
			if (upstream_fd.entry[i].stream == route->sink_stream) {
				entry = &upstream_fd.entry[i];
				break;
			}
		}

		if (!entry) {
			ret = -EPIPE;
			goto out_unlock;
		}

		if (fd->num_entries >= V4L2_FRAME_DESC_ENTRY_MAX) {
			ret = -E2BIG;
			goto out_unlock;
		}

		fd->entry[fd->num_entries] = *entry;
		fd->entry[fd->num_entries].stream = route->source_stream;
		fd->num_entries++;
	}

out_unlock:
	v4l2_subdev_unlock_state(state);

	return ret;
}

static int max96716a_set_routing(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 enum v4l2_subdev_format_whence which,
				 struct v4l2_subdev_krouting *routing)
{
	struct max96716a *priv = to_max96716a(sd);
	/* Initial state only; userspace replaces this through set_fmt(). */
	static const struct v4l2_mbus_framefmt format = {
		.width = 1920,
		.height = 1080,
		.code = MEDIA_BUS_FMT_SBGGR10_1X10,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_RAW,
		.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
		.quantization = V4L2_QUANTIZATION_DEFAULT,
		.xfer_func = V4L2_XFER_FUNC_DEFAULT,
	};
	int ret;

	if (which == V4L2_SUBDEV_FORMAT_ACTIVE && priv->enabled_streams)
		return -EBUSY;

	if (routing->num_routes > 1)
		return -EINVAL;

	ret = v4l2_subdev_routing_validate(sd, routing,
					   V4L2_SUBDEV_ROUTING_ONLY_1_TO_1);
	if (ret)
		return ret;

	return v4l2_subdev_set_routing_with_fmt(sd, state, routing, &format);
}

static int max96716a_init_state(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_route routes[] = {
		{
			.sink_pad = MAX96716A_PAD_SINK,
			.sink_stream = 0,
			.source_pad = MAX96716A_PAD_SOURCE,
			.source_stream = 0,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		},
	};
	struct v4l2_subdev_krouting routing = {
		.num_routes = ARRAY_SIZE(routes),
		.routes = routes,
	};

	return max96716a_set_routing(sd, state, V4L2_SUBDEV_FORMAT_ACTIVE,
				     &routing);
}

static int max96716a_get_mbus_config(struct v4l2_subdev *sd,
				     unsigned int pad,
				     struct v4l2_mbus_config *cfg)
{
	struct max96716a *priv = to_max96716a(sd);

	if (pad != MAX96716A_PAD_SOURCE)
		return -EINVAL;

	cfg->type = V4L2_MBUS_CSI2_DPHY;
	cfg->bus.mipi_csi2 = priv->mipi_csi2;

	return 0;
}

static int max96716a_enable_streams(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    u32 pad, u64 streams_mask)
{
	struct max96716a *priv = to_max96716a(sd);
	u64 sink_streams;
	int ret;

	if (pad != MAX96716A_PAD_SOURCE)
		return -EINVAL;
	if (!priv->source_sd)
		return -ENODEV;

	sink_streams = v4l2_subdev_state_xlate_streams(state,
						       MAX96716A_PAD_SOURCE,
						       MAX96716A_PAD_SINK,
						       &streams_mask);
	if (!sink_streams)
		return -EINVAL;

	ret = max96716a_start_output(priv);
	if (ret)
		return ret;

	ret = v4l2_subdev_enable_streams(priv->source_sd, priv->source_pad,
					 sink_streams);
	if (ret) {
		max96716a_stop_output(priv);
		return ret;
	}

	priv->enabled_streams |= streams_mask;

	return 0;
}

static int max96716a_disable_streams(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     u32 pad, u64 streams_mask)
{
	struct max96716a *priv = to_max96716a(sd);
	u64 sink_streams;
	int ret;

	if (pad != MAX96716A_PAD_SOURCE)
		return -EINVAL;
	if (!priv->source_sd)
		return -ENODEV;

	sink_streams = v4l2_subdev_state_xlate_streams(state,
						       MAX96716A_PAD_SOURCE,
						       MAX96716A_PAD_SINK,
						       &streams_mask);
	if (!sink_streams)
		return -EINVAL;

	ret = v4l2_subdev_disable_streams(priv->source_sd, priv->source_pad,
					  sink_streams);
	if (ret)
		return ret;

	priv->enabled_streams &= ~streams_mask;
	if (!priv->enabled_streams)
		max96716a_stop_output(priv);

	return 0;
}

static int max96716a_log_status(struct v4l2_subdev *sd)
{
	struct max96716a *priv = to_max96716a(sd);

	dev_info(priv->dev, "MAX96716A legacy bridge, link frequency %lld Hz\n",
		 priv->link_frequency);

	return 0;
}

static const struct v4l2_subdev_pad_ops max96716a_pad_ops = {
	.enable_streams = max96716a_enable_streams,
	.disable_streams = max96716a_disable_streams,
	.set_routing = max96716a_set_routing,
	.get_frame_desc = max96716a_get_frame_desc,
	.get_mbus_config = max96716a_get_mbus_config,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = max96716a_set_fmt,
};

static const struct v4l2_subdev_core_ops max96716a_core_ops = {
	.log_status = max96716a_log_status,
};

static const struct v4l2_subdev_video_ops max96716a_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_internal_ops max96716a_internal_ops = {
	.init_state = max96716a_init_state,
};

static const struct v4l2_subdev_ops max96716a_subdev_ops = {
	.video = &max96716a_video_ops,
	.core = &max96716a_core_ops,
	.pad = &max96716a_pad_ops,
};

static const struct media_entity_operations max96716a_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int max96716a_notify_bound(struct v4l2_async_notifier *notifier,
				  struct v4l2_subdev *subdev,
				  struct v4l2_async_connection *asd)
{
	struct max96716a *priv =
		container_of(notifier, struct max96716a, notifier);
	int ret;

	ret = media_entity_get_fwnode_pad(&subdev->entity, priv->source_ep,
					  MEDIA_PAD_FL_SOURCE);
	if (ret < 0)
		return ret;

	priv->source_sd = subdev;
	priv->source_pad = ret;

	ret = media_create_pad_link(&subdev->entity, priv->source_pad,
				    &priv->sd.entity, MAX96716A_PAD_SINK,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		priv->source_sd = NULL;

	return ret;
}

static void max96716a_notify_unbind(struct v4l2_async_notifier *notifier,
				    struct v4l2_subdev *subdev,
				    struct v4l2_async_connection *asd)
{
	struct max96716a *priv =
		container_of(notifier, struct max96716a, notifier);

	priv->source_sd = NULL;
}

static const struct v4l2_async_notifier_operations max96716a_notify_ops = {
	.bound = max96716a_notify_bound,
	.unbind = max96716a_notify_unbind,
};

static int max96716a_register_notifier(struct max96716a *priv)
{
	struct v4l2_async_connection *asd;
	int ret;

	if (!priv->source_ep)
		return 0;

	v4l2_async_subdev_nf_init(&priv->notifier, &priv->sd);
	asd = v4l2_async_nf_add_fwnode(&priv->notifier, priv->source_ep,
				       struct v4l2_async_connection);
	if (IS_ERR(asd)) {
		v4l2_async_nf_cleanup(&priv->notifier);
		return PTR_ERR(asd);
	}

	priv->notifier.ops = &max96716a_notify_ops;
	ret = v4l2_async_nf_register(&priv->notifier);
	if (ret)
		v4l2_async_nf_cleanup(&priv->notifier);

	return ret;
}

static int max96716a_parse_output_endpoint(struct max96716a *priv)
{
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(priv->dev),
					     MAX96716A_PAD_SOURCE, 0, 0);
	if (!ep)
		return dev_err_probe(priv->dev, -ENODEV,
				     "missing CSI-2 output endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &vep);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (vep.nr_of_link_frequencies != 1) {
		ret = -EINVAL;
		goto out_free;
	}

	if (vep.bus.mipi_csi2.num_data_lanes != 4) {
		ret = -EINVAL;
		goto out_free;
	}

	priv->link_frequency = vep.link_frequencies[0];
	priv->mipi_csi2 = vep.bus.mipi_csi2;

out_free:
	v4l2_fwnode_endpoint_free(&vep);

	return ret;
}

static int max96716a_parse_input_endpoint(struct max96716a *priv)
{
	struct fwnode_handle *ep;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(priv->dev),
					     MAX96716A_PAD_SINK, 0, 0);
	if (!ep)
		return dev_err_probe(priv->dev, -ENODEV,
				     "missing GMSL input endpoint\n");

	priv->source_ep = fwnode_graph_get_remote_endpoint(ep);
	fwnode_handle_put(ep);
	if (!priv->source_ep)
		return dev_err_probe(priv->dev, -ENODEV,
				     "missing serializer remote endpoint\n");

	return 0;
}

static int max96716a_register_subdev(struct max96716a *priv)
{
	struct v4l2_subdev *sd = &priv->sd;
	int ret;

	v4l2_i2c_subdev_init(sd, priv->client, &max96716a_subdev_ops);
	sd->internal_ops = &max96716a_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_STREAMS;
	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->entity.ops = &max96716a_entity_ops;

	priv->pads[MAX96716A_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	priv->pads[MAX96716A_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	v4l2_ctrl_handler_init(&priv->ctrl_handler, 1);
	v4l2_ctrl_new_int_menu(&priv->ctrl_handler, NULL, V4L2_CID_LINK_FREQ,
			       0, 0, &priv->link_frequency);
	if (priv->ctrl_handler.error) {
		ret = priv->ctrl_handler.error;
		goto err_free_ctrl;
	}
	sd->ctrl_handler = &priv->ctrl_handler;
	sd->state_lock = priv->ctrl_handler.lock;

	ret = media_entity_pads_init(&sd->entity, MAX96716A_NPADS, priv->pads);
	if (ret)
		goto err_free_ctrl;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_entity_cleanup;

	ret = max96716a_register_notifier(priv);
	if (ret)
		goto err_subdev_cleanup;

	ret = v4l2_async_register_subdev(sd);
	if (ret)
		goto err_notifier_cleanup;

	return 0;

err_notifier_cleanup:
	v4l2_async_nf_unregister(&priv->notifier);
	v4l2_async_nf_cleanup(&priv->notifier);
err_subdev_cleanup:
	v4l2_subdev_cleanup(sd);
err_entity_cleanup:
	media_entity_cleanup(&sd->entity);
err_free_ctrl:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);

	return ret;
}

static void max96716a_unregister_subdev(struct max96716a *priv)
{
	v4l2_async_unregister_subdev(&priv->sd);
	v4l2_async_nf_unregister(&priv->notifier);
	v4l2_async_nf_cleanup(&priv->notifier);
	v4l2_subdev_cleanup(&priv->sd);
	media_entity_cleanup(&priv->sd.entity);
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
}

static int max96716a_i2c_mux_select(struct i2c_mux_core *mux, u32 chan)
{
	return 0;
}

static int max96716a_i2c_mux_init(struct max96716a *priv)
{
	priv->mux = i2c_mux_alloc(priv->client->adapter, priv->dev, 1, 0,
				  I2C_MUX_LOCKED | I2C_MUX_GATE,
				  max96716a_i2c_mux_select, NULL);
	if (!priv->mux)
		return -ENOMEM;

	return i2c_mux_add_adapter(priv->mux, 0, 0);
}

static void max96716a_powerdown(struct max96716a *priv)
{
	if (priv->powerdown_gpio)
		gpiod_set_value_cansleep(priv->powerdown_gpio, 1);
}

static int max96716a_probe(struct i2c_client *client)
{
	struct max96716a *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &client->dev;
	priv->client = client;
	priv->source_pad = 0;

	priv->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	ret = max96716a_parse_output_endpoint(priv);
	if (ret)
		return ret;

	ret = max96716a_parse_input_endpoint(priv);
	if (ret)
		goto err_put_source_ep;

	priv->powerdown_gpio =
		devm_gpiod_get_optional(priv->dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->powerdown_gpio)) {
		ret = PTR_ERR(priv->powerdown_gpio);
		goto err_put_source_ep;
	}

	priv->poc = devm_regulator_get_optional(priv->dev, "port0-poc");
	if (IS_ERR(priv->poc)) {
		ret = PTR_ERR(priv->poc);
		if (ret != -ENODEV)
			goto err_put_source_ep;
		priv->poc = NULL;
	}

	if (priv->powerdown_gpio) {
		gpiod_set_value_cansleep(priv->powerdown_gpio, 1);
		udelay(2);
		gpiod_set_value_cansleep(priv->powerdown_gpio, 0);
		usleep_range(4000, 5000);
	}

	ret = max96716a_hw_init(priv);
	if (ret)
		goto err_powerdown;

	if (priv->poc) {
		ret = regulator_enable(priv->poc);
		if (ret)
			goto err_powerdown;
	}

	ret = max96716a_i2c_mux_init(priv);
	if (ret)
		goto err_disable_poc;

	ret = max96716a_register_subdev(priv);
	if (ret)
		goto err_del_mux;

	return 0;

err_del_mux:
	i2c_mux_del_adapters(priv->mux);
err_disable_poc:
	if (priv->poc)
		regulator_disable(priv->poc);
err_powerdown:
	max96716a_powerdown(priv);
err_put_source_ep:
	fwnode_handle_put(priv->source_ep);

	return ret;
}

static void max96716a_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct max96716a *priv = to_max96716a(sd);

	max96716a_unregister_subdev(priv);
	i2c_mux_del_adapters(priv->mux);
	if (priv->poc)
		regulator_disable(priv->poc);
	max96716a_powerdown(priv);
	fwnode_handle_put(priv->source_ep);
}

static const struct of_device_id max96716a_of_ids[] = {
	{ .compatible = "maxim,max96716a" },
	{ }
};
MODULE_DEVICE_TABLE(of, max96716a_of_ids);

static struct i2c_driver max96716a_i2c_driver = {
	.driver = {
		.name = "max96716a",
		.of_match_table = max96716a_of_ids,
	},
	.probe = max96716a_probe,
	.remove = max96716a_remove,
};
module_i2c_driver(max96716a_i2c_driver);

MODULE_DESCRIPTION("Maxim MAX96716A fixed-topology GMSL2 deserializer");
MODULE_LICENSE("GPL");
