// SPDX-License-Identifier: BSD-3-Clause

#include "v34_channel_frame_receiver.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static bool precoders_match_configuration(
	const struct v34_precoder *left,
	const struct v34_precoder *right)
{
	if (left->grid_step != right->grid_step)
		return false;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
	     ++tap) {
		if (left->coefficients[tap].real !=
			    right->coefficients[tap].real ||
		    left->coefficients[tap].imaginary !=
			    right->coefficients[tap].imaginary)
			return false;
	}
	return true;
}

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
	       precoders_match_configuration(
		       &receiver->receive_precoder,
		       &decoder->inverse_precoder);
}

enum v34_channel_frame_receiver_result
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
		return V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_CHANNEL_FRAME_RECEIVER_OK;
}

static enum v34_channel_frame_receiver_result
conditioner_result(enum v34_channel_conditioner_result result)
{
	return result == V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE ?
		V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE :
		V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
}

static enum v34_channel_frame_receiver_result
precoder_result(enum v34_precoder_result result)
{
	return result == V34_PRECODER_OUT_OF_RANGE ?
		V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE :
		V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
}

static enum v34_channel_frame_receiver_result decoder_result(
	enum v34_channel_frame_decoder_result result)
{
	switch (result) {
	case V34_CHANNEL_FRAME_DECODER_NUMERIC_FAILURE:
		return V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE;
	case V34_CHANNEL_FRAME_DECODER_OUT_OF_RANGE:
		return V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE;
	case V34_CHANNEL_FRAME_DECODER_OK:
	case V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT:
		break;
	}
	return V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
}

static enum v34_channel_frame_receiver_result
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
		struct qam_point projected;
		enum v34_precoder_result result =
			v34_precoder_inverse_step(
				receive_precoder,
				&current->channel_output[symbol],
				&mapper_output, &reconstructed);
		enum v34_nonlinear_result nonlinear_result;
		struct qam_point target;

		if (result != V34_PRECODER_OK)
			return precoder_result(result);
		nonlinear_result = v34_nonlinear_encode(
			normalizer->nonlinear_parameter,
			normalizer->precoded_mean_squared_magnitude,
			&reconstructed.precoded_symbol, &projected);
		if (nonlinear_result != V34_NONLINEAR_OK)
			return nonlinear_result ==
				       V34_NONLINEAR_NUMERIC_FAILURE ?
				V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE :
				V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
		/* The trellis slicer operates on the infinite odd lattice. During
		 * traceback its best current decision can therefore lie outside the
		 * finite shaped constellation even though a later modulo decision is
		 * valid. Scale that decision for equalizer feedback without applying
		 * the transmitter's finite-constellation peak check. */
		target = (struct qam_point){
			.in_phase = projected.in_phase *
				normalizer->normalization_gain,
			.quadrature = projected.quadrature *
				normalizer->normalization_gain,
		};
		if (!isfinite(target.in_phase) ||
		    !isfinite(target.quadrature))
			return V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE;
		decision_targets[symbol] = target;
	}
	return V34_CHANNEL_FRAME_RECEIVER_OK;
}

enum v34_channel_frame_receiver_result
v34_channel_frame_receiver_advance(
	struct v34_channel_frame_receiver *receiver,
	const struct qam_point
		normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_survivor_decision *current,
	struct qam_point
		decision_targets[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct v34_decided_4d_interval
		decided_frame[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready)
{
	struct v34_channel_frame_receiver next;
	struct v34_precoder tentative_precoder;
	struct qam_point precoded_estimates
		[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct qam_point channel_estimates
		[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_survivor_decision generated_current;
	struct qam_point generated_targets
		[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_decided_4d_interval generated_frame
		[V34_4D_INTERVALS_PER_MAPPING_FRAME];
	bool generated_ready;
	enum v34_channel_conditioner_result condition_result;
	enum v34_precoder_result prediction_result;
	enum v34_channel_frame_decoder_result frame_result;

	if (!v34_channel_frame_receiver_is_valid(receiver) ||
	    normalized == NULL || current == NULL || decision_targets == NULL ||
	    decided_frame == NULL || frame_ready == NULL)
		return V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	condition_result = v34_channel_condition_interval(
		&next.conditioner, normalized, precoded_estimates);
	if (condition_result != V34_CHANNEL_CONDITIONER_OK)
		return conditioner_result(condition_result);
	tentative_precoder = next.receive_precoder;
	prediction_result = v34_precoder_reconstruct_interval(
		&tentative_precoder, precoded_estimates, channel_estimates);
	if (prediction_result != V34_PRECODER_OK)
		return precoder_result(prediction_result);
	frame_result = v34_channel_frame_decoder_advance(
		&next.frame_decoder, channel_estimates, &generated_current,
		generated_frame, &generated_ready);
	if (frame_result != V34_CHANNEL_FRAME_DECODER_OK)
		return decoder_result(frame_result);
	{
		enum v34_channel_frame_receiver_result result =
			correct_receive_history(
				&next.receive_precoder, &next.conditioner.normalizer,
				&generated_current, generated_targets);

		if (result != V34_CHANNEL_FRAME_RECEIVER_OK)
			return result;
	}
	if (!v34_channel_frame_receiver_is_valid(&next))
		return V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*current = generated_current;
	memcpy(decision_targets, generated_targets, sizeof(generated_targets));
	*frame_ready = generated_ready;
	if (generated_ready)
		memcpy(decided_frame, generated_frame, sizeof(generated_frame));
	return V34_CHANNEL_FRAME_RECEIVER_OK;
}

const char *v34_channel_frame_receiver_result_name(
	enum v34_channel_frame_receiver_result result)
{
	switch (result) {
	case V34_CHANNEL_FRAME_RECEIVER_OK:
		return "ok";
	case V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
