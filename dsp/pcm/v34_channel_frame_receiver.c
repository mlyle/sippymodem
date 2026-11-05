// SPDX-License-Identifier: BSD-3-Clause

#include "v34_channel_frame_receiver.h"

#include <stddef.h>
#include <string.h>

bool v34_channel_frame_receiver_is_valid(
	const struct v34_channel_frame_receiver *receiver)
{
	struct v34_precoder_prediction prediction;
	const struct v34_channel_decoder *decoder;

	if (receiver == NULL ||
	    !v34_channel_conditioner_is_valid(
		    &receiver->conditioner) ||
	    !v34_precoder_is_valid(&receiver->receive_precoder) ||
	    v34_precoder_predict(
		    &receiver->receive_precoder, &prediction) !=
		    V34_PRECODER_OK ||
	    !v34_channel_frame_decoder_is_valid(
		    &receiver->frame_decoder))
		return false;
	decoder = &receiver->frame_decoder.channel_decoder;
	return v34_mapping_parameters_are_equal(
		       &receiver->conditioner.normalizer.parameters,
		       &decoder->parameters) &&
	       receiver->receive_precoder.grid_step ==
		       decoder->inverse_precoder.grid_step &&
	       v34_precoder_coefficients_are_equal(
		       receiver->receive_precoder.coefficients,
		       decoder->inverse_precoder.coefficients);
}

enum v34_data_receive_result
v34_channel_frame_receiver_init(
	struct v34_channel_frame_receiver *receiver,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection)
{
	struct v34_channel_frame_receiver initialized = { 0 };

	if (receiver == NULL ||
	    v34_channel_conditioner_init(
		    &initialized.conditioner, normalizer) !=
		    V34_CHANNEL_CONDITIONER_OK ||
	    v34_precoder_init(
		    &initialized.receive_precoder,
		    &initialized.conditioner.normalizer.parameters,
		    coefficients) != V34_PRECODER_OK ||
	    v34_channel_frame_decoder_init(
		    &initialized.frame_decoder, geometry, expanded_shaping,
		    coefficients, selection) !=
		    V34_CHANNEL_FRAME_DECODER_OK ||
	    !v34_channel_frame_receiver_is_valid(&initialized))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_RECEIVE_OK;
}

static enum v34_data_receive_result
conditioner_result(enum v34_channel_conditioner_result result)
{
	return result == V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE ?
		V34_DATA_RECEIVE_NUMERIC_FAILURE :
		V34_DATA_RECEIVE_INVALID_ARGUMENT;
}

static enum v34_data_receive_result
precoder_result(enum v34_precoder_result result)
{
	return result == V34_PRECODER_OUT_OF_RANGE ?
		V34_DATA_RECEIVE_OUT_OF_RANGE :
		V34_DATA_RECEIVE_INVALID_ARGUMENT;
}

static enum v34_data_receive_result decoder_result(
	enum v34_channel_frame_decoder_result result)
{
	switch (result) {
	case V34_CHANNEL_FRAME_DECODER_NUMERIC_FAILURE:
		return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	case V34_CHANNEL_FRAME_DECODER_OUT_OF_RANGE:
		return V34_DATA_RECEIVE_OUT_OF_RANGE;
	case V34_CHANNEL_FRAME_DECODER_OK:
	case V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT:
		break;
	}
	return V34_DATA_RECEIVE_INVALID_ARGUMENT;
}

static enum v34_data_receive_result
correct_receive_history(
	struct v34_precoder *receive_precoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_survivor_decision *current,
	struct qam_point
		decision_targets[V34_2D_SYMBOLS_PER_4D_INTERVAL])
{
	for (size_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		struct v34_grid_point mapper_output;
		struct v34_precoder_symbol reconstructed;
		enum v34_precoder_result result =
			v34_precoder_inverse_step(
				receive_precoder,
				&current->channel_output[symbol],
				&mapper_output, &reconstructed);
		enum v34_nonlinear_result nonlinear_result;

		if (result != V34_PRECODER_OK)
			return precoder_result(result);
		nonlinear_result = v34_project_decision(normalizer,
			&reconstructed.precoded_symbol, &decision_targets[symbol]);
		if (nonlinear_result != V34_NONLINEAR_OK)
			return nonlinear_result == V34_NONLINEAR_NUMERIC_FAILURE ?
				V34_DATA_RECEIVE_NUMERIC_FAILURE :
				V34_DATA_RECEIVE_INVALID_ARGUMENT;
		if (!point_is_finite(&decision_targets[symbol]))
			return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	}
	return V34_DATA_RECEIVE_OK;
}

enum v34_data_receive_result
v34_channel_frame_receiver_advance(
	struct v34_channel_frame_receiver *receiver,
	const struct qam_point normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_survivor_decision *current,
	struct qam_point decision_targets[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_decided_4d_interval decided_frame[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready)
{
	struct v34_channel_frame_receiver next;
	struct v34_channel_frame_output output;
	enum v34_data_receive_result result;

	if (!v34_channel_frame_receiver_is_valid(receiver) ||
	    normalized == NULL || current == NULL || decision_targets == NULL ||
	    decided_frame == NULL || frame_ready == NULL)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	next = *receiver;
	result = v34_channel_frame_receiver_advance_owned(&next, normalized, &output);
	if (result != V34_DATA_RECEIVE_OK)
		return result;
	*receiver = next;
	*current = output.current;
	memcpy(decision_targets, output.targets, sizeof(output.targets));
	*frame_ready = output.ready;
	if (output.ready)
		memcpy(decided_frame, output.frame, sizeof(output.frame));
	return V34_DATA_RECEIVE_OK;
}

enum v34_data_receive_result
v34_channel_frame_receiver_advance_owned(
	struct v34_channel_frame_receiver *receiver,
	const struct qam_point
		normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_channel_frame_output *output)
{
	struct v34_precoder tentative_precoder;
	struct qam_point precoded_estimates
		[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct qam_point channel_estimates
		[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	enum v34_channel_conditioner_result condition_result;
	enum v34_precoder_result prediction_result;
	enum v34_channel_frame_decoder_result frame_result;

	if (receiver == NULL || normalized == NULL || output == NULL)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	condition_result = v34_channel_condition_interval(
		&receiver->conditioner, normalized, precoded_estimates);
	if (condition_result != V34_CHANNEL_CONDITIONER_OK)
		return conditioner_result(condition_result);
	tentative_precoder = receiver->receive_precoder;
	prediction_result = v34_precoder_reconstruct_interval(
		&tentative_precoder, precoded_estimates, channel_estimates);
	if (prediction_result != V34_PRECODER_OK)
		return precoder_result(prediction_result);
	frame_result = receiver->conditioner.normalizer.nonlinear_parameter ==
			       V34_NONLINEAR_THETA_5_OVER_16 &&
		       v34_precoder_coefficients_are_zero(
			       receiver->receive_precoder.coefficients) ?
		v34_channel_frame_decoder_advance_projected(
			&receiver->frame_decoder, channel_estimates, normalized,
			&receiver->conditioner.normalizer, &output->current,
			output->frame, &output->ready) :
		v34_channel_frame_decoder_advance(
			&receiver->frame_decoder, channel_estimates, &output->current,
			output->frame, &output->ready);
	if (frame_result != V34_CHANNEL_FRAME_DECODER_OK)
		return decoder_result(frame_result);
	{
		enum v34_data_receive_result result =
			correct_receive_history(
				&receiver->receive_precoder, &receiver->conditioner.normalizer,
				&output->current, output->targets);

		if (result != V34_DATA_RECEIVE_OK)
			return result;
	}
	return V34_DATA_RECEIVE_OK;
}
