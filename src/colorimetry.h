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

/*
 * Pure CPU-side colorimetry (double precision, per NFR-04), the working-space
 * transfer functions, and the enum vocabulary. This module has no libobs
 * includes so it can be unit-tested outside the plugin.
 */

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

/* Linear RGB -> CIE XYZ (sRGB / BT.709 primaries, D65 white). */
void nb_rgb2xyz(const double rgb[3], double xyz[3]);

/* The row-major 3x3 RGB -> XYZ matrix itself (for composing transforms). */
const double *nb_rgb2xyz_matrix(void);

/* True once the RGB -> XYZ matrix is verified invertible (early init check). */
bool nb_colorimetry_init(void);

/* XYZ -> linear RGB matrix, or identity when singular (false). */
bool nb_xyz2rgb_matrix(double out[9]);

/* CIE LCh (L 0-100, C 0-100+, h degrees) -> Y-normalized XYZ.
 * *y_unnorm receives the unnormalized Y of the requested color. */
void nb_white_from_lch(double L, double C, double h_deg, double xyz_norm[3], double *y_unnorm);

/* 3x3 chromatic-adaptation matrix in XYZ mapping src_white -> dst_white
 * (both Y-normalized). Diagonal mode uses the identity cone matrix (FR-18). */
void nb_cat_matrix(int cat_method, const double src_white_norm[3], const double dst_white_norm[3], double out[9]);

void nb_linrgb_to_lab(const double rgb_lin[3], double lab[3]);

/* CIEDE2000 (Sharma, Wu & Dalal 2005). */
double nb_delta_e_2000(const double lab1[3], const double lab2[3]);

/* Working-space inverse EOTF: encoded (0-1) -> linear, clamped. */
double nb_decode_linear(enum nb_working_space ws, double v);

/* Encoded byte -> linear lookup table for the working space (FR-09..FR-12). */
void nb_build_lut(enum nb_working_space ws, float lut[256]);