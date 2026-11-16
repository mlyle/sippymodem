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
		for (unsigned j = 0; j <= i; ++j)
			*moments++ += row[i] * row[j];
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

/* Evaluate a real projection in column order, including the owner's seed
 * before the first product. Adding a separately reduced dot product changes
 * rounding when terms cancel. Arrays cover count columns and may alias;
 * count zero reads neither. Bounds and nonfinite admission stay with the
 * owner, without a solver-specific column limit or a hidden noise floor. */
static inline double gram_project(
	double initial, const double *coefficient, const double *row, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
		initial += coefficient[i] * row[i];
	return initial;
}

/* Unexplained energy of a least-squares fit: y'y - coefficient' X'y.
 * Subtract each product in column order; accumulating the projection first
 * changes rounding when most of the measured energy is explained. Arrays
 * cover count columns; count zero reads neither. Return the raw residual,
 * including negative roundoff or nonfinite values. Clamping, normalization
 * and model admission belong to the owner, as does establishing a valid fit. */
static inline double gram_residual(
	double energy, const double *coefficient, const double *rhs, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
		energy -= coefficient[i] * rhs[i];
	return energy;
}

/* Solve a packed symmetric system without changing its moments or RHS.
 * Arrays cover count coefficients and count * (count + 1) / 2 moments.
 * Keep the owner's pivot floor and arithmetic order; normalization and
 * numeric admission stay outside. Failure writes neither fitted coefficients
 * nor the optional determinant. Only active input/output entries are used;
 * inputs and outputs must be separate. */
bool gram_solve(
	const double *gram, const double *rhs, unsigned count,
	double minimum_pivot, double *fitted, double *determinant);

/* Solve a small row-packed Gram system without changing its moments.
 * Read only count * (count + 1) / 2 moments. Normalize columns before
 * elimination and reject unidentifiable fits using the relative
 * determinant. The caller owns any coefficient bounds or model selection.
 * Only the first count coefficients are written; failure may write a prefix. */
bool gram_fit(
	const double gram[GRAM_FIT_MOMENTS],
	const double rhs[GRAM_FIT_COLUMNS], unsigned count,
	double fitted[GRAM_FIT_COLUMNS]);

#endif
