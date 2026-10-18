// SPDX-License-Identifier: BSD-3-Clause

#include "v90_dil_sequence.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool pattern_bit(const uint8_t *pattern, uint8_t length, size_t index)
{
	index %= length;
	return ((pattern[index / 8u] >> (index % 8u)) & 1u) != 0u;
}

static uint16_t segment_symbols(
	const struct v90_dil_sequence *sequence, uint8_t segment)
{
	uint8_t ucode = sequence->parameters.training_ucodes[segment];
	uint8_t chord = ucode >> 4;

	return (uint16_t)(sequence->parameters.segment_length_codes[chord] + 1u) *
	       V90_DIL_SEGMENT_INTERVALS;
}

static bool position_is_usable(
	const struct v90_dil_sequence *sequence,
	uint8_t segment, uint16_t segment_symbol, bool complete)
{
	uint8_t ucode;
	uint8_t chord;
	uint16_t symbols;

	if (segment >= sequence->parameters.segment_count)
		return false;
	ucode = sequence->parameters.training_ucodes[segment];
	chord = ucode >> 4;
	if (ucode > 127u || chord >= V90_DIL_CHORDS ||
	    sequence->parameters.segment_length_codes[chord] > 127u ||
	    sequence->parameters.reference_ucodes[chord] > 127u)
		return false;

	symbols = segment_symbols(sequence, segment);
	if (complete)
		return sequence->finish_requested &&
		       (segment_symbol == 0u || segment_symbol == symbols);
	return segment_symbol < symbols;
}

static bool state_is_usable(const struct v90_dil_sequence *sequence)
{
	return sequence != NULL && g711_law_is_valid(sequence->law) &&
	       sequence->parameters.segment_count != 0u &&
	       sequence->parameters.sign_pattern_bits != 0u &&
	       sequence->parameters.sign_pattern_bits <= V90_DIL_MAX_PATTERN_BITS &&
	       sequence->parameters.training_pattern_bits != 0u &&
	       sequence->parameters.training_pattern_bits <= V90_DIL_MAX_PATTERN_BITS &&
	       position_is_usable(sequence, sequence->segment,
				  sequence->segment_symbol, sequence->complete);
}

bool v90_dil_sequence_is_valid(
	const struct v90_dil_sequence *sequence)
{
	return state_is_usable(sequence) &&
	       v90_dil_parameters_are_valid(&sequence->parameters);
}

enum v90_dil_sequence_result v90_dil_sequence_init(
	struct v90_dil_sequence *sequence,
	const struct v90_dil_parameters *parameters,
	enum g711_law law)
{
	struct v90_dil_sequence initialized;

	if (sequence == NULL || !g711_law_is_valid(law) ||
	    !v90_dil_parameters_are_valid(parameters) ||
	    parameters->segment_count == 0u)
		return V90_DIL_SEQUENCE_INVALID_ARGUMENT;
	initialized = (struct v90_dil_sequence){
		.parameters = *parameters,
		.law = law,
	};
	if (!v90_dil_sequence_is_valid(&initialized))
		return V90_DIL_SEQUENCE_INVALID_ARGUMENT;
	*sequence = initialized;
	return V90_DIL_SEQUENCE_OK;
}

enum v90_dil_sequence_result v90_dil_sequence_next(
	struct v90_dil_sequence *sequence,
	struct v90_pcm_symbol *symbol)
{
	struct v90_pcm_symbol generated;
	uint16_t next_symbol;
	uint8_t next_segment;
	uint8_t training_ucode;
	uint8_t chord;
	uint8_t ucode;
	bool positive;
	bool complete = false;

	if (sequence == NULL || symbol == NULL)
		return V90_DIL_SEQUENCE_INVALID_ARGUMENT;
	if (!state_is_usable(sequence))
		return V90_DIL_SEQUENCE_INVALID_STATE;
	if (sequence->complete)
		return V90_DIL_SEQUENCE_COMPLETE;

	training_ucode = sequence->parameters.training_ucodes[sequence->segment];
	chord = training_ucode >> 4;
	ucode = pattern_bit(
			sequence->parameters.training_pattern,
			sequence->parameters.training_pattern_bits,
			sequence->segment_symbol) ?
		training_ucode : sequence->parameters.reference_ucodes[chord];
	positive = pattern_bit(
		sequence->parameters.sign_pattern,
		sequence->parameters.sign_pattern_bits, sequence->segment_symbol);
	if (!v90_pcm_symbol_make(&generated, sequence->law, ucode, positive))
		return V90_DIL_SEQUENCE_INVALID_STATE;

	/* Only the cursor changes. Check the next position before committing
	 * either state or output, including a segment reached by this symbol. */
	next_symbol = sequence->segment_symbol + 1u;
	next_segment = sequence->segment;
	if (next_symbol == segment_symbols(sequence, next_segment)) {
		if (sequence->finish_requested)
			complete = true;
		else {
			next_symbol = 0u;
			next_segment = (uint8_t)(
				(next_segment + 1u) % sequence->parameters.segment_count);
		}
	}
	if (!position_is_usable(sequence, next_segment, next_symbol, complete))
		return V90_DIL_SEQUENCE_INVALID_STATE;
	sequence->segment_symbol = next_symbol;
	sequence->segment = next_segment;
	sequence->complete = complete;
	*symbol = generated;
	return V90_DIL_SEQUENCE_OK;
}

enum v90_dil_sequence_result v90_dil_sequence_request_finish(
	struct v90_dil_sequence *sequence)
{
	if (sequence == NULL)
		return V90_DIL_SEQUENCE_INVALID_ARGUMENT;
	if (!state_is_usable(sequence))
		return V90_DIL_SEQUENCE_INVALID_STATE;
	if (sequence->complete)
		return V90_DIL_SEQUENCE_COMPLETE;
	sequence->finish_requested = true;
	if (sequence->segment_symbol == 0u)
		sequence->complete = true;
	return V90_DIL_SEQUENCE_OK;
}

bool v90_dil_sequence_at_segment_boundary(
	const struct v90_dil_sequence *sequence)
{
	return state_is_usable(sequence) &&
	       (sequence->segment_symbol == 0u || sequence->complete);
}

bool v90_dil_sequence_is_complete(
	const struct v90_dil_sequence *sequence)
{
	return state_is_usable(sequence) && sequence->complete;
}

const char *v90_dil_sequence_result_name(
	enum v90_dil_sequence_result result)
{
	switch (result) {
	case V90_DIL_SEQUENCE_OK:
		return "ok";
	case V90_DIL_SEQUENCE_COMPLETE:
		return "complete";
	case V90_DIL_SEQUENCE_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_DIL_SEQUENCE_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
