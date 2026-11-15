// SPDX-License-Identifier: BSD-3-Clause

#include "pam_distance.h"

void pam_distance_prepare(const int16_t *points, unsigned count,
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
