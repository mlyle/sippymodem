// SPDX-License-Identifier: BSD-3-Clause

#include "v34_mapping_frame_transmitter.h"

#include <stddef.h>

static enum v34_mapping_frame_transmitter_result
mapping_encoder_result(
	enum v34_mapping_frame_encoder_result result)
{
	if (result == V34_MAPPING_FRAME_ENCODER_OUT_OF_RANGE)
		return V34_MAPPING_FRAME_TRANSMITTER_OUT_OF_RANGE;
	return V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT;
}

static enum v34_mapping_frame_transmitter_result
project_mapping_frame(
	const struct v34_encoded_mapping_frame *encoded,
	enum v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude,
	struct qam_point
		projected[V34_SYMBOLS_PER_MAPPING_FRAME])
{
	for (size_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		for (size_t symbol = 0;
		     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			size_t index =
				(size_t)interval *
					V34_2D_SYMBOLS_PER_4D_INTERVAL +
				symbol;
			enum v34_nonlinear_result result =
				v34_nonlinear_encode(
					nonlinear_parameter,
					mean_squared_magnitude,
					&encoded->interval[interval]
						 .precoded[symbol]
						 .precoded_symbol,
					&projected[index]);

			if (result ==
			    V34_NONLINEAR_NUMERIC_FAILURE)
				return V34_MAPPING_FRAME_TRANSMITTER_NUMERIC_FAILURE;
			if (result != V34_NONLINEAR_OK)
				return V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT;
		}
	}
	return V34_MAPPING_FRAME_TRANSMITTER_OK;
}

static enum v34_mapping_frame_transmitter_result
transmit_mapping_frame(
	struct v34_frame_schedule *schedule,
	struct v34_scrambler *scrambler,
	struct v34_4d_encoder *mapping_encoder,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit,
	enum v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude,
	struct v34_transmitted_mapping_frame *transmitted, bool project)
{
	struct v34_frame_schedule next_schedule;
	struct v34_scrambler next_scrambler;
	struct v34_4d_encoder next_encoder;
	struct v34_transmitted_mapping_frame generated = { 0 };
	uint8_t scrambled_primary_bits[
		V34_MAX_MAPPING_FRAME_BITS];
	enum v34_frame_schedule_result schedule_result;

	if (!v34_frame_schedule_is_valid(schedule) ||
	    !v34_scrambler_is_valid(scrambler) ||
	    !v34_4d_encoder_is_valid(mapping_encoder) ||
	    schedule->geometry.high_mapping_frame_bits !=
		    mapping_encoder->parameters.mapping_frame_bits ||
	    !v34_nonlinear_configuration_is_valid(
		    nonlinear_parameter, mean_squared_magnitude) ||
	    transmitted == NULL)
		return V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT;
	next_schedule = *schedule;
	next_scrambler = *scrambler;
	next_encoder = *mapping_encoder;
	schedule_result = v34_frame_schedule_next(
		&next_schedule, &generated.mapping_frame);
	if (schedule_result == V34_FRAME_SCHEDULE_FRAME_COMPLETE)
		return V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE;
	if (schedule_result != V34_FRAME_SCHEDULE_OK ||
	    primary_bit_count != generated.mapping_frame.primary_bits ||
	    primary_bits == NULL)
		return V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT;
	if (v34_scramble_unpacked_bits(
		    &next_scrambler, primary_bits, primary_bit_count,
		    scrambled_primary_bits) != V34_FRAMING_OK)
		return V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT;
	{
		enum v34_mapping_frame_encoder_result result =
			v34_encode_mapping_frame(
				&next_encoder, &generated.mapping_frame,
				scrambled_primary_bits, primary_bit_count,
				auxiliary_bit, &generated.encoded);

		if (result != V34_MAPPING_FRAME_ENCODER_OK)
			return mapping_encoder_result(result);
	}
	if (project) {
		enum v34_mapping_frame_transmitter_result result =
			project_mapping_frame(
				&generated.encoded, nonlinear_parameter,
				mean_squared_magnitude,
				generated.projected_symbol);

		if (result != V34_MAPPING_FRAME_TRANSMITTER_OK)
			return result;
	}
	*schedule = next_schedule;
	*scrambler = next_scrambler;
	*mapping_encoder = next_encoder;
	*transmitted = generated;
	return V34_MAPPING_FRAME_TRANSMITTER_OK;
}

enum v34_mapping_frame_transmitter_result v34_transmit_mapping_frame(
	struct v34_frame_schedule *schedule, struct v34_scrambler *scrambler,
	struct v34_4d_encoder *encoder, const uint8_t *primary_bits,
	size_t primary_bit_count, const uint8_t *auxiliary_bit,
	enum v34_nonlinear_parameter parameter, double mean_squared_magnitude,
	struct v34_transmitted_mapping_frame *transmitted)
{
	return transmit_mapping_frame(schedule, scrambler, encoder, primary_bits,
		primary_bit_count, auxiliary_bit, parameter, mean_squared_magnitude,
		transmitted, true);
}

enum v34_mapping_frame_transmitter_result v34_transmit_mapping_frame_q7(
	struct v34_frame_schedule *schedule, struct v34_scrambler *scrambler,
	struct v34_4d_encoder *encoder, const uint8_t *primary_bits,
	size_t primary_bit_count, const uint8_t *auxiliary_bit,
	struct v34_transmitted_mapping_frame *transmitted)
{
	return transmit_mapping_frame(schedule, scrambler, encoder, primary_bits,
		primary_bit_count, auxiliary_bit, V34_NONLINEAR_THETA_ZERO, 1.0,
		transmitted, false);
}
