/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"[drm-dp]: %s: " fmt, __func__

#if defined(CONFIG_ANDROID) && !defined(CONFIG_DEBUG_FS)
#define CONFIG_DEBUG_FS
#endif

#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>

#include "msm_drv.h"
#include "msm_kms.h"
#include "sde_connector.h"
#include "dp_drm.h"
#include "dp_debug.h"

#define to_dp_bridge(x)     container_of((x), struct dp_bridge, base)

enum dp_connector_hdr_state {
	HDR_DISABLE,
	HDR_ENABLE
};

static int get_sink_dc_support(struct dp_display *dp,
		struct drm_display_mode *mode)
{
	int dc_format = 0;
	struct drm_connector *connector = dp->connector;

	if ((mode->flags & DRM_MODE_FLAG_SUPPORTS_YUV420) &&
			(connector->display_info.edid_hdmi_dc_modes
			& DRM_EDID_YCBCR420_DC_30))
		if (dp->get_dc_support(dp, mode->clock,
				MSM_MODE_FLAG_COLOR_FORMAT_YCBCR420, true))
			dc_format |= MSM_MODE_FLAG_YUV420_DC_ENABLE;

	if ((mode->flags & DRM_MODE_FLAG_SUPPORTS_RGB) &&
			(connector->display_info.edid_hdmi_dc_modes
			 & DRM_EDID_HDMI_DC_30))
		if (dp->get_dc_support(dp, mode->clock,
				MSM_MODE_FLAG_COLOR_FORMAT_RGB444, true))
			dc_format |= MSM_MODE_FLAG_RGB444_DC_ENABLE;

	if ((mode->flags & DRM_MODE_FLAG_SUPPORTS_YUV422) &&
			(connector->display_info.edid_hdmi_dc_modes
			 & DRM_EDID_HDMI_DC_30))
		if (dp->get_dc_support(dp, mode->clock,
				MSM_MODE_FLAG_COLOR_FORMAT_YCBCR422, false))
			dc_format |= MSM_MODE_FLAG_YUV422_DC_ENABLE;

	return dc_format;
}

static u32 choose_best_format(struct dp_display *dp,
		struct drm_display_mode *mode)
{
	/*
	 * choose priority:
	 * 1. DC + RGB
	 * 2. DC + YUV422
	 * 3. DC + YUV420
	 * 4. RGB
	 * 5. YUV420
	 */
	int dc_format;

	dc_format = get_sink_dc_support(dp, mode);
	if (dc_format & MSM_MODE_FLAG_RGB444_DC_ENABLE)
		return (MSM_MODE_FLAG_COLOR_FORMAT_RGB444
				| MSM_MODE_FLAG_RGB444_DC_ENABLE);
	else if (dc_format & MSM_MODE_FLAG_YUV422_DC_ENABLE)
		return (MSM_MODE_FLAG_COLOR_FORMAT_YCBCR422
				| MSM_MODE_FLAG_YUV422_DC_ENABLE);
	else if (dc_format & MSM_MODE_FLAG_YUV420_DC_ENABLE)
		return (MSM_MODE_FLAG_COLOR_FORMAT_YCBCR420
				| MSM_MODE_FLAG_YUV420_DC_ENABLE);
	else if (mode->flags & DRM_MODE_FLAG_SUPPORTS_RGB)
		return MSM_MODE_FLAG_COLOR_FORMAT_RGB444;
	else if (mode->flags & DRM_MODE_FLAG_SUPPORTS_YUV420)
		return MSM_MODE_FLAG_COLOR_FORMAT_YCBCR420;

	pr_err("Can't get available best display format\n");

	return MSM_MODE_FLAG_COLOR_FORMAT_RGB444;
}

static void convert_to_dp_mode(const struct drm_display_mode *drm_mode,
		struct dp_display_mode *dp_mode, struct dp_display *dp)
{
	memset(dp_mode, 0, sizeof(*dp_mode));

	dp_mode->timing.out_format = MSM_MODE_FLAG_COLOR_FORMAT_RGB444;
	if (drm_mode->private_flags & MSM_MODE_FLAG_COLOR_FORMAT_YCBCR422)
		dp_mode->timing.out_format =
			MSM_MODE_FLAG_COLOR_FORMAT_YCBCR422;
	else if (drm_mode->private_flags & MSM_MODE_FLAG_COLOR_FORMAT_YCBCR420)
		dp_mode->timing.out_format =
			MSM_MODE_FLAG_COLOR_FORMAT_YCBCR420;

	dp_mode->timing.h_active = drm_mode->hdisplay;
	dp_mode->timing.h_back_porch = drm_mode->htotal - drm_mode->hsync_end;
	dp_mode->timing.h_sync_width = drm_mode->htotal -
			(drm_mode->hsync_start + dp_mode->timing.h_back_porch);
	dp_mode->timing.h_front_porch = drm_mode->hsync_start -
					 drm_mode->hdisplay;
	dp_mode->timing.h_skew = drm_mode->hskew;

	dp_mode->timing.v_active = drm_mode->vdisplay;
	dp_mode->timing.v_back_porch = drm_mode->vtotal - drm_mode->vsync_end;
	dp_mode->timing.v_sync_width = drm_mode->vtotal -
		(drm_mode->vsync_start + dp_mode->timing.v_back_porch);

	dp_mode->timing.v_front_porch = drm_mode->vsync_start -
					 drm_mode->vdisplay;

	dp_mode->timing.refresh_rate = drm_mode->vrefresh;

	dp_mode->timing.pixel_clk_khz = drm_mode->clock;

	dp_mode->timing.v_active_low =
		!!(drm_mode->flags & DRM_MODE_FLAG_NVSYNC);

	dp_mode->timing.h_active_low =
		!!(drm_mode->flags & DRM_MODE_FLAG_NHSYNC);

	dp_mode->flags = drm_mode->flags;

	dp_mode->timing.par = drm_mode->picture_aspect_ratio;
}

static void convert_to_drm_mode(const struct dp_display_mode *dp_mode,
				struct drm_display_mode *drm_mode)
{
	u32 flags = 0;

	memset(drm_mode, 0, sizeof(*drm_mode));

	drm_mode->hdisplay = dp_mode->timing.h_active;
	drm_mode->hsync_start = drm_mode->hdisplay +
				dp_mode->timing.h_front_porch;
	drm_mode->hsync_end = drm_mode->hsync_start +
			      dp_mode->timing.h_sync_width;
	drm_mode->htotal = drm_mode->hsync_end + dp_mode->timing.h_back_porch;
	drm_mode->hskew = dp_mode->timing.h_skew;

	drm_mode->vdisplay = dp_mode->timing.v_active;
	drm_mode->vsync_start = drm_mode->vdisplay +
				dp_mode->timing.v_front_porch;
	drm_mode->vsync_end = drm_mode->vsync_start +
			      dp_mode->timing.v_sync_width;
	drm_mode->vtotal = drm_mode->vsync_end + dp_mode->timing.v_back_porch;

	drm_mode->vrefresh = dp_mode->timing.refresh_rate;
	drm_mode->clock = dp_mode->timing.pixel_clk_khz;

	if (dp_mode->timing.h_active_low)
		flags |= DRM_MODE_FLAG_NHSYNC;
	else
		flags |= DRM_MODE_FLAG_PHSYNC;

	if (dp_mode->timing.v_active_low)
		flags |= DRM_MODE_FLAG_NVSYNC;
	else
		flags |= DRM_MODE_FLAG_PVSYNC;

	drm_mode->flags = flags;
	drm_mode->flags |= (dp_mode->flags & SDE_DRM_MODE_FLAG_FMT_MASK);

	drm_mode->picture_aspect_ratio = dp_mode->timing.par;

	drm_mode->type = 0x48;
	drm_mode_set_name(drm_mode);
}

static int dp_bridge_attach(struct drm_bridge *dp_bridge)
{
	struct dp_bridge *bridge = to_dp_bridge(dp_bridge);

	if (!dp_bridge) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	pr_debug("[%d] attached\n", bridge->id);

	return 0;
}

static void dp_bridge_pre_enable(struct drm_bridge *drm_bridge)
{
	int rc = 0;
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge) {
		pr_err("Invalid params\n");
		return;
	}

	bridge = to_dp_bridge(drm_bridge);
	dp = bridge->display;

	/* By this point mode should have been validated through mode_fixup */
	rc = dp->set_mode(dp, &bridge->dp_mode);
	if (rc) {
		pr_err("[%d] failed to perform a mode set, rc=%d\n",
		       bridge->id, rc);
		return;
	}

	rc = dp->prepare(dp);
	if (rc) {
		pr_err("[%d] DP display prepare failed, rc=%d\n",
		       bridge->id, rc);
		return;
	}

	rc = dp->enable(dp);
	if (rc) {
		pr_err("[%d] DP display enable failed, rc=%d\n",
		       bridge->id, rc);
		dp->unprepare(dp);
	}

}

static void dp_bridge_enable(struct drm_bridge *drm_bridge)
{
	int rc = 0;
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge) {
		pr_err("Invalid params\n");
		return;
	}

	bridge = to_dp_bridge(drm_bridge);
	dp = bridge->display;

	rc = dp->post_enable(dp);
	if (rc)
		pr_err("[%d] DP display post enable failed, rc=%d\n",
		       bridge->id, rc);
}

static void dp_bridge_disable(struct drm_bridge *drm_bridge)
{
	int rc = 0;
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge) {
		pr_err("Invalid params\n");
		return;
	}

	bridge = to_dp_bridge(drm_bridge);
	dp = bridge->display;

	if (!dp) {
		pr_err("dp is null\n");
		return;
	}

	if (dp->connector)
		sde_connector_helper_bridge_disable(dp->connector);

	rc = dp->pre_disable(dp);
	if (rc) {
		pr_err("[%d] DP display pre disable failed, rc=%d\n",
		       bridge->id, rc);
	}
}

static void dp_bridge_post_disable(struct drm_bridge *drm_bridge)
{
	int rc = 0;
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge) {
		pr_err("Invalid params\n");
		return;
	}

	bridge = to_dp_bridge(drm_bridge);
	dp = bridge->display;

	rc = dp->disable(dp);
	if (rc) {
		pr_err("[%d] DP display disable failed, rc=%d\n",
		       bridge->id, rc);
		return;
	}

	rc = dp->unprepare(dp);
	if (rc) {
		pr_err("[%d] DP display unprepare failed, rc=%d\n",
		       bridge->id, rc);
		return;
	}
}

static void dp_bridge_mode_set(struct drm_bridge *drm_bridge,
				struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge || !mode || !adjusted_mode) {
		pr_err("Invalid params\n");
		return;
	}

	bridge = to_dp_bridge(drm_bridge);
	dp = bridge->display;

	memset(&bridge->dp_mode, 0x0, sizeof(struct dp_display_mode));
	convert_to_dp_mode(adjusted_mode, &bridge->dp_mode, dp);
}

static bool dp_bridge_mode_fixup(struct drm_bridge *drm_bridge,
				  const struct drm_display_mode *mode,
				  struct drm_display_mode *adjusted_mode)
{
	bool ret = true;
	struct dp_display_mode dp_mode;
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge || !mode || !adjusted_mode) {
		pr_err("Invalid params\n");
		ret = false;
		goto end;
	}

	bridge = to_dp_bridge(drm_bridge);
	dp = bridge->display;

	convert_to_dp_mode(mode, &dp_mode, dp);
	convert_to_drm_mode(&dp_mode, adjusted_mode);

	/* Clear the private flags before assigning new one */
	adjusted_mode->private_flags = 0;

	adjusted_mode->private_flags |=
		choose_best_format(dp, adjusted_mode);
	SDE_DEBUG("Adjusted mode private flags: 0x%x\n",
			adjusted_mode->private_flags);

end:
	return ret;
}

static const struct drm_bridge_funcs dp_bridge_ops = {
	.attach       = dp_bridge_attach,
	.mode_fixup   = dp_bridge_mode_fixup,
	.pre_enable   = dp_bridge_pre_enable,
	.enable       = dp_bridge_enable,
	.disable      = dp_bridge_disable,
	.post_disable = dp_bridge_post_disable,
	.mode_set     = dp_bridge_mode_set,
};

int dp_connector_config_hdr(void *display,
	struct sde_connector_state *c_state)
{
	struct dp_display *dp = display;

	if (!display || !c_state) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	return dp->config_hdr(dp, &c_state->hdr_meta);
}

int dp_connector_post_init(struct drm_connector *connector, void *display)
{
	struct dp_display *dp_display = display;

	if (!dp_display)
		return -EINVAL;

	dp_display->connector = connector;

	if (dp_display->post_init)
		dp_display->post_init(dp_display);

	return 0;
}

int dp_connector_get_mode_info(const struct drm_display_mode *drm_mode,
	struct msm_mode_info *mode_info, u32 max_mixer_width, void *display)
{
	const u32 dual_lm = 2;
	const u32 single_lm = 1;
	const u32 single_intf = 1;
	const u32 no_enc = 0;
	struct msm_display_topology *topology;

	if (!drm_mode || !mode_info || !max_mixer_width) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	topology = &mode_info->topology;
	topology->num_lm = (max_mixer_width <= drm_mode->hdisplay) ?
							dual_lm : single_lm;
	topology->num_enc = no_enc;
	topology->num_intf = single_intf;

	mode_info->frame_rate = drm_mode->vrefresh;
	mode_info->vtotal = drm_mode->vtotal;
	mode_info->comp_info.comp_type = MSM_DISPLAY_COMPRESSION_NONE;

	return 0;
}

int dp_connector_get_info(struct msm_display_info *info, void *data)
{
	struct dp_display *display = data;

	if (!info || !display) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	info->intf_type = DRM_MODE_CONNECTOR_DisplayPort;

	info->num_of_h_tiles = 1;
	info->h_tile_instance[0] = 0;
	info->is_connected = display->is_connected;
	info->is_primary = display->is_primary;
	info->capabilities = MSM_DISPLAY_CAP_VID_MODE | MSM_DISPLAY_CAP_EDID |
		MSM_DISPLAY_CAP_HOT_PLUG;

	return 0;
}

bool dp_connector_mode_needs_full_range(void *data)
{
	struct dp_display *display = data;
	struct dp_bridge *bridge;
	struct dp_display_mode *mode;
	struct dp_panel_info *timing;

	if (!display) {
		pr_err("invalid input\n");
		return false;
	}

	bridge = display->bridge;
	if (!bridge) {
		pr_err("invalid bridge data\n");
		return false;
	}

	mode = &bridge->dp_mode;
	timing = &mode->timing;

	if (timing->h_active == 640 &&
	    timing->v_active == 480)
		return true;

	return false;
}

bool dp_connector_mode_is_cea_mode(void *data)
{
	struct dp_display *display = data;
	struct dp_bridge *bridge;
	struct dp_display_mode *mode;
	struct drm_display_mode drm_mode;
	struct dp_panel_info *timing;
	bool is_ce_mode = false;

	if (!display) {
		pr_err("invalid input\n");
		return false;
	}

	bridge = display->bridge;
	if (!bridge) {
		pr_err("invalid bridge data\n");
		return false;
	}

	mode = &bridge->dp_mode;
	timing = &mode->timing;

	if (timing->h_active == 640 &&
	    timing->v_active == 480)
		is_ce_mode = false;

	convert_to_drm_mode(mode, &drm_mode);
	drm_mode.flags &= ~SDE_DRM_MODE_FLAG_FMT_MASK;

	if (drm_match_cea_mode(&drm_mode) || drm_match_hdmi_mode(&drm_mode))
		is_ce_mode = true;

	pr_debug("%s: %s : %s video format\n", __func__,
			drm_mode.name, is_ce_mode ? "CE" : "IT");

	return is_ce_mode;
}

enum sde_csc_type dp_connector_get_csc_type(struct drm_connector *conn,
	void *data)
{
	struct dp_display *display = data;
	struct sde_connector_state *c_state;
	struct drm_msm_ext_hdr_metadata *hdr_meta;

	if (!display || !conn) {
		pr_err("invalid input\n");
		goto error;
	}

	c_state = to_sde_connector_state(conn->state);

	if (!c_state) {
		pr_err("invalid input\n");
		goto error;
	}

	hdr_meta = &c_state->hdr_meta;

	if ((hdr_meta->hdr_state == HDR_ENABLE)
		&& (hdr_meta->eotf != 0))
		return SDE_CSC_RGB2YUV_2020L;
	else if (dp_connector_mode_needs_full_range(data)
		|| conn->yuv_qs)
		return SDE_CSC_RGB2YUV_709FR;

error:
	return SDE_CSC_RGB2YUV_709L;
}

enum drm_connector_status dp_connector_detect(struct drm_connector *conn,
		bool force,
		void *display)
{
	enum drm_connector_status status = connector_status_unknown;
	struct msm_display_info info;
	int rc;

	if (!conn || !display)
		return status;

	/* get display dp_info */
	memset(&info, 0x0, sizeof(info));
	rc = dp_connector_get_info(&info, display);
	if (rc) {
		pr_err("failed to get display info, rc=%d\n", rc);
		return connector_status_disconnected;
	}

	if (info.capabilities & MSM_DISPLAY_CAP_HOT_PLUG)
		status = (info.is_connected ? connector_status_connected :
					      connector_status_disconnected);
	else
		status = connector_status_connected;

	conn->display_info.width_mm = info.width_mm;
	conn->display_info.height_mm = info.height_mm;

	return status;
}

void dp_connector_post_open(void *display)
{
	struct dp_display *dp;

	if (!display) {
		pr_err("invalid input\n");
		return;
	}

	dp = display;

	if (dp->post_open)
		dp->post_open(dp);
}

int dp_connector_get_modes(struct drm_connector *connector,
		void *display)
{
	int rc = 0;
	struct dp_display *dp;
	struct dp_display_mode *dp_mode = NULL;
	struct drm_display_mode *m, drm_mode;

	if (!connector || !display)
		return 0;

	dp = display;

	dp_mode = kzalloc(sizeof(*dp_mode),  GFP_KERNEL);
	if (!dp_mode)
		return 0;

	/* pluggable case assumes EDID is read when HPD */
	if (dp->is_connected) {
		rc = dp->get_modes(dp, dp_mode);
		if (!rc)
			pr_err("failed to get DP sink modes, rc=%d\n", rc);

		if (dp_mode->timing.pixel_clk_khz) { /* valid DP mode */
			memset(&drm_mode, 0x0, sizeof(drm_mode));
			convert_to_drm_mode(dp_mode, &drm_mode);
			m = drm_mode_duplicate(connector->dev, &drm_mode);
			if (!m) {
				pr_err("failed to add mode %ux%u\n",
				       drm_mode.hdisplay,
				       drm_mode.vdisplay);
				kfree(dp_mode);
				return 0;
			}
			m->width_mm = connector->display_info.width_mm;
			m->height_mm = connector->display_info.height_mm;
			drm_mode_probed_add(connector, m);
		}
	} else {
		pr_err("No sink connected\n");
	}
	kfree(dp_mode);

	return rc;
}

int dp_connnector_set_info_blob(struct drm_connector *connector,
		void *info, void *display, struct msm_mode_info *mode_info)
{
	struct dp_display *dp_display = display;
	const char *display_type = NULL;

	dp_display->get_display_type(dp_display, &display_type);
	sde_kms_info_add_keystr(info,
			"display type", display_type);

	return 0;
}

int dp_drm_bridge_init(void *data, struct drm_encoder *encoder)
{
	int rc = 0;
	struct dp_bridge *bridge;
	struct drm_device *dev;
	struct dp_display *display = data;
	struct msm_drm_private *priv = NULL;

	bridge = kzalloc(sizeof(*bridge), GFP_KERNEL);
	if (!bridge) {
		rc = -ENOMEM;
		goto error;
	}

	dev = display->drm_dev;
	bridge->display = display;
	bridge->base.funcs = &dp_bridge_ops;
	bridge->base.encoder = encoder;

	priv = dev->dev_private;

	rc = drm_bridge_attach(dev, &bridge->base);
	if (rc) {
		pr_err("failed to attach bridge, rc=%d\n", rc);
		goto error_free_bridge;
	}

	rc = display->request_irq(display);
	if (rc) {
		pr_err("request_irq failed, rc=%d\n", rc);
		goto error_free_bridge;
	}

	encoder->bridge = &bridge->base;
	priv->bridges[priv->num_bridges++] = &bridge->base;
	display->bridge = bridge;

	return 0;
error_free_bridge:
	kfree(bridge);
error:
	return rc;
}

void dp_drm_bridge_deinit(void *data)
{
	struct dp_display *display = data;
	struct dp_bridge *bridge = display->bridge;

	if (bridge && bridge->base.encoder)
		bridge->base.encoder->bridge = NULL;

	kfree(bridge);
}

enum drm_mode_status dp_connector_mode_valid(struct drm_connector *connector,
		struct drm_display_mode *mode,
		void *display)
{
	struct dp_display *dp_disp;
	struct dp_debug *debug;
	u32 pclk = 0;
	u32 rate_ratio = RGB_24BPP_TMDS_CHAR_RATE_RATIO;

	if (!mode || !display) {
		pr_err("invalid params\n");
		return MODE_ERROR;
	}

	dp_disp = display;
	debug = dp_disp->get_debug(dp_disp);

	mode->vrefresh = drm_mode_vrefresh(mode);

	if (!dp_disp->yuv_support) {
		mode->flags &= ~DRM_MODE_FLAG_SUPPORTS_YUV420;
		mode->flags &= ~DRM_MODE_FLAG_SUPPORTS_YUV422;
	}

	if (!dp_disp->vsc_sdp_supported(dp_disp))
		mode->flags &= ~DRM_MODE_FLAG_SUPPORTS_YUV420;

	if (!(mode->flags & SDE_DRM_MODE_FLAG_FMT_MASK))
		return MODE_BAD;

	if ((mode->flags & SDE_DRM_MODE_FLAG_FMT_MASK) ==
			DRM_MODE_FLAG_SUPPORTS_YUV420) {
		rate_ratio = YUV420_24BPP_TMDS_CHAR_RATE_RATIO;
	} else if ((mode->flags & DRM_MODE_FLAG_SUPPORTS_RGB) &&
			(mode->flags & DRM_MODE_FLAG_SUPPORTS_YUV420)) {
		if (mode->clock > dp_disp->max_pclk_khz) {
			/* Clear RGB and YUV422 support flags */
			mode->flags &= ~DRM_MODE_FLAG_SUPPORTS_RGB;
			mode->flags &= ~DRM_MODE_FLAG_SUPPORTS_YUV422;
			/* Only YUV420 format is now supported */
			rate_ratio = YUV420_24BPP_TMDS_CHAR_RATE_RATIO;
		}
	}

	pclk = mode->clock / rate_ratio;

	if (pclk > dp_disp->max_pclk_khz)
		return MODE_BAD;

	if (debug->debug_en && (mode->hdisplay != debug->hdisplay ||
			mode->vdisplay != debug->vdisplay ||
			mode->vrefresh != debug->vrefresh ||
			mode->picture_aspect_ratio != debug->aspect_ratio))
		return MODE_BAD;

	return dp_disp->validate_mode(dp_disp, pclk, mode->flags);
}
