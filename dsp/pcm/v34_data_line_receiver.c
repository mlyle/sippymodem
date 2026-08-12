// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_line_receiver.h"

#include <math.h>
#include <stddef.h>

static bool channels_are_equal(
	const struct qam_channel *left, const struct qam_channel *right)
{
	return left->sample_rate == right->sample_rate &&
	       left->symbol_rate_numerator == right->symbol_rate_numerator &&
	       left->symbol_rate_denominator == right->symbol_rate_denominator &&
	       left->carrier_frequency_numerator ==
		       right->carrier_frequency_numerator &&
	       left->carrier_frequency_denominator ==
		       right->carrier_frequency_denominator &&
	       left->rolloff == right->rolloff &&
	       left->pulse_span_symbols == right->pulse_span_symbols;
}

static bool geometries_are_equal(
	const struct v34_frame_geometry *left,
	const struct v34_frame_geometry *right)
{
	return left->symbol_rate == right->symbol_rate &&
	       left->data_bits_per_frame == right->data_bits_per_frame &&
	       left->primary_data_rate_2400_bps_units ==
		       right->primary_data_rate_2400_bps_units &&
	       left->data_frames_per_superframe ==
		       right->data_frames_per_superframe &&
	       left->mapping_frames_per_data_frame ==
		       right->mapping_frames_per_data_frame &&
	       left->high_mapping_frame_bits == right->high_mapping_frame_bits &&
	       left->high_mapping_frame_count == right->high_mapping_frame_count &&
	       left->auxiliary_bits_per_frame ==
		       right->auxiliary_bits_per_frame &&
	       left->auxiliary_channel_enabled ==
		       right->auxiliary_channel_enabled;
}

static const struct v34_frame_geometry *data_geometry(
	const struct v34_data_line_receiver *receiver)
{
	return &receiver->equalizer_receiver.data_receiver.data_receiver
		.schedule.geometry;
}

static bool waveform_matches_profile(
	const struct qam_demodulator *demodulator,
	const struct v34_phase3_profile *profile)
{
	struct qam_channel expected;

	return qam_demodulator_is_valid(demodulator) &&
	       v34_data_carrier_channel(
		       profile, demodulator->channel.sample_rate, &expected) ==
		       V34_DATA_CARRIER_OK &&
	       channels_are_equal(&demodulator->channel, &expected);
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
	       waveform_matches_profile(
		       &receiver->demodulator,
		       &receiver->transmitting_profile) &&
	       qam_demodulator_measure(&receiver->demodulator).timing_locked &&
	       v34_data_equalizer_receiver_is_valid(
		       &receiver->equalizer_receiver) &&
	       receiver->equalizer_receiver.training_outputs_remaining == 0 &&
	       geometries_are_equal(
		       &receiver->geometry, data_geometry(receiver));
}

enum v34_data_line_receiver_result v34_data_line_receiver_init(
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
	    !isfinite(configuration->equalizer_adaptation) ||
	    configuration->equalizer_adaptation < 0.0 ||
	    configuration->equalizer_adaptation > 1.0 ||
	    !waveform_matches_profile(
		    demodulator, &configuration->transmitting_profile) ||
	    !qam_demodulator_measure(demodulator).timing_locked ||
	    !v34_equalizer_handoff_is_valid(handoff))
		return V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT;
	initialized.demodulator = *demodulator;
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
	struct qam_point point;
	enum v34_data_line_receiver_result result;
	bool ready;

	if (!v34_data_line_receiver_is_valid(receiver) || current == NULL ||
	    received == NULL)
		return V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_DATA_LINE_RECEIVER_FAILED;
	if (qam_demodulator_sample(
		    &receiver->demodulator, sample, &point, &ready) != QAM_OK) {
		receiver->failed = true;
		return V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE;
	}
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
