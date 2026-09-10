// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_gain.h"
#include "memory_region.h"

#include <limits.h>
#include <math.h>

static bool valid(const struct pcm_gain *gain)
{
	return gain && isfinite(gain->lower) && gain->lower >= 0. &&
		gain->upper > 0. && gain->upper >= gain->lower &&
		isfinite(gain->correlation) && gain->correlation >= 0. &&
		isfinite(gain->energy) && gain->energy >= 0.;
}

void pcm_gain_init(struct pcm_gain *gain)
{
	if (gain)
		*gain = (struct pcm_gain){ .upper = INFINITY };
}

bool pcm_gain_observe(struct pcm_gain *gain, int16_t wire, int16_t sample)
{
	if (!valid(gain))
		return false;
	if (wire == 0)
		return sample == 0;

	double lower = sample == INT16_MIN ? -INFINITY : sample - .5;
	double upper = sample == INT16_MAX ? INFINITY : sample + .5;
	double a = lower / wire, b = upper / wire;
	struct pcm_gain next = *gain;

	if (wire < 0) {
		double swap = a;
		a = b;
		b = swap;
	}
	next.lower = fmax(next.lower, a);
	next.upper = fmin(next.upper, b);
	/* Clipped samples constrain the interval one-sidedly but must not bias
	 * a point estimate as though they were uncensored measurements. */
	if (sample != INT16_MIN && sample != INT16_MAX) {
		next.correlation += (double)wire * sample;
		next.energy += (double)wire * wire;
	}
	if (!valid(&next))
		return false;
	*gain = next;
	return true;
}

bool pcm_gain_estimate(const struct pcm_gain *gain, double *estimate)
{
	if (!estimate || !valid(gain) ||
		memory_regions_overlap(gain, sizeof(*gain), estimate, sizeof(*estimate)) ||
		!isfinite(gain->upper) || gain->energy <= 0. || gain->correlation <= 0.)
		return false;
	double value = fmax(gain->lower,
		fmin(gain->upper, gain->correlation / gain->energy));

	if (!isfinite(value) || value <= 0.)
		return false;
	*estimate = value;
	return true;
}
