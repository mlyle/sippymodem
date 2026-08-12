// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_s_transmitter.h"

#include <stddef.h>

static uint32_t expected_sample_count(
	const struct v34_phase3_s_transmitter *transmitter)
{
	uint64_t accepted_index = V34_PHASE3_S_SEQUENCE_SYMBOLS +
		V34_PHASE3_S_TAIL_SYMBOLS - 1u;
	uint64_t numerator = accepted_index *
		transmitter->line.carrier.channel.sample_rate *
		transmitter->line.profile.symbol_rate_denominator;
	uint64_t denominator =
		transmitter->line.profile.symbol_rate_numerator;
	uint64_t last_acceptance =
		(numerator + denominator - 1u) / denominator;
	uint64_t count = last_acceptance + 1u +
		V34_PHASE3_S_FILTER_TAIL_SAMPLES;

	return count <= UINT32_MAX ? (uint32_t)count : 0;
}

static bool stage_is_valid(
	const struct v34_phase3_s_transmitter *transmitter)
{
	bool sequence_complete = v34_phase3_s_sequence_is_complete(
		&transmitter->sequence);

	switch (transmitter->stage) {
	case V34_PHASE3_S_TRANSMIT_SYMBOLS:
		return transmitter->tail_symbols == 0 &&
		       transmitter->filter_tail_samples == 0;
	case V34_PHASE3_S_TRANSMIT_PULSE_TAIL:
		return sequence_complete && transmitter->tail_symbols > 0 &&
		       transmitter->tail_symbols <
			       V34_PHASE3_S_TAIL_SYMBOLS &&
		       transmitter->filter_tail_samples == 0;
	case V34_PHASE3_S_TRANSMIT_FILTER_TAIL:
		return sequence_complete &&
		       transmitter->tail_symbols ==
			       V34_PHASE3_S_TAIL_SYMBOLS &&
		       transmitter->filter_tail_samples <
			       V34_PHASE3_S_FILTER_TAIL_SAMPLES;
	case V34_PHASE3_S_TRANSMIT_COMPLETE:
		return sequence_complete &&
		       transmitter->tail_symbols ==
			       V34_PHASE3_S_TAIL_SYMBOLS &&
		       transmitter->filter_tail_samples ==
			       V34_PHASE3_S_FILTER_TAIL_SAMPLES;
	}
	return false;
}

bool v34_phase3_s_transmitter_is_valid(
	const struct v34_phase3_s_transmitter *transmitter)
{
	uint32_t expected_samples;

	if (transmitter == NULL ||
	    !v34_training_line_is_valid(&transmitter->line) ||
	    !v34_phase3_s_sequence_is_valid(&transmitter->sequence) ||
	    !stage_is_valid(transmitter))
		return false;
	expected_samples = expected_sample_count(transmitter);
	return expected_samples != 0 &&
	       transmitter->line.generated_samples <= expected_samples &&
	       (transmitter->stage != V34_PHASE3_S_TRANSMIT_COMPLETE ||
		transmitter->line.generated_samples == expected_samples);
}

bool v34_phase3_s_transmitter_is_complete(
	const struct v34_phase3_s_transmitter *transmitter)
{
	return v34_phase3_s_transmitter_is_valid(transmitter) &&
	       transmitter->stage == V34_PHASE3_S_TRANSMIT_COMPLETE;
}

static enum v34_phase3_s_transmitter_result line_result(
	enum v34_training_line_result result)
{
	switch (result) {
	case V34_TRAINING_LINE_OK:
		return V34_PHASE3_S_TRANSMITTER_OK;
	case V34_TRAINING_LINE_CLIPPING_RISK:
		return V34_PHASE3_S_TRANSMITTER_CLIPPING_RISK;
	case V34_TRAINING_LINE_NUMERIC_FAILURE:
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	case V34_TRAINING_LINE_INVALID_ARGUMENT:
	case V34_TRAINING_LINE_OUT_OF_RANGE:
		break;
	}
	return V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT;
}

enum v34_phase3_s_transmitter_result
v34_phase3_s_transmitter_init(
	struct v34_phase3_s_transmitter *transmitter,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, int16_t nominal_peak_amplitude)
{
	struct v34_phase3_s_transmitter initialized = {
		.stage = V34_PHASE3_S_TRANSMIT_SYMBOLS,
	};
	enum v34_phase3_s_transmitter_result result;

	if (transmitter == NULL)
		return V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT;
	result = line_result(v34_training_line_init(
		&initialized.line, profile, sample_rate,
		nominal_peak_amplitude));
	if (result != V34_PHASE3_S_TRANSMITTER_OK)
		return result;
	if (v34_phase3_s_sequence_init(&initialized.sequence) !=
		    V34_PHASE3_S_SEQUENCE_OK ||
	    !v34_phase3_s_transmitter_is_valid(&initialized))
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = initialized;
	return V34_PHASE3_S_TRANSMITTER_OK;
}

enum v34_phase3_s_transmitter_result
v34_phase3_s_transmitter_sample(
	struct v34_phase3_s_transmitter *transmitter,
	int16_t *sample)
{
	struct v34_phase3_s_transmitter next;
	const struct qam_point zero = { 0 };
	struct qam_point point = zero;
	const struct qam_point *point_pointer = NULL;
	enum v34_phase3_s_section section;
	enum v34_training_line_result result;
	int16_t next_sample;
	bool accepting;
	bool accepted;
	bool sequence_symbol = false;
	bool draining_filter;

	if (!v34_phase3_s_transmitter_is_valid(transmitter) || sample == NULL)
		return V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT;
	if (transmitter->stage == V34_PHASE3_S_TRANSMIT_COMPLETE)
		return V34_PHASE3_S_TRANSMITTER_COMPLETE;
	next = *transmitter;
	draining_filter =
		next.stage == V34_PHASE3_S_TRANSMIT_FILTER_TAIL;
	accepting = v34_training_line_needs_point(&next.line);
	if (accepting) {
		point_pointer = &point;
		if (next.stage == V34_PHASE3_S_TRANSMIT_SYMBOLS) {
			if (!v34_phase3_s_sequence_is_complete(&next.sequence)) {
				if (v34_phase3_s_sequence_next(
					    &next.sequence, &point, &section) !=
				    V34_PHASE3_S_SEQUENCE_OK)
					return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
				sequence_symbol = true;
			} else {
				next.stage = V34_PHASE3_S_TRANSMIT_PULSE_TAIL;
			}
		}
	}
	result = v34_training_line_sample(
		&next.line, point_pointer, &next_sample, &accepted);
	if (result != V34_TRAINING_LINE_OK)
		return line_result(result);
	if (accepted != accepting || (sequence_symbol && !accepted))
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	if (accepted && next.stage == V34_PHASE3_S_TRANSMIT_PULSE_TAIL) {
		next.tail_symbols++;
		if (next.tail_symbols == V34_PHASE3_S_TAIL_SYMBOLS)
			next.stage = V34_PHASE3_S_TRANSMIT_FILTER_TAIL;
	}
	if (draining_filter) {
		next.filter_tail_samples++;
		if (next.filter_tail_samples ==
		    V34_PHASE3_S_FILTER_TAIL_SAMPLES)
			next.stage = V34_PHASE3_S_TRANSMIT_COMPLETE;
	}
	if (!v34_phase3_s_transmitter_is_valid(&next))
		return V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = next;
	*sample = next_sample;
	return V34_PHASE3_S_TRANSMITTER_OK;
}

const char *v34_phase3_s_transmitter_result_name(
	enum v34_phase3_s_transmitter_result result)
{
	switch (result) {
	case V34_PHASE3_S_TRANSMITTER_OK:
		return "ok";
	case V34_PHASE3_S_TRANSMITTER_COMPLETE:
		return "complete";
	case V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE3_S_TRANSMITTER_CLIPPING_RISK:
		return "clipping risk";
	case V34_PHASE3_S_TRANSMITTER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
