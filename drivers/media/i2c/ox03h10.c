// SPDX-License-Identifier: GPL-2.0
/*
 * OmniVision OX03H10 sensor driver
 *
 * The fixed initialization sequence is from the BeagleY-AI camera register
 * capture. The output mode and chip identity come from the camera configuration.
 * The captured VTS value is retained verbatim and documented for confirmation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/videodev2.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "beagley_ox03h10_regs.h"

struct ox03h10_mode {
	const char *name;
	bool valid;

	unsigned int width;
	unsigned int height;
	unsigned int fps;
	unsigned int bpp;
	u64 link_frequency;
	u32 code;
	u8 data_type;
	u8 vc;
	u8 num_data_lanes;

	const struct reg_sequence *regs;
	unsigned int num_regs;
	const struct reg_sequence *start_regs;
	unsigned int num_start_regs;
	const struct reg_sequence *stop_regs;
	unsigned int num_stop_regs;
};

struct ox03h10 {
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct clk *xvclk;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_fwnode_endpoint ep;
	struct ox03h10_mode mode;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analog_gain;
	struct v4l2_ctrl *digital_gain;
	struct mutex lock;
	bool streaming;
};

static const s64 ox03h10_link_freq_menu[] = {
	750000000,
};

/*
 * Change these widths here if the verified OX03H10 register interface is not
 * 16-bit address / 8-bit data.
 */
static const struct regmap_config ox03h10_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
};

static int ox03h10_parse_mode(struct ox03h10 *sensor)
{
	if (sensor->ep.bus.mipi_csi2.num_data_lanes != 4)
		return dev_err_probe(sensor->dev, -EINVAL,
				     "CSV profile requires four CSI-2 lanes\n");
	if (sensor->ep.nr_of_link_frequencies != 1)
		return dev_err_probe(sensor->dev, -EINVAL,
				     "single-mode endpoint needs one link frequency\n");
	if (sensor->ep.link_frequencies[0] != 750000000ULL)
		return dev_err_probe(sensor->dev, -EINVAL,
				     "1920x1536 mode requires a 750 MHz link frequency\n");

	sensor->mode = (struct ox03h10_mode) {
		.name = "1920x1536-bggr-raw12-60fps",
		.valid = true,
		.width = 1920,
		.height = 1536,
		.fps = 60,
		.bpp = 12,
		.link_frequency = sensor->ep.link_frequencies[0],
		.code = MEDIA_BUS_FMT_SBGGR12_1X12,
		.data_type = 0x2c,
		.vc = 0,
		.num_data_lanes = sensor->ep.bus.mipi_csi2.num_data_lanes,
		.regs = ox03h10_init_regs,
		.num_regs = ARRAY_SIZE(ox03h10_init_regs),
		.start_regs = ox03h10_io_pad_regs,
		.num_start_regs = ARRAY_SIZE(ox03h10_io_pad_regs),
		.stop_regs = ox03h10_stream_stop_regs,
		.num_stop_regs = ARRAY_SIZE(ox03h10_stream_stop_regs),
	};

	return 0;
}

static inline struct ox03h10 *to_ox03h10(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ox03h10, sd);
}

static int ox03h10_write_sequence(struct ox03h10 *sensor,
				const struct reg_sequence *regs,
				unsigned int num_regs)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_regs; i++) {
		ret = regmap_write(sensor->regmap, regs[i].reg, regs[i].def);
		if (ret < 0) {
			dev_err(sensor->dev,
				"failed to write reg 0x%04x: %d\n",
				regs[i].reg, ret);
			return ret;
		}

		fsleep(regs[i].delay_us);
	}

	return 0;
}

static int ox03h10_power_on(struct ox03h10 *sensor)
{
	/* Rails and reset are board-managed; only enable the serializer clock. */
	return clk_prepare_enable(sensor->xvclk);
}

static void ox03h10_power_off(struct ox03h10 *sensor)
{
	clk_disable_unprepare(sensor->xvclk);
}

static int ox03h10_identify(struct ox03h10 *sensor)
{
	unsigned int id_a, id_b, id_c, version, split;
	int ret;

	ret = regmap_read(sensor->regmap, 0x300a, &id_a);
	if (ret)
		return ret;
	ret = regmap_read(sensor->regmap, 0x300b, &id_b);
	if (ret)
		return ret;
	ret = regmap_read(sensor->regmap, 0x300c, &id_c);
	if (ret)
		return ret;
	ret = regmap_read(sensor->regmap, 0x302a, &version);
	if (ret)
		return ret;
	ret = regmap_read(sensor->regmap, 0x302b, &split);
	if (ret)
		return ret;

	if (id_a != 0x58 || id_b != 0x03 || id_c != 0x48)
		return dev_err_probe(sensor->dev, -ENODEV,
				     "unexpected OX03H10 ID %02x:%02x:%02x\n",
				     id_a, id_b, id_c);

	dev_info(sensor->dev,
		 "OX03H10 ID %02x:%02x:%02x, version 0x%02x, split 0x%02x\n",
		 id_a, id_b, id_c, version, split);

	return 0;
}

static const struct ox03h10_mode *
ox03h10_find_mode(struct ox03h10 *sensor, unsigned int width,
		  unsigned int height, u32 code)
{
	if (!sensor->mode.valid || sensor->mode.width != width ||
	    sensor->mode.height != height || sensor->mode.code != code)
		return NULL;

	return &sensor->mode;
}

static bool ox03h10_mode_matches_endpoint(struct ox03h10 *sensor,
					const struct ox03h10_mode *mode)
{
	struct v4l2_fwnode_endpoint *ep = &sensor->ep;
	unsigned int i;

	if (mode->num_data_lanes != ep->bus.mipi_csi2.num_data_lanes)
		return false;

	if (!ep->nr_of_link_frequencies)
		return true;

	for (i = 0; i < ep->nr_of_link_frequencies; i++)
		if (ep->link_frequencies[i] == mode->link_frequency)
			return true;

	return false;
}

static int ox03h10_init_state(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state)
{
	struct ox03h10 *sensor = to_ox03h10(sd);
	const struct ox03h10_mode *mode = &sensor->mode;
	struct v4l2_subdev_route routes[] = {
		{
			.source_pad = 0,
			.source_stream = 0,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		},
	};
	struct v4l2_subdev_krouting routing = {
		.num_routes = ARRAY_SIZE(routes),
		.routes = routes,
	};
	struct v4l2_mbus_framefmt format = {
		.width = mode->width,
		.height = mode->height,
		.code = mode->code,
		.field = V4L2_FIELD_NONE,
		.colorspace = mode->code ? V4L2_COLORSPACE_RAW :
					   V4L2_COLORSPACE_DEFAULT,
		.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
		.quantization = V4L2_QUANTIZATION_DEFAULT,
		.xfer_func = V4L2_XFER_FUNC_DEFAULT,
	};

	return v4l2_subdev_set_routing_with_fmt(sd, state, &routing, &format);
}

static int ox03h10_set_fmt(struct v4l2_subdev *sd,
			 struct v4l2_subdev_state *state,
			 struct v4l2_subdev_format *format)
{
	struct ox03h10 *sensor = to_ox03h10(sd);
	const struct ox03h10_mode *mode;
	struct v4l2_mbus_framefmt *fmt;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE && sensor->streaming)
		return -EBUSY;

	fmt = v4l2_subdev_state_get_format(state, format->pad, format->stream);
	if (!fmt)
		return -EINVAL;

	mode = ox03h10_find_mode(sensor, format->format.width,
				  format->format.height,
			       format->format.code);
	if (!mode || !ox03h10_mode_matches_endpoint(sensor, mode))
		return -EINVAL;

	*fmt = format->format;
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = mode->code;
	fmt->field = V4L2_FIELD_NONE;
	format->format = *fmt;

	return 0;
}

static int ox03h10_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct ox03h10 *sensor = to_ox03h10(sd);

	if (code->pad != 0 || code->index != 0 || code->stream != 0 ||
	    !sensor->mode.valid ||
	    !ox03h10_mode_matches_endpoint(sensor, &sensor->mode))
		return -EINVAL;

	code->code = sensor->mode.code;

	return 0;
}

static int ox03h10_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct ox03h10 *sensor = to_ox03h10(sd);
	const struct ox03h10_mode *mode = &sensor->mode;

	if (fse->pad != 0 || fse->index != 0 || fse->stream != 0 ||
	    fse->code != mode->code || !mode->valid ||
	    !ox03h10_mode_matches_endpoint(sensor, mode))
		return -EINVAL;

	fse->min_width = mode->width;
	fse->max_width = mode->width;
	fse->min_height = mode->height;
	fse->max_height = mode->height;

	return 0;
}

static int ox03h10_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				struct v4l2_mbus_frame_desc *fd)
{
	struct ox03h10 *sensor = to_ox03h10(sd);
	struct v4l2_subdev_state *state;
	struct v4l2_mbus_framefmt *fmt;
	const struct ox03h10_mode *mode;
	int ret = 0;

	if (pad != 0)
		return -EINVAL;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	fmt = v4l2_subdev_state_get_format(state, 0, 0);
	if (!fmt) {
		ret = -EPIPE;
		goto out_unlock;
	}

	mode = ox03h10_find_mode(sensor, fmt->width, fmt->height, fmt->code);
	if (!mode || !ox03h10_mode_matches_endpoint(sensor, mode)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	memset(fd, 0, sizeof(*fd));
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;
	fd->entry[0].stream = 0;
	fd->entry[0].flags = V4L2_MBUS_FRAME_DESC_FL_LEN_MAX;
	fd->entry[0].length = mode->width * mode->height * mode->bpp / 8;
	fd->entry[0].pixelcode = mode->code;
	fd->entry[0].bus.csi2.vc = mode->vc;
	fd->entry[0].bus.csi2.dt = mode->data_type;

out_unlock:
	v4l2_subdev_unlock_state(state);

	return ret;
}

static int ox03h10_apply_controls(struct ox03h10 *sensor);

static int ox03h10_enable_streams(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				u32 pad, u64 streams_mask)
{
	struct ox03h10 *sensor = to_ox03h10(sd);
	struct v4l2_mbus_framefmt *fmt;
	const struct ox03h10_mode *mode;
	int ret;

	if (pad != 0 || streams_mask != BIT(0))
		return -EINVAL;

	mutex_lock(&sensor->lock);
	if (sensor->streaming) {
		ret = -EALREADY;
		goto out_unlock;
	}

	fmt = v4l2_subdev_state_get_format(state, 0, 0);
	if (!fmt) {
		ret = -EPIPE;
		goto out_unlock;
	}

	mode = ox03h10_find_mode(sensor, fmt->width, fmt->height, fmt->code);
	if (!mode || !ox03h10_mode_matches_endpoint(sensor, mode)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret)
		goto out_unlock;

	ret = ox03h10_write_sequence(sensor, mode->regs, mode->num_regs);
	if (ret)
		goto err_runtime_put;

	ret = ox03h10_write_sequence(sensor, mode->start_regs,
				   mode->num_start_regs);
	if (ret)
		goto err_runtime_put;

	ret = ox03h10_apply_controls(sensor);
	if (ret)
		goto err_runtime_put;

	ret = ox03h10_write_sequence(sensor, ox03h10_stream_on_regs,
				   ARRAY_SIZE(ox03h10_stream_on_regs));
	if (ret)
		goto err_runtime_put;

	sensor->streaming = true;
	mutex_unlock(&sensor->lock);

	return 0;

err_runtime_put:
	pm_runtime_put(sensor->dev);
out_unlock:
	mutex_unlock(&sensor->lock);

	return ret;
}

static int ox03h10_disable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct ox03h10 *sensor = to_ox03h10(sd);
	struct v4l2_mbus_framefmt *fmt;
	const struct ox03h10_mode *mode;
	int ret;

	if (pad != 0 || streams_mask != BIT(0))
		return -EINVAL;

	mutex_lock(&sensor->lock);
	if (!sensor->streaming) {
		ret = -EALREADY;
		goto out_unlock;
	}

	fmt = v4l2_subdev_state_get_format(state, 0, 0);
	if (!fmt) {
		ret = -EPIPE;
		goto out_unlock;
	}

	mode = ox03h10_find_mode(sensor, fmt->width, fmt->height, fmt->code);
	if (!mode || !ox03h10_mode_matches_endpoint(sensor, mode)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = ox03h10_write_sequence(sensor, mode->stop_regs,
				   mode->num_stop_regs);
	if (ret)
		goto out_unlock;

	sensor->streaming = false;
	pm_runtime_put_autosuspend(sensor->dev);
	mutex_unlock(&sensor->lock);

	return 0;

out_unlock:
	mutex_unlock(&sensor->lock);

	return ret;
}

static int ox03h10_get_frame_interval(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_frame_interval *fi)
{
	struct v4l2_mbus_framefmt *fmt;
	const struct ox03h10_mode *mode;

	if (fi->pad != 0)
		return -EINVAL;

	fmt = v4l2_subdev_state_get_format(state, 0, 0);
	if (!fmt)
		return -EINVAL;

	mode = ox03h10_find_mode(to_ox03h10(sd), fmt->width, fmt->height,
				  fmt->code);
	if (!mode)
		return -EINVAL;

	fi->interval = (struct v4l2_fract) {
		.numerator = 1,
		.denominator = mode->fps,
	};

	return 0;
}

static int ox03h10_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				 struct v4l2_mbus_config *cfg)
{
	struct ox03h10 *sensor = to_ox03h10(sd);

	if (pad != 0)
		return -EINVAL;

	cfg->type = sensor->ep.bus_type;
	cfg->bus.mipi_csi2 = sensor->ep.bus.mipi_csi2;

	return 0;
}

static const struct v4l2_subdev_video_ops ox03h10_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops ox03h10_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ox03h10_set_fmt,
	.enum_mbus_code = ox03h10_enum_mbus_code,
	.enum_frame_size = ox03h10_enum_frame_size,
	.get_frame_desc = ox03h10_get_frame_desc,
	.get_mbus_config = ox03h10_get_mbus_config,
	.get_frame_interval = ox03h10_get_frame_interval,
	.enable_streams = ox03h10_enable_streams,
	.disable_streams = ox03h10_disable_streams,
};

static const struct v4l2_subdev_internal_ops ox03h10_internal_ops = {
	.init_state = ox03h10_init_state,
};

static const struct v4l2_subdev_ops ox03h10_subdev_ops = {
	.video = &ox03h10_video_ops,
	.pad = &ox03h10_pad_ops,
};

#define OX03H10_EXPOSURE_MIN		2
#define OX03H10_EXPOSURE_MAX		802
#define OX03H10_EXPOSURE_DEF		0x2dd
#define OX03H10_ANALOG_GAIN_MIN		19
#define OX03H10_ANALOG_GAIN_MAX		248
#define OX03H10_ANALOG_GAIN_DEF		248
#define OX03H10_DIGITAL_GAIN_MIN		256
#define OX03H10_DIGITAL_GAIN_MAX		768
#define OX03H10_DIGITAL_GAIN_DEF		256

static int ox03h10_write_exposure(struct ox03h10 *sensor, u32 exposure)
{
	unsigned int high = exposure >> 8;
	unsigned int low = exposure & 0xff;
	int ret;

	/* The DCG and SPD/LOFIC exposures are kept together as in the ADAS
	 * reference implementation.
	 */
	ret = regmap_write(sensor->regmap, 0x3501, high);
	if (ret)
		return ret;
	ret = regmap_write(sensor->regmap, 0x3502, low);
	if (ret)
		return ret;
	ret = regmap_write(sensor->regmap, 0x3541, high);
	if (ret)
		return ret;

	return regmap_write(sensor->regmap, 0x3542, low);
}

static int ox03h10_write_analog_gain(struct ox03h10 *sensor, u32 gain)
{
	/* The sensor uses a 4.4 encoding in 0x3508/0x3509. */
	return regmap_bulk_write(sensor->regmap, 0x3508,
				 (u8[]){ gain >> 4, (gain & 0xf) << 4 }, 2);
}

static int ox03h10_write_digital_gain(struct ox03h10 *sensor, u32 gain)
{
	u16 value = gain * 2;
	u8 regs[4][2] = {
		{ value >> 7, (value << 1) & 0xff },
		{ value >> 7, (value << 1) & 0xff },
		{ value >> 7, (value << 1) & 0xff },
		{ value >> 7, (value << 1) & 0xff },
	};
	int ret;

	/* ADAS uses the four manual inverse-gain pairs as an equal per-channel
	 * digital gain path.  Keep that behavior even though the datasheet
	 * names these registers Manual AWB inverse gain.
	 */
	ret = regmap_bulk_write(sensor->regmap, 0x5da0, regs, sizeof(regs));
	if (ret)
		return ret;

	return regmap_write(sensor->regmap, 0x5da8, 0x01);
}

static int ox03h10_apply_controls(struct ox03h10 *sensor)
{
	int ret;

	ret = ox03h10_write_exposure(sensor, sensor->exposure->cur.val);
	if (ret)
		return ret;

	ret = ox03h10_write_analog_gain(sensor, sensor->analog_gain->cur.val);
	if (ret)
		return ret;

	return ox03h10_write_digital_gain(sensor, sensor->digital_gain->cur.val);
}

static int ox03h10_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ox03h10 *sensor = container_of(ctrl->handler,
					     struct ox03h10, ctrl_handler);
	int ret;

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret < 0)
		return ret;

	mutex_lock(&sensor->lock);
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = ox03h10_write_exposure(sensor, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ox03h10_write_analog_gain(sensor, ctrl->val);
		break;
	case V4L2_CID_GAIN:
		ret = ox03h10_write_digital_gain(sensor, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&sensor->lock);

	pm_runtime_mark_last_busy(sensor->dev);
	pm_runtime_put_autosuspend(sensor->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ox03h10_ctrl_ops = {
	.s_ctrl = ox03h10_s_ctrl,
};

/*
 * The exposure unit is a double row. Values and encodings follow the J722S
 * ADAS OX03H10 reference: HCG is dynamic, SPD exposure follows HCG, and the
 * other conversion-gain channels retain their mode defaults.
 */
static int ox03h10_init_controls(struct ox03h10 *sensor)
{
	struct v4l2_ctrl *link_freq;

	v4l2_ctrl_handler_init(&sensor->ctrl_handler, 4);
	link_freq = v4l2_ctrl_new_int_menu(&sensor->ctrl_handler, NULL,
					  V4L2_CID_LINK_FREQ,
					  ARRAY_SIZE(ox03h10_link_freq_menu) - 1,
					  0, ox03h10_link_freq_menu);
	if (link_freq)
		link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->exposure = v4l2_ctrl_new_std(&sensor->ctrl_handler,
					     &ox03h10_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     OX03H10_EXPOSURE_MIN,
					     OX03H10_EXPOSURE_MAX, 1,
					     OX03H10_EXPOSURE_DEF);
	sensor->analog_gain = v4l2_ctrl_new_std(&sensor->ctrl_handler,
						&ox03h10_ctrl_ops,
						V4L2_CID_ANALOGUE_GAIN,
						OX03H10_ANALOG_GAIN_MIN,
						OX03H10_ANALOG_GAIN_MAX, 1,
						OX03H10_ANALOG_GAIN_DEF);
	sensor->digital_gain = v4l2_ctrl_new_std(&sensor->ctrl_handler,
						 &ox03h10_ctrl_ops,
						 V4L2_CID_GAIN,
						 OX03H10_DIGITAL_GAIN_MIN,
						 OX03H10_DIGITAL_GAIN_MAX, 1,
						 OX03H10_DIGITAL_GAIN_DEF);

	if (sensor->ctrl_handler.error)
		return sensor->ctrl_handler.error;

	return 0;
}
static int ox03h10_register_subdev(struct ox03h10 *sensor)
{
	struct v4l2_subdev *sd = &sensor->sd;
	int ret;

	v4l2_i2c_subdev_init(sd, sensor->client, &ox03h10_subdev_ops);
	sd->internal_ops = &ox03h10_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS |
		     V4L2_SUBDEV_FL_STREAMS;

	ret = ox03h10_init_controls(sensor);
	if (ret) {
		v4l2_ctrl_handler_free(&sensor->ctrl_handler);
		return ret;
	}
	sd->ctrl_handler = &sensor->ctrl_handler;

	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&sd->entity, 1, &sensor->pad);
	if (ret)
		goto err_free_ctrl;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_entity_cleanup;

	ret = v4l2_async_register_subdev_sensor(sd);
	if (ret)
		goto err_subdev_cleanup;

	return 0;

err_subdev_cleanup:
	v4l2_subdev_cleanup(sd);
err_entity_cleanup:
	media_entity_cleanup(&sd->entity);
err_free_ctrl:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);

	return ret;
}

static void ox03h10_unregister_subdev(struct ox03h10 *sensor)
{
	v4l2_async_unregister_subdev(&sensor->sd);
	v4l2_subdev_cleanup(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
}

static int __maybe_unused ox03h10_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ox03h10 *sensor = to_ox03h10(sd);

	return ox03h10_power_on(sensor);
}

static int __maybe_unused ox03h10_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ox03h10 *sensor = to_ox03h10(sd);

	ox03h10_power_off(sensor);

	return 0;
}

static const struct dev_pm_ops ox03h10_pm_ops = {
	SET_RUNTIME_PM_OPS(ox03h10_runtime_suspend, ox03h10_runtime_resume, NULL)
};

static int ox03h10_probe(struct i2c_client *client)
{
	struct ox03h10 *sensor;
	struct fwnode_handle *ep;
	int ret;

	sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = &client->dev;
	sensor->client = client;
	mutex_init(&sensor->lock);

	sensor->xvclk = devm_clk_get(sensor->dev, "xvclk");
	if (IS_ERR(sensor->xvclk))
		return PTR_ERR(sensor->xvclk);

	sensor->regmap = devm_regmap_init_i2c(client, &ox03h10_regmap_config);
	if (IS_ERR(sensor->regmap))
		return PTR_ERR(sensor->regmap);

	sensor->ep.bus_type = V4L2_MBUS_CSI2_DPHY;
	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(sensor->dev), 0, 0, 0);
	if (!ep)
		return dev_err_probe(sensor->dev, -ENODEV,
				     "missing CSI-2 endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &sensor->ep);
	fwnode_handle_put(ep);
	if (ret)
		return ret;
	if (sensor->ep.bus.mipi_csi2.num_data_lanes != 4) {
		ret = -EINVAL;
		goto err_free_endpoint;
	}

	ret = ox03h10_parse_mode(sensor);
	if (ret)
		goto err_free_endpoint;

	ret = ox03h10_power_on(sensor);
	if (ret)
		goto err_free_endpoint;

	ret = ox03h10_identify(sensor);
	if (ret)
		goto err_power_off;

	ret = ox03h10_register_subdev(sensor);
	if (ret)
		goto err_power_off;

	pm_runtime_set_active(sensor->dev);
	pm_runtime_get_noresume(sensor->dev);
	pm_runtime_enable(sensor->dev);

	pm_runtime_set_autosuspend_delay(sensor->dev, 1000);
	pm_runtime_use_autosuspend(sensor->dev);
	pm_runtime_put_autosuspend(sensor->dev);

	return 0;

err_power_off:
	ox03h10_power_off(sensor);
err_free_endpoint:
	v4l2_fwnode_endpoint_free(&sensor->ep);

	return ret;
}

static void ox03h10_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ox03h10 *sensor = to_ox03h10(sd);

	pm_runtime_disable(sensor->dev);
	pm_runtime_put_noidle(sensor->dev);
	ox03h10_unregister_subdev(sensor);

	if (!pm_runtime_status_suspended(sensor->dev))
		ox03h10_power_off(sensor);
	pm_runtime_set_suspended(sensor->dev);
	v4l2_fwnode_endpoint_free(&sensor->ep);
}

static const struct of_device_id ox03h10_of_ids[] = {
	{ .compatible = "ovti,ox03h10" },
	{ }
};
MODULE_DEVICE_TABLE(of, ox03h10_of_ids);

static struct i2c_driver ox03h10_i2c_driver = {
	.driver = {
		.name = "ox03h10",
		.pm = &ox03h10_pm_ops,
		.of_match_table = ox03h10_of_ids,
	},
	.probe = ox03h10_probe,
	.remove = ox03h10_remove,
};
module_i2c_driver(ox03h10_i2c_driver);

MODULE_DESCRIPTION("OmniVision OX03H10 sensor driver");
MODULE_LICENSE("GPL");
