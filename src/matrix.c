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

#include "matrix.h"

#include <math.h>

void nb_mat3_mul(const double a[9], const double b[9], double out[9])
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			out[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}

bool nb_mat3_inv(const double m[9], double out[9])
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

void nb_mat3_apply(const double m[9], const double v[3], double out[3])
{
	for (int r = 0; r < 3; r++)
		out[r] = m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
}