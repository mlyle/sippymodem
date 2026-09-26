// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_decision_receiver.h"

#include "memory_region.h"

#include <stddef.h>

bool v34_data_decision_receiver_is_valid(
	const struct v34_data_decision_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_data_channel_receiver_is_valid(&receiver->data_receiver))
		return false;
	return receiver->has_first_symbol ?
		       v34_adaptive_equalizer_observation_is_valid(
			       &receiver->first_observation) :
		       v34_adaptive_equalizer_observation_is_zero(
			       &receiver->first_observation);
}

enum v34_data_receive_result
v34_data_decision_receiver_init(
	struct v34_data_decision_receiver *receiver,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection,
	enum v34_station_role transmitting_role)
{
	struct v34_data_decision_receiver initialized = { 0 };

	if (receiver == NULL ||
	    v34_data_channel_receiver_init(
		    &initialized.data_receiver, normalizer, geometry,
		    expanded_shaping, coefficients, selection,
		    transmitting_role) != V34_DATA_RECEIVE_OK ||
	    !v34_data_decision_receiver_is_valid(&initialized))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_RECEIVE_OK;
}

bool v34_data_decision_receiver_is_receiving_b1(
	const struct v34_data_decision_receiver *receiver)
{
	return v34_data_decision_receiver_is_valid(receiver) &&
	       v34_data_channel_receiver_is_receiving_b1(
		       &receiver->data_receiver);
}

static struct qam_point normalized_observation(
	const struct v34_adaptive_equalizer_observation *observation)
{
	return (struct qam_point){
		.in_phase = observation->decision.in_phase /
			(double)V34_TRAINING_FIXED_AMPLITUDE,
		.quadrature = observation->decision.quadrature /
			(double)V34_TRAINING_FIXED_AMPLITUDE,
	};
}

static bool fixed_target(
	const struct qam_point *normalized, struct v34_analytic_sample *fixed)
{
	return point_is_finite(normalized) &&
	       qam_coordinate_to_s16(
		       normalized->in_phase * V34_TRAINING_FIXED_AMPLITUDE,
		       &fixed->in_phase) &&
	       qam_coordinate_to_s16(
		       normalized->quadrature * V34_TRAINING_FIXED_AMPLITUDE,
		       &fixed->quadrature);
}

enum v34_data_receive_result
v34_data_decision_receiver_cancel_interval(
	struct v34_data_decision_receiver *receiver)
{
	if (!v34_data_decision_receiver_is_valid(receiver))
		return V34_DATA_RECEIVE_INVALID_STATE;
	if (!receiver->has_first_symbol)
		return V34_DATA_RECEIVE_INVALID_STATE;
	receiver->first_observation =
		(struct v34_adaptive_equalizer_observation){ 0 };
	receiver->has_first_symbol = false;
	return V34_DATA_RECEIVE_OK;
}

enum v34_data_receive_result
v34_data_decision_receiver_observe(
	struct v34_data_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_data_decision_output *output)
{
	const struct memory_region regions[] = {
		{ receiver, receiver == NULL ? 0 : sizeof(*receiver) },
		{ observation, sizeof(*observation) },
		{ output, sizeof(*output) },
	};
	struct v34_data_decision_receiver next;
	struct v34_data_decision_output generated = { 0 };
	struct qam_point normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct qam_point targets[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_analytic_sample fixed_targets[
		V34_2D_SYMBOLS_PER_4D_INTERVAL];
	enum v34_data_receive_result result;

	if (observation == NULL || output == NULL ||
	    memory_region_set_overlaps(regions, sizeof regions / sizeof *regions))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	if (!v34_data_decision_receiver_is_valid(receiver))
		return V34_DATA_RECEIVE_INVALID_STATE;
	if (!v34_adaptive_equalizer_observation_is_valid(observation))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	if (!receiver->has_first_symbol) {
		generated.equalizer[0] =
			(struct v34_adaptive_equalizer_decision){
				.observation = *observation,
				.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
			};
		receiver->first_observation = *observation;
		receiver->has_first_symbol = true;
		*output = generated;
		return V34_DATA_RECEIVE_OK;
	}
	next = *receiver;
	normalized[1] = normalized_observation(observation);
	normalized[0] = normalized_observation(&next.first_observation);
	result = v34_data_channel_receiver_advance(
		&next.data_receiver, normalized, &generated.current, targets,
		&generated.received);
	if (result == V34_DATA_RECEIVE_OK)
		result = V34_DATA_RECEIVE_INTERVAL_READY;
	else if (result == V34_DATA_RECEIVE_INVALID_ARGUMENT)
		return V34_DATA_RECEIVE_INVALID_STATE;
	if (result != V34_DATA_RECEIVE_INTERVAL_READY &&
	    !v34_data_receive_emits_mapping_frame(result))
		return result;
	for (size_t symbol = 0; symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
	     ++symbol)
		if (!fixed_target(&targets[symbol], &fixed_targets[symbol]))
			return V34_DATA_RECEIVE_OUT_OF_RANGE;
	generated.equalizer[0] =
		(struct v34_adaptive_equalizer_decision){
			.observation = next.first_observation,
			.target = fixed_targets[0],
			.decoder_power_q15 =
				V34_DATA_DECISION_DECODER_POWER_Q15,
			.adaptation = V34_ADAPTIVE_EQUALIZER_FULL,
		};
	generated.equalizer[1] =
		(struct v34_adaptive_equalizer_decision){
			.observation = *observation,
			.target = fixed_targets[1],
			.decoder_power_q15 =
				V34_DATA_DECISION_DECODER_POWER_Q15,
			.adaptation = V34_ADAPTIVE_EQUALIZER_FULL,
		};
	next.first_observation =
		(struct v34_adaptive_equalizer_observation){ 0 };
	next.has_first_symbol = false;
	if (!v34_data_decision_receiver_is_valid(&next))
		return V34_DATA_RECEIVE_INVALID_STATE;
	if (!v34_data_receive_emits_mapping_frame(result))
		generated.received = (struct v34_received_mapping_frame){ 0 };
	*receiver = next;
	*output = generated;
	return result;
}
