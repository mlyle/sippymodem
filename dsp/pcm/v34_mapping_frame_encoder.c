// SPDX-License-Identifier: BSD-3-Clause

#include "v34_mapping_frame_encoder.h"

#include <stddef.h>

_Static_assert(
	(unsigned int)V34_RING_INDICES_PER_4D_INTERVAL ==
		(unsigned int)V34_2D_SYMBOLS_PER_4D_INTERVAL,
	"ring and symbol counts must match");

static enum v34_mapping_frame_encoder_result encode_interval(
	struct v34_4d_encoder *encoder,
	const struct v34_mapping_frame *mapping_frame,
	const struct v34_parsed_mapping_frame *parsed,
	const struct v34_ring_indices *ring_indices,
	uint8_t interval,
	struct v34_4d_encoder_output *output)
{
	const struct v34_4d_encoder_input input = {
		.mapping = parsed->interval[interval],
		.ring_index = {
			ring_indices->index[interval][0],
			ring_indices->index[interval][1],
		},
		.inversion_bit = (uint8_t)(
			(mapping_frame->inversion_4d_mask >> interval) & 1u),
	};
	enum v34_4d_encoder_result result =
		v34_4d_encoder_step(encoder, &input, output);

	switch (result) {
	case V34_4D_ENCODER_OK:
		return V34_MAPPING_FRAME_ENCODER_OK;
	case V34_4D_ENCODER_OUT_OF_RANGE:
		return V34_MAPPING_FRAME_ENCODER_OUT_OF_RANGE;
	case V34_4D_ENCODER_INVALID_ARGUMENT:
		break;
	}
	return V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT;
}

enum v34_mapping_frame_encoder_result
v34_encode_mapping_frame(
	struct v34_4d_encoder *encoder,
	const struct v34_mapping_frame *mapping_frame,
	const uint8_t *scrambled_primary_bits,
	size_t scrambled_primary_bit_count, const uint8_t *auxiliary_bit,
	struct v34_encoded_mapping_frame *encoded)
{
	struct v34_4d_encoder next;
	struct v34_encoded_mapping_frame generated = { 0 };

	if (!v34_4d_encoder_is_valid(encoder) ||
	    mapping_frame == NULL ||
	    mapping_frame->inversion_4d_mask >=
		    (1u << V34_4D_INTERVALS_PER_MAPPING_FRAME) ||
	    encoded == NULL)
		return V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT;
	next = *encoder;
	if (v34_parse_mapping_frame(
		    &next.parameters, mapping_frame, scrambled_primary_bits,
		    scrambled_primary_bit_count, auxiliary_bit,
		    &generated.parsed) != V34_PARSER_OK ||
	    v34_shell_map(
		    &next.parameters, generated.parsed.shell_label,
		    &generated.ring_indices) != V34_SHELL_MAPPER_OK)
		return V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT;
	for (uint8_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		enum v34_mapping_frame_encoder_result result =
			encode_interval(
				&next, mapping_frame, &generated.parsed,
				&generated.ring_indices, interval,
				&generated.interval[interval]);

		if (result != V34_MAPPING_FRAME_ENCODER_OK)
			return result;
	}
	*encoder = next;
	*encoded = generated;
	return V34_MAPPING_FRAME_ENCODER_OK;
}

const char *v34_mapping_frame_encoder_result_name(
	enum v34_mapping_frame_encoder_result result)
{
	switch (result) {
	case V34_MAPPING_FRAME_ENCODER_OK:
		return "ok";
	case V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_MAPPING_FRAME_ENCODER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
