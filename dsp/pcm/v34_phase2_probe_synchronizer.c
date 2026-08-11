// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_probe_synchronizer.h"

#include <math.h>
#include <stddef.h>

static const double V34_PHASE2_PROBE_SYNCHRONIZER_MINIMUM_CORRELATION = 0.80;
static const int16_t V34_PHASE2_PROBE_SYNCHRONIZER_REFERENCE_AMPLITUDE =
	10000;

_Static_assert(
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES > 0,
	"V.34 probe synchronization needs a non-empty search prefix");
_Static_assert(
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES <= UINT16_MAX,
	"V.34 probe synchronization counters must contain the capture");

bool sippymodem_v34_phase2_probe_synchronizer_is_valid(
	const struct sippymodem_v34_phase2_probe_synchronizer *synchronizer)
{
	if (synchronizer == NULL ||
	    (unsigned int)synchronizer->phase >
		    SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND ||
	    synchronizer->samples_observed >
		    SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES ||
	    synchronizer->leading_samples >
		    SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES ||
	    !isfinite(synchronizer->normalized_correlation) ||
	    synchronizer->normalized_correlation < 0.0 ||
	    synchronizer->normalized_correlation > 1.0)
		return false;

	switch (synchronizer->phase) {
	case SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING:
		return synchronizer->samples_observed <
			       SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
		       synchronizer->leading_samples == 0 &&
		       synchronizer->normalized_correlation == 0.0;
	case SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED:
		return synchronizer->samples_observed ==
			       SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
		       synchronizer->normalized_correlation >=
			       V34_PHASE2_PROBE_SYNCHRONIZER_MINIMUM_CORRELATION;
	case SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND:
		return synchronizer->samples_observed ==
			       SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
		       synchronizer->normalized_correlation <
			       V34_PHASE2_PROBE_SYNCHRONIZER_MINIMUM_CORRELATION;
	}
	return false;
}

enum sippymodem_v34_phase2_probe_synchronizer_result
sippymodem_v34_phase2_probe_synchronizer_init(
	struct sippymodem_v34_phase2_probe_synchronizer *synchronizer)
{
	struct sippymodem_v34_phase2_probe_synchronizer initialized = { 0 };

	if (synchronizer == NULL ||
	    !sippymodem_v34_phase2_probe_synchronizer_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT;
	*synchronizer = initialized;
	return SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_OK;
}

static bool generate_reference(
	int16_t reference[SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES])
{
	struct sippymodem_v34_phase2_probe_transmitter transmitter;

	if (sippymodem_v34_phase2_probe_transmitter_init(
		    &transmitter,
		    V34_PHASE2_PROBE_SYNCHRONIZER_REFERENCE_AMPLITUDE) !=
	    SIPPYMODEM_V34_PHASE2_PROBE_OK)
		return false;
	for (size_t index = 0;
	     index < SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES; ++index)
		if (sippymodem_v34_phase2_probe_transmitter_sample(
			    &transmitter, SIPPYMODEM_V34_PHASE2_PROBE_L1,
			    &reference[index]) != SIPPYMODEM_V34_PHASE2_PROBE_OK)
			return false;
	return true;
}

static double normalized_correlation(
	const int16_t *received,
	const int16_t reference[SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES])
{
	double correlation;
	double dot_product = 0.0;
	double received_energy = 0.0;
	double reference_energy = 0.0;

	for (size_t index = 0;
	     index < SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES; ++index) {
		double received_sample = received[index];
		double reference_sample = reference[index];

		dot_product += received_sample * reference_sample;
		received_energy += received_sample * received_sample;
		reference_energy += reference_sample * reference_sample;
	}
	if (received_energy == 0.0 || reference_energy == 0.0)
		return 0.0;
	correlation =
		fabs(dot_product) / sqrt(received_energy * reference_energy);
	/* Roundoff may exceed the mathematical upper bound by a few ulps. */
	return correlation > 1.0 ? 1.0 : correlation;
}

static bool find_onset(
	struct sippymodem_v34_phase2_probe_synchronizer *synchronizer,
	struct sippymodem_v34_phase2_probe_synchronizer_event *event)
{
	int16_t reference[SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES];
	double best_correlation = 0.0;
	uint16_t best_candidate = 0;

	if (!generate_reference(reference))
		return false;
	for (uint16_t candidate = 0;
	     candidate <=
	     SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES;
	     ++candidate) {
		double correlation = normalized_correlation(
			&synchronizer->capture[candidate], reference);

		if (correlation > best_correlation) {
			best_correlation = correlation;
			best_candidate = candidate;
		}
	}
	synchronizer->leading_samples = best_candidate;
	synchronizer->normalized_correlation = best_correlation;
	event->leading_samples = best_candidate;
	event->normalized_correlation = best_correlation;
	if (best_correlation >=
	    V34_PHASE2_PROBE_SYNCHRONIZER_MINIMUM_CORRELATION) {
		synchronizer->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_ACQUIRED;
		event->replay_samples =
			(uint16_t)(synchronizer->samples_observed - best_candidate);
	} else {
		synchronizer->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NOT_FOUND;
	}
	return true;
}

enum sippymodem_v34_phase2_probe_synchronizer_result
sippymodem_v34_phase2_probe_synchronizer_sample(
	struct sippymodem_v34_phase2_probe_synchronizer *synchronizer,
	int16_t received_sample,
	struct sippymodem_v34_phase2_probe_synchronizer_event *event)
{
	struct sippymodem_v34_phase2_probe_synchronizer next;
	struct sippymodem_v34_phase2_probe_synchronizer_event emitted = { 0 };

	if (!sippymodem_v34_phase2_probe_synchronizer_is_valid(synchronizer) ||
	    event == NULL)
		return SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT;
	next = *synchronizer;
	if (next.phase ==
	    SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING) {
		next.capture[next.samples_observed++] = received_sample;
		if (next.samples_observed ==
			    SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
		    !find_onset(&next, &emitted))
			return SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT;
	}
	if (!sippymodem_v34_phase2_probe_synchronizer_is_valid(&next))
		return SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT;
	*synchronizer = next;
	*event = emitted;
	return SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_OK;
}

uint16_t sippymodem_v34_phase2_probe_synchronizer_replay_count(
	const struct sippymodem_v34_phase2_probe_synchronizer *synchronizer)
{
	if (!sippymodem_v34_phase2_probe_synchronizer_is_valid(synchronizer) ||
	    synchronizer->phase !=
		    SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED)
		return 0;
	return (uint16_t)(synchronizer->samples_observed -
			  synchronizer->leading_samples);
}

enum sippymodem_v34_phase2_probe_synchronizer_result
sippymodem_v34_phase2_probe_synchronizer_replay(
	const struct sippymodem_v34_phase2_probe_synchronizer *synchronizer,
	uint16_t replay_index, int16_t *sample)
{
	uint16_t replay_count =
		sippymodem_v34_phase2_probe_synchronizer_replay_count(
			synchronizer);

	if (sample == NULL || replay_index >= replay_count)
		return SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT;
	*sample = synchronizer->capture[synchronizer->leading_samples +
				       replay_index];
	return SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_OK;
}

const char *sippymodem_v34_phase2_probe_synchronizer_result_name(
	enum sippymodem_v34_phase2_probe_synchronizer_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
