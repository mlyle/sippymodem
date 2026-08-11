// SPDX-License-Identifier: BSD-3-Clause

#include "v34_mapping_frame_decoder.h"

#include "v34_parser.h"
#include "v34_shell_mapper.h"

#include <stddef.h>

static bool mapping_frame_is_bounded(
	const struct sippymodem_v34_mapping_frame *mapping_frame)
{
	return mapping_frame != NULL &&
	       mapping_frame->inversion_4d_mask <
		       (1u << SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME);
}

static bool decode_intervals(
	struct sippymodem_v34_4d_decoder *decoder,
	const struct sippymodem_v34_decided_4d_interval
		decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct sippymodem_v34_parsed_mapping_frame *parsed,
	struct sippymodem_v34_ring_indices *rings)
{
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		struct sippymodem_v34_decoded_4d_interval decoded;

		if (sippymodem_v34_decode_4d_interval(
			    decoder, &decided[interval], &decoded) !=
		    SIPPYMODEM_V34_4D_DECODER_OK)
			return false;
		parsed->interval[interval] = decoded.mapping;
		for (uint8_t symbol = 0;
		     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol)
			rings->index[interval][symbol] =
				decoded.ring_index[symbol];
	}
	return true;
}

enum sippymodem_v34_mapping_frame_decoder_result
sippymodem_v34_decode_mapping_frame(
	struct sippymodem_v34_4d_decoder *decoder,
	const struct sippymodem_v34_mapping_frame *mapping_frame,
	const struct sippymodem_v34_decided_4d_interval
		decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct sippymodem_v34_recovered_mapping_frame *recovered)
{
	struct sippymodem_v34_4d_decoder next;
	struct sippymodem_v34_recovered_mapping_frame generated = { 0 };
	struct sippymodem_v34_parsed_mapping_frame parsed = { 0 };
	struct sippymodem_v34_ring_indices rings = { 0 };
	uint8_t *auxiliary_bit;

	if (!sippymodem_v34_4d_decoder_is_valid(decoder) ||
	    !mapping_frame_is_bounded(mapping_frame) || decided == NULL ||
	    recovered == NULL)
		return SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT;
	next = *decoder;
	if (!decode_intervals(&next, decided, &parsed, &rings) ||
	    sippymodem_v34_shell_unmap(
		    &next.parameters, &rings, &parsed.shell_label) !=
		    SIPPYMODEM_V34_SHELL_MAPPER_OK)
		return SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT;
	generated.primary_bit_count = mapping_frame->primary_bits;
	generated.auxiliary_bit_count =
		(uint8_t)mapping_frame->carries_auxiliary_bit;
	auxiliary_bit = mapping_frame->carries_auxiliary_bit ?
		&generated.auxiliary_bit :
		NULL;
	if (sippymodem_v34_recover_mapping_frame_bits(
		    &next.parameters, mapping_frame, &parsed,
		    generated.primary_bits, generated.primary_bit_count,
		    auxiliary_bit) != SIPPYMODEM_V34_PARSER_OK)
		return SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT;
	*decoder = next;
	*recovered = generated;
	return SIPPYMODEM_V34_MAPPING_FRAME_DECODER_OK;
}

const char *sippymodem_v34_mapping_frame_decoder_result_name(
	enum sippymodem_v34_mapping_frame_decoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_MAPPING_FRAME_DECODER_OK:
		return "ok";
	case SIPPYMODEM_V34_MAPPING_FRAME_DECODER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
