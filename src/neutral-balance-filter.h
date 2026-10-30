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
 * v2 (OBS-NB-PRD-002): format-aware, linear-light, CAT-based white balance.
 *
 * Pipeline per frame (single-pass shader, three ordered stages):
 *   [stage 1] range is already full-range RGB (OBS converts YUV on-GPU per
 *             FR-07); inverse EOTF of the detected working space
 *   [stage 2] 3x3 chromatic-adaptation matrix (float3 rows) computed on the
 *             CPU from the measured illuminant XYZ and the target LCh white
 *   [stage 3] forward EOTF back to the source encoding
 *
 * The measured illuminant comes from Grey World / Shades of Grey statistics
 * (linear light, FR-09..FR-13) or the eyedropper (FR-25: sampled in the
 * native encoding, linearized before solving). Statistics and the eyedropper
 * run on an RGBA8 read-back; 8-bit samples are linearized through a 256-entry
 * LUT. The CPU-side colorimetry (LCh -> XYZ, CAT, DeltaE00) runs in double
 * precision (NFR-04).
 */

#pragma once

#include <obs.h>
#include <obs-source.h>
#include <graphics/graphics.h>
#include <graphics/vec3.h>
#include <util/platform.h>
#include <util/threading.h>

enum nb_method {
	NB_METHOD_GREY_WORLD = 0,
	NB_METHOD_SHADES_OF_GREY = 1,
	NB_METHOD_MANUAL = 2,
};

enum nb_cat_method {
	NB_CAT_BRADFORD = 0,
	NB_CAT_CAT02 = 1,
	NB_CAT_CAT16 = 2,
	NB_CAT_VON_KRIES = 3, /* HPE cone space */
	NB_CAT_DIAGONAL = 4,  /* identity cone matrix (legacy comparison, FR-18) */
};

enum nb_working_space {
	NB_WS_SRGB = 0,
	NB_WS_BT709 = 1,
};

#define NB_STATS_W 160
#define NB_STATS_H 90
#define NB_STATS_INTERVAL_NS (100 * 1000 * 1000) /* 10 Hz */
#define NB_SMOOTH_ALPHA 0.2f                     /* FR-13 */

struct neutral_balance {
	obs_source_t *context;

	/* GPU resources - render thread only */
	gs_effect_t *effect;
	gs_eparam_t *param_image;
	gs_eparam_t *param_cat0; /* float3 rows of the 3x3 CAT matrix */
	gs_eparam_t *param_cat1;
	gs_eparam_t *param_cat2;
	gs_eparam_t *param_ws;    /* 0 = sRGB transfer, 1 = BT.709 transfer */
	gs_eparam_t *param_split; /* > 0: left fraction shows the input */
	gs_texrender_t *input_render;
	bool logged_begin_fail;

	/* statistics pipeline - render thread only */
	gs_texrender_t *stats_render;
	gs_stagesurf_t *stats_surface;
	uint64_t last_stats_ts;

	/* eyedropper capture - render thread only */
	gs_stagesurf_t *capture_surface;
	uint32_t capture_w, capture_h;
	uint8_t *capture_buf;
	volatile bool capture_requested;
	volatile bool capture_ready;
	os_event_t *capture_done;

	/* settings */
	int method; /* measurement source */
	float shades_p;
	int cat_method;
	float target_hue;       /* CIE LCh h, degrees */
	float target_luminance; /* CIE LCh L, 0-100   */
	float target_chroma;    /* CIE LCh C, 0-100   */
	long sample_radius;
	bool show_split;
	float last_sample[3]; /* measured color, working-space encoding, 0-1 */
	bool has_sample;

	/* detected format (FR-01..FR-04) */
	enum gs_color_space source_space;
	enum nb_working_space ws;
	bool logged_space_fallback;
	float inv_lut[256]; /* encoded byte -> linear, per working space */

	/* correction model */
	float measured_lin[3]; /* linear illuminant estimate (temporally smoothed) */
	bool has_measurement;
	float cat_matrix[9]; /* linear RGB -> linear RGB, incl. exposure term */
	bool cat_dirty;

	/* diagnostics (FR-27, FR-28) */
	double match_de;
	bool has_match;
};

extern struct obs_source_info neutral_balance_filter_info;

/* CPU-side colorimetry (double precision) exposed for tests/diagnostics. */
void nb_rgb2xyz(const double rgb[3], double xyz[3]);
void nb_white_from_lch(double L, double C, double h_deg, double xyz_norm[3], double *y_unnorm);
void nb_cat_matrix(int cat_method, const double src_white_norm[3], const double dst_white_norm[3], double out[9]);
double nb_delta_e_2000(const double lab1[3], const double lab2[3]);
void nb_linrgb_to_lab(const double rgb_lin[3], double lab[3]);
