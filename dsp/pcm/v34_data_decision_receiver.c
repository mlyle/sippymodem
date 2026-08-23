// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_decision_receiver.h"

#include "memory_region.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
	V34_DATA_DECISION_DECODER_POWER_Q15 = 0x2000,
	V34_DATA_DECISION_B1_POWER_Q15 = 0x7000,
};

static bool normalized_point_is_finite(const struct qam_point *point)
{
	return isfinite(point->in_phase) && isfinite(point->quadrature);
}

static bool normalizers_match(
	const struct v34_power_normalizer *left,
	const struct v34_power_normalizer *right)
{
	return v34_mapping_parameters_are_equal(
		       &left->parameters, &right->parameters) &&
	       left->precoded_mean_squared_magnitude ==
		       right->precoded_mean_squared_magnitude &&
	       left->projected_mean_squared_magnitude ==
		       right->projected_mean_squared_magnitude &&
	       left->normalization_gain == right->normalization_gain &&
	       left->maximum_precoded_point_power ==
		       right->maximum_precoded_point_power &&
	       left->maximum_projected_point_power ==
		       right->maximum_projected_point_power &&
	       left->maximum_normalized_point_power ==
		       right->maximum_normalized_point_power &&
	       left->calibration_symbol_count ==
		       right->calibration_symbol_count &&
	       left->nonlinear_parameter == right->nonlinear_parameter;
}

static bool b1_reference_matches_data_path(
	const struct v34_data_decision_receiver *receiver)
{
	const struct v34_data_points *reference = &receiver->b1_reference;
	const struct v34_channel_frame_receiver *channel =
		&receiver->data_receiver.channel_receiver;
	const struct v34_channel_decoder *decoder =
		&channel->frame_decoder.channel_decoder;
	const struct v34_data_receiver *data =
		&receiver->data_receiver.data_receiver;

	return reference->local_role == data->transmitting_role &&
	       v34_frame_geometry_is_equal(
		       &reference->schedule.geometry,
		       &channel->frame_decoder.channel_schedule.geometry) &&
	       v34_frame_geometry_is_equal(
		       &reference->schedule.geometry, &data->schedule.geometry) &&
	       v34_mapping_parameters_are_equal(
		       &reference->encoder.parameters,
		       &decoder->parameters) &&
	       v34_mapping_parameters_are_equal(
		       &reference->encoder.parameters,
		       &data->mapping_decoder.parameters) &&
	       normalizers_match(
		       &reference->normalizer,
		       &channel->conditioner.normalizer) &&
	       reference->encoder.convolutional_encoder.selection ==
		       decoder->survivor.selection &&
	       memcmp(reference->encoder.precoder.coefficients,
		      decoder->inverse_precoder.coefficients,
		      sizeof(reference->encoder.precoder.coefficients)) == 0 &&
	       memcmp(reference->encoder.precoder.coefficients,
		      channel->receive_precoder.coefficients,
		      sizeof(reference->encoder.precoder.coefficients)) == 0;
}

static bool b1_reference_is_complete(
	const struct v34_data_points *reference)
{
	return v34_frame_schedule_is_complete(&reference->schedule) &&
	       v34_data_points_needs_mapping_frame(reference);
}

bool v34_data_decision_receiver_is_valid(
	const struct v34_data_decision_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_data_channel_receiver_is_valid(&receiver->data_receiver) ||
	    !v34_data_points_is_valid(&receiver->b1_reference) ||
	    !b1_reference_matches_data_path(receiver) ||
	    !normalized_point_is_finite(&receiver->first_normalized))
		return false;
	if (receiver->b1_reference_active ==
		    b1_reference_is_complete(&receiver->b1_reference) ||
	    (receiver->b1_reference_active &&
	     (!v34_data_channel_receiver_is_receiving_b1(
		     &receiver->data_receiver) ||
	      receiver->has_first_symbol !=
		      ((receiver->b1_reference.next_point & 1u) != 0u))))
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
	struct v34_mapping_parameters parameters;
	struct v34_frame_schedule schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;

	if (receiver == NULL ||
	    v34_mapping_parameters_init(
		    &parameters, geometry, expanded_shaping) !=
		    V34_MAPPING_PARAMETERS_OK ||
	    v34_b1_frame_schedule_init(&schedule, geometry) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_transmit_scrambler_init(
		    &scrambler, transmitting_role) != V34_FRAMING_OK ||
	    v34_4d_encoder_init(
		    &encoder, &parameters, coefficients, selection) !=
		    V34_4D_ENCODER_OK ||
	    v34_data_points_init(
		    &initialized.b1_reference, &schedule, &scrambler,
		    &encoder, normalizer, transmitting_role) !=
		    V34_DATA_POINTS_OK ||
	    v34_data_channel_receiver_init(
		    &initialized.data_receiver, normalizer, geometry,
		    expanded_shaping, coefficients, selection,
		    transmitting_role) != V34_DATA_RECEIVE_OK)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	initialized.b1_reference_active = true;
	if (!v34_data_decision_receiver_is_valid(&initialized))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_RECEIVE_OK;
}

static bool next_b1_reference(
	struct v34_data_decision_receiver *receiver, struct qam_point *point)
{
	struct v34_data_points *reference = &receiver->b1_reference;
	uint8_t ones[V34_MAX_MAPPING_FRAME_BITS];
	struct v34_mapping_frame frame;
	const uint8_t auxiliary = 1u;

	if (v34_data_points_needs_mapping_frame(reference)) {
		memset(ones, 1, sizeof(ones));
		if (v34_data_points_preview_mapping_frame(reference, &frame) !=
			    V34_DATA_POINTS_OK ||
		    v34_data_points_queue_mapping_frame(
			    reference, ones, frame.primary_bits,
			    frame.carries_auxiliary_bit ? &auxiliary : NULL) !=
			    V34_DATA_POINTS_OK)
			return false;
	}
	if (v34_data_points_next(reference, point) != V34_DATA_POINTS_OK)
		return false;
	if (b1_reference_is_complete(reference))
		receiver->b1_reference_active = false;
	return true;
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
	return normalized_point_is_finite(normalized) &&
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
	bool known_b1_symbol;

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
	if (observation->epoch == 0u || observation->lineage == 0u)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	next = *receiver;
	normalized[1] = normalized_observation(observation);
	known_b1_symbol = next.b1_reference_active;
	if (known_b1_symbol &&
	    !next_b1_reference(&next, &normalized[1]))
		return V34_DATA_RECEIVE_INVALID_STATE;
	if (!next.has_first_symbol) {
		next.first_normalized = normalized[1];
		next.has_first_symbol = true;
		if (known_b1_symbol) {
			if (!fixed_target(&normalized[1], &target))
				return V34_DATA_RECEIVE_OUT_OF_RANGE;
			generated.equalizer =
				(struct v34_adaptive_equalizer_decision){
					.observation = *observation,
					.target = target,
					.decoder_power_q15 =
						V34_DATA_DECISION_B1_POWER_Q15,
					.adaptation =
						V34_ADAPTIVE_EQUALIZER_FULL,
				};
		} else {
			generated.equalizer = skip_decision(observation);
		}
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
		.decoder_power_q15 = known_b1_symbol ?
			V34_DATA_DECISION_B1_POWER_Q15 :
			V34_DATA_DECISION_DECODER_POWER_Q15,
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
