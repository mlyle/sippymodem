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
static inline void pam_distance_prepare(const int16_t *points, unsigned count,
	double *step, double *inverse_step, struct pam_distance_range *ranges)
{
	*step = *inverse_step = 0;
	if (count < 2)
		return;
	double spacing = points[1] - points[0];
	unsigned i = 2;
	while (i < count && points[i] - points[i - 1] == spacing)
		i++;
	if (i == count) {
		*step = spacing;
		*inverse_step = 1. / spacing;
		return;
	}

	/* For x in [L,U), retain the last point <= L and the first >= U,
	 * clamped to the endpoints. Monotone cursors preserve exact brackets
	 * even when many points share one bin. */
	unsigned low = 0, high = 0;
	for (unsigned bin = 0; bin < PAM_DISTANCE_BINS; ++bin) {
		int lower = (int)(bin << PAM_DISTANCE_BIN_SHIFT);
		int upper = lower + (1 << PAM_DISTANCE_BIN_SHIFT);
		while (low + 1 < count && points[low + 1] <= lower)
			low++;
		while (high + 1 < count && points[high] < upper)
			high++;
		ranges[bin] = (struct pam_distance_range){ low, high };
	}
}

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
#if defined(__x86_64__)
		/* Bracketed directed distances avoid an x86 libm minimum call. */
		double below = x - points[low], above = points[high] - x;
		error = below < above ? below : above;
#else
		error = fmin(fabs(x - points[low]), fabs(x - points[high]));
#endif
	}
	error -= half_width;
	return error > 0 ? error * error : 0;
}

#endif /* PAM_DISTANCE_H */
