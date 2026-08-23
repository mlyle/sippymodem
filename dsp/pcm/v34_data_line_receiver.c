// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_line_receiver.h"

#include "v34_pulse_modulator.h"

#include <math.h>
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

static bool correct_training_gain(
	const struct v34_equalizer_handoff *handoff,
	double transmit_point_gain,
	struct v34_equalizer_handoff *corrected)
{
	double reciprocal;

	if (!v34_equalizer_handoff_is_valid(handoff) ||
	    !isfinite(transmit_point_gain) || transmit_point_gain <= 0.0 ||
	    corrected == NULL)
		return false;
	*corrected = *handoff;
	reciprocal = 1.0 / transmit_point_gain;
	if (!isfinite(reciprocal) || reciprocal <= 0.0)
		return false;
	for (size_t tap = 0; tap < corrected->equalizer.tap_count; ++tap) {
		corrected->equalizer.coefficients[tap].in_phase *= reciprocal;
		corrected->equalizer.coefficients[tap].quadrature *= reciprocal;
	}
	corrected->equalizer.last_output.in_phase *= reciprocal;
	corrected->equalizer.last_output.quadrature *= reciprocal;
	return v34_equalizer_handoff_is_valid(corrected);
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
	       isfinite(receiver->transmit_point_gain) &&
	       receiver->transmit_point_gain > 0.0 &&
	       line_matches_profile(
		       &receiver->line,
		       &receiver->transmitting_profile) &&
	       v34_data_equalizer_receiver_is_valid(
		       &receiver->equalizer_receiver) &&
	       receiver->equalizer_receiver.training_outputs_remaining == 0 &&
	       v34_frame_geometry_is_equal(
		       &receiver->geometry, data_geometry(receiver));
}

enum v34_data_line_receiver_result v34_data_line_receiver_init(
	struct v34_data_line_receiver *receiver,
	const struct qam_demodulator *demodulator,
	const struct v34_equalizer_handoff *handoff,
	const struct v34_data_line_receiver_configuration *configuration)
{
	struct v34_data_line_receiver initialized = { 0 };
	struct v34_equalizer_handoff corrected_handoff;

	if (receiver == NULL || configuration == NULL ||
	    !v34_phase3_profile_is_valid(
		    &configuration->transmitting_profile) ||
	    !v34_frame_geometry_is_valid(&configuration->geometry) ||
	    configuration->transmitting_profile.symbol_rate !=
		    configuration->geometry.symbol_rate ||
	    !isfinite(configuration->transmit_point_gain) ||
	    configuration->transmit_point_gain <= 0.0 ||
	    !isfinite(configuration->equalizer_adaptation) ||
	    configuration->equalizer_adaptation < 0.0 ||
	    configuration->equalizer_adaptation > 1.0 ||
	    !line_matches_profile(
		    demodulator, &configuration->transmitting_profile) ||
	    !correct_training_gain(
		    handoff, configuration->transmit_point_gain,
		    &corrected_handoff))
		return V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT;
	initialized.line = *demodulator;
	initialized.transmitting_profile =
		configuration->transmitting_profile;
	initialized.geometry = configuration->geometry;
	initialized.transmit_point_gain = configuration->transmit_point_gain;
	if (v34_data_equalizer_receiver_init_from_handoff(
		    &initialized.equalizer_receiver, &corrected_handoff,
		    &configuration->normalizer, &configuration->geometry,
		    configuration->expanded_shaping,
		    configuration->precoder_coefficients,
		    configuration->trellis_selection,
		    configuration->transmitting_profile.local_role,
		    configuration->equalizer_adaptation) !=
		    V34_DATA_EQUALIZER_RECEIVER_OK ||
	    !v34_data_line_receiver_is_valid(&initialized))
		return V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_DATA_LINE_RECEIVER_OK;
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

static enum v34_data_line_receiver_result data_result(
	enum v34_data_equalizer_receiver_result result)
{
	switch (result) {
	case V34_DATA_EQUALIZER_RECEIVER_OK:
		return V34_DATA_LINE_RECEIVER_OK;
	case V34_DATA_EQUALIZER_RECEIVER_INTERVAL_READY:
		return V34_DATA_LINE_RECEIVER_INTERVAL_READY;
	case V34_DATA_EQUALIZER_RECEIVER_MAPPING_FRAME_READY:
		return V34_DATA_LINE_RECEIVER_MAPPING_FRAME_READY;
	case V34_DATA_EQUALIZER_RECEIVER_B1_IN_PROGRESS:
		return V34_DATA_LINE_RECEIVER_B1_IN_PROGRESS;
	case V34_DATA_EQUALIZER_RECEIVER_B1_COMPLETE:
		return V34_DATA_LINE_RECEIVER_B1_COMPLETE;
	case V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE:
		return V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE;
	case V34_DATA_EQUALIZER_RECEIVER_OUT_OF_RANGE:
		return V34_DATA_LINE_RECEIVER_OUT_OF_RANGE;
	case V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE;
}

static bool result_is_terminal(enum v34_data_line_receiver_result result)
{
	return result == V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE ||
	       result == V34_DATA_LINE_RECEIVER_OUT_OF_RANGE;
}

enum v34_data_line_receiver_result v34_data_line_receiver_sample(
	struct v34_data_line_receiver *receiver, int16_t sample,
	struct v34_survivor_decision *current,
	struct v34_received_mapping_frame *received)
{
	struct qam_demodulator next_line;
	struct qam_point point;
	enum v34_data_line_receiver_result result;
	bool ready;

	if (!v34_data_line_receiver_is_valid(receiver) || current == NULL ||
	    received == NULL)
		return V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_DATA_LINE_RECEIVER_FAILED;
	next_line = receiver->line;
	if (qam_demodulator_sample(
		    &next_line, sample, &point, &ready) != QAM_OK) {
		receiver->failed = true;
		return V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE;
	}
	receiver->line = next_line;
	if (!ready)
		return V34_DATA_LINE_RECEIVER_OK;
	result = data_result(v34_data_equalizer_receive_point(
		&receiver->equalizer_receiver, &point, current, received));
	if (result_is_terminal(result))
		receiver->failed = true;
	return result;
}

const char *v34_data_line_receiver_result_name(
	enum v34_data_line_receiver_result result)
{
	switch (result) {
	case V34_DATA_LINE_RECEIVER_OK:
		return "ok";
	case V34_DATA_LINE_RECEIVER_INTERVAL_READY:
		return "interval ready";
	case V34_DATA_LINE_RECEIVER_MAPPING_FRAME_READY:
		return "mapping frame ready";
	case V34_DATA_LINE_RECEIVER_B1_IN_PROGRESS:
		return "B1 in progress";
	case V34_DATA_LINE_RECEIVER_B1_COMPLETE:
		return "B1 complete";
	case V34_DATA_LINE_RECEIVER_FAILED:
		return "failed";
	case V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case V34_DATA_LINE_RECEIVER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
