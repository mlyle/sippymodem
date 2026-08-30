// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_line_receiver.h"

#include <stddef.h>

static const struct v34_frame_geometry *data_geometry(
	const struct v34_data_line_receiver *receiver)
{
	return &receiver->equalizer_receiver.data_receiver.data_receiver
		.schedule.geometry;
}

static bool line_matches_profile(
	const struct qam_demodulator *line,
	const struct v34_phase3_profile *transmitting_profile)
{
	return qam_demodulator_is_valid(line) &&
	       qam_demodulator_measure(line).timing_locked &&
	       v34_line_channel_matches(
		       transmitting_profile, &line->channel);
}

bool v34_data_line_receiver_is_valid(
	const struct v34_data_line_receiver *receiver)
{
	return receiver != NULL &&
	       v34_phase3_profile_is_valid(
		       &receiver->transmitting_profile) &&
	       v34_frame_geometry_is_valid(&receiver->geometry) &&
	       receiver->transmitting_profile.symbol_rate ==
		       receiver->geometry.symbol_rate &&
	       line_matches_profile(
		       &receiver->line,
		       &receiver->transmitting_profile) &&
	       v34_data_equalizer_receiver_is_valid(
		       &receiver->equalizer_receiver) &&
	       v34_frame_geometry_is_equal(
		       &receiver->geometry, data_geometry(receiver));
}

enum v34_data_receive_result v34_data_line_receiver_init(
	struct v34_data_line_receiver *receiver,
	const struct qam_demodulator *demodulator,
	const struct v34_equalizer_handoff *handoff,
	const struct v34_data_line_receiver_configuration *configuration)
{
	struct v34_data_line_receiver initialized = { 0 };
	if (receiver == NULL || configuration == NULL ||
	    !v34_phase3_profile_is_valid(
		    &configuration->transmitting_profile) ||
	    !v34_frame_geometry_is_valid(&configuration->geometry) ||
	    configuration->transmitting_profile.symbol_rate !=
		    configuration->geometry.symbol_rate ||
	    !qam_adaptation_is_valid(
		    configuration->equalizer_adaptation) ||
	    !line_matches_profile(
		    demodulator, &configuration->transmitting_profile) ||
	    !v34_equalizer_handoff_is_valid(handoff))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	initialized.line = *demodulator;
	initialized.transmitting_profile =
		configuration->transmitting_profile;
	initialized.geometry = configuration->geometry;
	if (v34_data_equalizer_receiver_init_from_handoff(
		    &initialized.equalizer_receiver, handoff,
		    &configuration->normalizer, &configuration->geometry,
		    configuration->expanded_shaping,
		    configuration->precoder_coefficients,
		    configuration->trellis_selection,
		    configuration->transmitting_profile.local_role,
		    configuration->equalizer_adaptation) !=
		    V34_DATA_RECEIVE_OK ||
	    !v34_data_line_receiver_is_valid(&initialized))
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_RECEIVE_OK;
}

bool v34_data_line_receiver_has_failed(
	const struct v34_data_line_receiver *receiver)
{
	return v34_data_line_receiver_is_valid(receiver) && receiver->failed;
}

bool v34_data_line_receiver_is_receiving_b1(
	const struct v34_data_line_receiver *receiver)
{
	return v34_data_line_receiver_is_valid(receiver) && !receiver->failed &&
	       v34_data_equalizer_receiver_is_receiving_b1(
		       &receiver->equalizer_receiver);
}

static bool result_is_terminal(enum v34_data_receive_result result)
{
	return result == V34_DATA_RECEIVE_FAILED ||
	       result == V34_DATA_RECEIVE_NUMERIC_FAILURE ||
	       result == V34_DATA_RECEIVE_OUT_OF_RANGE;
}

enum v34_data_receive_result v34_data_line_receiver_sample(
	struct v34_data_line_receiver *receiver, int16_t sample,
	struct v34_survivor_decision *current,
	struct v34_received_mapping_frame *received)
{
	struct qam_demodulator next_line;
	struct qam_point point;
	enum v34_data_receive_result result;
	bool ready;

	if (!v34_data_line_receiver_is_valid(receiver) || current == NULL ||
	    received == NULL)
		return V34_DATA_RECEIVE_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_DATA_RECEIVE_FAILED;
	next_line = receiver->line;
	if (qam_demodulator_sample(
		    &next_line, sample, &point, &ready) != QAM_OK) {
		receiver->failed = true;
		return V34_DATA_RECEIVE_NUMERIC_FAILURE;
	}
	receiver->line = next_line;
	if (!ready)
		return V34_DATA_RECEIVE_OK;
	result = v34_data_equalizer_receive_point(
		&receiver->equalizer_receiver, &point, current, received);
	if (result == V34_DATA_RECEIVE_INVALID_ARGUMENT ||
	    result == V34_DATA_RECEIVE_INVALID_STATE)
		result = V34_DATA_RECEIVE_NUMERIC_FAILURE;
	if (result_is_terminal(result))
		receiver->failed = true;
	return result;
}
