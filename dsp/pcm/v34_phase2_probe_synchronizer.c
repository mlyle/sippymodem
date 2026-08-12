// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_probe_synchronizer.h"

#include "v34_phase2_probe_spectrum.h"

#include <math.h>
#include <stddef.h>

static const double
	V34_PHASE2_PROBE_SYNCHRONIZER_MINIMUM_ENERGY_FRACTION = 0.60;
static const double
	V34_PHASE2_PROBE_SYNCHRONIZER_PLATEAU_TOLERANCE = 0.001;

enum {
	V34_PHASE2_PROBE_OMITTED_TONES = 4,
};

_Static_assert(
	V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES > 0,
	"V.34 probe synchronization needs a non-empty search prefix");
_Static_assert(
	V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES <= UINT16_MAX,
	"V.34 probe synchronization counters must contain the capture");
_Static_assert(
	V34_PHASE2_PROBE_HIGHEST_HARMONIC -
		V34_PHASE2_PROBE_OMITTED_TONES ==
		V34_PHASE2_PROBE_TONE_COUNT,
	"V.34 probe energy selection must contain every transmitted tone");

bool v34_phase2_probe_synchronizer_is_valid(
	const struct v34_phase2_probe_synchronizer *synchronizer)
{
	if (synchronizer == NULL ||
	    (unsigned int)synchronizer->phase >
		    V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND ||
	    synchronizer->samples_observed >
		    V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES ||
	    synchronizer->leading_samples >
		    V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES ||
	    !isfinite(synchronizer->probe_energy_fraction) ||
	    synchronizer->probe_energy_fraction < 0.0 ||
	    synchronizer->probe_energy_fraction > 1.0)
		return false;

	switch (synchronizer->phase) {
	case V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING:
		return synchronizer->samples_observed <
			       V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
		       synchronizer->leading_samples == 0 &&
		       synchronizer->probe_energy_fraction == 0.0;
	case V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED:
		return synchronizer->samples_observed ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
		       synchronizer->probe_energy_fraction >=
			       V34_PHASE2_PROBE_SYNCHRONIZER_MINIMUM_ENERGY_FRACTION;
	case V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND:
		return synchronizer->samples_observed ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
		       synchronizer->probe_energy_fraction <
			       V34_PHASE2_PROBE_SYNCHRONIZER_MINIMUM_ENERGY_FRACTION;
	}
	return false;
}

enum v34_phase2_probe_synchronizer_result
v34_phase2_probe_synchronizer_init(
	struct v34_phase2_probe_synchronizer *synchronizer)
{
	struct v34_phase2_probe_synchronizer initialized = { 0 };

	if (synchronizer == NULL ||
	    !v34_phase2_probe_synchronizer_is_valid(&initialized))
		return V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT;
	*synchronizer = initialized;
	return V34_PHASE2_PROBE_SYNCHRONIZER_OK;
}

static bool harmonic_is_omitted(unsigned int harmonic)
{
	return harmonic == 6 || harmonic == 8 || harmonic == 12 ||
	       harmonic == 16;
}

static bool measure_probe_energy_fraction(
	const int16_t *received, double *fraction)
{
	struct v34_phase2_probe_spectrum spectrum;
	struct v34_phase2_probe_spectrum_measurement measurement;
	double probe_mean_square = 0.0;
	double sample_mean_square = 0.0;

	if (v34_phase2_probe_spectrum_init(&spectrum) !=
	    V34_PHASE2_PROBE_SPECTRUM_OK)
		return false;
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_PERIOD_SAMPLES; ++index) {
		double sample = received[index];

		if (v34_phase2_probe_spectrum_sample(
			    &spectrum, received[index]) !=
		    V34_PHASE2_PROBE_SPECTRUM_OK)
			return false;
		sample_mean_square += sample * sample;
	}
	if (sample_mean_square == 0.0) {
		*fraction = 0.0;
		return true;
	}
	if (v34_phase2_probe_spectrum_measure(
		    &spectrum, &measurement) !=
	    V34_PHASE2_PROBE_SPECTRUM_OK)
		return false;
	for (unsigned int harmonic = 1;
	     harmonic <= V34_PHASE2_PROBE_HIGHEST_HARMONIC;
	     ++harmonic)
		if (!harmonic_is_omitted(harmonic))
			probe_mean_square +=
				measurement.harmonics[harmonic - 1u]
					.mean_square;
	sample_mean_square /= V34_PHASE2_PROBE_PERIOD_SAMPLES;
	*fraction = probe_mean_square / sample_mean_square;
	/* Roundoff may exceed the mathematical upper bound by a few ulps. */
	if (*fraction > 1.0)
		*fraction = 1.0;
	return isfinite(*fraction);
}

static bool find_onset(
	struct v34_phase2_probe_synchronizer *synchronizer,
	struct v34_phase2_probe_synchronizer_event *event)
{
	double fractions[V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES + 1u];
	double best_fraction = 0.0;
	uint16_t best_candidate = 0;

	for (uint16_t candidate = 0;
	     candidate <=
	     V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES;
	     ++candidate) {
		double fraction;

		if (!measure_probe_energy_fraction(
			    &synchronizer->capture[candidate], &fraction))
			return false;
		fractions[candidate] = fraction;
		if (fraction > best_fraction) {
			best_fraction = fraction;
			best_candidate = candidate;
		}
	}
	/* A periodic probe gives every post-onset candidate the same spectrum.
	 * Select the first point on that plateau instead of letting floating-point
	 * roundoff move the edge later in an otherwise identical period. */
	for (uint16_t candidate = 0; candidate < best_candidate; ++candidate)
		if (fractions[candidate] >=
		    best_fraction -
			    V34_PHASE2_PROBE_SYNCHRONIZER_PLATEAU_TOLERANCE) {
			best_candidate = candidate;
			best_fraction = fractions[candidate];
			break;
		}
	synchronizer->leading_samples = best_candidate;
	synchronizer->probe_energy_fraction = best_fraction;
	event->leading_samples = best_candidate;
	event->probe_energy_fraction = best_fraction;
	if (best_fraction >=
	    V34_PHASE2_PROBE_SYNCHRONIZER_MINIMUM_ENERGY_FRACTION) {
		synchronizer->phase =
			V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED;
		event->kind =
			V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_ACQUIRED;
		event->replay_samples =
			(uint16_t)(synchronizer->samples_observed - best_candidate);
	} else {
		synchronizer->phase =
			V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND;
		event->kind =
			V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NOT_FOUND;
	}
	return true;
}

enum v34_phase2_probe_synchronizer_result
v34_phase2_probe_synchronizer_sample(
	struct v34_phase2_probe_synchronizer *synchronizer,
	int16_t received_sample,
	struct v34_phase2_probe_synchronizer_event *event)
{
	struct v34_phase2_probe_synchronizer next;
	struct v34_phase2_probe_synchronizer_event emitted = { 0 };

	if (!v34_phase2_probe_synchronizer_is_valid(synchronizer) ||
	    event == NULL)
		return V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT;
	next = *synchronizer;
	if (next.phase ==
	    V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING) {
		next.capture[next.samples_observed++] = received_sample;
		if (next.samples_observed ==
			    V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
		    !find_onset(&next, &emitted))
			return V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT;
	}
	if (!v34_phase2_probe_synchronizer_is_valid(&next))
		return V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT;
	*synchronizer = next;
	*event = emitted;
	return V34_PHASE2_PROBE_SYNCHRONIZER_OK;
}

uint16_t v34_phase2_probe_synchronizer_replay_count(
	const struct v34_phase2_probe_synchronizer *synchronizer)
{
	if (!v34_phase2_probe_synchronizer_is_valid(synchronizer) ||
	    synchronizer->phase !=
		    V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED)
		return 0;
	return (uint16_t)(synchronizer->samples_observed -
			  synchronizer->leading_samples);
}

enum v34_phase2_probe_synchronizer_result
v34_phase2_probe_synchronizer_replay(
	const struct v34_phase2_probe_synchronizer *synchronizer,
	uint16_t replay_index, int16_t *sample)
{
	uint16_t replay_count =
		v34_phase2_probe_synchronizer_replay_count(
			synchronizer);

	if (sample == NULL || replay_index >= replay_count)
		return V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT;
	*sample = synchronizer->capture[synchronizer->leading_samples +
				       replay_index];
	return V34_PHASE2_PROBE_SYNCHRONIZER_OK;
}

const char *v34_phase2_probe_synchronizer_result_name(
	enum v34_phase2_probe_synchronizer_result result)
{
	switch (result) {
	case V34_PHASE2_PROBE_SYNCHRONIZER_OK:
		return "ok";
	case V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
