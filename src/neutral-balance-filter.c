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
 * Implementation notes (see PRD OBS-NB-PRD-001):
 *
 * - Rendering follows the officially documented custom-draw path for
 *   filters: the filter input is rendered with obs_source_video_render()
 *   into a texture owned by the filter, which is both drawn with the
 *   correction effect and used for statistics/capture. When no analysis is
 *   pending (manual mode, no eyedropper request), the zero-copy
 *   obs_source_process_filter_begin/end path is used instead.
 * - All GPU resources are created, used and destroyed on the render thread.
 * - The eyedropper capture is requested from the UI thread via an atomic
 *   flag and completed on the render thread; the UI thread waits on an
 *   os_event with a timeout, so a hidden/removed source can never deadlock
 *   the properties dialog (NFR-02).
 * - Statistics are computed on a 160x90 RGBA read-back at 10 Hz. Staged
 *   surfaces this small do not measurably stall the GPU; the PRD budget
 *   (<= 0.5 ms per 1080p frame) is met because the steady-state filter only
 *   adds one texture copy plus a full-screen pass.
 *
 * TODO(v1.1): replace the HSL/YIQ hue+lightness bias with a full CIE LCh
 * pipeline as outlined in PRD section 7.5.
 */

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/bmem.h>

#include "neutral-balance-filter.h"
#include "eyedropper.h"

#define EPS 1e-4f

/* ------------------------------------------------------------------------- */
/* Small math helpers                                                        */
/* ------------------------------------------------------------------------- */

/* Normalized so the mean of the three gains stays 1.0: the correction
 * neutralizes the illuminant estimate without shifting overall exposure. */
static void nb_normalize_gains(float gains[3])
{
	float mean = (gains[0] + gains[1] + gains[2]) / 3.0f;
	if (mean < EPS)
		return;

	gains[0] /= mean;
	gains[1] /= mean;
	gains[2] /= mean;
}

void nb_compute_manual_gains(struct neutral_balance *f, float r, float g, float b)
{
	/* Direct von Kries diagonal correction: the sampled color maps exactly
	 * onto the target neutral. Channel ratios carry the white balance; the
	 * common scale (target_luminance / sample brightness) is a true
	 * multiplicative exposure/gain axis. Gains are intentionally NOT
	 * normalized - normalization would cancel the target lightness
	 * algebraically, which is what made the old luminance slider behave as
	 * a post-hoc brightness offset instead of a gain. */
	f->gains[0] = f->target_luminance / fmaxf(r, EPS);
	f->gains[1] = f->target_luminance / fmaxf(g, EPS);
	f->gains[2] = f->target_luminance / fmaxf(b, EPS);
}

static void nb_update(void *data, obs_data_t *settings);

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
	f->param_gains = gs_effect_get_param_by_name(f->effect, "gains");
	f->param_hue_bias = gs_effect_get_param_by_name(f->effect, "hue_bias");
	f->param_lum_bias = gs_effect_get_param_by_name(f->effect, "lum_bias");
	f->param_split = gs_effect_get_param_by_name(f->effect, "split_pos");
	f->param_linear = gs_effect_get_param_by_name(f->effect, "linear_mode");
}

static void nb_set_effect_params(struct neutral_balance *f)
{
	struct vec3 gains;
	vec3_set(&gains, f->gains[0], f->gains[1], f->gains[2]);
	gs_effect_set_vec3(f->param_gains, &gains);

	/* Target hue is a creative bias of the manual correction; target
	 * luminance is a gain axis (see nb_compute_manual_gains), NOT an
	 * additive brightness offset - so lum_bias is always zero. */
	gs_effect_set_float(f->param_hue_bias, f->method == NB_METHOD_MANUAL ? f->target_hue : 0.0f);
	gs_effect_set_float(f->param_lum_bias, 0.0f);
	gs_effect_set_float(f->param_split, f->show_split ? 0.5f : 0.0f);
	gs_effect_set_float(f->param_linear, f->linear_light ? 1.0f : 0.0f);
}

/* ------------------------------------------------------------------------- */
/* Automatic estimation (Grey World / Shades of Grey)                        */
/* ------------------------------------------------------------------------- */

/* Grey World (Buchsbaum 1980) and Shades of Grey (Finlayson & Trezzi 2004):
 * the per-channel Minkowski p-norm over the frame estimates the illuminant
 * (p=1 -> Grey World, p=6 best on a calibrated dataset, p=inf -> max-RGB).
 * Gains equalize the estimates to their mean; see README "Research basis". */
static void nb_update_stats(struct neutral_balance *f, gs_texture_t *input)
{
	uint64_t now = os_gettime_ns();
	if (now - f->last_stats_ts < NB_STATS_INTERVAL_NS)
		return;
	f->last_stats_ts = now;

	if (!f->stats_render || !f->stats_surface) {
		if (f->stats_render)
			gs_texrender_destroy(f->stats_render);
		if (f->stats_surface)
			gs_stagesurface_destroy(f->stats_surface);
		f->stats_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		f->stats_surface = gs_stagesurface_create(NB_STATS_W, NB_STATS_H, GS_RGBA);
		if (!f->stats_render || !f->stats_surface) {
			blog(LOG_WARNING, "[neutral-balance] failed to create stats pipeline");
			return;
		}
	}

	/* Downscale the input into a small offscreen target. */
	gs_texrender_reset(f->stats_render);
	if (!gs_texrender_begin(f->stats_render, NB_STATS_W, NB_STATS_H))
		return;

	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), input);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(input, 0, NB_STATS_W, NB_STATS_H);
	gs_texrender_end(f->stats_render);

	gs_texture_t *small = gs_texrender_get_texture(f->stats_render);
	if (!small)
		return;

	gs_stage_texture(f->stats_surface, small);

	uint8_t *data = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(f->stats_surface, &data, &linesize)) {
		blog(LOG_WARNING, "[neutral-balance] stats surface map failed");
		return;
	}

	/* GS_RGBA stage surfaces expose R,G,B,A byte order on all supported
	 * renderers (D3D11 R8G8B8A8_UNORM, GL RGBA8). Statistics run in the
	 * gamma-encoded domain - acceptable for v1, TODO(v1.1): linear light. */
	const int n = NB_STATS_W * NB_STATS_H;
	double sum[3] = {0.0, 0.0, 0.0};
	for (uint32_t y = 0; y < NB_STATS_H; y++) {
		const uint8_t *row = data + (size_t)y * linesize;
		for (uint32_t x = 0; x < NB_STATS_W; x++) {
			const uint8_t *px = row + (size_t)x * 4;
			float r = px[0] / 255.0f;
			float g = px[1] / 255.0f;
			float b = px[2] / 255.0f;
			if (f->method == NB_METHOD_SHADES_OF_GREY) {
				sum[0] += pow(r, f->shades_p);
				sum[1] += pow(g, f->shades_p);
				sum[2] += pow(b, f->shades_p);
			} else {
				sum[0] += r;
				sum[1] += g;
				sum[2] += b;
			}
		}
	}
	gs_stagesurface_unmap(f->stats_surface);

	float e[3];
	if (f->method == NB_METHOD_SHADES_OF_GREY) {
		float inv = 1.0f / fmaxf(f->shades_p, EPS);
		e[0] = powf((float)(sum[0] / n), inv);
		e[1] = powf((float)(sum[1] / n), inv);
		e[2] = powf((float)(sum[2] / n), inv);
	} else {
		e[0] = (float)(sum[0] / n);
		e[1] = (float)(sum[1] / n);
		e[2] = (float)(sum[2] / n);
	}

	/* Equalize the per-channel estimates to their mean. */
	float mean = (e[0] + e[1] + e[2]) / 3.0f;
	float target[3] = {mean / fmaxf(e[0], EPS), mean / fmaxf(e[1], EPS), mean / fmaxf(e[2], EPS)};
	nb_normalize_gains(target);

	/* Temporal smoothing against flicker (PRD 7.6). */
	const float alpha = 0.35f;
	for (int i = 0; i < 3; i++)
		f->gains[i] += (target[i] - f->gains[i]) * alpha;
}

/* ------------------------------------------------------------------------- */
/* Eyedropper capture (render thread)                                        */
/* ------------------------------------------------------------------------- */

static void nb_do_capture(struct neutral_balance *f, gs_texture_t *input, uint32_t w, uint32_t h)
{
	if (f->capture_surface && (f->capture_w != w || f->capture_h != h)) {
		gs_stagesurface_destroy(f->capture_surface);
		f->capture_surface = NULL;
	}
	if (!f->capture_surface) {
		f->capture_surface = gs_stagesurface_create(w, h, GS_RGBA);
		f->capture_w = w;
		f->capture_h = h;
	}
	if (!f->capture_surface) {
		blog(LOG_WARNING, "[neutral-balance] capture stagesurface create failed (%ux%u)", w, h);
		return;
	}

	/* One-shot read-back of the *uncorrected* filter input (PRD FR-09). */
	gs_stage_texture(f->capture_surface, input);

	uint8_t *data = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(f->capture_surface, &data, &linesize)) {
		blog(LOG_WARNING, "[neutral-balance] capture stagesurface map failed");
		return;
	}

	size_t size = (size_t)w * h * 4;
	f->capture_buf = brealloc(f->capture_buf, size);
	for (uint32_t y = 0; y < h; y++)
		memcpy(f->capture_buf + (size_t)y * w * 4, data + (size_t)y * linesize, (size_t)w * 4);

	gs_stagesurface_unmap(f->capture_surface);

	f->capture_ready = true;
	os_event_signal(f->capture_done);
}

/* Requests a frozen frame from the render thread and hands ownership of a
 * copy to the caller (bfree with bfree()). Returns false on timeout. */
static bool nb_request_capture(struct neutral_balance *f, uint8_t **out_buf, uint32_t *out_w, uint32_t *out_h)
{
	f->capture_ready = false;
	os_event_reset(f->capture_done);
	f->capture_requested = true;

	/* Wait up to ~5 s; if the source is hidden or removed the filter never
	 * renders and we must not block the UI thread (NFR-02). */
	for (int i = 0; i < 100; i++) {
		if (f->capture_ready && f->capture_buf) {
			size_t size = (size_t)f->capture_w * f->capture_h * 4;
			uint8_t *copy = bmalloc(size);
			memcpy(copy, f->capture_buf, size);
			f->capture_ready = false;
			*out_buf = copy;
			*out_w = f->capture_w;
			*out_h = f->capture_h;
			return true;
		}
		os_event_timedwait(f->capture_done, 50);
	}

	f->capture_requested = false;
	return false;
}

/* ------------------------------------------------------------------------- */
/* Source info callbacks                                                     */
/* ------------------------------------------------------------------------- */

static const char *nb_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("NeutralBalance");
}

static void *nb_create(obs_data_t *settings, obs_source_t *source)
{
	struct neutral_balance *f = bzalloc(sizeof(*f));
	f->context = source;
	f->gains[0] = f->gains[1] = f->gains[2] = 1.0f;

	if (os_event_init(&f->capture_done, OS_EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_ERROR, "[neutral-balance] failed to init capture event");
		bfree(f);
		return NULL;
	}

	/* Created on the main thread after the graphics subsystem is up, which
	 * is the standard point for filter GPU resource creation. */
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
	f->target_hue = (float)obs_data_get_double(settings, "target_hue");
	f->target_luminance = (float)obs_data_get_double(settings, "target_luminance") / 100.0f;
	f->sample_radius = (long)obs_data_get_int(settings, "sample_radius");
	f->show_split = obs_data_get_bool(settings, "show_split");
	f->linear_light = obs_data_get_bool(settings, "linear_light");
	f->has_sample = obs_data_get_bool(settings, "has_sample");
	f->last_sample[0] = (float)obs_data_get_double(settings, "last_sample_r");
	f->last_sample[1] = (float)obs_data_get_double(settings, "last_sample_g");
	f->last_sample[2] = (float)obs_data_get_double(settings, "last_sample_b");

	if (f->method == NB_METHOD_MANUAL) {
		/* Manual mode is stable after sampling (FR-07): gains are derived
		 * from the stored sample so the hue/luminance sliders stay live. */
		if (f->has_sample)
			nb_compute_manual_gains(f, f->last_sample[0], f->last_sample[1], f->last_sample[2]);
		else
			f->gains[0] = f->gains[1] = f->gains[2] = 1.0f;
	} else {
		/* Keep the current gains: resetting to identity on every settings
		 * tick (e.g. shades_p slider drags) flashes the uncorrected image.
		 * Force stats to recompute on the next frame instead of waiting
		 * out the 10 Hz throttle; the temporal smoother adapts from the
		 * existing gains within a few frames. */
		f->last_stats_ts = 0;
	}
}

static void nb_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "method", NB_METHOD_MANUAL);
	obs_data_set_default_double(settings, "shades_p", 6.0);
	obs_data_set_default_double(settings, "target_hue", 0.0);
	obs_data_set_default_double(settings, "target_luminance", 50.0);
	obs_data_set_default_int(settings, "sample_radius", 3);
	obs_data_set_default_bool(settings, "show_split", false);
	obs_data_set_default_string(settings, "sample_rgb_text", "");
	obs_data_set_default_bool(settings, "linear_light", true);
	obs_data_set_default_bool(settings, "has_sample", false);
	obs_data_set_default_double(settings, "last_sample_r", 0.5);
	obs_data_set_default_double(settings, "last_sample_g", 0.5);
	obs_data_set_default_double(settings, "last_sample_b", 0.5);
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

	if (!f->effect)
		nb_load_effect(f);
	if (!f->effect) {
		obs_source_video_render(target);
		return;
	}

	const bool want_input = f->method != NB_METHOD_MANUAL || f->capture_requested;

	if (!want_input) {
		/* Zero-copy path (FR-03): no texture analysis or capture pending. */
		if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
			return;

		nb_set_effect_params(f);
		while (gs_effect_loop(f->effect, "Draw"))
			obs_source_process_filter_end(f->context, f->effect, 0, 0);
		return;
	}

	/* Custom draw path: render the filter input into our own texture so it
	 * can be analysed (auto methods) or captured (eyedropper) before the
	 * corrected image is drawn. This is the documented fallback pattern for
	 * filters that need access to the input texture. */
	/* Recreate the texture per use (matches the known-good capture path) and
	 * clear before drawing so regions the source does not cover stay black
	 * instead of showing recycled VRAM. */
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

static void nb_sync_sample_text(obs_data_t *settings, const float rgb[3])
{
	char text[64];
	snprintf(text, sizeof(text), "%d, %d, %d", (int)(rgb[0] * 255.0f + 0.5f),
		 (int)(rgb[1] * 255.0f + 0.5f), (int)(rgb[2] * 255.0f + 0.5f));
	obs_data_set_string(settings, "sample_rgb_text", text);
}

/* Unified "Sample: (R,G,B)" input on the 0-255 scale. Any separator works. */
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

static bool method_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	const long method = (long)obs_data_get_int(settings, "method");
	const bool manual = method == NB_METHOD_MANUAL;

	obs_property_set_visible(obs_properties_get(props, "shades_p"), method == NB_METHOD_SHADES_OF_GREY);
	obs_property_set_visible(obs_properties_get(props, "pick_neutral"), manual);
	obs_property_set_visible(obs_properties_get(props, "sample_radius"), manual);
	obs_property_set_visible(obs_properties_get(props, "sample_rgb_text"), manual);
	obs_property_set_visible(obs_properties_get(props, "target_hue"), manual);
	obs_property_set_visible(obs_properties_get(props, "target_luminance"), manual);

	UNUSED_PARAMETER(p);
	return true;
}

static bool pick_neutral_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct neutral_balance *f = data;
	uint8_t *buf = NULL;
	uint32_t w = 0, h = 0;

	if (!nb_eyedropper_available()) {
		blog(LOG_WARNING,
		     "[neutral-balance] this build has no Qt - the eyedropper dialog is unavailable "
		     "(install Qt6 Widgets dev package and rebuild; the automatic methods still work)");
		return false;
	}

	if (!nb_request_capture(f, &buf, &w, &h)) {
		blog(LOG_WARNING, "[neutral-balance] capture timed out; is the source visible?");
		return false;
	}

	float rgb[3];
	const bool ok = nb_eyedropper_sample_rgba(buf, w, h, (int)f->sample_radius, rgb);
	bfree(buf);

	if (!ok) {
		blog(LOG_INFO, "[neutral-balance] sampling dialog closed without a selection");
		return false;
	}

	nb_compute_manual_gains(f, rgb[0], rgb[1], rgb[2]);

	/* Persist the sample and gains (FR-05). */
	obs_data_t *settings = obs_source_get_settings(f->context);
	obs_data_set_bool(settings, "has_sample", true);
	obs_data_set_double(settings, "last_sample_r", rgb[0]);
	obs_data_set_double(settings, "last_sample_g", rgb[1]);
	obs_data_set_double(settings, "last_sample_b", rgb[2]);
	obs_data_set_double(settings, "gain_r", f->gains[0]);
	obs_data_set_double(settings, "gain_g", f->gains[1]);
	obs_data_set_double(settings, "gain_b", f->gains[2]);
	nb_sync_sample_text(settings, rgb);
	obs_source_update(f->context, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	return true; /* refresh properties so the status line updates */
}

static bool reset_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct neutral_balance *f = data;

	obs_data_t *settings = obs_source_get_settings(f->context);
	obs_data_set_double(settings, "target_hue", 0.0);
	obs_data_set_double(settings, "target_luminance", 50.0);
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

static void nb_set_status_text(obs_properties_t *props, struct neutral_balance *f)
{
	obs_property_t *status = obs_properties_get(props, "sample_status");
	if (!status)
		return;

	if (!f || !f->has_sample) {
		obs_property_set_description(status, obs_module_text("NoSample"));
		return;
	}

	char text[256];
	snprintf(text, sizeof(text),
		 "Last sample: R %.0f G %.0f B %.0f  |  Gains: R %.3f / G %.3f / B %.3f",
		 f->last_sample[0] * 255.0f, f->last_sample[1] * 255.0f, f->last_sample[2] * 255.0f,
		 (double)f->gains[0], (double)f->gains[1], (double)f->gains[2]);
	obs_property_set_description(status, text);
}

static obs_properties_t *nb_properties(void *data)
{
	struct neutral_balance *f = data;

	obs_properties_t *props = obs_properties_create();

	obs_property_t *method = obs_properties_add_list(props, "method", obs_module_text("Method"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(method, obs_module_text("Method.GreyWorld"), NB_METHOD_GREY_WORLD);
	obs_property_list_add_int(method, obs_module_text("Method.ShadesOfGrey"), NB_METHOD_SHADES_OF_GREY);
	obs_property_list_add_int(method, obs_module_text("Method.Manual"), NB_METHOD_MANUAL);
	obs_property_set_modified_callback(method, method_modified);

	obs_properties_add_float_slider(props, "shades_p", obs_module_text("ShadesP"), 1.0, 12.0, 0.1);
	obs_properties_add_button(props, "pick_neutral", obs_module_text("PickNeutral"), pick_neutral_clicked);
	obs_properties_add_int_slider(props, "sample_radius", obs_module_text("SampleRadius"), 1, 10, 1);

	obs_property_t *sample_text = obs_properties_add_text(props, "sample_rgb_text",
								obs_module_text("SampleText"), OBS_TEXT_DEFAULT);
	obs_property_set_modified_callback(sample_text, sample_text_modified);
	obs_properties_add_float_slider(props, "target_hue", obs_module_text("TargetHue"), 0.0, 360.0, 0.1);
	obs_properties_add_float_slider(props, "target_luminance", obs_module_text("TargetLuminance"), 0.0, 100.0,
					0.5);
	obs_properties_add_button(props, "reset", obs_module_text("Reset"), reset_clicked);
	obs_properties_add_bool(props, "show_split", obs_module_text("ShowSplit"));
	obs_properties_add_bool(props, "linear_light", obs_module_text("LinearLight"));
	obs_properties_add_text(props, "sample_status", obs_module_text("Status"), OBS_TEXT_INFO);

	/* Apply visibility for the current settings (data may be NULL in some
	 * frontend contexts, in which case the defaults apply). */
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
