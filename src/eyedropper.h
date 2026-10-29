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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true when the plugin was built with the Qt sampling dialog. */
bool nb_eyedropper_available(void);

/* Shows the frozen-frame sampling dialog.
 *
 * data     - RGBA8 frame buffer (row-major, width*4 bytes per row)
 * width    - frame width in pixels
 * height   - frame height in pixels
 * radius   - sampling radius in pixels (average over (2r+1)^2)
 * out_rgb  - receives the averaged normalized color (0..1) on accept
 *
 * Returns true when the user accepted a sample. */
bool nb_eyedropper_sample_rgba(const uint8_t *data, uint32_t width, uint32_t height, int radius, float out_rgb[3]);

#ifdef __cplusplus
}
#endif
