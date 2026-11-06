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
 * Automatic illuminant estimation (FR-09..FR-13). Renders the filter input to
 * a small RGBA8 read-back at NB_STATS_INTERVAL_NS, linearizes each byte with
 * the working-space LUT, accumulates Grey World or Shades of Grey statistics,
 * and feeds the result into the temporally-smoothed measurement model.
 * All functions here run on the render thread only.
 */

#include <math.h>

#include <obs-module.h>

#include "neutral-balance-filter.h"

#define EPS 1e-6

void nb_update_stats(struct neutral_balance *f, gs_texture_t *input)
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

	/* Grey World (Buchsbaum 1980) / Shades of Grey (Finlayson & Trezzi
	 * 2004) in LINEAR light (FR-09..FR-12): decode each 8-bit sample
	 * through the inverse EOTF LUT of the detected working space before
	 * accumulating. */
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

	/* The per-channel estimate IS the illuminant color (FR-16): adapt it
	 * to the target with the CAT instead of equalizing means with raw
	 * gains. */
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