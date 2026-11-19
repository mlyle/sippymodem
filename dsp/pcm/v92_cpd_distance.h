// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPD_DISTANCE_H
#define V92_CPD_DISTANCE_H

#include "pam_distance.h"
#include "v92_cpd.h"

#include <string.h>

struct v92_cpd_distance {
	struct v92_cpd_constellation points;
	double step, inverse_step;
	struct pam_distance_range range[PAM_DISTANCE_BINS];
};

/* One zero-initialized bank per datapump. B1 and online timing observe the
 * same receive mapping in successive parts of an epoch. Keep owned levels
 * beside their brackets so in-place CPd replacement cannot reuse stale cuts. */
struct v92_cpd_distances {
	struct v92_cpd_distance set[V92_CPD_MAX_CONSTELLATIONS];
};

static inline void v92_cpd_distances_prepare(struct v92_cpd_distances *distances,
	const struct v92_cpd_parameters *cpd)
{
	_Static_assert(V92_CPD_MAX_CONSTELLATION_POINTS <= UINT8_MAX + 1u,
		       "point brackets fit in byte indices");
	for (unsigned i = 0; i < V92_CPD_MAX_CONSTELLATIONS; ++i) {
		const struct v92_cpd_constellation *points = &cpd->constellation[i];
		struct v92_cpd_distance *entry = &distances->set[i];
		if (entry->points.count == points->count &&
		    memcmp(entry->points.positive_linear, points->positive_linear,
			   points->count * sizeof(points->positive_linear[0])) == 0)
			continue;
		entry->points = *points;
		pam_distance_prepare(points->positive_linear, points->count,
			&entry->step, &entry->inverse_step, entry->range);
	}
}

#endif
