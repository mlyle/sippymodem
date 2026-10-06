// SPDX-License-Identifier: BSD-3-Clause

#include "v90_sign_encoder.h"
#include "v90_sign_history.h"

#include "packed_bits.h"

#include <string.h>

bool v90_sign_encoder_is_valid(
	const struct v90_sign_encoder *encoder)
{
	return encoder != NULL && v90_sign_history_is_valid(
		encoder->sign_redundancy, encoder->previous_odd,
		encoder->previous_pcm_sign, encoder->previous_shaping_signs);
}

enum v90_sign_encoder_result v90_sign_encoder_init(
	struct v90_sign_encoder *encoder, uint8_t sign_redundancy)
{
	struct v90_sign_encoder initialized = {
		.sign_redundancy = sign_redundancy,
	};

	if (encoder == NULL || sign_redundancy > 3)
		return V90_SIGN_ENCODER_INVALID_ARGUMENT;
	*encoder = initialized;
	return V90_SIGN_ENCODER_OK;
}

static void encode_unshaped(
	struct v90_sign_encoder *encoder, const uint8_t *bits,
	uint8_t signs[V90_CP_FRAME_INTERVALS])
{
	uint8_t previous = encoder->previous_pcm_sign;

	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		previous ^= bits[interval];
		signs[interval] = previous;
	}
	encoder->previous_pcm_sign = previous;
}

static void encode_initial_shaping_signs(
	struct v90_sign_encoder *encoder, const uint8_t *bits,
	uint8_t signs[V90_CP_FRAME_INTERVALS])
{
	size_t frame_count = encoder->sign_redundancy;
	size_t frame_length = V90_CP_FRAME_INTERVALS / frame_count;
	size_t input = 0;
	size_t output = 0;
	uint8_t previous_odd = encoder->previous_odd;

	for (size_t frame = 0; frame < frame_count; ++frame) {
		uint8_t differentially_encoded[V90_CP_FRAME_INTERVALS] = { 0 };

		for (size_t position = 1;
		     position < frame_length; ++position) {
			uint8_t value = bits[input++];

			if ((position & 1u) != 0) {
				value ^= previous_odd;
				previous_odd = value;
			}
			differentially_encoded[position] = value;
		}
		for (size_t position = 0;
		     position < frame_length; ++position) {
			uint8_t sign = differentially_encoded[position] ^
				       encoder->previous_shaping_signs[position];

			encoder->previous_shaping_signs[position] = sign;
			signs[output++] = sign;
		}
	}
	encoder->previous_odd = previous_odd;
}

enum v90_sign_encoder_result v90_sign_encoder_encode(
	struct v90_sign_encoder *encoder,
	const uint8_t *bits, size_t bit_count,
	uint8_t signs[V90_CP_FRAME_INTERVALS])
{
	struct v90_sign_encoder next;
	uint8_t encoded[V90_CP_FRAME_INTERVALS];
	size_t required;

	if (encoder == NULL || bits == NULL || signs == NULL)
		return V90_SIGN_ENCODER_INVALID_ARGUMENT;
	if (!v90_sign_encoder_is_valid(encoder))
		return V90_SIGN_ENCODER_INVALID_STATE;
	required = V90_CP_FRAME_INTERVALS - encoder->sign_redundancy;
	if (bit_count != required ||
	    !unpacked_bits_are_binary(bits, bit_count))
		return V90_SIGN_ENCODER_INVALID_BITS;
	next = *encoder;
	if (next.sign_redundancy == 0)
		encode_unshaped(&next, bits, encoded);
	else
		encode_initial_shaping_signs(&next, bits, encoded);
	if (!v90_sign_encoder_is_valid(&next))
		return V90_SIGN_ENCODER_INVALID_STATE;
	*encoder = next;
	memcpy(signs, encoded, sizeof(encoded));
	return V90_SIGN_ENCODER_OK;
}

const char *v90_sign_encoder_result_name(
	enum v90_sign_encoder_result result)
{
	switch (result) {
	case V90_SIGN_ENCODER_OK:
		return "ok";
	case V90_SIGN_ENCODER_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_SIGN_ENCODER_INVALID_STATE:
		return "invalid state";
	case V90_SIGN_ENCODER_INVALID_BITS:
		return "invalid bits";
	}
	return "unknown";
}
