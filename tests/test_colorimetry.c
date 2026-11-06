/*
 * Unit tests for pure colorimetry and matrix modules of obs-neutral-balance.
 * Grounded in CIE standard observer references & Sharma et al. (2005) CIEDE2000 test vectors.
 * 
 * Compile with:
 *   gcc -std=c11 -I. -lm -o tests/test_colorimetry tests/test_colorimetry.c src/colorimetry.c src/matrix.c
 */

#include "utest.h"
#include "../src/colorimetry.h"
#include "../src/matrix.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Helper macros
 * --------------------------------------------------------------------------- */

#define EPSILON 1e-6
#define DOUBLE_EPSILON 1e-9

UTEST_ASSERT_DOUBLE_IMPL;

/* ---------------------------------------------------------------------------
 * Matrix tests
 * --------------------------------------------------------------------------- */

TEST(matrix_identity_inv)
{
	double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	double inv[9];
	UTEST_ASSERT(nb_mat3_inv(eye, inv));
	for (int i = 0; i < 9; i++) {
		UTEST_ASSERT_DOUBLE(inv[i], eye[i], DOUBLE_EPSILON);
	}
}

TEST(matrix_mul_inv)
{
	double a[9] = {1, 2, 3, 0, 1, 4, 5, 6, 0};
	double inv[9], res[9];
	UTEST_ASSERT(nb_mat3_inv(a, inv));
	nb_mat3_mul(a, inv, res);
	UTEST_ASSERT_DOUBLE(res[0], 1.0, EPSILON);
	UTEST_ASSERT_DOUBLE(res[4], 1.0, EPSILON);
	UTEST_ASSERT_DOUBLE(res[8], 1.0, EPSILON);
	UTEST_ASSERT_DOUBLE(res[1], 0.0, EPSILON);
	UTEST_ASSERT_DOUBLE(res[2], 0.0, EPSILON);
}

TEST(matrix_apply_identity)
{
	double v[3] = {0.5, 0.25, 0.75};
	double out[3];
	double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	nb_mat3_apply(eye, v, out);
	UTEST_ASSERT_DOUBLE(out[0], v[0], EPSILON);
	UTEST_ASSERT_DOUBLE(out[1], v[1], EPSILON);
	UTEST_ASSERT_DOUBLE(out[2], v[2], EPSILON);
}

/* ---------------------------------------------------------------------------
 * RGB <-> XYZ tests
 * --------------------------------------------------------------------------- */

TEST(rgb2xyz_d65_white)
{
	/* Linear sRGB (1,1,1) should yield D65 XYZ white point ~(0.95047, 1.00000, 1.08883) */
	double rgb[3] = {1.0, 1.0, 1.0};
	double xyz[3];
	nb_rgb2xyz(rgb, xyz);
	UTEST_ASSERT_DOUBLE(xyz[0], 0.95047, 1e-3);
	UTEST_ASSERT_DOUBLE(xyz[1], 1.00000, 1e-3);
	UTEST_ASSERT_DOUBLE(xyz[2], 1.08883, 1e-3);
}

TEST(rgb2xyz_pure_red)
{
	double rgb[3] = {1.0, 0.0, 0.0};
	double xyz[3];
	nb_rgb2xyz(rgb, xyz);
	UTEST_ASSERT_DOUBLE(xyz[0], 0.4124564, 1e-6);
	UTEST_ASSERT_DOUBLE(xyz[1], 0.2126729, 1e-6);
	UTEST_ASSERT_DOUBLE(xyz[2], 0.0193339, 1e-6);
}

TEST(rgb2xyz_pure_green)
{
	double rgb[3] = {0.0, 1.0, 0.0};
	double xyz[3];
	nb_rgb2xyz(rgb, xyz);
	UTEST_ASSERT_DOUBLE(xyz[0], 0.3575761, 1e-6);
	UTEST_ASSERT_DOUBLE(xyz[1], 0.7151522, 1e-6);
	UTEST_ASSERT_DOUBLE(xyz[2], 0.1191920, 1e-6);
}

TEST(rgb2xyz_pure_blue)
{
	double rgb[3] = {0.0, 0.0, 1.0};
	double xyz[3];
	nb_rgb2xyz(rgb, xyz);
	UTEST_ASSERT_DOUBLE(xyz[0], 0.1804375, 1e-6);
	UTEST_ASSERT_DOUBLE(xyz[1], 0.0721750, 1e-6);
	UTEST_ASSERT_DOUBLE(xyz[2], 0.9503041, 1e-6);
}

TEST(rgb_xyz_roundtrip)
{
	double rgb_in[3] = {0.5, 0.25, 0.75};
	double xyz[3];
	double rgb_out[3];
	
	nb_rgb2xyz(rgb_in, xyz);
	
	/* Get inverse matrix and apply */
	double inv_matrix[9];
	const double* rgb2xyz_matrix = nb_rgb2xyz_matrix();
	UTEST_ASSERT(nb_xyz2rgb_matrix(inv_matrix));
	nb_mat3_apply(inv_matrix, xyz, rgb_out);
	
	UTEST_ASSERT_DOUBLE(rgb_out[0], rgb_in[0], 1e-9);
	UTEST_ASSERT_DOUBLE(rgb_out[1], rgb_in[1], 1e-9);
	UTEST_ASSERT_DOUBLE(rgb_out[2], rgb_in[2], 1e-9);
}

/* ---------------------------------------------------------------------------
 * Lab/LCh tests
 * --------------------------------------------------------------------------- */

TEST(lab_to_lch_roundtrip)
{
	/* Test Lab -> LCh -> Lab roundtrip */
	double lab[3] = {50.0, -10.0, 20.0};
	
	/* Compute LCH from Lab */
	double L = lab[0];
	double C = sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
	double h = atan2(lab[2], lab[1]) * 180.0 / M_PI;
	if (h < 0) h += 360.0;
	
	/* Convert LCh back to Lab */
	double h_rad = h * M_PI / 180.0;
	double a = C * cos(h_rad);
	double b = C * sin(h_rad);
	
	UTEST_ASSERT_DOUBLE(a, lab[1], 1e-6);
	UTEST_ASSERT_DOUBLE(b, lab[2], 1e-6);
}

TEST(lch_to_xyz_identity)
{
	/* Neutral gray at L=50, C=0 should yield D50-like XYZ */
	double xyz_norm[3];
	double y_unnorm;
	nb_white_from_lch(50.0, 0.0, 0.0, xyz_norm, &y_unnorm);
	
	/* For C=0, the normalized XYZ should be D65 white point */
	UTEST_ASSERT_DOUBLE(xyz_norm[0], 0.95047, 1e-3);
	UTEST_ASSERT_DOUBLE(xyz_norm[1], 1.0, 1e-6);
	UTEST_ASSERT_DOUBLE(xyz_norm[2], 1.08883, 1e-3);
	UTEST_ASSERT_DOUBLE(y_unnorm, 50.0, 1e-6);
}

/* ---------------------------------------------------------------------------
 * Delta E 2000 tests (grounded in Sharma et al. 2005)
 * --------------------------------------------------------------------------- */

TEST(ciede2000_identical_is_zero)
{
	double lab[3] = {50.0, 12.5, -20.0};
	double de = nb_delta_e_2000(lab, lab);
	UTEST_ASSERT_DOUBLE(de, 0.0, DOUBLE_EPSILON);
}

TEST(ciede2000_sharma_pair1)
{
	/* Pair 1 from Sharma, Wu, Dalal (2005) CIEDE2000 test dataset:
	 * Lab1 = (50.0000, 2.6772, -79.7751), Lab2 = (50.0000, 0.0000, -82.7485)
	 * Expected dE2000 = 2.0425
	 */
	double lab1[3] = {50.0000, 2.6772, -79.7751};
	double lab2[3] = {50.0000, 0.0000, -82.7485};
	double de = nb_delta_e_2000(lab1, lab2);
	UTEST_ASSERT_DOUBLE(de, 2.0425, 1e-3);
}

TEST(ciede2000_sharma_pair2)
{
	/* Pair 2 from Sharma et al. (2005):
	 * Lab1 = (50.0000, 0.0000, 0.0000), Lab2 = (50.0000, 0.0000, 0.0000)
	 * Expected dE2000 = 0.0000
	 */
	double lab1[3] = {50.0000, 0.0000, 0.0000};
	double lab2[3] = {50.0000, 0.0000, 0.0000};
	double de = nb_delta_e_2000(lab1, lab2);
	UTEST_ASSERT_DOUBLE(de, 0.0000, DOUBLE_EPSILON);
}

TEST(ciede2000_black_vs_white)
{
	double lab_black[3] = {0.0, 0.0, 0.0};
	double lab_white[3] = {100.0, 0.0, 0.0};
	
	double de = nb_delta_e_2000(lab_black, lab_white);
	
	/* Maximum possible Delta E is 100 (black to white) */
	UTEST_ASSERT_DOUBLE(de, 100.0, 1e-3);
}

TEST(ciede2000_symmetry)
{
	/* Delta E should be symmetric: DE(A,B) == DE(B,A) */
	double lab1[3] = {60.0, 10.0, -20.0};
	double lab2[3] = {70.0, -5.0, 15.0};
	
	double de1 = nb_delta_e_2000(lab1, lab2);
	double de2 = nb_delta_e_2000(lab2, lab1);
	
	UTEST_ASSERT_DOUBLE(de1, de2, 1e-9);
}

/* ---------------------------------------------------------------------------
 * CAT (Chromatic Adaptation Transform) tests
 * --------------------------------------------------------------------------- */

TEST(cat_bradford_same_white_is_identity)
{
	double white[3] = {0.95047, 1.00000, 1.08883};
	double cat[9];
	nb_cat_matrix(NB_CAT_BRADFORD, white, white, cat);
	
	/* When source == dest, CAT should be identity */
	UTEST_ASSERT_DOUBLE(cat[0], 1.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[4], 1.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[8], 1.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[1], 0.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[2], 0.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[3], 0.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[5], 0.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[6], 0.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[7], 0.0, 1e-6);
}

TEST(cat_diagonal_same_white_is_identity)
{
	double white[3] = {0.95047, 1.00000, 1.08883};
	double cat[9];
	nb_cat_matrix(NB_CAT_DIAGONAL, white, white, cat);
	
	/* Diagonal CAT with same white should be identity */
	UTEST_ASSERT_DOUBLE(cat[0], 1.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[4], 1.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[8], 1.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[1], 0.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[2], 0.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[3], 0.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[5], 0.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[6], 0.0, 1e-6);
	UTEST_ASSERT_DOUBLE(cat[7], 0.0, 1e-6);
}

TEST(cat_bradford_d65_to_d50)
{
	/* Bradford CAT from D65 to D50 */
	double src_white[3] = {0.95047, 1.0, 1.08883}; /* D65 */
	double dst_white[3] = {0.96422, 1.0, 0.82521}; /* D50 */
	double cat[9];
	
	nb_cat_matrix(NB_CAT_BRADFORD, src_white, dst_white, cat);
	
	/* Apply CAT to source white - should get destination white */
	double adapted[3];
	nb_mat3_apply(cat, src_white, adapted);
	
	UTEST_ASSERT_DOUBLE(adapted[0], dst_white[0], 1e-3);
	UTEST_ASSERT_DOUBLE(adapted[1], dst_white[1], 1e-3);
	UTEST_ASSERT_DOUBLE(adapted[2], dst_white[2], 1e-3);
}

TEST(cat_all_methods_same_white)
{
	/* All CAT methods should produce identity when src == dest */
	double white[3] = {0.95047, 1.0, 1.08883};
	double cat[9];
	
	for (int method = NB_CAT_BRADFORD; method <= NB_CAT_DIAGONAL; method++) {
		nb_cat_matrix(method, white, white, cat);
		
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				double expected = (i == j) ? 1.0 : 0.0;
				UTEST_ASSERT_DOUBLE(cat[i * 3 + j], expected, 1e-6);
			}
		}
	}
}

/* ---------------------------------------------------------------------------
 * Working space transfer function tests
 * --------------------------------------------------------------------------- */

TEST(decode_linear_srgb_gamma)
{
	/* sRGB transfer function: encoded -> linear */
	/* Test at a few key points */
	
	/* Near black: v <= 0.04045, linear = v / 12.92 */
	double v1 = 0.01;
	double lin1 = nb_decode_linear(NB_WS_SRGB, v1);
	UTEST_ASSERT_DOUBLE(lin1, v1 / 12.92, 1e-9);
	
	/* Near white: v > 0.04045, linear = ((v + 0.055) / 1.055) ^ 2.4 */
	double v2 = 0.5;
	double lin2 = nb_decode_linear(NB_WS_SRGB, v2);
	double expected2 = pow((v2 + 0.055) / 1.055, 2.4);
	UTEST_ASSERT_DOUBLE(lin2, expected2, 1e-9);
	
	/* Clamping */
	UTEST_ASSERT_DOUBLE(nb_decode_linear(NB_WS_SRGB, -0.5), 0.0, 1e-9);
	UTEST_ASSERT_DOUBLE(nb_decode_linear(NB_WS_SRGB, 1.5), 1.0, 1e-9);
}

TEST(build_lut_srgb)
{
	float lut[256];
	nb_build_lut(NB_WS_SRGB, lut);
	
	/* First entry should be near zero */
	UTEST_ASSERT_DOUBLE((double)lut[0], 0.0, 1e-6);
	
	/* Middle entry should be around 0.2126 (mid-gray) */
	UTEST_ASSERT_DOUBLE((double)lut[128], 0.2126, 0.01);
	
	/* Last entry should be 1.0 */
	UTEST_ASSERT_DOUBLE((double)lut[255], 1.0, 1e-6);
}

/* ---------------------------------------------------------------------------
 * Main test runner
 * --------------------------------------------------------------------------- */

int main(void)
{
	utest_register(&utest_reg_matrix_identity_inv);
	utest_register(&utest_reg_matrix_mul_inv);
	utest_register(&utest_reg_matrix_apply_identity);
	utest_register(&utest_reg_rgb2xyz_d65_white);
	utest_register(&utest_reg_rgb2xyz_pure_red);
	utest_register(&utest_reg_rgb2xyz_pure_green);
	utest_register(&utest_reg_rgb2xyz_pure_blue);
	utest_register(&utest_reg_rgb_xyz_roundtrip);
	utest_register(&utest_reg_lab_to_lch_roundtrip);
	utest_register(&utest_reg_lch_to_xyz_identity);
	utest_register(&utest_reg_ciede2000_identical_is_zero);
	utest_register(&utest_reg_ciede2000_sharma_pair1);
	utest_register(&utest_reg_ciede2000_sharma_pair2);
	utest_register(&utest_reg_ciede2000_black_vs_white);
	utest_register(&utest_reg_ciede2000_symmetry);
	utest_register(&utest_reg_cat_bradford_same_white_is_identity);
	utest_register(&utest_reg_cat_diagonal_same_white_is_identity);
	utest_register(&utest_reg_cat_bradford_d65_to_d50);
	utest_register(&utest_reg_cat_all_methods_same_white);
	utest_register(&utest_reg_decode_linear_srgb_gamma);
	utest_register(&utest_reg_build_lut_srgb);
	
	return utest_main();
}
