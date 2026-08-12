// SPDX-License-Identifier: BSD-3-Clause

#include "v90_pcm_frame_decoder.h"

#include <string.h>

static bool plane_is_valid(enum v90_pcm_mapping_plane plane)
{
	return plane == V90_PCM_MAPPING_TRANSMIT ||
	       plane == V90_PCM_MAPPING_CODEC;
}

bool v90_pcm_frame_decoder_is_valid(
	const struct v90_pcm_frame_decoder *decoder)
{
	return decoder != NULL &&
	       v90_pcm_mapping_is_valid(&decoder->mapping) &&
	       v90_sign_decoder_is_valid(&decoder->signs) &&
	       decoder->signs.sign_redundancy ==
		       decoder->mapping.sign_redundancy &&
	       plane_is_valid(decoder->decision_plane);
}

enum v90_pcm_frame_decoder_result v90_pcm_frame_decoder_init(
	struct v90_pcm_frame_decoder *decoder,
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane decision_plane)
{
	struct v90_pcm_frame_decoder initialized = { 0 };

	if (decoder == NULL || mapping == NULL ||
	    !plane_is_valid(decision_plane))
		return V90_PCM_FRAME_DECODER_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(mapping))
		return V90_PCM_FRAME_DECODER_INVALID_PARAMETERS;
	initialized.mapping = *mapping;
	initialized.decision_plane = decision_plane;
	if (v90_sign_decoder_init(
		    &initialized.signs, mapping->sign_redundancy) !=
	    V90_SIGN_DECODER_OK)
		return V90_PCM_FRAME_DECODER_INVALID_PARAMETERS;
	if (!v90_pcm_frame_decoder_is_valid(&initialized))
		return V90_PCM_FRAME_DECODER_INTERNAL_ERROR;
	*decoder = initialized;
	return V90_PCM_FRAME_DECODER_OK;
}

static bool decode_labels(
	const struct v90_pcm_frame_decoder *decoder,
	const uint8_t *ucodes,
	uint8_t labels[V90_CP_FRAME_INTERVALS])
{
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (v90_pcm_mapping_label(
			    &decoder->mapping, decoder->decision_plane,
			    interval, ucodes[interval], &labels[interval]) !=
		    V90_PCM_MAPPING_OK)
			return false;
	}
	return true;
}

enum v90_pcm_frame_decoder_result v90_pcm_frame_decoder_decode(
	struct v90_pcm_frame_decoder *decoder,
	const uint8_t ucodes[V90_CP_FRAME_INTERVALS],
	const uint8_t signs[V90_CP_FRAME_INTERVALS], size_t symbol_count,
	struct v90_pcm_frame_decode_output *output)
{
	struct v90_pcm_frame_decoder next;
	struct v90_pcm_frame_decode_output generated = { 0 };
	struct v90_sign_decode_output sign_output;
	uint8_t modulus_bits[V90_PCM_FRAME_MAX_BITS];
	uint8_t labels[V90_CP_FRAME_INTERVALS];
	enum v90_pcm_mapping_result mapping_result;

	if (decoder == NULL || ucodes == NULL || signs == NULL ||
	    output == NULL)
		return V90_PCM_FRAME_DECODER_INVALID_ARGUMENT;
	if (!v90_pcm_frame_decoder_is_valid(decoder))
		return V90_PCM_FRAME_DECODER_INVALID_STATE;
	if (symbol_count != V90_CP_FRAME_INTERVALS)
		return V90_PCM_FRAME_DECODER_INVALID_SYMBOLS;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (signs[interval] > 1)
			return V90_PCM_FRAME_DECODER_INVALID_SYMBOLS;
	}
	if (!decode_labels(decoder, ucodes, labels))
		return V90_PCM_FRAME_DECODER_INVALID_SYMBOLS;
	mapping_result = v90_pcm_mapping_decode_modulus(
		&decoder->mapping, labels, modulus_bits,
		decoder->mapping.modulus_bits);
	if (mapping_result == V90_PCM_MAPPING_OUT_OF_RANGE)
		return V90_PCM_FRAME_DECODER_INVALID_SYMBOLS;
	if (mapping_result != V90_PCM_MAPPING_OK)
		return V90_PCM_FRAME_DECODER_INTERNAL_ERROR;
	next = *decoder;
	if (v90_sign_decoder_decode(
		    &next.signs, signs, symbol_count, &sign_output) !=
	    V90_SIGN_DECODER_OK)
		return V90_PCM_FRAME_DECODER_INTERNAL_ERROR;
	if (!v90_pcm_frame_decoder_is_valid(&next) ||
	    sign_output.bit_count != decoder->mapping.sign_bits ||
	    sign_output.rule_count != decoder->mapping.sign_redundancy ||
	    decoder->mapping.frame_bits > V90_PCM_FRAME_MAX_BITS)
		return V90_PCM_FRAME_DECODER_INTERNAL_ERROR;
	memcpy(generated.bits, sign_output.bits,
	       decoder->mapping.sign_bits);
	memcpy(generated.bits + decoder->mapping.sign_bits,
	       modulus_bits, decoder->mapping.modulus_bits);
	memcpy(generated.rules, sign_output.rules,
	       sign_output.rule_count * sizeof(sign_output.rules[0]));
	generated.bit_count = decoder->mapping.frame_bits;
	generated.rule_count = sign_output.rule_count;
	*decoder = next;
	*output = generated;
	return V90_PCM_FRAME_DECODER_OK;
}

const char *v90_pcm_frame_decoder_result_name(
	enum v90_pcm_frame_decoder_result result)
{
	switch (result) {
	case V90_PCM_FRAME_DECODER_OK:
		return "ok";
	case V90_PCM_FRAME_DECODER_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PCM_FRAME_DECODER_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_PCM_FRAME_DECODER_INVALID_STATE:
		return "invalid state";
	case V90_PCM_FRAME_DECODER_INVALID_SYMBOLS:
		return "invalid symbols";
	case V90_PCM_FRAME_DECODER_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
