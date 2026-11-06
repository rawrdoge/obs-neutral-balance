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

#include <stdint.h>

#include "neutral-balance-filter.h"

/* Stages the pending capture request (render thread only): called from
 * video_render when f->capture_requested is set. Consumes the flag. */
void nb_do_capture(struct neutral_balance *f, gs_texture_t *input, uint32_t w, uint32_t h);

/* Blocks the UI thread until the render thread has produced one frame, then
 * hands over an owned copy of it. Returns false on timeout (source hidden or
 * removed - NFR-02) - the caller must bfree() *out_buf on success. */
bool nb_request_capture(struct neutral_balance *f, uint8_t **out_buf, uint32_t *out_w, uint32_t *out_h);