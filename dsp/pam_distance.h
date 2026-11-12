// SPDX-License-Identifier: BSD-3-Clause

#ifndef PAM_DISTANCE_H
#define PAM_DISTANCE_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
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

/* The owner supplies 1..256 strictly increasing positive signed-16-bit
 * levels. A zero step denotes a singleton or a nonuniform alphabet. */
static inline double pam_distance_step(const int16_t *points, unsigned count)
{
	if (count < 2)
		return 0;
	double step = points[1] - points[0];
	for (unsigned i = 2; i < count; ++i)
		if (points[i] - points[i - 1] != step)
			return 0;
	return step;
}

/* Optional owned search brackets. For x in [L,U), retain the last point
 * <= L and the first >= U, clamped to the endpoints. Monotone cursors
 * preserve exact brackets even when many points share one bin. */
static inline void pam_distance_ranges(struct pam_distance_range *ranges,
	const int16_t *points, unsigned count)
{
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
 * half_width and a finite squared residual; step is from pam_distance_step and
 * inverse_step is 1/step when positive. No alphabet pointer is retained.
 * Uniform alphabets never read ranges and need no division or search.
 * Nonuniform alphabets may omit ranges for a full binary search. */
static inline double pam_distance_score(const int16_t *points, unsigned count,
	double step, double inverse_step, double half_width, double x,
	const struct pam_distance_range *ranges, bool cached)
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
		unsigned low = 0, high = count - 1;
		if (cached) {
			const struct pam_distance_range *range =
				&ranges[(unsigned)x >> PAM_DISTANCE_BIN_SHIFT];
			low = range->low;
			high = range->high;
		}
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

/* Optional brackets for callers that choose their search at run time. */
static inline double pam_distance_squared(const int16_t *points, unsigned count,
	double step, double inverse_step, double half_width, double x,
	const struct pam_distance_range *ranges)
{
	return pam_distance_score(points, count, step, inverse_step, half_width, x,
		ranges, ranges != NULL);
}

/* A configured owner guarantees brackets for every nonuniform alphabet.
 * Keep that invariant explicit so its hot loop needs no null-cache branch. */
static inline double pam_distance_squared_cached(const int16_t *points, unsigned count,
	double step, double inverse_step, double half_width, double x,
	const struct pam_distance_range *ranges)
{
	return pam_distance_score(points, count, step, inverse_step, half_width, x, ranges, true);
}

#endif /* PAM_DISTANCE_H */
