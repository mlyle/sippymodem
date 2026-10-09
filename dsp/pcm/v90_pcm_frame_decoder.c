// SPDX-License-Identifier: BSD-3-Clause

#include "v90_pcm_frame_decoder.h"

#include "memory_region.h"
#include "packed_bits.h"

#include <string.h>

bool v90_pcm_frame_decoder_is_valid(
	const struct v90_pcm_frame_decoder *decoder)
{
	return decoder != NULL &&
	       v90_pcm_mapping_is_valid(&decoder->mapping) &&
	       v90_sign_decoder_is_valid(&decoder->signs) &&
	       decoder->signs.sign_redundancy ==
		       decoder->mapping.sign_redundancy &&
	       v90_pcm_mapping_plane_is_valid(decoder->decision_plane);
}

enum v90_pcm_frame_decoder_result v90_pcm_frame_decoder_init(
	struct v90_pcm_frame_decoder *decoder,
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane decision_plane)
{
	struct v90_pcm_frame_decoder initialized = { 0 };

	if (decoder == NULL || mapping == NULL ||
	    !v90_pcm_mapping_plane_is_valid(decision_plane))
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
	const struct pcm_constellation *constellations =
		decoder->decision_plane == V90_PCM_MAPPING_CODEC ?
			decoder->mapping.codec : decoder->mapping.transmit;

	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (!pcm_constellation_descending_rank(
			    &constellations[interval], ucodes[interval],
			    &labels[interval]))
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
	struct v90_sign_decoder next_signs;
	struct v90_pcm_frame_decode_output generated = { 0 };
	struct v90_sign_decode_output sign_output;
	uint8_t modulus_bits[PCM_FRAME_MAX_BITS];
	uint8_t labels[V90_CP_FRAME_INTERVALS];
	enum v90_pcm_mapping_result mapping_result;

	if (decoder == NULL || ucodes == NULL || signs == NULL ||
	    output == NULL || memory_regions_overlap(
		    decoder, sizeof(*decoder), output, sizeof(*output)))
		return V90_PCM_FRAME_DECODER_INVALID_ARGUMENT;
	if (!v90_pcm_frame_decoder_is_valid(decoder))
		return V90_PCM_FRAME_DECODER_INVALID_STATE;
	if (symbol_count != V90_CP_FRAME_INTERVALS)
		return V90_PCM_FRAME_DECODER_INVALID_SYMBOLS;
	if (!unpacked_bits_are_binary(signs, symbol_count))
		return V90_PCM_FRAME_DECODER_INVALID_SYMBOLS;
	if (!decode_labels(decoder, ucodes, labels))
		return V90_PCM_FRAME_DECODER_INVALID_SYMBOLS;
	mapping_result = v90_pcm_mapping_decode_modulus(
		&decoder->mapping, labels, modulus_bits,
		decoder->mapping.modulus_bits);
	if (mapping_result == V90_PCM_MAPPING_OUT_OF_RANGE)
		return V90_PCM_FRAME_DECODER_INVALID_SYMBOLS;
	if (mapping_result != V90_PCM_MAPPING_OK)
		return V90_PCM_FRAME_DECODER_INTERNAL_ERROR;
	next_signs = decoder->signs;
	if (v90_sign_decoder_decode(
		    &next_signs, signs, symbol_count, &sign_output) !=
	    V90_SIGN_DECODER_OK)
		return V90_PCM_FRAME_DECODER_INTERNAL_ERROR;
	if (sign_output.bit_count != decoder->mapping.sign_bits ||
	    sign_output.rule_count != decoder->mapping.sign_redundancy)
		return V90_PCM_FRAME_DECODER_INTERNAL_ERROR;
	memcpy(generated.bits, sign_output.bits,
	       decoder->mapping.sign_bits);
	memcpy(generated.bits + decoder->mapping.sign_bits,
	       modulus_bits, decoder->mapping.modulus_bits);
	memcpy(generated.rules, sign_output.rules,
	       sign_output.rule_count * sizeof(sign_output.rules[0]));
	generated.bit_count = decoder->mapping.frame_bits;
	generated.rule_count = sign_output.rule_count;
	decoder->signs = next_signs;
	*output = generated;
	return V90_PCM_FRAME_DECODER_OK;
}
