// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_mapping_frame_transmitter.h"

#include <stddef.h>

static enum sippymodem_v34_mapping_frame_transmitter_result
mapping_encoder_result(
	enum sippymodem_v34_mapping_frame_encoder_result result)
{
	if (result == SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OUT_OF_RANGE)
		return SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OUT_OF_RANGE;
	return SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT;
}

static enum sippymodem_v34_mapping_frame_transmitter_result
project_mapping_frame(
	const struct sippymodem_v34_encoded_mapping_frame *encoded,
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude,
	struct sippymodem_qam_point
		projected[SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME])
{
	for (size_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		for (size_t symbol = 0;
		     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			size_t index =
				(size_t)interval *
					SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL +
				symbol;
			enum sippymodem_v34_nonlinear_result result =
				sippymodem_v34_nonlinear_encode(
					nonlinear_parameter,
					mean_squared_magnitude,
					&encoded->interval[interval]
						 .precoded[symbol]
						 .precoded_symbol,
					&projected[index]);

			if (result ==
			    SIPPYMODEM_V34_NONLINEAR_NUMERIC_FAILURE)
				return SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_NUMERIC_FAILURE;
			if (result != SIPPYMODEM_V34_NONLINEAR_OK)
				return SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT;
		}
	}
	return SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK;
}

enum sippymodem_v34_mapping_frame_transmitter_result
sippymodem_v34_transmit_mapping_frame(
	struct sippymodem_v34_frame_schedule *schedule,
	struct sippymodem_v34_scrambler *scrambler,
	struct sippymodem_v34_4d_encoder *mapping_encoder,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit,
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude,
	struct sippymodem_v34_transmitted_mapping_frame *transmitted)
{
	struct sippymodem_v34_frame_schedule next_schedule;
	struct sippymodem_v34_scrambler next_scrambler;
	struct sippymodem_v34_4d_encoder next_encoder;
	struct sippymodem_v34_transmitted_mapping_frame generated = { 0 };
	uint8_t scrambled_primary_bits[
		SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
	enum sippymodem_v34_frame_schedule_result schedule_result;

	if (!sippymodem_v34_frame_schedule_is_valid(schedule) ||
	    !sippymodem_v34_scrambler_is_valid(scrambler) ||
	    !sippymodem_v34_4d_encoder_is_valid(mapping_encoder) ||
	    schedule->geometry.high_mapping_frame_bits !=
		    mapping_encoder->parameters.mapping_frame_bits ||
	    !sippymodem_v34_nonlinear_configuration_is_valid(
		    nonlinear_parameter, mean_squared_magnitude) ||
	    transmitted == NULL)
		return SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT;
	next_schedule = *schedule;
	next_scrambler = *scrambler;
	next_encoder = *mapping_encoder;
	schedule_result = sippymodem_v34_frame_schedule_next(
		&next_schedule, &generated.mapping_frame);
	if (schedule_result == SIPPYMODEM_V34_FRAME_SCHEDULE_FRAME_COMPLETE)
		return SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE;
	if (schedule_result != SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    primary_bit_count != generated.mapping_frame.primary_bits ||
	    primary_bits == NULL)
		return SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT;
	for (size_t index = 0; index < primary_bit_count; ++index) {
		if (sippymodem_v34_scramble_group(
			    &next_scrambler, primary_bits[index], 1,
			    &scrambled_primary_bits[index]) !=
		    SIPPYMODEM_V34_FRAMING_OK)
			return SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT;
	}
	{
		enum sippymodem_v34_mapping_frame_encoder_result result =
			sippymodem_v34_encode_mapping_frame(
				&next_encoder, &generated.mapping_frame,
				scrambled_primary_bits, primary_bit_count,
				auxiliary_bit, &generated.encoded);

		if (result != SIPPYMODEM_V34_MAPPING_FRAME_ENCODER_OK)
			return mapping_encoder_result(result);
	}
	{
		enum sippymodem_v34_mapping_frame_transmitter_result result =
			project_mapping_frame(
				&generated.encoded, nonlinear_parameter,
				mean_squared_magnitude,
				generated.projected_symbol);

		if (result != SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK)
			return result;
	}
	*schedule = next_schedule;
	*scrambler = next_scrambler;
	*mapping_encoder = next_encoder;
	*transmitted = generated;
	return SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK;
}

const char *sippymodem_v34_mapping_frame_transmitter_result_name(
	enum sippymodem_v34_mapping_frame_transmitter_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK:
		return "ok";
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE:
		return "frame complete";
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OUT_OF_RANGE:
		return "out of range";
	case SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
