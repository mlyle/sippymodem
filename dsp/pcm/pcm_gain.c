// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_gain.h"
#include "memory_region.h"

#include <limits.h>
#include <math.h>

bool pcm_gain_is_valid(const struct pcm_gain *gain)
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

static bool intersect_sample(const struct pcm_gain *gain,
	int16_t wire, int16_t sample, double *lower, double *upper)
{
	if (!pcm_gain_is_valid(gain))
		return false;
	*lower = gain->lower;
	*upper = gain->upper;
	if (wire == 0)
		return sample == 0;

	double a = (sample == INT16_MIN ? -INFINITY : sample - .5) / wire;
	double b = (sample == INT16_MAX ? INFINITY : sample + .5) / wire;

	if (wire < 0) {
		double swap = a;
		a = b;
		b = swap;
	}
	*lower = fmax(*lower, a);
	*upper = fmin(*upper, b);
	return isfinite(*lower) && *upper > 0. && *upper >= *lower;
}

bool pcm_gain_contains(const struct pcm_gain *gain, int16_t wire, int16_t sample)
{
	double lower, upper;
	return intersect_sample(gain, wire, sample, &lower, &upper);
}

bool pcm_gain_observed_cell(const struct pcm_gain *gain, enum g711_law law,
	uint8_t octet, int16_t sample, int polarity,
	bool probability, struct pcm_gain_interval *interval)
{
	struct g711_interval cell;

	if (!interval || (polarity != 1 && polarity != -1) || !g711_law_is_valid(law) ||
	    memory_regions_overlap(gain, sizeof(*gain), interval, sizeof(*interval)) ||
	    !pcm_gain_contains(gain, g711_decode(law, octet), sample) ||
	    !g711_octet_interval(law, octet, &cell))
		return false;
	struct g711_probability_interval bounds = probability ?
		g711_probability_bounds(cell) :
		(struct g711_probability_interval){ cell.lower, cell.upper };
	*interval = (struct pcm_gain_interval){
		.lower = polarity > 0 ? bounds.lower : -bounds.upper,
		.upper = polarity > 0 ? bounds.upper : -bounds.lower,
	};
	return true;
}

bool pcm_gain_observe(struct pcm_gain *gain, int16_t wire, int16_t sample)
{
	double lower, upper;
	if (!intersect_sample(gain, wire, sample, &lower, &upper))
		return false;
	if (wire == 0)
		return true;
	struct pcm_gain next = *gain;
	next.lower = lower;
	next.upper = upper;
	/* Clipped samples constrain the interval one-sidedly but must not bias
	 * a point estimate as though they were uncensored measurements. */
	if (sample != INT16_MIN && sample != INT16_MAX) {
		next.correlation += (double)wire * sample;
		next.energy += (double)wire * wire;
	}
	if (!pcm_gain_is_valid(&next))
		return false;
	*gain = next;
	return true;
}

bool pcm_gain_estimate(const struct pcm_gain *gain, double *estimate)
{
	if (!estimate || !pcm_gain_is_valid(gain) ||
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
