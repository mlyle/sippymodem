// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_probe.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_PHASE2_PROBE_TWO_PI =
	6.283185307179586476925286766559;
static const double V34_PHASE2_PROBE_L1_GAIN =
	1.9952623149688795; /* 10 raised to 6 dB / 20. */

/* An exhaustive pass over the 160 generated samples gives a normalized
 * crest of 1.102688593934759. At +6 dB, 14893 reaches +32767 while 14894
 * would clip. Keep this scalar bound beside the generating formula. */
enum { V34_PHASE2_PROBE_MAX_NOMINAL_AMPLITUDE = 14893 };

_Static_assert(V34_PHASE2_PROBE_PERIOD_SAMPLES == 160,
	       "V.34 probe period must contain three 150 Hz cycles");
_Static_assert(V34_PHASE2_PROBE_L1_SAMPLES ==
		       8 * V34_PHASE2_PROBE_PERIOD_SAMPLES,
	       "V.34 L1 must contain 24 fundamental repetitions");

static bool probe_level_is_valid(
	enum v34_phase2_probe_level level)
{
	return level == V34_PHASE2_PROBE_L1 ||
	       level == V34_PHASE2_PROBE_L2;
}

static bool harmonic_is_inverted(unsigned int harmonic)
{
	/* These are the eight probing tones whose initial phase is 180 degrees;
	 * every other included tone starts at zero degrees. */
	switch (harmonic) {
	case 2:
	case 11:
	case 15:
	case 18:
	case 20:
	case 21:
	case 22:
	case 23:
		return true;
	default:
		return false;
	}
}

static double normalized_probe_sample(uint16_t period_index)
{
	double fundamental_angle =
		V34_PHASE2_PROBE_TWO_PI * 3.0 * (double)period_index /
		(double)V34_PHASE2_PROBE_PERIOD_SAMPLES;
	double fundamental_cosine = cos(fundamental_angle);
	double previous_cosine = 1.0;
	double harmonic_cosine = fundamental_cosine;
	double sum = 0.0;

	/* cos(n*x) = 2*cos(x)*cos((n-1)*x) - cos((n-2)*x). This produces
	 * all 25 candidate harmonics from one cosine evaluation. */
	for (unsigned int harmonic = 1;
	     harmonic <= V34_PHASE2_PROBE_HIGHEST_HARMONIC;
	     ++harmonic) {
		double next_cosine =
			2.0 * fundamental_cosine * harmonic_cosine -
			previous_cosine;

		if (v34_phase2_probe_harmonic_is_transmitted(harmonic))
			sum += harmonic_is_inverted(harmonic) ?
				       -harmonic_cosine :
				       harmonic_cosine;
		previous_cosine = harmonic_cosine;
		harmonic_cosine = next_cosine;
	}
	/* Orthogonal equal-amplitude cosines have aggregate power proportional
	 * to their count. This makes L2 power equal to one nominal sinusoid. */
	return sum / sqrt((double)V34_PHASE2_PROBE_TONE_COUNT);
}

bool v34_phase2_probe_transmitter_is_valid(
	const struct v34_phase2_probe_transmitter *transmitter)
{
	return transmitter != NULL && transmitter->nominal_amplitude > 0 &&
	       transmitter->nominal_amplitude <=
		       V34_PHASE2_PROBE_MAX_NOMINAL_AMPLITUDE &&
	       transmitter->period_index <
		       V34_PHASE2_PROBE_PERIOD_SAMPLES;
}

enum v34_phase2_probe_result
v34_phase2_probe_transmitter_init(
	struct v34_phase2_probe_transmitter *transmitter,
	int16_t nominal_amplitude)
{
	struct v34_phase2_probe_transmitter initialized = {
		.nominal_amplitude = nominal_amplitude,
	};

	if (transmitter == NULL ||
	    !v34_phase2_probe_transmitter_is_valid(&initialized))
		return V34_PHASE2_PROBE_INVALID_ARGUMENT;
	*transmitter = initialized;
	return V34_PHASE2_PROBE_OK;
}

enum v34_phase2_probe_result
v34_phase2_probe_transmitter_sample(
	struct v34_phase2_probe_transmitter *transmitter,
	enum v34_phase2_probe_level level, int16_t *sample)
{
	double level_gain;
	long generated;
	uint16_t next_index;

	if (!v34_phase2_probe_transmitter_is_valid(transmitter) ||
	    !probe_level_is_valid(level) || sample == NULL)
		return V34_PHASE2_PROBE_INVALID_ARGUMENT;
	level_gain = level == V34_PHASE2_PROBE_L1 ?
			     V34_PHASE2_PROBE_L1_GAIN :
			     1.0;
	generated = lround(
		normalized_probe_sample(transmitter->period_index) *
		(double)transmitter->nominal_amplitude * level_gain);
	if (generated < INT16_MIN || generated > INT16_MAX)
		return V34_PHASE2_PROBE_INVALID_ARGUMENT;
	next_index = (uint16_t)(transmitter->period_index + 1u);
	if (next_index == V34_PHASE2_PROBE_PERIOD_SAMPLES)
		next_index = 0;
	transmitter->period_index = next_index;
	*sample = (int16_t)generated;
	return V34_PHASE2_PROBE_OK;
}
