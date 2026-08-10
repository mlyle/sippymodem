// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_mapping_frame_encoder.h"

#include <stddef.h>

_Static_assert(
	(unsigned int)SIPPYMODEM_V34_RING_INDICES_PER_4D_INTERVAL ==
		(unsigned int)SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL,
	"ring and symbol counts must match");

static enum sippymodem_v34_mapping_frame_encoder_result encode_interval(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_mapping_frame *mapping_frame,
	const struct sippymodem_v34_parsed_mapping_frame *parsed,
	const struct sippymodem_v34_ring_indices *ring_indices,
	uint8_t interval,
	struct sippymodem_v34_4d_encoder_output *output)
{
	const struct sippymodem_v34_4d_encoder_input input = {
		.mapping = parsed->interval[interval],
		.ring_index = {
			ring_indices->index[interval][0],
			ring_indices->index[interval][1],
		},
		.inversion_bit = (uint8_t)(
			(mapping_frame->inversion_4d_mask >> interval) & 1u),
	};
	enum sippymodem_v34_4d_encoder_result result =
		sippymodem_v34_4d_encoder_step(encoder, &input, output);

	switch (result) {
	case SIPPYMODEM_V34_4D_ENCODER_OK:
		return SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK;
	case SIPPYMODEM_V34_4D_ENCODER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OUT_OF_RANGE;
	case SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT;
}

enum sippymodem_v34_mapping_frame_encoder_result
sippymodem_v34_encode_mapping_frame(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_mapping_frame *mapping_frame,
	const uint8_t *scrambled_primary_bits,
	size_t scrambled_primary_bit_count, const uint8_t *auxiliary_bit,
	struct sippymodem_v34_encoded_mapping_frame *encoded)
{
	struct sippymodem_v34_4d_encoder next;
	struct sippymodem_v34_encoded_mapping_frame generated = { 0 };

	if (!sippymodem_v34_4d_encoder_is_valid(encoder) ||
	    mapping_frame == NULL ||
	    mapping_frame->inversion_4d_mask >=
		    (1u << SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME) ||
	    encoded == NULL)
		return SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT;
	next = *encoder;
	if (sippymodem_v34_parse_mapping_frame(
		    &next.parameters, mapping_frame, scrambled_primary_bits,
		    scrambled_primary_bit_count, auxiliary_bit,
		    &generated.parsed) != SIPPYMODEM_V34_PARSER_OK ||
	    sippymodem_v34_shell_map(
		    &next.parameters, generated.parsed.shell_label,
		    &generated.ring_indices) != SIPPYMODEM_V34_SHELL_MAPPER_OK)
		return SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT;
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		enum sippymodem_v34_mapping_frame_encoder_result result =
			encode_interval(
				&next, mapping_frame, &generated.parsed,
				&generated.ring_indices, interval,
				&generated.interval[interval]);

		if (result != SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK)
			return result;
	}
	*encoder = next;
	*encoded = generated;
	return SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK;
}

const char *sippymodem_v34_mapping_frame_encoder_result_name(
	enum sippymodem_v34_mapping_frame_encoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK:
		return "ok";
	case SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
