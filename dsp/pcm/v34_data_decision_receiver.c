// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_decision_receiver.h"

#include "memory_region.h"
#include "sample_math.h"

#include <stddef.h>

bool v34_data_decision_receiver_is_valid(
	const struct v34_data_decision_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_data_channel_receiver_is_valid(&receiver->data_receiver) ||
	    !v34_b1_gain_is_valid(&receiver->b1_gain))
		return false;
	if (receiver->b1_gain.active && receiver->b1_gain.count !=
	    2u * receiver->data_receiver.channel_receiver.frame_decoder
		    .channel_decoder.survivor.path_intervals + receiver->has_first_symbol)
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
	v34_b1_gain_init(&initialized.b1_gain, normalizer, geometry,
		coefficients, selection, transmitting_role);
	*receiver = initialized;
	return V34_DATA_RECEIVE_OK;
}

bool v34_data_decision_receiver_is_receiving_b1(
	const struct v34_data_decision_receiver *receiver)
{
	return v34_data_decision_receiver_is_valid(receiver) &&
	       receiver->data_receiver.data_receiver.b1_active;
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
	       sample_round_checked_s16(
		       normalized->in_phase * V34_TRAINING_FIXED_AMPLITUDE,
		       &fixed->in_phase) &&
	       sample_round_checked_s16(
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
	/* An abandoned half-pair no longer has a known B1 waveform position. */
	receiver->b1_gain = (struct v34_b1_gain){ 0 };
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
		if (receiver->data_receiver.data_receiver.b1_active)
			(void)v34_b1_gain_apply(&receiver->b1_gain,
				&receiver->data_receiver.channel_receiver.conditioner.normalizer);
		generated.equalizer[0] =
			(struct v34_adaptive_equalizer_decision){
				.observation = *observation,
				.adaptation = V34_ADAPTIVE_EQUALIZER_SKIP,
			};
		receiver->first_observation = *observation;
		receiver->has_first_symbol = true;
		if (receiver->data_receiver.data_receiver.b1_active) {
			struct qam_point point = normalized_observation(observation);
			v34_b1_gain_observe(&receiver->b1_gain,
				&receiver->data_receiver.channel_receiver.conditioner.normalizer, &point);
		}
		*output = generated;
		return V34_DATA_RECEIVE_OK;
	}
	normalized[1] = normalized_observation(observation);
	normalized[0] = normalized_observation(&receiver->first_observation);
	/* The child owns rollback and returns only finite decision targets.
	 * After its commit, fixed-range rejection merely skips adaptation; no
	 * remaining operation can fail or needs a second decoder snapshot. */
	result = v34_data_channel_receiver_advance(
		&receiver->data_receiver, normalized, &generated.current, targets,
		&generated.received);
	if (result == V34_DATA_RECEIVE_OK)
		result = V34_DATA_RECEIVE_INTERVAL_READY;
	else if (result == V34_DATA_RECEIVE_INVALID_ARGUMENT)
		return V34_DATA_RECEIVE_INVALID_STATE;
	if (result != V34_DATA_RECEIVE_INTERVAL_READY &&
	    !v34_data_receive_emits_mapping_frame(result))
		return result;
	/* Observe only accepted input. A rejected second symbol must leave the
	 * fit and the retained first symbol available for an identical retry. */
	if (receiver->data_receiver.data_receiver.b1_active)
		v34_b1_gain_observe(&receiver->b1_gain,
			&receiver->data_receiver.channel_receiver.conditioner.normalizer, &normalized[1]);
	for (size_t symbol = 0; symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		const struct v34_adaptive_equalizer_observation *input =
			symbol == 0 ? &receiver->first_observation : observation;
		bool representable;

		representable = fixed_target(&targets[symbol], &fixed_targets[symbol]);
		/* The trellis makes present-time decisions on an infinite lattice.
		 * Noise can select a finite point beyond the equalizer's fixed range.
		 * Keep decoding and skip carrier/tap adaptation for that symbol.
		 * Its observed point supplies zero residual to feedback history;
		 * clipping or wrapping the target would teach a false decision. */
		generated.equalizer[symbol] =
			(struct v34_adaptive_equalizer_decision){
				.observation = *input,
				.target = representable ? fixed_targets[symbol] : input->decision,
				.decoder_power_q15 = representable ?
					V34_DATA_DECISION_DECODER_POWER_Q15 : 0,
				.adaptation = representable ? V34_ADAPTIVE_EQUALIZER_FULL :
					V34_ADAPTIVE_EQUALIZER_SKIP,
			};
	}
	receiver->first_observation =
		(struct v34_adaptive_equalizer_observation){ 0 };
	receiver->has_first_symbol = false;
	/* The child preserves its configuration and receive invariants. Clearing
	 * the retained symbol establishes this owner's remaining invariant. */
	if (!v34_data_receive_emits_mapping_frame(result))
		generated.received = (struct v34_received_mapping_frame){ 0 };
	*output = generated;
	return result;
}
