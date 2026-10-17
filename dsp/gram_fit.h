// SPDX-License-Identifier: BSD-3-Clause

#ifndef GRAM_FIT_H
#define GRAM_FIT_H

#include <float.h>
#include <math.h>
#include <stdbool.h>

enum { GRAM_FIT_COLUMNS = 4 };

/* Solve a small Gram system without changing its moments. Normalize columns
 * before elimination and reject unidentifiable fits using the relative
 * determinant. The caller owns any coefficient bounds or model selection.
 * Only the first count coefficients are written; failure may write a prefix. */
static inline bool gram_fit(
	const double gram[GRAM_FIT_COLUMNS][GRAM_FIT_COLUMNS],
	const double rhs[GRAM_FIT_COLUMNS], unsigned count,
	double fitted[GRAM_FIT_COLUMNS])
{
	double matrix[GRAM_FIT_COLUMNS][GRAM_FIT_COLUMNS + 1] = { { 0 } };
	double scale[GRAM_FIT_COLUMNS], determinant = 1.;

	if (!count || count > GRAM_FIT_COLUMNS) return false;
	for (unsigned i = 0; i < count; i++) {
		if (!(gram[i][i] > 0.)) return false;
		scale[i] = sqrt(gram[i][i]);
	}
	for (unsigned i = 0; i < count; i++) {
		for (unsigned j = 0; j < count; j++)
			matrix[i][j] = gram[i][j] / (scale[i] * scale[j]);
		matrix[i][count] = rhs[i] / scale[i];
	}
	for (unsigned i = 0; i < count; i++) {
		double pivot = matrix[i][i];
		if (!(pivot > 0.)) return false;
		determinant *= pivot;
		for (unsigned k = i; k <= count; k++) matrix[i][k] /= pivot;
		for (unsigned j = 0; j < count; j++) if (j != i) {
			double factor = matrix[j][i];
			for (unsigned k = i; k <= count; k++) matrix[j][k] -= factor * matrix[i][k];
		}
	}
	if (!(determinant > 64. * DBL_EPSILON)) return false;
	for (unsigned i = 0; i < count; i++) {
		fitted[i] = matrix[i][count] / scale[i];
		if (!isfinite(fitted[i])) return false;
	}
	return true;
}

#endif
