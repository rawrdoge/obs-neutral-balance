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

#pragma once

#include <stdatomic.h>

#include <obs.h>
#include <obs-source.h>
#include <graphics/graphics.h>
#include <graphics/vec3.h>
#include <util/platform.h>

enum nb_method {
	NB_METHOD_GREY_WORLD = 0,
	NB_METHOD_SHADES_OF_GREY = 1,
	NB_METHOD_MANUAL = 2,
};

/* Statistics are gathered from a small downscaled copy of the frame; this
 * keeps CPU-side reads cheap and well under the PRD performance budget. */
#define NB_STATS_W 160
#define NB_STATS_H 90
#define NB_STATS_INTERVAL_NS (100 * 1000 * 1000) /* 10 Hz */

struct neutral_balance {
	obs_source_t *context;

	/* GPU resources - render thread only */
	gs_effect_t *effect;
	gs_eparam_t *param_image;
	gs_eparam_t *param_gains;
	gs_eparam_t *param_hue_bias;
	gs_eparam_t *param_lum_bias;
	gs_texrender_t *input_render;

	/* automatic estimation pipeline - render thread only */
	gs_texrender_t *stats_render;
	gs_stagesurf_t *stats_surface;
	uint64_t last_stats_ts;

	/* eyedropper capture pipeline - render thread only */
	gs_stagesurf_t *capture_surface;
	uint32_t capture_w, capture_h;
	uint8_t *capture_buf; /* owned, freed in destroy */
	atomic_bool capture_requested;
	volatile bool capture_ready;
	os_event_t *capture_done;

	/* settings (mirror obs_data_t, see nb_update) */
	int method;
	float shades_p;
	float gains[3];
	float target_hue;       /* degrees, 0..360 */
	float target_luminance; /* 0..1 */
	long sample_radius;
	float last_sample[3];
	bool has_sample;
};

extern struct obs_source_info neutral_balance_filter_info;

void nb_compute_manual_gains(struct neutral_balance *f, float r, float g, float b);
