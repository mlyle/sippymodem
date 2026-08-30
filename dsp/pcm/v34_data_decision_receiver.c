// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_decision_receiver.h"

#include "memory_region.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

enum {
	V34_DATA_DECISION_DECODER_POWER_Q15 = 0x2000,
};

bool v34_data_decision_receiver_is_valid(
	const struct v34_data_decision_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_data_channel_receiver_is_valid(&receiver->data_receiver) ||
	    !point_is_finite(&receiver->first_normalized))
		return false;
	return receiver->has_first_symbol ||
	       (receiver->first_normalized.in_phase == 0.0 &&
		receiver->first_normalized.quadrature == 0.0);
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
			(double)V34_DATA_DECISION_FIXED_AMPLITUDE,
		.quadrature = observation->decision.quadrature /
			(double)V34_DATA_DECISION_FIXED_AMPLITUDE,
	};
}

static bool fixed_coordinate(double normalized, int16_t *fixed)
{
	double scaled = normalized * V34_DATA_DECISION_FIXED_AMPLITUDE;
	long rounded;

	if (!isfinite(scaled) || scaled < (double)INT16_MIN - 0.5 ||
	    scaled > (double)INT16_MAX + 0.5)
		return false;
	rounded = lround(scaled);
	if (rounded < INT16_MIN || rounded > INT16_MAX)
		return false;
	*fixed = (int16_t)rounded;
	return true;
}

static bool fixed_target(
	const struct qam_point *normalized, struct v34_analytic_sample *fixed)
{
	return point_is_finite(normalized) &&
	       fixed_coordinate(normalized->in_phase, &fixed->in_phase) &&
	       fixed_coordinate(normalized->quadrature, &fixed->quadrature);
}

static struct v34_adaptive_equalizer_decision skip_decision(
	const struct v34_adaptive_equalizer_observation *observation)
{
	return (struct v34_adaptive_equalizer_decision){
		.observation = *observation,
		.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
	};
}

enum v34_data_receive_result
v34_data_decision_receiver_observe(
	struct v34_data_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_data_decision_output *output)
{
	struct v34_data_decision_receiver next;
	struct v34_data_decision_output generated = { 0 };
	struct qam_point normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct qam_point targets[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_analytic_sample target;
	enum v34_data_receive_result result;

	if (observation == NULL || output == NULL ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    observation, sizeof(*observation)) ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    output, sizeof(*output)) ||
	    memory_regions_overlap(
		    observation, sizeof(*observation), output,
		    sizeof(*output)))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	if (!v34_data_decision_receiver_is_valid(receiver))
		return V34_DATA_RECEIVE_INVALID_STATE;
	if (!v34_adaptive_equalizer_observation_is_valid(observation))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	next = *receiver;
	normalized[1] = normalized_observation(observation);
	if (!next.has_first_symbol) {
		next.first_normalized = normalized[1];
		next.has_first_symbol = true;
		generated.equalizer = skip_decision(observation);
		if (!v34_data_decision_receiver_is_valid(&next))
			return V34_DATA_RECEIVE_INVALID_STATE;
		*receiver = next;
		*output = generated;
		return V34_DATA_RECEIVE_OK;
	}
	normalized[0] = next.first_normalized;
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
	if (!fixed_target(&targets[1], &target))
		return V34_DATA_RECEIVE_OUT_OF_RANGE;
	generated.equalizer = (struct v34_adaptive_equalizer_decision){
		.observation = *observation,
		.target = target,
		.decoder_power_q15 = V34_DATA_DECISION_DECODER_POWER_Q15,
		.adaptation = V34_ADAPTIVE_EQUALIZER_FULL,
	};
	next.first_normalized = (struct qam_point){ 0 };
	next.has_first_symbol = false;
	if (!v34_data_decision_receiver_is_valid(&next))
		return V34_DATA_RECEIVE_INVALID_STATE;
	if (!v34_data_receive_emits_mapping_frame(result))
		generated.received = (struct v34_received_mapping_frame){ 0 };
	*receiver = next;
	*output = generated;
	return result;
}
