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
 * Eyedropper frame capture: the UI thread sets capture_requested and waits;
 * the render thread stages the *uncorrected* filter input, copies it into
 * capture_buf, and signals capture_done. Only public libobs primitives and
 * the volatile flag / event pair coordinate the two threads.
 */

#include <string.h>

#include <obs-module.h>
#include <util/bmem.h>

#include "capture.h"

void nb_do_capture(struct neutral_balance *f, gs_texture_t *input, uint32_t w, uint32_t h)
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

bool nb_request_capture(struct neutral_balance *f, uint8_t **out_buf, uint32_t *out_w, uint32_t *out_h)
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