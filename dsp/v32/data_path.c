// SPDX-License-Identifier: BSD-3-Clause

#include "data_path.h"

#include "decision.h"
#include "integer_math.h"
#include "memory_region.h"
#include "sample_math.h"

#include <stddef.h>

static int32_t repeated_pair_energy(
	int16_t previous_i, int16_t previous_q,
	const struct v32_equalizer_observation *observation)
{
	int16_t difference_i = (int16_t)(
		(uint16_t)previous_i - (uint16_t)observation->i);
	int16_t difference_q = (int16_t)(
		(uint16_t)previous_q - (uint16_t)observation->q);
	uint32_t energy = sample_pair_power_s16(difference_i, difference_q);

	return arithmetic_shift_right_u32(energy, 16);
}

enum online_signal { ONLINE_NONE, ONLINE_RETRAIN, ONLINE_CONTROL };

static enum online_signal observe_online_signal(
	struct v32_data_retrain_detector *detector,
	const struct v32_equalizer_observation *observation)
{
	int32_t energy = repeated_pair_energy(
		detector->i_previous, detector->q_previous, observation);
	/* A signed high word of pair energy fits the helper's sample input. */
	int16_t average = wrap_s16_from_s32(smooth_q15_s16(
		detector->average_error, (int16_t)energy, 0x7800));
	uint16_t count = detector->repeated_pair_observations + 1u;

	detector->i_previous = detector->i_last;
	detector->q_previous = detector->q_last;
	detector->i_last = observation->i;
	detector->q_last = observation->q;
	detector->average_error = average;
	if (average <= 0x04ff) {
		/* The short preamble reverses after 56 symbols (V.32bis 8).
		 * Distinguish it from the longer full-retrain carrier. */
		if (average < (int16_t)arithmetic_shift_right_u32(
				      (uint32_t)energy, 3) &&
		    count > 0x0014 && count <= 0x003b) {
			detector->repeated_pair_observations = 0;
			return ONLINE_CONTROL;
		}
		detector->repeated_pair_observations = count;
		if (count == V32_DATA_RETRAIN_OBSERVATIONS) {
			detector->repeated_pair_observations = 0;
			return ONLINE_RETRAIN;
		}
		return ONLINE_NONE;
	}
	detector->repeated_pair_observations = 0;
	return ONLINE_NONE;
}

bool v32_data_path_is_valid(const struct v32_data_path *path)
{
	return path != NULL && v32_b1_path_is_complete(&path->b1) &&
	       path->retrain.repeated_pair_observations <
		       V32_DATA_RETRAIN_OBSERVATIONS &&
	       path->control_remaining <= V32_DATA_CONTROL_OBSERVATIONS &&
	       (!path->control_remaining ||
		(v32_uncoded_receiver_is_valid(&path->control) &&
		 v32_rate_signal_detector_is_valid(&path->control_rate)));
}

/* The scrambler is self-synchronizing, so the bounded probe also tolerates
 * a few uncertain observations at the preamble/rate boundary. Only repeated,
 * valid rate words can turn a preamble candidate into a control request. */
static bool observe_control(struct v32_data_path *path,
	enum online_signal signal, const struct v32_equalizer_observation *observation,
	struct v32_data_batch *batch)
{
	if (signal == ONLINE_CONTROL) {
		struct v32_symbol_decoder seed;
		enum v32_transmitter_role role = path->b1.mode == V32_B1_MODE_TRELLIS ?
			path->b1.receiver.trellis.descrambler.transmitter_role :
			path->b1.receiver.uncoded.decoder.descrambler.transmitter_role;
		if (v32_symbol_decoder_init(&seed, role, 0) != V32_FRAMING_OK ||
		    v32_uncoded_receiver_init(&path->control, &seed) != V32_UNCODED_RECEIVER_OK ||
		    v32_rate_signal_detector_init(&path->control_rate) != V32_RATE_SIGNAL_OK)
			return false;
		path->control_remaining = V32_DATA_CONTROL_OBSERVATIONS;
	}
	if (signal == ONLINE_RETRAIN)
		path->control_remaining = 0;
	if (path->control_remaining) {
		struct v32_uncoded_symbol symbol;
		struct v32_rate_offer offer;
		enum v32_rate_signal_event event;
		if (v32_uncoded_receiver_observe(&path->control, 2, observation, false,
			&symbol) != V32_UNCODED_RECEIVER_OK ||
		    v32_rate_signal_detector_observe(&path->control_rate, symbol.group,
			&event, &offer) != V32_RATE_SIGNAL_OK)
			return false;
		--path->control_remaining;
		if (event == V32_CLEARDOWN_SIGNAL_DETECTED) {
			batch->cleardown_requested = true;
			path->control_remaining = 0;
		} else if (event == V32_RATE_SIGNAL_DETECTED) {
			batch->rate_request = offer;
			path->control_remaining = 0;
		}
	}
	return true;
}

static bool append_group(
	struct v32_data_batch *batch, uint8_t value, uint8_t bit_count)
{
	if (batch->count >= V32_DATA_PATH_MAX_GROUPS ||
	    bit_count == 0 || bit_count > V32_TRELLIS_MAX_DATA_BITS ||
	    value >= (UINT8_C(1) << bit_count))
		return false;
	batch->groups[batch->count++] = (struct v32_data_group){
		.value = value,
		.bit_count = bit_count,
	};
	return true;
}

static enum v32_data_path_result decide_observation(
	const struct v32_data_path *path,
	const struct v32_equalizer_observation *observation,
	union v32_b1_receiver *next_receiver,
	struct v32_equalizer_decision *equalizer,
	struct v32_data_batch *batch, bool *b1_matches)
{
	*next_receiver = path->b1.receiver;
	switch (path->b1.mode) {
	case V32_B1_MODE_UNCODED: {
		struct v32_uncoded_symbol symbol;

		if (v32_uncoded_receiver_observe(
			    &next_receiver->uncoded, path->b1.data_bits_per_symbol,
			    observation, true, &symbol) != V32_UNCODED_RECEIVER_OK ||
		    !append_group(
			    batch, symbol.group,
			    path->b1.data_bits_per_symbol))
			return V32_DATA_PATH_INVALID_STATE;
		*equalizer = symbol.equalizer;
		return V32_DATA_PATH_OK;
	}
	case V32_B1_MODE_TRELLIS: {
		struct v32_tcm_decision decision;
		struct v32_trellis_receiver_symbol symbol;

		if (v32_decide_tcm(
			    observation, path->b1.data_bits_per_symbol, true,
			    &decision) != V32_DECISION_OK ||
		    v32_trellis_receiver_decode(
			    &next_receiver->trellis, observation, &symbol) !=
			    V32_TRELLIS_RECEIVER_OK ||
		    (symbol.ready &&
		     !append_group(
			     batch, symbol.group,
			     path->b1.data_bits_per_symbol)))
			return V32_DATA_PATH_INVALID_STATE;
		*b1_matches = next_receiver->trellis.b1_matches;
		*equalizer = decision.equalizer;
		return V32_DATA_PATH_OK;
	}
	}
	return V32_DATA_PATH_INVALID_STATE;
}

static enum v32_data_path_result commit_observations(
	struct v32_data_path *path,
	enum v32_receive_path_result receive_result,
	struct v32_equalizer_observation *observation,
	struct v32_data_batch *batch)
{
	while (receive_result == V32_RECEIVE_PATH_OBSERVATION_READY) {
		union v32_b1_receiver next_receiver;
		struct v32_data_retrain_detector next_retrain = path->retrain;
		struct v32_equalizer_decision decision;
		bool b1_matches = path->b1.b1_matches;
		enum online_signal signal = ONLINE_NONE;

		if (path->b1.receive.line.gain.signal_present)
			signal = observe_online_signal(
				&next_retrain, observation);
		else
			next_retrain = (struct v32_data_retrain_detector){ 0 };

		if (!observe_control(path, signal, observation, batch))
			return V32_DATA_PATH_INVALID_STATE;

		if (decide_observation(
			    path, observation, &next_receiver, &decision, batch,
			    &b1_matches) != V32_DATA_PATH_OK)
			return V32_DATA_PATH_INVALID_STATE;
		receive_result = v32_receive_path_commit(
			&path->b1.receive, &decision, observation);
		if (receive_result != V32_RECEIVE_PATH_OK &&
		    receive_result != V32_RECEIVE_PATH_OBSERVATION_READY)
			return V32_DATA_PATH_INVALID_STATE;
		path->b1.receiver = next_receiver;
		path->b1.observation_epoch = decision.observation.epoch;
		path->b1.b1_matches = b1_matches;
		path->retrain = next_retrain;
		batch->retrain_requested =
			batch->retrain_requested || (signal == ONLINE_RETRAIN);
		if (batch->cleardown_requested)
			batch->count = 0;
	}
	if (receive_result != V32_RECEIVE_PATH_OK)
		return receive_result == V32_RECEIVE_PATH_INVALID_ARGUMENT ?
			       V32_DATA_PATH_INVALID_ARGUMENT :
			       V32_DATA_PATH_INVALID_STATE;
	return v32_data_path_is_valid(path) ? V32_DATA_PATH_OK :
					      V32_DATA_PATH_INVALID_STATE;
}

enum v32_data_path_result v32_data_path_init(
	struct v32_data_path *path,
	const struct v32_b1_path *b1,
	struct v32_data_batch *batch)
{
	const struct memory_region regions[] = {
		{ path, sizeof(*path) },
		{ b1, sizeof(*b1) },
		{ batch, sizeof(*batch) },
	};
	struct v32_data_path initialized = { 0 };
	struct v32_data_batch generated = { 0 };
	enum v32_data_path_result result = V32_DATA_PATH_OK;

	if (path == NULL || b1 == NULL || batch == NULL ||
	    memory_region_set_overlaps(regions, 3) ||
	    !v32_b1_path_is_complete(b1))
		return V32_DATA_PATH_INVALID_ARGUMENT;
	initialized.b1 = *b1;
	if (initialized.b1.receive.equalizer.decision_pending) {
		struct v32_equalizer_observation observation =
			initialized.b1.receive.equalizer.pending_observation;

		result = commit_observations(
			&initialized, V32_RECEIVE_PATH_OBSERVATION_READY,
			&observation, &generated);
	}
	if (result != V32_DATA_PATH_OK ||
	    !v32_data_path_is_valid(&initialized))
		return result == V32_DATA_PATH_INVALID_ARGUMENT ?
			       result : V32_DATA_PATH_INVALID_STATE;
	*path = initialized;
	*batch = generated;
	return V32_DATA_PATH_OK;
}

enum v32_data_path_result v32_data_path_sample(
	struct v32_data_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference,
	struct v32_data_batch *batch)
{
	const struct memory_region regions[] = {
		{ path, sizeof(*path) },
		{ batch, sizeof(*batch) },
		{ accepted_reference,
		  accepted_reference != NULL ? sizeof(*accepted_reference) : 0 },
	};
	struct v32_equalizer_observation observation;
	struct v32_data_batch generated = { 0 };
	enum v32_receive_path_result receive_result;
	enum v32_data_path_result result;

	if (!v32_data_path_is_valid(path))
		return V32_DATA_PATH_INVALID_STATE;
	if (batch == NULL ||
	    memory_region_set_overlaps(regions, 3))
		return V32_DATA_PATH_INVALID_ARGUMENT;
	receive_result = v32_receive_path_sample(
		&path->b1.receive, received, accepted_reference, &observation);
	result = commit_observations(
		path, receive_result, &observation, &generated);
	if (result != V32_DATA_PATH_OK)
		return result;
	*batch = generated;
	return V32_DATA_PATH_OK;
}
