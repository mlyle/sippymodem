// SPDX-License-Identifier: BSD-3-Clause

#include "gram_fit.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_symmetric_moments(void)
{
	enum { COLUMNS = 14, MOMENTS = COLUMNS * (COLUMNS + 1) / 2 };
	double packed[MOMENTS] = { 0 }, dense[COLUMNS][COLUMNS] = { { 0 } };
	bool used[MOMENTS] = { false };

	for (unsigned i = 0; i < COLUMNS; i++)
		for (unsigned j = 0; j <= i; j++) {
			unsigned index = gram_moment_index(i, j);
			assert(index < MOMENTS && !used[index]);
			used[index] = true;
			assert(gram_moment_index(j, i) == index);
		}
	for (unsigned i = 0; i < MOMENTS; i++) assert(used[i]);
	/* Include DC, correlated delayed features, both signs and full-scale
	 * products. Reconstruction must preserve every dense normal equation. */
	for (unsigned n = 0; n < 1024; n++) {
		double row[COLUMNS];
		for (unsigned i = 0; i + 1 < COLUMNS; i++)
			row[i] = (int)((n * 12347u + i * 31337u) & 65535u) - 32768;
		row[COLUMNS - 1] = 1.;
		for (unsigned i = 0; i < COLUMNS; i++) {
			for (unsigned j = 0; j < COLUMNS; j++) dense[i][j] += row[i] * row[j];
			for (unsigned j = 0; j <= i; j++) packed[gram_moment_index(i, j)] += row[i] * row[j];
		}
	}
	for (unsigned i = 0; i < COLUMNS; i++)
		for (unsigned j = 0; j < COLUMNS; j++)
			assert(packed[gram_moment_index(i, j)] == dense[i][j]);
}

/* Fit a polynomial from independent samples, with widely different column
 * units. Truncating the degree exercises every supported system size. */
static void test_polynomial(unsigned count)
{
	const double expected[4] = { -.5, 2., -.25, 3. };
	const double units[4] = { .001, 1000., .1, 10. };
	double gram[4][4] = { { 0 } }, rhs[4] = { 0 };
	double fitted[4] = { 99., 99., 99., 99. };

	for (int n = -8; n <= 8; n++) {
		double x[4], power = 1., y = 0.;
		for (unsigned i = 0; i < count; i++) {
			x[i] = power * units[i];
			y += expected[i] * x[i];
			power *= n / 8.;
		}
		for (unsigned i = 0; i < count; i++) {
			rhs[i] += x[i] * y;
			for (unsigned j = 0; j < count; j++) gram[i][j] += x[i] * x[j];
		}
	}
	double saved_gram[4][4], saved_rhs[4];
	memcpy(saved_gram, gram, sizeof(gram));
	memcpy(saved_rhs, rhs, sizeof(rhs));
	assert(gram_fit(gram, rhs, count, fitted));
	assert(memcmp(saved_gram, gram, sizeof(gram)) == 0);
	assert(memcmp(saved_rhs, rhs, sizeof(rhs)) == 0);
	for (unsigned i = 0; i < count; i++) assert(fabs(fitted[i] - expected[i]) < 1e-7);
	for (unsigned i = count; i < 4; i++) assert(fitted[i] == 99.);
}

static void test_rejection(void)
{
	double gram[4][4] = { { 1., 1. }, { 1., 1. } };
	double rhs[4] = { 1., 1. }, fitted[4];

	/* Duplicate columns, then nearly dependent columns on either side of
	 * the normalized determinant limit. Global amplitude must not decide. */
	assert(!gram_fit(gram, rhs, 2, fitted));
	for (unsigned scale = 0; scale < 2; scale++) {
		double amplitude = scale ? 1e12 : 1e-12;
		gram[0][0] = gram[1][1] = amplitude;
		gram[0][1] = gram[1][0] = amplitude * (1. - 4. * DBL_EPSILON);
		assert(!gram_fit(gram, rhs, 2, fitted));
		gram[0][1] = gram[1][0] = amplitude * (1. - 256. * DBL_EPSILON);
		assert(gram_fit(gram, rhs, 2, fitted));
	}
	gram[0][0] = 0.;
	assert(!gram_fit(gram, rhs, 1, fitted));
	gram[0][0] = -1.;
	assert(!gram_fit(gram, rhs, 1, fitted));
	gram[0][0] = NAN;
	assert(!gram_fit(gram, rhs, 1, fitted));
	gram[0][0] = 1.;
	rhs[0] = INFINITY;
	assert(!gram_fit(gram, rhs, 1, fitted));
	assert(!gram_fit(gram, rhs, 0, fitted));
	assert(!gram_fit(gram, rhs, 5, fitted));
}

static void test_elimination(void)
{
	double matrix[4][5] = {
		{ 4., 2., 0., 8., 99. },
		{ 2., 5., 1., 13., 99. },
		{ 0., 1., 3., 5., 99. },
		{ 99., 99., 99., 99., 99. },
	};
	double determinant = 99.;

	assert(gram_eliminate(matrix, 3, 0., &determinant));
	assert(determinant == 44.);
	assert(matrix[0][3] == 1. && matrix[1][3] == 2. && matrix[2][3] == 1.);
	for (unsigned i = 0; i < 4; i++) assert(matrix[i][4] == 99.);
	for (unsigned i = 0; i < 5; i++) assert(matrix[3][i] == 99.);

	/* The channel designer's absolute pivot floor is intentionally different
	 * from the normalized fit's determinant limit and prefix fit's zero floor. */
	matrix[0][0] = 1e-8;
	matrix[0][1] = 2e-8;
	determinant = 99.;
	assert(!gram_eliminate(matrix, 1, 1e-8, &determinant));
	assert(determinant == 99.);
	assert(gram_eliminate(matrix, 1, 0., NULL) && matrix[0][1] == 2.);
	matrix[0][0] = nextafter(1e-8, INFINITY);
	matrix[0][1] = matrix[0][0] * 2.;
	assert(gram_eliminate(matrix, 1, 1e-8, NULL) && matrix[0][1] == 2.);
	matrix[0][0] = 0.;
	assert(!gram_eliminate(matrix, 1, 0., &determinant) && determinant == 99.);
	matrix[0][0] = NAN;
	assert(!gram_eliminate(matrix, 1, 0., NULL));
	assert(!gram_eliminate(matrix, 0, 0., NULL));
	assert(!gram_eliminate(matrix, 5, 0., NULL));
	assert(!gram_eliminate(matrix, 1, -1., NULL));
	assert(!gram_eliminate(matrix, 1, NAN, NULL));
}

int main(void)
{
	test_symmetric_moments();
	for (unsigned count = 1; count <= 4; count++) test_polynomial(count);
	test_rejection();
	test_elimination();
	puts("Gram fitting tests passed");
	return 0;
}
