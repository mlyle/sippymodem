// SPDX-License-Identifier: BSD-3-Clause

#ifndef GRAM_FIT_H
#define GRAM_FIT_H

#include <float.h>
#include <math.h>
#include <stdbool.h>

enum {
	GRAM_FIT_COLUMNS = 4,
	GRAM_FIT_MOMENTS = GRAM_FIT_COLUMNS * (GRAM_FIT_COLUMNS + 1) / 2,
};

/* Row-packed lower triangle of a symmetric Gram matrix. Owners bound their
 * feature count and allocate columns * (columns + 1) / 2 moments. Keep each
 * product sum in sample order, then use either index order when fitting. */
static inline unsigned gram_moment_index(unsigned a, unsigned b)
{
	if (a < b) {
		unsigned swap = a;
		a = b;
		b = swap;
	}
	return a * (a + 1u) / 2u + b;
}

/* Add one feature row and target to an owned least-squares system, retaining
 * sample order in every sum. Arrays must be separate and cover count
 * features/RHS entries and count * (count + 1) / 2 packed moments. Count
 * zero accesses no arrays. Feature construction, energy and fit admission
 * remain with the owner; this does not impose the solver's column limit. */
static inline void gram_accumulate(
	double *moments, double *rhs,
	const double *row, unsigned count, double target)
{
	for (unsigned i = 0; i < count; ++i) {
		rhs[i] += row[i] * target;
		unsigned first = gram_moment_index(i, 0);
		for (unsigned j = 0; j <= i; ++j)
			moments[first + j] += row[i] * row[j];
	}
}

/* Gather an ordered subset of columns and its RHS from a packed system.
 * Source arrays must cover every selected column; destination arrays cover
 * count RHS entries and count * (count + 1) / 2 moments. Columns may repeat
 * or be reordered. All arrays must be separate; count zero accesses none.
 * Values are copied without arithmetic or a solver-specific column limit. */
static inline void gram_select(
	const double *restrict moments, const double *restrict rhs,
	const unsigned *restrict columns, unsigned count,
	double *restrict selected, double *restrict target)
{
	for (unsigned i = 0; i < count; ++i) {
		target[i] = rhs[columns[i]];
		for (unsigned j = 0; j <= i; ++j)
			selected[gram_moment_index(i, j)] =
				moments[gram_moment_index(columns[i], columns[j])];
	}
}

/* Reduce an owned augmented Gram matrix in place. Keep the owner's pivot
 * floor and arithmetic order; normalization and fit admission stay outside.
 * The right-hand side is column count. Failure leaves partially reduced
 * scratch, but does not write the optional determinant. */
static inline bool gram_eliminate(
	double matrix[GRAM_FIT_COLUMNS][GRAM_FIT_COLUMNS + 1],
	unsigned count, double minimum_pivot, double *determinant)
{
	double product = 1.;

	if (!count || count > GRAM_FIT_COLUMNS ||
	    !isfinite(minimum_pivot) || minimum_pivot < 0.) return false;
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

/* Solve a small row-packed Gram system without changing its moments.
 * Read only count * (count + 1) / 2 moments. Normalize columns before
 * elimination and reject unidentifiable fits using the relative
 * determinant. The caller owns any coefficient bounds or model selection.
 * Only the first count coefficients are written; failure may write a prefix. */
static inline bool gram_fit(
	const double gram[GRAM_FIT_MOMENTS],
	const double rhs[GRAM_FIT_COLUMNS], unsigned count,
	double fitted[GRAM_FIT_COLUMNS])
{
	double matrix[GRAM_FIT_COLUMNS][GRAM_FIT_COLUMNS + 1] = { { 0 } };
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
	if (!gram_eliminate(matrix, count, 0., &determinant)) return false;
	if (!(determinant > 64. * DBL_EPSILON)) return false;
	for (unsigned i = 0; i < count; i++) {
		fitted[i] = matrix[i][count] / scale[i];
		if (!isfinite(fitted[i])) return false;
	}
	return true;
}

#endif
