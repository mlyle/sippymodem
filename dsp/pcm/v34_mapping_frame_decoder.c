// SPDX-License-Identifier: BSD-3-Clause

#include "v34_mapping_frame_decoder.h"

#include "v34_parser.h"
#include "v34_shell_mapper.h"

#include <stddef.h>

static bool mapping_frame_is_bounded(
	const struct v34_mapping_frame *mapping_frame)
{
	return mapping_frame != NULL &&
	       mapping_frame->inversion_4d_mask <
		       (1u << V34_4D_INTERVALS_PER_MAPPING_FRAME);
}

static bool decode_intervals(
	struct v34_4d_decoder *decoder,
	const struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct v34_parsed_mapping_frame *parsed,
	struct v34_ring_indices *rings)
{
	for (uint8_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		struct v34_decoded_4d_interval decoded;

		if (v34_decode_4d_interval(
			    decoder, &decided[interval], &decoded) !=
		    V34_4D_DECODER_OK)
			return false;
		parsed->interval[interval] = decoded.mapping;
		for (uint8_t symbol = 0;
		     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol)
			rings->index[interval][symbol] =
				decoded.ring_index[symbol];
	}
	return true;
}

enum v34_mapping_frame_decoder_result
v34_decode_mapping_frame(
	struct v34_4d_decoder *decoder,
	const struct v34_mapping_frame *mapping_frame,
	const struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct v34_recovered_mapping_frame *recovered)
{
	struct v34_4d_decoder next;
	struct v34_recovered_mapping_frame generated = { 0 };
	struct v34_parsed_mapping_frame parsed = { 0 };
	struct v34_ring_indices rings = { 0 };
	uint8_t *auxiliary_bit;
	uint8_t shell_bits;
	bool exact_shell_decision;

	if (!v34_4d_decoder_is_valid(decoder) ||
	    !mapping_frame_is_bounded(mapping_frame) || decided == NULL ||
	    recovered == NULL)
		return V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT;
	next = *decoder;
	shell_bits = next.parameters.shell_mapper_bits;
	if (!mapping_frame->high_frame && shell_bits != 0)
		shell_bits--;
	if (!decode_intervals(&next, decided, &parsed, &rings) ||
	    v34_shell_unmap_decision(
		    &next.parameters, &rings, shell_bits, &parsed.shell_label,
		    &exact_shell_decision) != V34_SHELL_MAPPER_OK)
		return V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT;
	generated.primary_bit_count = mapping_frame->primary_bits;
	generated.shell_decision_exact = exact_shell_decision;
	generated.auxiliary_bit_count =
		(uint8_t)mapping_frame->carries_auxiliary_bit;
	auxiliary_bit = mapping_frame->carries_auxiliary_bit ?
		&generated.auxiliary_bit :
		NULL;
	if (v34_recover_mapping_frame_bits(
		    &next.parameters, mapping_frame, &parsed,
		    generated.primary_bits, generated.primary_bit_count,
		    auxiliary_bit) != V34_PARSER_OK)
		return V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT;
	*decoder = next;
	*recovered = generated;
	return V34_MAPPING_FRAME_DECODER_OK;
}

const char *v34_mapping_frame_decoder_result_name(
	enum v34_mapping_frame_decoder_result result)
{
	switch (result) {
	case V34_MAPPING_FRAME_DECODER_OK:
		return "ok";
	case V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
