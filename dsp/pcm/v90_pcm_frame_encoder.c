// SPDX-License-Identifier: BSD-3-Clause

#include "v90_pcm_frame_encoder.h"

#include "packed_bits.h"

#include <string.h>

static bool shaper_matches_mapping(
	const struct v90_spectral_shaper *shaper,
	const struct v90_pcm_mapping *mapping)
{
	return v90_spectral_shaper_is_valid(shaper) &&
	       shaper->law == mapping->law &&
	       shaper->shaping_a1_q1_6 == mapping->shaping_a1_q1_6 &&
	       shaper->shaping_a2_q1_6 == mapping->shaping_a2_q1_6 &&
	       shaper->shaping_b1_q1_6 == mapping->shaping_b1_q1_6 &&
	       shaper->shaping_b2_q1_6 == mapping->shaping_b2_q1_6 &&
	       shaper->sign_redundancy == mapping->sign_redundancy &&
	       shaper->lookahead == mapping->spectral_lookahead;
}

bool v90_pcm_frame_encoder_is_valid(
	const struct v90_pcm_frame_encoder *encoder)
{
	if (encoder == NULL ||
	    !v90_pcm_mapping_is_valid(&encoder->mapping) ||
	    !v90_sign_encoder_is_valid(&encoder->signs) ||
	    encoder->signs.sign_redundancy !=
		    encoder->mapping.sign_redundancy)
		return false;
	if (encoder->mapping.sign_redundancy == 0)
		return true;
	return shaper_matches_mapping(
		       &encoder->shaper, &encoder->mapping) &&
	       encoder->finishing == encoder->shaper.finishing;
}

enum v90_pcm_frame_encoder_result v90_pcm_frame_encoder_init(
	struct v90_pcm_frame_encoder *encoder,
	const struct v90_pcm_mapping *mapping)
{
	struct v90_pcm_frame_encoder initialized = { 0 };

	if (encoder == NULL || mapping == NULL)
		return V90_PCM_FRAME_ENCODER_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(mapping))
		return V90_PCM_FRAME_ENCODER_INVALID_PARAMETERS;
	initialized.mapping = *mapping;
	if (v90_sign_encoder_init(
		    &initialized.signs, mapping->sign_redundancy) !=
	    V90_SIGN_ENCODER_OK)
		return V90_PCM_FRAME_ENCODER_INVALID_PARAMETERS;
	if (mapping->sign_redundancy != 0 &&
	    v90_spectral_shaper_init(&initialized.shaper, mapping) !=
		    V90_SPECTRAL_SHAPER_OK)
		return V90_PCM_FRAME_ENCODER_INVALID_PARAMETERS;
	if (!v90_pcm_frame_encoder_is_valid(&initialized))
		return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
	*encoder = initialized;
	return V90_PCM_FRAME_ENCODER_OK;
}

static bool encode_unsigned_symbols(
	const struct v90_pcm_mapping *mapping,
	const uint8_t *modulus_bits, uint8_t *ucodes)
{
	uint8_t labels[V90_CP_FRAME_INTERVALS];

	if (v90_pcm_mapping_encode_modulus(
		    mapping, modulus_bits, mapping->modulus_bits,
		    labels) != V90_PCM_MAPPING_OK)
		return false;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (!pcm_constellation_descending_select(
			    &mapping->transmit[interval], labels[interval],
			    &ucodes[interval]))
			return false;
	}
	return true;
}

static bool encode_unshaped(
	const struct v90_pcm_mapping *mapping,
	const uint8_t *ucodes, const uint8_t *signs,
	struct v90_pcm_frame_output *output)
{
	struct v90_pcm_frame_output generated = {
		.symbol_count = V90_CP_FRAME_INTERVALS,
	};

	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (!v90_pcm_symbol_make(
			    &generated.symbols[interval], mapping->law,
			    ucodes[interval], signs[interval] != 0))
			return false;
	}
	*output = generated;
	return true;
}

static enum v90_pcm_frame_encoder_result encode_shaped(
	struct v90_pcm_frame_encoder *encoder,
	const uint8_t *ucodes, const uint8_t *signs,
	struct v90_pcm_frame_output *output)
{
	struct v90_pcm_frame_output generated = { 0 };
	size_t frame_count = encoder->mapping.sign_redundancy;
	size_t frame_length = V90_CP_FRAME_INTERVALS / frame_count;

	for (size_t frame = 0; frame < frame_count; ++frame) {
		struct v90_spectral_output shaped;
		enum v90_spectral_shaper_result result =
			v90_spectral_shaper_push(
				&encoder->shaper,
				ucodes + frame * frame_length,
				signs + frame * frame_length,
				frame_length, &shaped);

		if (result == V90_SPECTRAL_SHAPER_BUFFERING)
			continue;
		if (result == V90_SPECTRAL_SHAPER_NUMERIC_FAILURE)
			return V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE;
		if (result != V90_SPECTRAL_SHAPER_FRAME_READY ||
		    generated.symbol_count + shaped.symbol_count >
			    V90_CP_FRAME_INTERVALS ||
		    generated.rule_count >= 3)
			return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
		memcpy(&generated.symbols[generated.symbol_count],
		       shaped.symbols,
		       shaped.symbol_count * sizeof(shaped.symbols[0]));
		generated.symbol_count += shaped.symbol_count;
		generated.rules[generated.rule_count++] = shaped.rule;
	}
	if (generated.symbol_count == 0)
		return V90_PCM_FRAME_ENCODER_BUFFERING;
	*output = generated;
	return V90_PCM_FRAME_ENCODER_SYMBOLS_READY;
}

enum v90_pcm_frame_encoder_result v90_pcm_frame_encoder_process(
	struct v90_pcm_frame_encoder *encoder,
	const uint8_t *bits, size_t bit_count,
	struct v90_pcm_frame_output *output)
{
	struct v90_pcm_frame_encoder next;
	struct v90_pcm_frame_output generated;
	uint8_t initial_signs[V90_CP_FRAME_INTERVALS];
	uint8_t ucodes[V90_CP_FRAME_INTERVALS];
	enum v90_pcm_frame_encoder_result result;

	if (encoder == NULL || bits == NULL || output == NULL)
		return V90_PCM_FRAME_ENCODER_INVALID_ARGUMENT;
	if (!v90_pcm_frame_encoder_is_valid(encoder) || encoder->finishing)
		return V90_PCM_FRAME_ENCODER_INVALID_STATE;
	if (bit_count != encoder->mapping.frame_bits ||
	    !unpacked_bits_are_binary(bits, bit_count))
		return V90_PCM_FRAME_ENCODER_INVALID_BITS;
	next = *encoder;
	if (v90_sign_encoder_encode(
		    &next.signs, bits, next.mapping.sign_bits,
		    initial_signs) != V90_SIGN_ENCODER_OK ||
	    !encode_unsigned_symbols(
		    &next.mapping, bits + next.mapping.sign_bits,
		    ucodes))
		return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
	if (next.mapping.sign_redundancy == 0) {
		if (!encode_unshaped(
			    &next.mapping, ucodes, initial_signs,
			    &generated))
			return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
		result = V90_PCM_FRAME_ENCODER_SYMBOLS_READY;
	} else {
		result = encode_shaped(
			&next, ucodes, initial_signs, &generated);
	}
	if ((result != V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
	     result != V90_PCM_FRAME_ENCODER_BUFFERING) ||
	    !v90_pcm_frame_encoder_is_valid(&next))
		return result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY ||
		       result == V90_PCM_FRAME_ENCODER_BUFFERING ?
			       V90_PCM_FRAME_ENCODER_INTERNAL_ERROR : result;
	*encoder = next;
	if (result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY)
		*output = generated;
	return result;
}

enum v90_pcm_frame_encoder_result v90_pcm_frame_encoder_finish(
	struct v90_pcm_frame_encoder *encoder,
	struct v90_pcm_frame_output *output)
{
	struct v90_pcm_frame_encoder next;
	struct v90_pcm_frame_output generated = { 0 };
	struct v90_spectral_output shaped;
	enum v90_spectral_shaper_result result;

	if (encoder == NULL || output == NULL)
		return V90_PCM_FRAME_ENCODER_INVALID_ARGUMENT;
	if (!v90_pcm_frame_encoder_is_valid(encoder))
		return V90_PCM_FRAME_ENCODER_INVALID_STATE;
	next = *encoder;
	next.finishing = true;
	if (next.mapping.sign_redundancy == 0) {
		*encoder = next;
		return V90_PCM_FRAME_ENCODER_COMPLETE;
	}
	result = v90_spectral_shaper_finish(&next.shaper, &shaped);
	if (result == V90_SPECTRAL_SHAPER_COMPLETE) {
		if (!v90_pcm_frame_encoder_is_valid(&next))
			return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
		*encoder = next;
		return V90_PCM_FRAME_ENCODER_COMPLETE;
	}
	if (result == V90_SPECTRAL_SHAPER_NUMERIC_FAILURE)
		return V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE;
	if (result != V90_SPECTRAL_SHAPER_FRAME_READY)
		return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
	memcpy(generated.symbols, shaped.symbols,
	       shaped.symbol_count * sizeof(shaped.symbols[0]));
	generated.symbol_count = shaped.symbol_count;
	generated.rules[0] = shaped.rule;
	generated.rule_count = 1;
	if (!v90_pcm_frame_encoder_is_valid(&next))
		return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
	*encoder = next;
	*output = generated;
	return V90_PCM_FRAME_ENCODER_SYMBOLS_READY;
}

const char *v90_pcm_frame_encoder_result_name(
	enum v90_pcm_frame_encoder_result result)
{
	switch (result) {
	case V90_PCM_FRAME_ENCODER_OK:
		return "ok";
	case V90_PCM_FRAME_ENCODER_SYMBOLS_READY:
		return "symbols ready";
	case V90_PCM_FRAME_ENCODER_BUFFERING:
		return "buffering";
	case V90_PCM_FRAME_ENCODER_COMPLETE:
		return "complete";
	case V90_PCM_FRAME_ENCODER_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PCM_FRAME_ENCODER_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_PCM_FRAME_ENCODER_INVALID_STATE:
		return "invalid state";
	case V90_PCM_FRAME_ENCODER_INVALID_BITS:
		return "invalid bits";
	case V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE:
		return "numeric failure";
	case V90_PCM_FRAME_ENCODER_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
