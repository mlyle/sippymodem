// SPDX-License-Identifier: BSD-3-Clause

#ifndef PAM_DISTANCE_H
#define PAM_DISTANCE_H

#include <math.h>
#include <stdint.h>

enum {
	PAM_DISTANCE_BIN_SHIFT = 6,
	PAM_DISTANCE_BINS = (INT16_MAX + 1u) >> PAM_DISTANCE_BIN_SHIFT,
};

_Static_assert((PAM_DISTANCE_BINS << PAM_DISTANCE_BIN_SHIFT) == INT16_MAX + 1u,
	       "distance brackets must cover the positive input domain");

struct pam_distance_range {
	uint8_t low, high;
};

/* Prepare an owned alphabet of 0..256 strictly increasing positive signed-
 * 16-bit levels. Empty and singleton alphabets need no brackets; uniform
 * alphabets leave them untouched too. No alphabet pointer is retained. */
void pam_distance_prepare(const int16_t *points, unsigned count,
	double *step, double *inverse_step, struct pam_distance_range *ranges);

/* Squared distance from [x-half_width,x+half_width] to the symmetric signed
 * alphabet. The validated owner supplies finite x, nonnegative finite
 * half_width and a finite squared residual, with 1..256 levels prepared above.
 * Uniform alphabets never read ranges and need no division or search.
 * Nonuniform alphabets always use their owner's search brackets. */
static inline double pam_distance_squared(const int16_t *points, unsigned count,
	double step, double inverse_step, double half_width, double x,
	const struct pam_distance_range *ranges)
{
	x = fabs(x);
	double first = points[0], last = points[count - 1];
	double error;
	if (x <= first) {
		error = first - x;
	} else if (x >= last) {
		error = x - last;
	} else if (step > 0) {
		unsigned lower = (unsigned)((x - first) * inverse_step);
		error = x - (first + lower * step);
		if (error > step * .5)
			error -= step;
		error = fabs(error);
	} else {
		/* Interior x is positive and below INT16_MAX before conversion. */
		const struct pam_distance_range *range =
			&ranges[(unsigned)x >> PAM_DISTANCE_BIN_SHIFT];
		unsigned low = range->low, high = range->high;
		while (high - low > 1) {
			unsigned middle = low + (high - low) / 2;
			if (points[middle] < x)
				low = middle;
			else
				high = middle;
		}
		/* Brackets make both directed distances nonnegative. */
		double below = x - points[low], above = points[high] - x;
#if defined(__x86_64__)
		/* Avoid an x86 libm minimum call. */
		error = below < above ? below : above;
#else
		error = fmin(below, above);
#endif
	}
	error -= half_width;
	return error > 0 ? error * error : 0;
}

#endif /* PAM_DISTANCE_H */
