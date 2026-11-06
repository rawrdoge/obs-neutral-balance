/*
 * obs-neutral-balance - manual white balance correction filter for OBS Studio
 * Copyright (C) 2026 obs-neutral-balance contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * v2 corrective implementation of OBS-NB-PRD-002:
 *
 * - Color-space discovery per frame via obs_source_get_color_space()
 *   (FR-01..FR-04); working space is linear sRGB or linear BT.709.
 * - The OBS GPU pipeline already converts YUV sources to full-range RGB
 *   (FR-07), so no second conversion is performed. Video-range metadata
 *   (FR-05..FR-08) is consumed through the modern frame2 path wherever the
 *   filter touches raw frames; this filter is a GPU (video_render) filter,
 *   and the RGB texture it receives is full range, which the UI reports.
 * - Grey World (Buchsbaum 1980) and Shades of Grey (Finlayson & Trezzi
 *   2004) statistics are gathered in LINEAR light via a 256-entry inverse
 *   EOTF LUT applied to the 8-bit read-back (FR-09..FR-13).
 * - The correction is a full 3x3 chromatic adaptation transform (FR-14..18)
 *   computed in double precision on the CPU from the measured illuminant
 *   XYZ and the user target in CIE LCh (FR-19..24). The legacy diagonal
 *   mode is the identity-cone special case, sharing the same pipeline.
 * - A DeltaE00 diagnostic (FR-28) audits the corrected sample against the
 *   target (CIEDE2000, Sharma et al. 2005).
 *
 * Only public libobs APIs are used (obs.h, obs-source.h,
 * graphics/graphics.h, graphics/vec3.h, util headers).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs-module.h>
#include <util/bmem.h>

#include "neutral-balance-filter.h"
#include "colorimetry.h"
#include "matrix.h"
#include "stats.h"
#include "capture.h"
#include "eyedropper.h"

#define EPS 1e-6

/* ------------------------------------------------------------------------- */
/* Working-space transfer functions (CPU side, for LUT + diagnostics)        */
/* ------------------------------------------------------------------------- */

void nb_set_working_space(struct neutral_balance *f, enum gs_color_space cs)
{
	enum nb_working_space ws;
	switch (cs) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		ws = NB_WS_SRGB;
		break;
	case GS_CS_709_EXTENDED:
	case GS_CS_709_SCRGB:
		ws = NB_WS_BT709;
		break;
	default:
		ws = NB_WS_SRGB;
		if (!f->logged_space_fallback) {
			blog(LOG_WARNING, "[neutral-balance] unsupported color space %d; "
					  "falling back to linear sRGB", (int)cs);
			f->logged_space_fallback = true;
		}
		break;
	}
	if (ws != f->ws) {
		f->ws = ws;
		nb_build_lut(f->ws, f->inv_lut);
		f->cat_dirty = true;
	}
	f->source_space = cs;
}

const char *nb_ws_name(const struct neutral_balance *f)
{
	return f->ws == NB_WS_BT709 ? "linear BT.709" : "linear sRGB";
}

/* ------------------------------------------------------------------------- */
/* Correction model                                                          */
/* ------------------------------------------------------------------------- */

/* Rebuild cat_matrix from the measured illuminant and the target LCh. */
void nb_rebuild_cat(struct neutral_balance *f)
{
	if (!f->has_measurement) {
		for (int i = 0; i < 9; i++)
			f->cat_matrix[i] = (i % 4 == 0) ? 1.0f : 0.0f;
		f->cat_dirty = false;
		return;
	}

	double meas_xyz[3];
	nb_rgb2xyz((const double[]){f->measured_lin[0], f->measured_lin[1], f->measured_lin[2]}, meas_xyz);
	const double y_meas = fmax(meas_xyz[1], EPS);
	double src_white[3] = {meas_xyz[0] / y_meas, 1.0, meas_xyz[2] / y_meas};

	double dst_white[3], y_tgt;
	nb_white_from_lch(f->target_luminance, f->target_chroma, f->target_hue, dst_white, &y_tgt);

	double adapt[9];
	nb_cat_matrix(f->cat_method, src_white, dst_white, adapt);

	double xyz2rgb[9];
	if (!nb_xyz2rgb_matrix(xyz2rgb))
		return;

	double rgb2xyz_then_adapt[9], full[9];
	nb_mat3_mul(adapt, nb_rgb2xyz_matrix(), rgb2xyz_then_adapt);
	nb_mat3_mul(xyz2rgb, rgb2xyz_then_adapt, full);

	/* Exposure term: the measured color's Y maps onto the target L (FR-20). */
	const double exposure = y_tgt / y_meas;
	for (int i = 0; i < 9; i++)
		f->cat_matrix[i] = (float)(full[i] * exposure);

	f->cat_dirty = false;
	f->has_match = false;
}

void nb_set_measurement(struct neutral_balance *f, const double lin_rgb[3], bool smooth)
{
	if (smooth && f->has_measurement) {
		for (int i = 0; i < 3; i++)
			f->measured_lin[i] += (float)(lin_rgb[i] - f->measured_lin[i]) * NB_SMOOTH_ALPHA;
	} else {
		for (int i = 0; i < 3; i++)
			f->measured_lin[i] = (float)lin_rgb[i];
	}
	f->has_measurement = true;
	f->cat_dirty = true;
}

/* FR-25/FR-26: measured color is stored encoded; linearize before solving. */
void nb_measurement_from_sample(struct neutral_balance *f)
{
	double lin[3];
	for (int i = 0; i < 3; i++)
		lin[i] = nb_decode_linear(f->ws, f->last_sample[i]);
	nb_set_measurement(f, lin, false);
}

/* ------------------------------------------------------------------------- */
/* Effect                                                                    */
/* ------------------------------------------------------------------------- */

static void nb_load_effect(struct neutral_balance *f)
{
	if (f->effect)
		return;

	char *path = obs_module_file("neutral-balance.effect");
	char *error = NULL;
	f->effect = gs_effect_create_from_file(path, &error);
	if (error) {
		blog(LOG_WARNING, "[neutral-balance] effect compile error: %s", error);
		bfree(error);
	}
	bfree(path);

	if (!f->effect) {
		blog(LOG_ERROR, "[neutral-balance] failed to load neutral-balance.effect");
		return;
	}

	f->param_image = gs_effect_get_param_by_name(f->effect, "image");
	f->param_cat0 = gs_effect_get_param_by_name(f->effect, "cat_row0");
	f->param_cat1 = gs_effect_get_param_by_name(f->effect, "cat_row1");
	f->param_cat2 = gs_effect_get_param_by_name(f->effect, "cat_row2");
	f->param_ws = gs_effect_get_param_by_name(f->effect, "ws_mode");
	f->param_split = gs_effect_get_param_by_name(f->effect, "split_pos");

	if (!f->param_image || !f->param_cat0 || !f->param_cat1 || !f->param_cat2 || !f->param_ws ||
	    !f->param_split)
		blog(LOG_WARNING, "[neutral-balance] effect is missing expected uniforms");
}

static void nb_set_effect_params(struct neutral_balance *f)
{
	if (f->cat_dirty)
		nb_rebuild_cat(f);

	struct vec3 r0, r1, r2;
	vec3_set(&r0, f->cat_matrix[0], f->cat_matrix[1], f->cat_matrix[2]);
	vec3_set(&r1, f->cat_matrix[3], f->cat_matrix[4], f->cat_matrix[5]);
	vec3_set(&r2, f->cat_matrix[6], f->cat_matrix[7], f->cat_matrix[8]);
	gs_effect_set_vec3(f->param_cat0, &r0);
	gs_effect_set_vec3(f->param_cat1, &r1);
	gs_effect_set_vec3(f->param_cat2, &r2);
	gs_effect_set_float(f->param_ws, f->ws == NB_WS_BT709 ? 1.0f : 0.0f);
	gs_effect_set_float(f->param_split, f->show_split ? 0.5f : 0.0f);
}

/* ------------------------------------------------------------------------- */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------- */

static void nb_run_match(struct neutral_balance *f)
{
	if (!f->has_measurement)
		return;
	if (f->cat_dirty)
		nb_rebuild_cat(f);

	const double m[3] = {f->measured_lin[0], f->measured_lin[1], f->measured_lin[2]};
	double mat[9], corrected[3];
	for (int i = 0; i < 9; i++)
		mat[i] = f->cat_matrix[i];
	nb_mat3_apply(mat, m, corrected);

	double lab_sample[3], lab_target[3];
	nb_linrgb_to_lab(corrected, lab_sample);
	const double hrad = f->target_hue * M_PI / 180.0;
	lab_target[0] = f->target_luminance;
	lab_target[1] = f->target_chroma * cos(hrad);
	lab_target[2] = f->target_chroma * sin(hrad);

	f->match_de = nb_delta_e_2000(lab_sample, lab_target);
	f->has_match = true;
}

/* Dialog info callback (FR-27): show linear RGB + CIE XYZ + adapted result. */
static void nb_sample_info(const float rgb[3], char *out, size_t cap, void *user)
{
	struct neutral_balance *f = user;
	double lin[3], xyz[3];
	for (int i = 0; i < 3; i++)
		lin[i] = nb_decode_linear(f->ws, rgb[i]);
	nb_rgb2xyz(lin, xyz);
	const double y = fmax(xyz[1], EPS);

	if (f->cat_dirty)
		nb_rebuild_cat(f);
	double mat[9], adapted[3];
	for (int i = 0; i < 9; i++)
		mat[i] = f->cat_matrix[i];
	nb_mat3_apply(mat, lin, adapted);

	snprintf(out, cap,
		 "\nLinear RGB: %.4f, %.4f, %.4f\n"
		 "CIE XYZ: %.4f, %.4f, %.4f (Y=1)\n"
		 "Adapted to: %.3f, %.3f, %.3f",
		 lin[0], lin[1], lin[2], xyz[0] / y, 1.0, xyz[2] / y, adapted[0], adapted[1], adapted[2]);
}

static void nb_set_status_text(obs_properties_t *props, struct neutral_balance *f)
{
	obs_property_t *ws = obs_properties_get(props, "ws_status");
	if (ws)
		obs_property_set_description(ws, f ? nb_ws_name(f) : "linear sRGB");

	obs_property_t *range = obs_properties_get(props, "range_status");
	if (range)
		obs_property_set_description(range, "FULL (OBS GPU path delivers full-range RGB)");

	obs_property_t *diag = obs_properties_get(props, "diag_status");
	if (!diag)
		return;

	if (!f || !f->has_measurement) {
		obs_property_set_description(diag, "No measurement yet.");
		return;
	}

	double xyz[3];
	nb_rgb2xyz((const double[]){f->measured_lin[0], f->measured_lin[1], f->measured_lin[2]}, xyz);
	const double y = fmax(xyz[1], EPS);

	char text[512];
	snprintf(text, sizeof(text),
		 "Measured linear RGB: %.4f %.4f %.4f | XYZ: %.4f %.4f %.4f | CAT rows: %.3f %.3f %.3f / "
		 "%.3f %.3f %.3f / %.3f %.3f %.3f%s%s",
		 f->measured_lin[0], f->measured_lin[1], f->measured_lin[2], xyz[0] / y, 1.0, xyz[2] / y,
		 f->cat_matrix[0], f->cat_matrix[1], f->cat_matrix[2], f->cat_matrix[3], f->cat_matrix[4],
		 f->cat_matrix[5], f->cat_matrix[6], f->cat_matrix[7], f->cat_matrix[8],
		 f->has_match ? " | dE00 vs target: " : "", f->has_match ? "" : "");
	if (f->has_match) {
		size_t len = strlen(text);
		snprintf(text + len, sizeof(text) - len, "%.3f", f->match_de);
	}
	obs_property_set_description(diag, text);
}

/* ------------------------------------------------------------------------- */
/* Source info callbacks                                                     */
/* ------------------------------------------------------------------------- */

static void nb_update(void *data, obs_data_t *settings);

static const char *nb_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("NeutralBalance");
}

static void *nb_create(obs_data_t *settings, obs_source_t *source)
{
	struct neutral_balance *f = bzalloc(sizeof(*f));
	f->context = source;
	f->method = NB_METHOD_MANUAL;
	f->cat_method = NB_CAT_BRADFORD;
	f->target_luminance = 50.0f;
	f->source_space = GS_CS_SRGB;
	f->ws = NB_WS_SRGB;
	nb_build_lut(f->ws, f->inv_lut);
	f->cat_dirty = true;

	if (!nb_colorimetry_init()) {
		blog(LOG_ERROR, "[neutral-balance] RGB->XYZ matrix is singular");
		bfree(f);
		return NULL;
	}

	if (os_event_init(&f->capture_done, OS_EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_ERROR, "[neutral-balance] failed to init capture event");
		bfree(f);
		return NULL;
	}

	obs_enter_graphics();
	f->input_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	nb_load_effect(f);
	obs_leave_graphics();

	nb_update(f, settings);
	return f;
}

static void nb_destroy(void *data)
{
	struct neutral_balance *f = data;

	obs_enter_graphics();
	if (f->effect)
		gs_effect_destroy(f->effect);
	if (f->input_render)
		gs_texrender_destroy(f->input_render);
	if (f->stats_render)
		gs_texrender_destroy(f->stats_render);
	if (f->stats_surface)
		gs_stagesurface_destroy(f->stats_surface);
	if (f->capture_surface)
		gs_stagesurface_destroy(f->capture_surface);
	obs_leave_graphics();

	if (f->capture_done)
		os_event_destroy(f->capture_done);
	bfree(f->capture_buf);
	bfree(f);
}

static void nb_update(void *data, obs_data_t *settings)
{
	struct neutral_balance *f = data;

	f->method = (int)obs_data_get_int(settings, "method");
	f->shades_p = (float)obs_data_get_double(settings, "shades_p");
	f->cat_method = (int)obs_data_get_int(settings, "cat_method");
	f->target_hue = (float)obs_data_get_double(settings, "target_hue");
	f->target_luminance = (float)obs_data_get_double(settings, "target_luminance");
	f->target_chroma = (float)obs_data_get_double(settings, "target_chroma");
	f->sample_radius = (long)obs_data_get_int(settings, "sample_radius");
	f->show_split = obs_data_get_bool(settings, "show_split");
	f->has_sample = obs_data_get_bool(settings, "has_sample");
	f->last_sample[0] = (float)obs_data_get_double(settings, "last_sample_r");
	f->last_sample[1] = (float)obs_data_get_double(settings, "last_sample_g");
	f->last_sample[2] = (float)obs_data_get_double(settings, "last_sample_b");

	if (f->method == NB_METHOD_MANUAL && f->has_sample)
		nb_measurement_from_sample(f);
	else if (f->method != NB_METHOD_MANUAL)
		f->last_stats_ts = 0; /* recompute on the next frame */

	f->cat_dirty = true;
}

static void nb_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "method", NB_METHOD_MANUAL);
	obs_data_set_default_double(settings, "shades_p", 6.0);
	obs_data_set_default_int(settings, "cat_method", NB_CAT_BRADFORD);
	obs_data_set_default_double(settings, "target_hue", 0.0);
	obs_data_set_default_double(settings, "target_luminance", 50.0);
	obs_data_set_default_double(settings, "target_chroma", 0.0);
	obs_data_set_default_int(settings, "sample_radius", 3);
	obs_data_set_default_bool(settings, "show_split", false);
	obs_data_set_default_bool(settings, "has_sample", false);
	obs_data_set_default_double(settings, "last_sample_r", 0.5);
	obs_data_set_default_double(settings, "last_sample_g", 0.5);
	obs_data_set_default_double(settings, "last_sample_b", 0.5);
	obs_data_set_default_string(settings, "sample_rgb_text", "");
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

static void nb_video_render(void *data, gs_effect_t *effect)
{
	struct neutral_balance *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	if (!target)
		return;

	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	if (!w || !h)
		return;

	/* FR-01: query the source's actual color space every frame. */
	static const enum gs_color_space preferred[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED,
							GS_CS_709_SCRGB};
	const enum gs_color_space cs = obs_source_get_color_space(target, OBS_COUNTOF(preferred), preferred);
	nb_set_working_space(f, cs);

	if (!f->effect)
		nb_load_effect(f);
	if (!f->effect) {
		obs_source_video_render(target);
		return;
	}

	const bool want_input = f->method != NB_METHOD_MANUAL || f->capture_requested;

	if (!want_input) {
		/* Zero-copy path; declare the active color space for the pass. */
		if (!obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA, cs,
								      OBS_ALLOW_DIRECT_RENDERING))
			return;

		nb_set_effect_params(f);
		while (gs_effect_loop(f->effect, "Draw"))
			obs_source_process_filter_end(f->context, f->effect, 0, 0);
		return;
	}

	/* Custom draw path: render the filter input into our own texture so it
	 * can be analysed (auto methods) or captured (eyedropper). */
	gs_texrender_reset(f->input_render);
	if (!gs_texrender_begin(f->input_render, w, h)) {
		if (!f->logged_begin_fail) {
			blog(LOG_WARNING, "[neutral-balance] texrender_begin failed (%ux%u); "
					  "capture/analysis disabled", w, h);
			f->logged_begin_fail = true;
		}
		obs_source_video_render(target);
		return;
	}
	struct vec4 black;
	vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
	obs_source_video_render(target);
	gs_texrender_end(f->input_render);
	f->logged_begin_fail = false;

	gs_texture_t *input = gs_texrender_get_texture(f->input_render);
	if (!input)
		return;

	if (f->method != NB_METHOD_MANUAL)
		nb_update_stats(f, input);

	if (f->capture_requested) {
		nb_do_capture(f, input, w, h);
		f->capture_requested = false;
	}

	gs_effect_set_texture(f->param_image, input);
	nb_set_effect_params(f);
	while (gs_effect_loop(f->effect, "Draw"))
		gs_draw_sprite(input, 0, w, h);

	UNUSED_PARAMETER(effect);
}

static enum gs_color_space nb_video_get_color_space(void *data, size_t count,
						    const enum gs_color_space *preferred_spaces)
{
	struct neutral_balance *f = data;
	return obs_source_get_color_space(obs_filter_get_target(f->context), count, preferred_spaces);
}

/* ------------------------------------------------------------------------- */
/* Properties UI                                                             */
/* ------------------------------------------------------------------------- */

static bool method_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	const long method = (long)obs_data_get_int(settings, "method");
	const bool manual = method == NB_METHOD_MANUAL;

	obs_property_set_visible(obs_properties_get(props, "shades_p"), method == NB_METHOD_SHADES_OF_GREY);
	obs_property_set_visible(obs_properties_get(props, "pick_neutral"), manual);
	obs_property_set_visible(obs_properties_get(props, "sample_radius"), manual);
	obs_property_set_visible(obs_properties_get(props, "sample_rgb_text"), manual);

	UNUSED_PARAMETER(p);
	return true;
}

static void nb_sync_sample_text(obs_data_t *settings, const float rgb[3])
{
	char text[64];
	snprintf(text, sizeof(text), "%d, %d, %d", (int)(rgb[0] * 255.0f + 0.5f), (int)(rgb[1] * 255.0f + 0.5f),
		 (int)(rgb[2] * 255.0f + 0.5f));
	obs_data_set_string(settings, "sample_rgb_text", text);
}

/* Unified "Measured Sample: (R,G,B)" input on the 0-255 scale. Any separator
 * works; values are stored in the working-space encoding and linearized at
 * use (FR-25). */
static bool sample_text_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	const char *s = obs_data_get_string(settings, "sample_rgb_text");
	int vals[3];
	int i = 0;

	while (*s && i < 3) {
		char *end = NULL;
		const long v = strtol(s, &end, 10);
		if (end == s) {
			s++;
			continue;
		}
		vals[i++] = (int)v;
		s = end;
	}

	if (i == 3) {
		obs_data_set_double(settings, "last_sample_r", (double)vals[0] / 255.0);
		obs_data_set_double(settings, "last_sample_g", (double)vals[1] / 255.0);
		obs_data_set_double(settings, "last_sample_b", (double)vals[2] / 255.0);
		obs_data_set_bool(settings, "has_sample", true);
	}

	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	return true;
}

static bool pick_neutral_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct neutral_balance *f = data;
	uint8_t *buf = NULL;
	uint32_t w = 0, h = 0;

	if (!nb_eyedropper_available()) {
		blog(LOG_WARNING, "[neutral-balance] built without Qt - eyedropper unavailable");
		return false;
	}

	if (!nb_request_capture(f, &buf, &w, &h)) {
		blog(LOG_WARNING, "[neutral-balance] capture timed out; is the source visible?");
		return false;
	}

	float rgb[3];
	const bool ok = nb_eyedropper_sample_rgba(buf, w, h, (int)f->sample_radius, rgb, nb_sample_info, f);
	bfree(buf);

	if (!ok) {
		blog(LOG_INFO, "[neutral-balance] sampling dialog closed without a selection");
		return false;
	}

	/* Store the measured color in the native encoding (FR-25). */
	obs_data_t *settings = obs_source_get_settings(f->context);
	obs_data_set_bool(settings, "has_sample", true);
	obs_data_set_double(settings, "last_sample_r", rgb[0]);
	obs_data_set_double(settings, "last_sample_g", rgb[1]);
	obs_data_set_double(settings, "last_sample_b", rgb[2]);
	nb_sync_sample_text(settings, rgb);
	obs_source_update(f->context, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	return true;
}

static bool reset_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct neutral_balance *f = data;

	obs_data_t *settings = obs_source_get_settings(f->context);
	obs_data_set_int(settings, "cat_method", NB_CAT_BRADFORD);
	obs_data_set_double(settings, "target_hue", 0.0);
	obs_data_set_double(settings, "target_luminance", 50.0);
	obs_data_set_double(settings, "target_chroma", 0.0);
	obs_data_set_bool(settings, "has_sample", false);
	obs_data_set_double(settings, "last_sample_r", 0.5);
	obs_data_set_double(settings, "last_sample_g", 0.5);
	obs_data_set_double(settings, "last_sample_b", 0.5);
	obs_data_set_string(settings, "sample_rgb_text", "");
	obs_source_update(f->context, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	return true;
}

static bool match_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct neutral_balance *f = data;

	if (!f->has_measurement) {
		blog(LOG_INFO, "[neutral-balance] Match Darktable: no measurement yet");
		return false;
	}

	nb_run_match(f);
	blog(LOG_INFO, "[neutral-balance] dE00 vs target: %.3f", f->match_de);

	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	return true;
}

static obs_properties_t *nb_properties(void *data)
{
	struct neutral_balance *f = data;

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "ws_status", obs_module_text("WorkingSpace"), OBS_TEXT_INFO);
	obs_properties_add_text(props, "range_status", obs_module_text("VideoRange"), OBS_TEXT_INFO);

	obs_property_t *method = obs_properties_add_list(props, "method", obs_module_text("Method"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(method, obs_module_text("Method.GreyWorld"), NB_METHOD_GREY_WORLD);
	obs_property_list_add_int(method, obs_module_text("Method.ShadesOfGrey"), NB_METHOD_SHADES_OF_GREY);
	obs_property_list_add_int(method, obs_module_text("Method.Manual"), NB_METHOD_MANUAL);
	obs_property_set_modified_callback(method, method_modified);

	obs_property_t *cat = obs_properties_add_list(props, "cat_method", obs_module_text("CatMethod"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cat, obs_module_text("Cat.Bradford"), NB_CAT_BRADFORD);
	obs_property_list_add_int(cat, obs_module_text("Cat.CAT02"), NB_CAT_CAT02);
	obs_property_list_add_int(cat, obs_module_text("Cat.CAT16"), NB_CAT_CAT16);
	obs_property_list_add_int(cat, obs_module_text("Cat.VonKries"), NB_CAT_VON_KRIES);
	obs_property_list_add_int(cat, obs_module_text("Cat.Diagonal"), NB_CAT_DIAGONAL);

	obs_properties_add_float_slider(props, "shades_p", obs_module_text("ShadesP"), 1.0, 12.0, 0.1);
	obs_properties_add_button(props, "pick_neutral", obs_module_text("PickNeutral"), pick_neutral_clicked);
	obs_properties_add_int_slider(props, "sample_radius", obs_module_text("SampleRadius"), 1, 10, 1);

	obs_property_t *sample_text = obs_properties_add_text(props, "sample_rgb_text",
								obs_module_text("SampleText"), OBS_TEXT_DEFAULT);
	obs_property_set_modified_callback(sample_text, sample_text_modified);

	obs_properties_add_float_slider(props, "target_hue", obs_module_text("TargetHue"), 0.0, 360.0, 0.1);
	obs_properties_add_float_slider(props, "target_luminance", obs_module_text("TargetLuminance"), 0.0, 100.0,
					0.5);
	obs_properties_add_float_slider(props, "target_chroma", obs_module_text("TargetChroma"), 0.0, 100.0, 0.5);

	obs_properties_add_bool(props, "show_split", obs_module_text("ShowSplit"));
	obs_properties_add_button(props, "match_darktable", obs_module_text("MatchDarktable"), match_clicked);
	obs_properties_add_button(props, "reset", obs_module_text("Reset"), reset_clicked);
	obs_properties_add_text(props, "diag_status", obs_module_text("Diagnostics"), OBS_TEXT_INFO);

	obs_data_t *settings = f ? obs_source_get_settings(f->context) : NULL;
	method_modified(props, method, settings);
	if (settings)
		obs_data_release(settings);

	nb_set_status_text(props, f);
	return props;
}

/* ------------------------------------------------------------------------- */
/* Registration                                                              */
/* ------------------------------------------------------------------------- */

struct obs_source_info neutral_balance_filter_info = {
	.id = "obs_neutral_balance_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = nb_get_name,
	.create = nb_create,
	.destroy = nb_destroy,
	.get_properties = nb_properties,
	.get_defaults = nb_get_defaults,
	.update = nb_update,
	.video_render = nb_video_render,
	.video_get_color_space = nb_video_get_color_space,
	.icon_type = OBS_ICON_TYPE_COLOR,
};
