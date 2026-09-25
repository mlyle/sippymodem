// SPDX-License-Identifier: BSD-3-Clause

#include "v90_sign_decoder.h"

#include "memory_region.h"

#include "packed_bits.h"

bool v90_sign_decoder_is_valid(
	const struct v90_sign_decoder *decoder)
{
	size_t active_shaping_signs;

	if (decoder == NULL || decoder->sign_redundancy > 3 ||
	    decoder->previous_odd > 1 ||
	    decoder->previous_pcm_sign > 1)
		return false;
	active_shaping_signs = decoder->sign_redundancy == 0 ?
				       0 :
				       V90_CP_FRAME_INTERVALS /
					       decoder->sign_redundancy;
	for (size_t index = 0;
	     index < V90_CP_FRAME_INTERVALS; ++index) {
		if (decoder->previous_shaping_signs[index] > 1 ||
		    (index >= active_shaping_signs &&
		     decoder->previous_shaping_signs[index] != 0))
			return false;
	}
	if (decoder->sign_redundancy == 0)
		return decoder->previous_odd == 0 &&
		       !decoder->trellis_state;
	return decoder->previous_pcm_sign == 0;
}

enum v90_sign_decoder_result v90_sign_decoder_init(
	struct v90_sign_decoder *decoder, uint8_t sign_redundancy)
{
	struct v90_sign_decoder initialized = {
		.sign_redundancy = sign_redundancy,
	};

	if (decoder == NULL || sign_redundancy > 3)
		return V90_SIGN_DECODER_INVALID_ARGUMENT;
	*decoder = initialized;
	return V90_SIGN_DECODER_OK;
}

static void decode_unshaped(
	struct v90_sign_decoder *decoder, const uint8_t *signs,
	struct v90_sign_decode_output *output)
{
	uint8_t previous = decoder->previous_pcm_sign;

	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		output->bits[interval] = signs[interval] ^ previous;
		previous = signs[interval];
	}
	decoder->previous_pcm_sign = previous;
	output->bit_count = V90_CP_FRAME_INTERVALS;
}

static enum v90_spectral_rule identify_rule(
	const struct v90_sign_decoder *decoder, uint8_t first_sign)
{
	bool first_inverted =
		first_sign != decoder->previous_shaping_signs[0];

	if (!decoder->trellis_state)
		return first_inverted ?
			       V90_SPECTRAL_RULE_B : V90_SPECTRAL_RULE_A;
	return first_inverted ?
		       V90_SPECTRAL_RULE_C : V90_SPECTRAL_RULE_D;
}

static void decode_shaped(
	struct v90_sign_decoder *decoder, const uint8_t *signs,
	struct v90_sign_decode_output *output)
{
	size_t frame_count = decoder->sign_redundancy;
	size_t frame_length = V90_CP_FRAME_INTERVALS / frame_count;
	size_t bit_index = 0;

	for (size_t frame = 0; frame < frame_count; ++frame) {
		size_t offset = frame * frame_length;
		enum v90_spectral_rule rule = identify_rule(
			decoder, signs[offset]);

		output->rules[frame] = rule;
		for (size_t position = 0;
		     position < frame_length; ++position) {
			uint8_t initial = v90_spectral_rule_apply(
				signs[offset + position], position, rule);
			uint8_t differential =
				initial ^
				decoder->previous_shaping_signs[position];

			decoder->previous_shaping_signs[position] = initial;
			if (position == 0)
				continue;
			if ((position & 1u) != 0) {
				uint8_t encoded = differential;

				differential ^= decoder->previous_odd;
				decoder->previous_odd = encoded;
			}
			output->bits[bit_index++] = differential;
		}
		decoder->trellis_state = (rule & 1u) != 0;
	}
	output->bit_count = (uint8_t)bit_index;
	output->rule_count = (uint8_t)frame_count;
}

enum v90_sign_decoder_result v90_sign_decoder_decode(
	struct v90_sign_decoder *decoder,
	const uint8_t signs[V90_CP_FRAME_INTERVALS], size_t sign_count,
	struct v90_sign_decode_output *output)
{
	struct v90_sign_decoder next;
	struct v90_sign_decode_output generated = { 0 };

	if (decoder == NULL || signs == NULL || output == NULL ||
	    memory_regions_overlap(decoder, sizeof(*decoder),
				   output, sizeof(*output)))
		return V90_SIGN_DECODER_INVALID_ARGUMENT;
	if (!v90_sign_decoder_is_valid(decoder))
		return V90_SIGN_DECODER_INVALID_STATE;
	if (sign_count != V90_CP_FRAME_INTERVALS ||
	    !unpacked_bits_are_binary(signs, sign_count))
		return V90_SIGN_DECODER_INVALID_SIGNS;
	next = *decoder;
	if (next.sign_redundancy == 0)
		decode_unshaped(&next, signs, &generated);
	else
		decode_shaped(&next, signs, &generated);
	if (!v90_sign_decoder_is_valid(&next) ||
	    generated.bit_count !=
		    V90_CP_FRAME_INTERVALS - next.sign_redundancy ||
	    generated.rule_count != next.sign_redundancy)
		return V90_SIGN_DECODER_INVALID_STATE;
	*decoder = next;
	*output = generated;
	return V90_SIGN_DECODER_OK;
}

const char *v90_sign_decoder_result_name(
	enum v90_sign_decoder_result result)
{
	switch (result) {
	case V90_SIGN_DECODER_OK:
		return "ok";
	case V90_SIGN_DECODER_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_SIGN_DECODER_INVALID_STATE:
		return "invalid state";
	case V90_SIGN_DECODER_INVALID_SIGNS:
		return "invalid signs";
	}
	return "unknown";
}
