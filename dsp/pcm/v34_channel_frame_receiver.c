// SPDX-License-Identifier: BSD-3-Clause

#include "v34_channel_frame_receiver.h"

#include <stddef.h>
#include <string.h>

static bool mapping_parameters_are_equal(
	const struct sippymodem_v34_mapping_parameters *left,
	const struct sippymodem_v34_mapping_parameters *right)
{
	return left->constellation_points == right->constellation_points &&
	       left->mapping_frame_bits == right->mapping_frame_bits &&
	       left->shell_mapper_bits == right->shell_mapper_bits &&
	       left->point_index_bits_per_2d_symbol ==
		       right->point_index_bits_per_2d_symbol &&
	       left->ring_count == right->ring_count &&
	       left->expanded_shaping == right->expanded_shaping;
}

static bool precoders_match_configuration(
	const struct sippymodem_v34_precoder *left,
	const struct sippymodem_v34_precoder *right)
{
	if (left->grid_step != right->grid_step)
		return false;
	for (size_t tap = 0; tap < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
	     ++tap) {
		if (left->coefficients[tap].real !=
			    right->coefficients[tap].real ||
		    left->coefficients[tap].imaginary !=
			    right->coefficients[tap].imaginary)
			return false;
	}
	return true;
}

bool sippymodem_v34_channel_frame_receiver_is_valid(
	const struct sippymodem_v34_channel_frame_receiver *receiver)
{
	struct sippymodem_v34_precoder_prediction prediction;
	const struct sippymodem_v34_channel_decoder *decoder;

	if (receiver == NULL ||
	    !sippymodem_v34_channel_conditioner_is_valid(
		    &receiver->conditioner) ||
	    !sippymodem_v34_precoder_is_valid(&receiver->receive_precoder) ||
	    sippymodem_v34_precoder_predict(
		    &receiver->receive_precoder, &prediction) !=
		    SIPPYMODEM_V34_PRECODER_OK ||
	    !sippymodem_v34_channel_frame_decoder_is_valid(
		    &receiver->frame_decoder))
		return false;
	decoder = &receiver->frame_decoder.channel_decoder;
	return mapping_parameters_are_equal(
		       &receiver->conditioner.normalizer.parameters,
		       &decoder->parameters) &&
	       precoders_match_configuration(
		       &receiver->receive_precoder,
		       &decoder->inverse_precoder);
}

enum sippymodem_v34_channel_frame_receiver_result
sippymodem_v34_channel_frame_receiver_init(
	struct sippymodem_v34_channel_frame_receiver *receiver,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection selection)
{
	struct sippymodem_v34_channel_frame_receiver initialized = { 0 };

	if (receiver == NULL ||
	    sippymodem_v34_channel_conditioner_init(
		    &initialized.conditioner, normalizer) !=
		    SIPPYMODEM_V34_CHANNEL_CONDITIONER_OK ||
	    sippymodem_v34_precoder_init(
		    &initialized.receive_precoder,
		    &initialized.conditioner.normalizer.parameters,
		    coefficients) != SIPPYMODEM_V34_PRECODER_OK ||
	    sippymodem_v34_channel_frame_decoder_init(
		    &initialized.frame_decoder, geometry, expanded_shaping,
		    coefficients, selection) !=
		    SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OK ||
	    !sippymodem_v34_channel_frame_receiver_is_valid(&initialized))
		return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK;
}

static enum sippymodem_v34_channel_frame_receiver_result
conditioner_result(enum sippymodem_v34_channel_conditioner_result result)
{
	return result == SIPPYMODEM_V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE ?
		SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE :
		SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
}

static enum sippymodem_v34_channel_frame_receiver_result
precoder_result(enum sippymodem_v34_precoder_result result)
{
	return result == SIPPYMODEM_V34_PRECODER_OUT_OF_RANGE ?
		SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE :
		SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
}

static enum sippymodem_v34_channel_frame_receiver_result decoder_result(
	enum sippymodem_v34_channel_frame_decoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE;
	case SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OK:
	case SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
}

static enum sippymodem_v34_channel_frame_receiver_result
correct_receive_history(
	struct sippymodem_v34_precoder *receive_precoder,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_qam_point
		decision_targets[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL])
{
	for (size_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		struct sippymodem_v34_grid_point mapper_output;
		struct sippymodem_v34_precoder_symbol reconstructed;
		struct sippymodem_qam_point projected;
		enum sippymodem_v34_precoder_result result =
			sippymodem_v34_precoder_inverse_step(
				receive_precoder,
				&current->channel_output[symbol],
				&mapper_output, &reconstructed);
		enum sippymodem_v34_nonlinear_result nonlinear_result;
		enum sippymodem_v34_symbol_power_result power_result;

		if (result != SIPPYMODEM_V34_PRECODER_OK)
			return precoder_result(result);
		nonlinear_result = sippymodem_v34_nonlinear_encode(
			normalizer->nonlinear_parameter,
			normalizer->precoded_mean_squared_magnitude,
			&reconstructed.precoded_symbol, &projected);
		if (nonlinear_result != SIPPYMODEM_V34_NONLINEAR_OK)
			return nonlinear_result ==
				       SIPPYMODEM_V34_NONLINEAR_NUMERIC_FAILURE ?
				SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE :
				SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
		power_result = sippymodem_v34_normalize_power(
			normalizer, &projected, &decision_targets[symbol]);
		switch (power_result) {
		case SIPPYMODEM_V34_SYMBOL_POWER_OK:
			break;
		case SIPPYMODEM_V34_SYMBOL_POWER_OUT_OF_RANGE:
			return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE;
		case SIPPYMODEM_V34_SYMBOL_POWER_NUMERIC_FAILURE:
			return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE;
		case SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT:
			return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
		}
	}
	return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK;
}

enum sippymodem_v34_channel_frame_receiver_result
sippymodem_v34_channel_frame_receiver_advance(
	struct sippymodem_v34_channel_frame_receiver *receiver,
	const struct sippymodem_qam_point
		normalized[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_qam_point
		decision_targets[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct sippymodem_v34_decided_4d_interval
		decided_frame[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	bool *frame_ready)
{
	struct sippymodem_v34_channel_frame_receiver next;
	struct sippymodem_v34_precoder tentative_precoder;
	struct sippymodem_qam_point precoded_estimates
		[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct sippymodem_qam_point channel_estimates
		[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct sippymodem_v34_survivor_decision generated_current;
	struct sippymodem_qam_point generated_targets
		[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct sippymodem_v34_decided_4d_interval generated_frame
		[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
	bool generated_ready;
	enum sippymodem_v34_channel_conditioner_result condition_result;
	enum sippymodem_v34_precoder_result prediction_result;
	enum sippymodem_v34_channel_frame_decoder_result frame_result;

	if (!sippymodem_v34_channel_frame_receiver_is_valid(receiver) ||
	    normalized == NULL || current == NULL || decision_targets == NULL ||
	    decided_frame == NULL || frame_ready == NULL)
		return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	condition_result = sippymodem_v34_channel_condition_interval(
		&next.conditioner, normalized, precoded_estimates);
	if (condition_result != SIPPYMODEM_V34_CHANNEL_CONDITIONER_OK)
		return conditioner_result(condition_result);
	tentative_precoder = next.receive_precoder;
	prediction_result = sippymodem_v34_precoder_reconstruct_interval(
		&tentative_precoder, precoded_estimates, channel_estimates);
	if (prediction_result != SIPPYMODEM_V34_PRECODER_OK)
		return precoder_result(prediction_result);
	frame_result = sippymodem_v34_channel_frame_decoder_advance(
		&next.frame_decoder, channel_estimates, &generated_current,
		generated_frame, &generated_ready);
	if (frame_result != SIPPYMODEM_V34_CHANNEL_FRAME_DECODER_OK)
		return decoder_result(frame_result);
	{
		enum sippymodem_v34_channel_frame_receiver_result result =
			correct_receive_history(
				&next.receive_precoder, &next.conditioner.normalizer,
				&generated_current, generated_targets);

		if (result != SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK)
			return result;
	}
	if (!sippymodem_v34_channel_frame_receiver_is_valid(&next))
		return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*current = generated_current;
	memcpy(decision_targets, generated_targets, sizeof(generated_targets));
	*frame_ready = generated_ready;
	if (generated_ready)
		memcpy(decided_frame, generated_frame, sizeof(generated_frame));
	return SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK;
}

const char *sippymodem_v34_channel_frame_receiver_result_name(
	enum sippymodem_v34_channel_frame_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case SIPPYMODEM_V34_CHANNEL_FRAME_RECEIVER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
