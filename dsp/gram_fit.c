// SPDX-License-Identifier: BSD-3-Clause

#include "gram_fit.h"

/* Reduce owned dense scratch after the caller validates count (1..4) and
 * the finite, nonnegative pivot floor. Preserve arithmetic order and publish
 * the optional determinant only on success. This is shared internally by
 * packed raw solves and direct construction of normalized systems. */
static bool gram_reduce(
	double matrix[GRAM_FIT_COLUMNS][GRAM_FIT_COLUMNS + 1],
	unsigned count, double minimum_pivot, double *determinant)
{
	double product = 1.;

	for (unsigned i = 0; i < count; i++) {
		double pivot = matrix[i][i];
		if (!(pivot > minimum_pivot)) return false;
		if (determinant) product *= pivot;
		for (unsigned k = i; k <= count; k++) matrix[i][k] /= pivot;
		for (unsigned j = 0; j < count; j++) if (j != i) {
			double factor = matrix[j][i];
			for (unsigned k = i; k <= count; k++) matrix[j][k] -= factor * matrix[i][k];
		}
	}
	if (determinant) *determinant = product;
	return true;
}

/* Solve a packed symmetric system without changing its moments or RHS.
 * Arrays cover count coefficients and count * (count + 1) / 2 moments.
 * Keep the owner's pivot floor and arithmetic order; normalization and
 * numeric admission stay outside. Failure writes neither fitted coefficients
 * nor the optional determinant. Only active input/output entries are used;
 * inputs and outputs must be separate. */
bool gram_solve(
	const double *gram, const double *rhs, unsigned count,
	double minimum_pivot, double *fitted, double *determinant)
{
	double matrix[GRAM_FIT_COLUMNS][GRAM_FIT_COLUMNS + 1];

	if (!count || count > GRAM_FIT_COLUMNS ||
	    !isfinite(minimum_pivot) || minimum_pivot < 0.) return false;
	for (unsigned i = 0; i < count; i++) {
		for (unsigned j = 0; j <= i; j++)
			matrix[i][j] = matrix[j][i] = gram[gram_moment_index(i, j)];
		matrix[i][count] = rhs[i];
	}
	if (!gram_reduce(matrix, count, minimum_pivot, determinant)) return false;
	for (unsigned i = 0; i < count; i++) fitted[i] = matrix[i][count];
	return true;
}

/* Solve a small row-packed Gram system without changing its moments.
 * Read only count * (count + 1) / 2 moments. Normalize columns before
 * elimination and reject unidentifiable fits using the relative
 * determinant. The caller owns any coefficient bounds or model selection.
 * Only the first count coefficients are written; failure may write a prefix. */
bool gram_fit(
	const double gram[GRAM_FIT_MOMENTS],
	const double rhs[GRAM_FIT_COLUMNS], unsigned count,
	double fitted[GRAM_FIT_COLUMNS])
{
	double matrix[GRAM_FIT_COLUMNS][GRAM_FIT_COLUMNS + 1];
	double scale[GRAM_FIT_COLUMNS], determinant = 1.;

	if (!count || count > GRAM_FIT_COLUMNS) return false;
	for (unsigned i = 0; i < count; i++) {
		if (!(gram[gram_moment_index(i, i)] > 0.)) return false;
		scale[i] = sqrt(gram[gram_moment_index(i, i)]);
	}
	for (unsigned i = 0; i < count; i++) {
		for (unsigned j = 0; j <= i; j++) {
			double value = gram[gram_moment_index(i, j)] / (scale[i] * scale[j]);
			matrix[i][j] = matrix[j][i] = value;
		}
		matrix[i][count] = rhs[i] / scale[i];
	}
	if (!gram_reduce(matrix, count, 0., &determinant)) return false;
	if (!(determinant > 64. * DBL_EPSILON)) return false;
	for (unsigned i = 0; i < count; i++) {
		fitted[i] = matrix[i][count] / scale[i];
		if (!isfinite(fitted[i])) return false;
	}
	return true;
}
