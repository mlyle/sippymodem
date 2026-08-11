// SPDX-License-Identifier: BSD-3-Clause

#include "v34_4d_encoder.h"

#include <stddef.h>

static enum sippymodem_v34_precoder_grid_step expected_grid_step(
	const struct sippymodem_v34_mapping_parameters *parameters)
{
	return parameters->mapping_frame_bits < 56u ?
		SIPPYMODEM_V34_PRECODER_GRID_STEP_2 :
		SIPPYMODEM_V34_PRECODER_GRID_STEP_4;
}

bool sippymodem_v34_4d_encoder_is_valid(
	const struct sippymodem_v34_4d_encoder *encoder)
{
	return encoder != NULL &&
	       sippymodem_v34_mapping_parameters_is_valid(
		       &encoder->parameters) &&
	       sippymodem_v34_precoder_is_valid(&encoder->precoder) &&
	       encoder->precoder.grid_step ==
		       expected_grid_step(&encoder->parameters) &&
	       sippymodem_v34_convolutional_encoder_is_valid(
		       &encoder->convolutional_encoder) &&
	       encoder->previous_differential_output < 4u;
}

enum sippymodem_v34_4d_encoder_result sippymodem_v34_4d_encoder_init(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection trellis_selection)
{
	struct sippymodem_v34_4d_encoder initialized = { 0 };

	if (encoder == NULL ||
	    !sippymodem_v34_mapping_parameters_is_valid(parameters))
		return SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT;
	initialized.parameters = *parameters;
	if (sippymodem_v34_precoder_init(
		    &initialized.precoder, parameters, coefficients) !=
		    SIPPYMODEM_V34_PRECODER_OK ||
	    sippymodem_v34_convolutional_encoder_init(
		    &initialized.convolutional_encoder, trellis_selection) !=
		    SIPPYMODEM_V34_TRELLIS_OK)
		return SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT;
	*encoder = initialized;
	return SIPPYMODEM_V34_4D_ENCODER_OK;
}

bool sippymodem_v34_4d_encoder_input_is_valid(
	const struct sippymodem_v34_4d_encoder_input *input,
	const struct sippymodem_v34_mapping_parameters *parameters)
{
	if (input == NULL || input->mapping.rotation_bit > 1u ||
	    input->mapping.differential_input > 3u || input->inversion_bit > 1u)
		return false;
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		const struct sippymodem_v34_2d_mapping_input mapping = {
			.ring_index = input->ring_index[symbol],
			.point_index = input->mapping.point_index[symbol],
		};

		if (!sippymodem_v34_2d_mapping_input_is_valid(
			    &mapping, parameters))
			return false;
	}
	return true;
}

static enum sippymodem_v34_4d_encoder_result precoder_result(
	enum sippymodem_v34_precoder_result result)
{
	return result == SIPPYMODEM_V34_PRECODER_OUT_OF_RANGE ?
		SIPPYMODEM_V34_4D_ENCODER_OUT_OF_RANGE :
		SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT;
}

static enum sippymodem_v34_4d_encoder_result map_first_symbol(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_4d_encoder_input *input,
	struct sippymodem_v34_4d_encoder_output *output)
{
	const struct sippymodem_v34_2d_mapping_input mapping = {
		.ring_index = input->ring_index[0],
		.point_index = input->mapping.point_index[0],
		.clockwise_quarter_turns = output->differential_output,
	};
	enum sippymodem_v34_precoder_result result;

	if (sippymodem_v34_map_2d_symbol(
		    &encoder->parameters, &mapping, &output->mapped[0]) !=
	    SIPPYMODEM_V34_MAPPER_OK)
		return SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT;
	result = sippymodem_v34_precoder_step(
		&encoder->precoder, &output->mapped[0].mapper_output,
		&output->precoded[0]);
	return result == SIPPYMODEM_V34_PRECODER_OK ?
		SIPPYMODEM_V34_4D_ENCODER_OK :
		precoder_result(result);
}

static enum sippymodem_v34_4d_encoder_result prepare_second_symbol(
	const struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_4d_encoder_input *input,
	struct sippymodem_v34_precoder_prediction *prediction,
	struct sippymodem_v34_4d_encoder_output *output)
{
	enum sippymodem_v34_precoder_result result =
		sippymodem_v34_precoder_predict(&encoder->precoder, prediction);

	if (result != SIPPYMODEM_V34_PRECODER_OK)
		return precoder_result(result);
	if (sippymodem_v34_modulo_encoder_output(
		    &output->precoded[0].lattice_offset,
		    &prediction->lattice_offset, &output->modulo_output) !=
		    SIPPYMODEM_V34_TRELLIS_OK ||
	    sippymodem_v34_convolutional_encoder_output(
		    &encoder->convolutional_encoder,
		    &output->convolutional_output) != SIPPYMODEM_V34_TRELLIS_OK)
		return SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT;
	output->trellis_output =
		output->modulo_output ^ output->convolutional_output ^
		input->inversion_bit;
	return SIPPYMODEM_V34_4D_ENCODER_OK;
}

static enum sippymodem_v34_4d_encoder_result map_second_symbol(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_4d_encoder_input *input,
	struct sippymodem_v34_4d_encoder_output *output)
{
	const struct sippymodem_v34_2d_mapping_input mapping = {
		.ring_index = input->ring_index[1],
		.point_index = input->mapping.point_index[1],
		.clockwise_quarter_turns = (uint8_t)(
			(output->differential_output +
			 2u * input->mapping.rotation_bit +
			 output->trellis_output) &
			3u),
	};
	enum sippymodem_v34_precoder_result result;

	if (sippymodem_v34_map_2d_symbol(
		    &encoder->parameters, &mapping, &output->mapped[1]) !=
	    SIPPYMODEM_V34_MAPPER_OK)
		return SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT;
	result = sippymodem_v34_precoder_step(
		&encoder->precoder, &output->mapped[1].mapper_output,
		&output->precoded[1]);
	return result == SIPPYMODEM_V34_PRECODER_OK ?
		SIPPYMODEM_V34_4D_ENCODER_OK :
		precoder_result(result);
}

static bool predictions_are_equal(
	const struct sippymodem_v34_precoder_prediction *prediction,
	const struct sippymodem_v34_precoder_symbol *symbol)
{
	return prediction->filter_output.in_phase ==
		       symbol->filter_output.in_phase &&
	       prediction->filter_output.quadrature ==
		       symbol->filter_output.quadrature &&
	       prediction->lattice_offset.in_phase ==
		       symbol->lattice_offset.in_phase &&
	       prediction->lattice_offset.quadrature ==
		       symbol->lattice_offset.quadrature;
}

static enum sippymodem_v34_4d_encoder_result advance_trellis(
	struct sippymodem_v34_4d_encoder *encoder,
	struct sippymodem_v34_4d_encoder_output *output)
{
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (sippymodem_v34_grid_subset_label(
			    &output->precoded[symbol].channel_output,
			    &output->subset_label[symbol]) !=
		    SIPPYMODEM_V34_TRELLIS_OK)
			return SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT;
	}
	if (sippymodem_v34_convolutional_input_from_subset_labels(
		    output->subset_label[0], output->subset_label[1],
		    &output->convolutional_input) != SIPPYMODEM_V34_TRELLIS_OK ||
	    sippymodem_v34_convolutional_encoder_advance(
		    &encoder->convolutional_encoder,
		    &output->convolutional_input) != SIPPYMODEM_V34_TRELLIS_OK)
		return SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT;
	return SIPPYMODEM_V34_4D_ENCODER_OK;
}

enum sippymodem_v34_4d_encoder_result sippymodem_v34_4d_encoder_step(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_4d_encoder_input *input,
	struct sippymodem_v34_4d_encoder_output *output)
{
	struct sippymodem_v34_4d_encoder next;
	struct sippymodem_v34_4d_encoder_output generated = { 0 };
	struct sippymodem_v34_precoder_prediction second_prediction;
	enum sippymodem_v34_4d_encoder_result result;

	if (!sippymodem_v34_4d_encoder_is_valid(encoder) || output == NULL ||
	    !sippymodem_v34_4d_encoder_input_is_valid(
		    input, &encoder->parameters))
		return SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT;
	next = *encoder;
	generated.differential_output = (uint8_t)(
		(next.previous_differential_output +
		 input->mapping.differential_input) &
		3u);
	next.previous_differential_output = generated.differential_output;
	result = map_first_symbol(&next, input, &generated);
	if (result != SIPPYMODEM_V34_4D_ENCODER_OK)
		return result;
	result = prepare_second_symbol(
		&next, input, &second_prediction, &generated);
	if (result != SIPPYMODEM_V34_4D_ENCODER_OK)
		return result;
	result = map_second_symbol(&next, input, &generated);
	if (result != SIPPYMODEM_V34_4D_ENCODER_OK)
		return result;
	if (!predictions_are_equal(&second_prediction, &generated.precoded[1]))
		return SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT;
	result = advance_trellis(&next, &generated);
	if (result != SIPPYMODEM_V34_4D_ENCODER_OK)
		return result;
	*encoder = next;
	*output = generated;
	return SIPPYMODEM_V34_4D_ENCODER_OK;
}

const char *sippymodem_v34_4d_encoder_result_name(
	enum sippymodem_v34_4d_encoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_4D_ENCODER_OK:
		return "ok";
	case SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_4D_ENCODER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
