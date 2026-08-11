// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_probe_sequence.h"

#include <stddef.h>

static uint32_t l2_limit_samples(
	const struct v34_phase2_probe_sequence *sequence)
{
	return (uint32_t)
		       V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES +
	       sequence->round_trip_delay_samples;
}

static bool waveform_position_is_valid(
	const struct v34_phase2_probe_sequence *sequence)
{
	return sequence->waveform.period_index ==
	       sequence->samples_emitted %
		       V34_PHASE2_PROBE_PERIOD_SAMPLES;
}

static uint32_t l2_samples_emitted(
	const struct v34_phase2_probe_sequence *sequence)
{
	return sequence->samples_emitted -
	       V34_PHASE2_PROBE_L1_SAMPLES;
}

bool v34_phase2_probe_sequence_is_valid(
	const struct v34_phase2_probe_sequence *sequence)
{
	uint32_t l2_limit;

	if (sequence == NULL ||
	    !v34_phase2_probe_transmitter_is_valid(
		    &sequence->waveform) ||
	    !waveform_position_is_valid(sequence))
		return false;
	l2_limit = l2_limit_samples(sequence);
	switch (sequence->phase) {
	case V34_PHASE2_PROBE_SEQUENCE_PHASE_L1:
		return sequence->samples_emitted <
		       V34_PHASE2_PROBE_L1_SAMPLES;
	case V34_PHASE2_PROBE_SEQUENCE_PHASE_L2:
		return sequence->samples_emitted >=
			       V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_samples_emitted(sequence) < l2_limit;
	case V34_PHASE2_PROBE_SEQUENCE_PHASE_COMPLETE:
		return sequence->samples_emitted >
			       V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_samples_emitted(sequence) < l2_limit;
	case V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED:
		return sequence->samples_emitted >=
			       V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_samples_emitted(sequence) == l2_limit;
	}
	return false;
}

enum v34_phase2_probe_sequence_result
v34_phase2_probe_sequence_init(
	struct v34_phase2_probe_sequence *sequence,
	int16_t nominal_amplitude, uint16_t round_trip_delay_samples)
{
	struct v34_phase2_probe_sequence initialized = {
		.phase = V34_PHASE2_PROBE_SEQUENCE_PHASE_L1,
		.round_trip_delay_samples = round_trip_delay_samples,
	};

	if (sequence == NULL ||
	    v34_phase2_probe_transmitter_init(
		    &initialized.waveform, nominal_amplitude) !=
		    V34_PHASE2_PROBE_OK ||
	    !v34_phase2_probe_sequence_is_valid(&initialized))
		return V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT;
	*sequence = initialized;
	return V34_PHASE2_PROBE_SEQUENCE_OK;
}

static enum v34_phase2_probe_sequence_result generate_probe(
	struct v34_phase2_probe_sequence *sequence,
	enum v34_phase2_probe_level level, int16_t *sample)
{
	return v34_phase2_probe_transmitter_sample(
		       &sequence->waveform, level, sample) ==
		       V34_PHASE2_PROBE_OK ?
		       V34_PHASE2_PROBE_SEQUENCE_OK :
		       V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT;
}

static enum v34_phase2_probe_sequence_result emit_l1(
	struct v34_phase2_probe_sequence *sequence,
	int16_t *sample,
	struct v34_phase2_probe_sequence_event *event)
{
	if (generate_probe(
		    sequence, V34_PHASE2_PROBE_L1, sample) !=
	    V34_PHASE2_PROBE_SEQUENCE_OK)
		return V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT;
	sequence->samples_emitted++;
	if (sequence->samples_emitted ==
	    V34_PHASE2_PROBE_L1_SAMPLES) {
		sequence->phase =
			V34_PHASE2_PROBE_SEQUENCE_PHASE_L2;
		event->kind =
			V34_PHASE2_PROBE_SEQUENCE_EVENT_L2_STARTED;
	}
	return V34_PHASE2_PROBE_SEQUENCE_OK;
}

static enum v34_phase2_probe_sequence_result emit_l2(
	struct v34_phase2_probe_sequence *sequence,
	int16_t *sample,
	struct v34_phase2_probe_sequence_event *event)
{
	if (generate_probe(
		    sequence, V34_PHASE2_PROBE_L2, sample) !=
	    V34_PHASE2_PROBE_SEQUENCE_OK)
		return V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT;
	sequence->samples_emitted++;
	if (l2_samples_emitted(sequence) == l2_limit_samples(sequence)) {
		sequence->phase =
			V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED;
		event->kind =
			V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED;
		event->l2_samples = l2_samples_emitted(sequence);
	}
	return V34_PHASE2_PROBE_SEQUENCE_OK;
}

enum v34_phase2_probe_sequence_result
v34_phase2_probe_sequence_sample(
	struct v34_phase2_probe_sequence *sequence,
	bool finish_l2, int16_t *sample,
	struct v34_phase2_probe_sequence_event *event)
{
	struct v34_phase2_probe_sequence next;
	struct v34_phase2_probe_sequence_event emitted = { 0 };
	int16_t generated = 0;
	enum v34_phase2_probe_sequence_result result =
		V34_PHASE2_PROBE_SEQUENCE_OK;

	if (!v34_phase2_probe_sequence_is_valid(sequence) ||
	    sample == NULL || event == NULL)
		return V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT;
	next = *sequence;
	switch (next.phase) {
	case V34_PHASE2_PROBE_SEQUENCE_PHASE_L1:
		if (finish_l2)
			return V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT;
		result = emit_l1(&next, &generated, &emitted);
		break;
	case V34_PHASE2_PROBE_SEQUENCE_PHASE_L2:
		if (finish_l2) {
			if (l2_samples_emitted(&next) == 0)
				return V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT;
			next.phase =
				V34_PHASE2_PROBE_SEQUENCE_PHASE_COMPLETE;
			emitted.kind =
				V34_PHASE2_PROBE_SEQUENCE_EVENT_COMPLETED;
			emitted.l2_samples = l2_samples_emitted(&next);
		} else {
			result = emit_l2(&next, &generated, &emitted);
		}
		break;
	case V34_PHASE2_PROBE_SEQUENCE_PHASE_COMPLETE:
	case V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED:
		break;
	}
	if (result != V34_PHASE2_PROBE_SEQUENCE_OK ||
	    !v34_phase2_probe_sequence_is_valid(&next))
		return V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT;
	*sequence = next;
	*sample = generated;
	*event = emitted;
	return V34_PHASE2_PROBE_SEQUENCE_OK;
}

const char *v34_phase2_probe_sequence_result_name(
	enum v34_phase2_probe_sequence_result result)
{
	switch (result) {
	case V34_PHASE2_PROBE_SEQUENCE_OK:
		return "ok";
	case V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
