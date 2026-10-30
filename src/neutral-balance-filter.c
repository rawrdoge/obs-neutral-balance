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
#include "eyedropper.h"

#define EPS 1e-6

/* ------------------------------------------------------------------------- */
/* 3x3 matrix helpers (row-major, double precision)                          */
/* ------------------------------------------------------------------------- */

static void nb_mat3_mul(const double a[9], const double b[9], double out[9])
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			out[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}

static bool nb_mat3_inv(const double m[9], double out[9])
{
	const double a = m[0], b = m[1], c = m[2];
	const double d = m[3], e = m[4], f = m[5];
	const double g = m[6], h = m[7], i = m[8];
	const double A = e * i - f * h;
	const double B = -(d * i - f * g);
	const double C = d * h - e * g;
	const double det = a * A + b * B + c * C;
	if (fabs(det) < 1e-12)
		return false;
	const double id = 1.0 / det;
	out[0] = A * id;
	out[1] = -(b * i - c * h) * id;
	out[2] = (b * f - c * e) * id;
	out[3] = B * id;
	out[4] = (a * i - c * g) * id;
	out[5] = -(a * f - c * d) * id;
	out[6] = C * id;
	out[7] = -(a * h - b * g) * id;
	out[8] = (a * e - b * d) * id;
	return true;
}

static void nb_mat3_apply(const double m[9], const double v[3], double out[3])
{
	for (int r = 0; r < 3; r++)
		out[r] = m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
}

/* ------------------------------------------------------------------------- */
/* Colorimetry (double precision on the CPU, per NFR-04)                     */
/* ------------------------------------------------------------------------- */

/* sRGB / BT.709 primaries, D65 white (both working spaces share these). */
static const double NB_RGB2XYZ[9] = {0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522,
				     0.0721750, 0.0193339, 0.1191920, 0.9503041};

static double NB_XYZ2RGB[9];

static const double NB_D65[3] = {0.95047, 1.0, 1.08883};

/* Cone-response matrices for the CAT methods (FR-15). */
static const double NB_BRADFORD[9] = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367,
				      0.0389, -0.0685, 1.0296};
static const double NB_CAT02[9] = {0.7328, 0.4296, -0.1624, -0.7036, 1.6975, 0.0061,
				   0.0030, 0.0136, 0.9834};
static const double NB_CAT16[9] = {0.401288, 0.650173, -0.051461, -0.250268, 1.204414, 0.045854,
				   -0.002079, 0.048952, 0.953127};
static const double NB_HPE[9] = {0.40024, 0.70760, -0.08081, -0.22630, 1.16532, 0.04570,
				 0.0, 0.0, 0.91822};

void nb_rgb2xyz(const double rgb[3], double xyz[3])
{
	nb_mat3_apply(NB_RGB2XYZ, rgb, xyz);
}

/* Lab/LCh transfer function and its inverse (CIE 1976). */
static double nb_lab_f(double t)
{
	return t > 0.008856 ? cbrt(t) : (903.3 * t + 16.0) / 116.0;
}

static double nb_lab_f_inv(double t)
{
	const double t3 = t * t * t;
	return t3 > 0.008856 ? t3 : (116.0 * t - 16.0) / 903.3;
}

/* CIE LCh (L 0-100, C 0-100+, h degrees) -> XYZ. xyz_norm is normalized to
 * Y = 1 (the adaptation white); *y_unnorm receives the unnormalized Y (== L). */
void nb_white_from_lch(double L, double C, double h_deg, double xyz_norm[3], double *y_unnorm)
{
	const double h = h_deg * M_PI / 180.0;
	const double a = C * cos(h);
	const double b = C * sin(h);
	const double fy = (L + 16.0) / 116.0;
	const double fx = fy + a / 500.0;
	const double fz = fy - b / 200.0;

	double xyz[3];
	xyz[0] = NB_D65[0] * nb_lab_f_inv(fx);
	xyz[1] = NB_D65[1] * nb_lab_f_inv(fy);
	xyz[2] = NB_D65[2] * nb_lab_f_inv(fz);

	const double y = xyz[1];
	*y_unnorm = y;
	if (y > EPS) {
		xyz_norm[0] = xyz[0] / y;
		xyz_norm[1] = 1.0;
		xyz_norm[2] = xyz[2] / y;
	} else {
		xyz_norm[0] = NB_D65[0];
		xyz_norm[1] = 1.0;
		xyz_norm[2] = NB_D65[2];
	}
}

/* Full chromatic-adaptation matrix in XYZ: maps src_white -> dst_white
 * (both Y-normalized). Diagonal mode uses the identity cone matrix (FR-18). */
void nb_cat_matrix(int cat_method, const double src_white_norm[3], const double dst_white_norm[3],
		   double out[9])
{
	const double *cone = NB_BRADFORD;
	switch (cat_method) {
	case NB_CAT_CAT02:
		cone = NB_CAT02;
		break;
	case NB_CAT_CAT16:
		cone = NB_CAT16;
		break;
	case NB_CAT_VON_KRIES:
		cone = NB_HPE;
		break;
	case NB_CAT_DIAGONAL: {
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				out[i * 3 + j] = (i == j) ? dst_white_norm[i] / fmax(src_white_norm[i], EPS) : 0.0;
		return;
	}
	default:
		break;
	}

	double cone_inv[9];
	if (!nb_mat3_inv(cone, cone_inv)) {
		for (int i = 0; i < 9; i++)
			out[i] = (i % 4 == 0) ? 1.0 : 0.0;
		return;
	}

	double src_cone[3], dst_cone[3];
	nb_mat3_apply(cone, src_white_norm, src_cone);
	nb_mat3_apply(cone, dst_white_norm, dst_cone);

	double d[9] = {0};
	for (int i = 0; i < 3; i++)
		d[i * 3 + i] = dst_cone[i] / fmax(src_cone[i], EPS);

	double tmp[9];
	nb_mat3_mul(d, cone, tmp);
	nb_mat3_mul(cone_inv, tmp, out);
}

void nb_linrgb_to_lab(const double rgb_lin[3], double lab[3])
{
	double xyz[3];
	nb_rgb2xyz(rgb_lin, xyz);

	const double fx = nb_lab_f(xyz[0] / NB_D65[0]);
	const double fy = nb_lab_f(xyz[1] / NB_D65[1]);
	const double fz = nb_lab_f(xyz[2] / NB_D65[2]);
	lab[0] = 116.0 * fy - 16.0;
	lab[1] = 500.0 * (fx - fy);
	lab[2] = 200.0 * (fy - fz);
}

/* CIEDE2000 (Sharma, Wu & Dalal 2005). */
double nb_delta_e_2000(const double lab1[3], const double lab2[3])
{
	const double L1 = lab1[0], a1 = lab1[1], b1 = lab1[2];
	const double L2 = lab2[0], a2 = lab2[1], b2 = lab2[2];
	const double C1 = sqrt(a1 * a1 + b1 * b1);
	const double C2 = sqrt(a2 * a2 + b2 * b2);
	const double Cb7 = pow((C1 + C2) / 2.0, 7.0);
	const double G = 0.5 * (1.0 - sqrt(Cb7 / (Cb7 + pow(25.0, 7.0))));
	const double a1p = (1.0 + G) * a1, a2p = (1.0 + G) * a2;
	const double C1p = sqrt(a1p * a1p + b1 * b1);
	const double C2p = sqrt(a2p * a2p + b2 * b2);
	double h1p = atan2(b1, a1p) * 180.0 / M_PI;
	double h2p = atan2(b2, a2p) * 180.0 / M_PI;
	if (h1p < 0)
		h1p += 360.0;
	if (h2p < 0)
		h2p += 360.0;

	const double dLp = L2 - L1;
	const double dCp = C2p - C1p;
	double dhp = 0.0;
	if (C1p * C2p > 0.0) {
		dhp = h2p - h1p;
		if (dhp > 180.0)
			dhp -= 360.0;
		else if (dhp < -180.0)
			dhp += 360.0;
	}
	const double dHp = 2.0 * sqrt(C1p * C2p) * sin(dhp * M_PI / 360.0);

	const double Lbp = (L1 + L2) / 2.0;
	const double Cbp = (C1p + C2p) / 2.0;
	double hbp = h1p + h2p;
	if (C1p * C2p > 0.0) {
		const double d = fabs(h1p - h2p);
		if (d <= 180.0)
			hbp = (h1p + h2p) / 2.0;
		else if (h1p + h2p < 360.0)
			hbp = (h1p + h2p + 360.0) / 2.0;
		else
			hbp = (h1p + h2p - 360.0) / 2.0;
	}

	const double T = 1.0 - 0.17 * cos((hbp - 30.0) * M_PI / 180.0) +
			 0.24 * cos(2.0 * hbp * M_PI / 180.0) + 0.32 * cos((3.0 * hbp + 6.0) * M_PI / 180.0) -
			 0.20 * cos((4.0 * hbp - 63.0) * M_PI / 180.0);
	const double dtheta = 30.0 * exp(-pow((hbp - 275.0) / 25.0, 2.0));
	const double Cbp7 = pow(Cbp, 7.0);
	const double Rc = 2.0 * sqrt(Cbp7 / (Cbp7 + pow(25.0, 7.0)));
	const double Sl = 1.0 + (0.015 * pow(Lbp - 50.0, 2.0)) / sqrt(20.0 + pow(Lbp - 50.0, 2.0));
	const double Sc = 1.0 + 0.045 * Cbp;
	const double Sh = 1.0 + 0.015 * Cbp * T;
	const double Rt = -sin(2.0 * dtheta * M_PI / 180.0) * Rc;

	const double dL = dLp / Sl, dC = dCp / Sc, dH = dHp / Sh;
	return sqrt(dL * dL + dC * dC + dH * dH + Rt * dC * dH);
}

/* ------------------------------------------------------------------------- */
/* Working-space transfer functions (CPU side, for LUT + diagnostics)        */
/* ------------------------------------------------------------------------- */

static double nb_decode_linear(const struct neutral_balance *f, double v)
{
	v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
	if (f->ws == NB_WS_BT709)
		return v <= 0.081 ? v / 4.5 : pow((v + 0.099) / 1.099, 1.0 / 0.45);
	return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static void nb_build_lut(struct neutral_balance *f)
{
	for (int i = 0; i < 256; i++)
		f->inv_lut[i] = (float)nb_decode_linear(f, i / 255.0);
}

static void nb_set_working_space(struct neutral_balance *f, enum gs_color_space cs)
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
		nb_build_lut(f);
		f->cat_dirty = true;
	}
	f->source_space = cs;
}

static const char *nb_ws_name(const struct neutral_balance *f)
{
	return f->ws == NB_WS_BT709 ? "linear BT.709" : "linear sRGB";
}

/* ------------------------------------------------------------------------- */
/* Correction model                                                          */
/* ------------------------------------------------------------------------- */

/* Rebuild cat_matrix from the measured illuminant and the target LCh. */
static void nb_rebuild_cat(struct neutral_balance *f)
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

	double rgb2xyz_then_adapt[9], full[9];
	nb_mat3_mul(adapt, NB_RGB2XYZ, rgb2xyz_then_adapt);
	nb_mat3_mul(NB_XYZ2RGB, rgb2xyz_then_adapt, full);

	/* Exposure term: the measured color's Y maps onto the target L (FR-20). */
	const double exposure = y_tgt / y_meas;
	for (int i = 0; i < 9; i++)
		f->cat_matrix[i] = (float)(full[i] * exposure);

	f->cat_dirty = false;
	f->has_match = false;
}

static void nb_set_measurement(struct neutral_balance *f, const double lin_rgb[3], bool smooth)
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
static void nb_measurement_from_sample(struct neutral_balance *f)
{
	double lin[3];
	for (int i = 0; i < 3; i++)
		lin[i] = nb_decode_linear(f, f->last_sample[i]);
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
/* Automatic estimation (Grey World / Shades of Grey, linear light)          */
/* ------------------------------------------------------------------------- */

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

	/* Grey World (Buchsbaum 1980) / Shades of Grey (Finlayson & Trezzi 2004)
	 * in LINEAR light (FR-09..FR-12): decode each 8-bit sample through the
	 * inverse EOTF LUT of the detected working space before accumulating. */
	const int n = NB_STATS_W * NB_STATS_H;
	double sum[3] = {0.0, 0.0, 0.0};
	for (uint32_t y = 0; y < NB_STATS_H; y++) {
		const uint8_t *row = data + (size_t)y * linesize;
		for (uint32_t x = 0; x < NB_STATS_W; x++) {
			const uint8_t *px = row + (size_t)x * 4;
			const float lr = f->inv_lut[px[0]];
			const float lg = f->inv_lut[px[1]];
			const float lb = f->inv_lut[px[2]];
			if (f->method == NB_METHOD_SHADES_OF_GREY) {
				sum[0] += pow(lr, f->shades_p);
				sum[1] += pow(lg, f->shades_p);
				sum[2] += pow(lb, f->shades_p);
			} else {
				sum[0] += lr;
				sum[1] += lg;
				sum[2] += lb;
			}
		}
	}
	gs_stagesurface_unmap(f->stats_surface);

	/* The per-channel estimate IS the illuminant color (FR-16): adapt it to
	 * the target with the CAT instead of equalizing means with raw gains. */
	double e[3];
	if (f->method == NB_METHOD_SHADES_OF_GREY) {
		float inv = 1.0f / fmaxf(f->shades_p, EPS);
		e[0] = pow((float)(sum[0] / n), inv);
		e[1] = pow((float)(sum[1] / n), inv);
		e[2] = pow((float)(sum[2] / n), inv);
	} else {
		e[0] = (float)(sum[0] / n);
		e[1] = (float)(sum[1] / n);
		e[2] = (float)(sum[2] / n);
	}

	nb_set_measurement(f, e, true);
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

	/* One-shot read-back of the *uncorrected* filter input in its native
	 * encoding (FR-25); linearization happens after the click. */
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

static bool nb_request_capture(struct neutral_balance *f, uint8_t **out_buf, uint32_t *out_w, uint32_t *out_h)
{
	f->capture_ready = false;
	os_event_reset(f->capture_done);
	f->capture_requested = true;

	/* Wait up to ~2 s; if the source is hidden or removed the filter never
	 * renders and we must not block the UI thread (NFR-02). */
	for (int i = 0; i < 40; i++) {
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
		lin[i] = nb_decode_linear(f, rgb[i]);
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
		snprintf(text + len - 1, sizeof(text) - len + 1, "%.3f", f->match_de);
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
	nb_build_lut(f);
	f->cat_dirty = true;

	if (!nb_mat3_inv(NB_RGB2XYZ, NB_XYZ2RGB)) {
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
