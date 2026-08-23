// SPDX-License-Identifier: BSD-3-Clause

#include "v34_4d_encoder.h"

#include <stddef.h>

static enum v34_precoder_grid_step expected_grid_step(
	const struct v34_mapping_parameters *parameters)
{
	return parameters->mapping_frame_bits < 56u ?
		V34_PRECODER_GRID_STEP_2 :
		V34_PRECODER_GRID_STEP_4;
}

bool v34_4d_encoder_is_valid(
	const struct v34_4d_encoder *encoder)
{
	return encoder != NULL &&
	       v34_mapping_parameters_is_valid(
		       &encoder->parameters) &&
	       v34_precoder_is_valid(&encoder->precoder) &&
	       encoder->precoder.grid_step ==
		       expected_grid_step(&encoder->parameters) &&
	       v34_convolutional_encoder_is_valid(
		       &encoder->convolutional_encoder) &&
	       encoder->previous_differential_output < 4u;
}

enum v34_4d_encoder_result v34_4d_encoder_init(
	struct v34_4d_encoder *encoder,
	const struct v34_mapping_parameters *parameters,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection trellis_selection)
{
	struct v34_4d_encoder initialized = { 0 };

	if (encoder == NULL ||
	    !v34_mapping_parameters_is_valid(parameters))
		return V34_4D_ENCODER_INVALID_ARGUMENT;
	initialized.parameters = *parameters;
	if (v34_precoder_init(
		    &initialized.precoder, parameters, coefficients) !=
		    V34_PRECODER_OK ||
	    v34_convolutional_encoder_init(
		    &initialized.convolutional_encoder, trellis_selection) !=
		    V34_TRELLIS_OK)
		return V34_4D_ENCODER_INVALID_ARGUMENT;
	*encoder = initialized;
	return V34_4D_ENCODER_OK;
}

bool v34_4d_encoder_input_is_valid(
	const struct v34_4d_encoder_input *input,
	const struct v34_mapping_parameters *parameters)
{
	if (input == NULL || input->mapping.rotation_bit > 1u ||
	    input->mapping.differential_input > 3u || input->inversion_bit > 1u)
		return false;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		const struct v34_2d_mapping_input mapping = {
			.ring_index = input->ring_index[symbol],
			.point_index = input->mapping.point_index[symbol],
		};

		if (!v34_2d_mapping_input_is_valid(
			    &mapping, parameters))
			return false;
	}
	return true;
}

static enum v34_4d_encoder_result precoder_result(
	enum v34_precoder_result result)
{
	return result == V34_PRECODER_OUT_OF_RANGE ?
		V34_4D_ENCODER_OUT_OF_RANGE :
		V34_4D_ENCODER_INVALID_ARGUMENT;
}

static enum v34_4d_encoder_result map_first_symbol(
	struct v34_4d_encoder *encoder,
	const struct v34_4d_encoder_input *input,
	struct v34_4d_encoder_output *output)
{
	const struct v34_2d_mapping_input mapping = {
		.ring_index = input->ring_index[0],
		.point_index = input->mapping.point_index[0],
		.clockwise_quarter_turns = output->differential_output,
	};
	enum v34_precoder_result result;

	if (v34_map_2d_symbol(
		    &encoder->parameters, &mapping, &output->mapped[0]) !=
	    V34_MAPPER_OK)
		return V34_4D_ENCODER_INVALID_ARGUMENT;
	result = v34_precoder_step(
		&encoder->precoder, &output->mapped[0].mapper_output,
		&output->precoded[0]);
	return result == V34_PRECODER_OK ?
		V34_4D_ENCODER_OK :
		precoder_result(result);
}

static enum v34_4d_encoder_result prepare_second_symbol(
	const struct v34_4d_encoder *encoder,
	const struct v34_4d_encoder_input *input,
	struct v34_precoder_prediction *prediction,
	struct v34_4d_encoder_output *output)
{
	enum v34_precoder_result result =
		v34_precoder_predict(&encoder->precoder, prediction);

	if (result != V34_PRECODER_OK)
		return precoder_result(result);
	if (v34_modulo_encoder_output(
		    &output->precoded[0].lattice_offset,
		    &prediction->lattice_offset, &output->modulo_output) !=
		    V34_TRELLIS_OK ||
	    v34_convolutional_encoder_output(
		    &encoder->convolutional_encoder,
		    &output->convolutional_output) != V34_TRELLIS_OK)
		return V34_4D_ENCODER_INVALID_ARGUMENT;
	output->trellis_output =
		output->modulo_output ^ output->convolutional_output ^
		input->inversion_bit;
	return V34_4D_ENCODER_OK;
}

static enum v34_4d_encoder_result map_second_symbol(
	struct v34_4d_encoder *encoder,
	const struct v34_4d_encoder_input *input,
	struct v34_4d_encoder_output *output)
{
	const struct v34_2d_mapping_input mapping = {
		.ring_index = input->ring_index[1],
		.point_index = input->mapping.point_index[1],
		.clockwise_quarter_turns = (uint8_t)(
			(output->differential_output +
			 2u * input->mapping.rotation_bit +
			 output->trellis_output) &
			3u),
	};
	enum v34_precoder_result result;

	if (v34_map_2d_symbol(
		    &encoder->parameters, &mapping, &output->mapped[1]) !=
	    V34_MAPPER_OK)
		return V34_4D_ENCODER_INVALID_ARGUMENT;
	result = v34_precoder_step(
		&encoder->precoder, &output->mapped[1].mapper_output,
		&output->precoded[1]);
	return result == V34_PRECODER_OK ?
		V34_4D_ENCODER_OK :
		precoder_result(result);
}

static bool predictions_are_equal(
	const struct v34_precoder_prediction *prediction,
	const struct v34_precoder_symbol *symbol)
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

static enum v34_4d_encoder_result advance_trellis(
	struct v34_4d_encoder *encoder,
	struct v34_4d_encoder_output *output)
{
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (v34_grid_subset_label(
			    &output->precoded[symbol].channel_output,
			    &output->subset_label[symbol]) !=
		    V34_TRELLIS_OK)
			return V34_4D_ENCODER_INVALID_ARGUMENT;
	}
	if (v34_convolutional_input_from_subset_labels(
		    output->subset_label[0], output->subset_label[1],
		    &output->convolutional_input) != V34_TRELLIS_OK ||
	    v34_convolutional_encoder_advance(
		    &encoder->convolutional_encoder,
		    &output->convolutional_input) != V34_TRELLIS_OK)
		return V34_4D_ENCODER_INVALID_ARGUMENT;
	return V34_4D_ENCODER_OK;
}

enum v34_4d_encoder_result v34_4d_encoder_step(
	struct v34_4d_encoder *encoder,
	const struct v34_4d_encoder_input *input,
	struct v34_4d_encoder_output *output)
{
	struct v34_4d_encoder next;
	struct v34_4d_encoder_output generated = { 0 };
	struct v34_precoder_prediction second_prediction;
	enum v34_4d_encoder_result result;

	if (!v34_4d_encoder_is_valid(encoder) || output == NULL ||
	    !v34_4d_encoder_input_is_valid(
		    input, &encoder->parameters))
		return V34_4D_ENCODER_INVALID_ARGUMENT;
	next = *encoder;
	generated.differential_output = (uint8_t)(
		(next.previous_differential_output +
		 input->mapping.differential_input) &
		3u);
	next.previous_differential_output = generated.differential_output;
	result = map_first_symbol(&next, input, &generated);
	if (result != V34_4D_ENCODER_OK)
		return result;
	result = prepare_second_symbol(
		&next, input, &second_prediction, &generated);
	if (result != V34_4D_ENCODER_OK)
		return result;
	result = map_second_symbol(&next, input, &generated);
	if (result != V34_4D_ENCODER_OK)
		return result;
	if (!predictions_are_equal(&second_prediction, &generated.precoded[1]))
		return V34_4D_ENCODER_INVALID_ARGUMENT;
	result = advance_trellis(&next, &generated);
	if (result != V34_4D_ENCODER_OK)
		return result;
	*encoder = next;
	*output = generated;
	return V34_4D_ENCODER_OK;
}
