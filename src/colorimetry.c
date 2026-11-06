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

#include "colorimetry.h"
#include "matrix.h"

#include <math.h>

#define EPS 1e-6

/* ------------------------------------------------------------------------- */
/* Colorimetry (double precision on the CPU, per NFR-04)                     */
/* ------------------------------------------------------------------------- */

/* sRGB / BT.709 primaries, D65 white (both working spaces share these). */
static const double NB_RGB2XYZ[9] = {0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522,
				     0.0721750, 0.0193339, 0.1191920, 0.9503041};

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

const double *nb_rgb2xyz_matrix(void)
{
	return NB_RGB2XYZ;
}

bool nb_colorimetry_init(void)
{
	double out[9];
	return nb_mat3_inv(NB_RGB2XYZ, out);
}

bool nb_xyz2rgb_matrix(double out[9])
{
	return nb_mat3_inv(NB_RGB2XYZ, out);
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

double nb_decode_linear(enum nb_working_space ws, double v)
{
	v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
	if (ws == NB_WS_BT709)
		return v <= 0.081 ? v / 4.5 : pow((v + 0.099) / 1.099, 1.0 / 0.45);
	return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

void nb_build_lut(enum nb_working_space ws, float lut[256])
{
	for (int i = 0; i < 256; i++)
		lut[i] = (float)nb_decode_linear(ws, i / 255.0);
}